#include "ReaderWorker.h"

#include "EmvSession.h"

#include <format>

namespace
{
    // Upper bound on each wait so a stop request is noticed even if SCardCancel races.
    constexpr DWORD StatusPollTimeoutMs = 1000;

    std::string ScardError(LONG rc)
    {
        char* buffer = nullptr;
        const DWORD length = FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, static_cast<DWORD>(rc), 0, reinterpret_cast<LPSTR>(&buffer), 0, nullptr);

        std::string text = std::format("0x{:08X}", static_cast<uint32_t>(rc));
        if (length > 0 && buffer != nullptr)
        {
            std::string message(buffer, length);
            while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' '))
            {
                message.pop_back();
            }
            text += " " + message;
        }
        if (buffer != nullptr)
        {
            LocalFree(buffer);
        }
        return text;
    }

    const char* ProtocolName(DWORD protocol)
    {
        switch (protocol)
        {
        case SCARD_PROTOCOL_T0: return "T=0";
        case SCARD_PROTOCOL_T1: return "T=1";
        default:                return "raw";
        }
    }
}

ReaderWorker::ReaderWorker(size_t index, std::wstring readerName, bool verbose)
    : readerName_(std::move(readerName)),
      log_(std::format("[Reader {}]", index + 1), console::ColorForWorker(index), verbose)
{
}

ReaderWorker::~ReaderWorker()
{
    RequestStop();
    Join();
}

void ReaderWorker::Start()
{
    thread_ = std::thread(&ReaderWorker::Run, this);
}

void ReaderWorker::RequestStop()
{
    stopRequested_ = true;
    if (const SCARDCONTEXT context = context_.load(); context != 0)
    {
        SCardCancel(context); // Wakes up a blocked SCardGetStatusChange.
    }
}

void ReaderWorker::Join()
{
    if (thread_.joinable())
    {
        thread_.join();
    }
}

void ReaderWorker::Run()
{
    // Each worker uses its own resource manager context.
    SCARDCONTEXT context = 0;
    LONG rc = SCardEstablishContext(SCARD_SCOPE_USER, nullptr, nullptr, &context);
    if (rc != SCARD_S_SUCCESS)
    {
        log_.Info("Could not establish smart card context: " + ScardError(rc));
        finished_ = true;
        return;
    }
    context_ = context;

    log_.Info("Waiting for a card...");

    SCARD_READERSTATEW state{};
    state.szReader = readerName_.c_str();
    state.dwCurrentState = SCARD_STATE_UNAWARE;

    bool cardPresent = false;

    while (!stopRequested_)
    {
        rc = SCardGetStatusChangeW(context, StatusPollTimeoutMs, &state, 1);
        if (rc == SCARD_E_TIMEOUT)
        {
            continue;
        }
        if (rc == SCARD_E_CANCELLED)
        {
            break;
        }
        if (rc != SCARD_S_SUCCESS)
        {
            log_.Info("Reader monitoring failed: " + ScardError(rc));
            if (rc == SCARD_E_SERVICE_STOPPED || rc == SCARD_E_NO_SERVICE ||
                rc == SCARD_E_NO_READERS_AVAILABLE || rc == SCARD_E_UNKNOWN_READER ||
                rc == SCARD_E_READER_UNAVAILABLE || rc == SCARD_E_INVALID_HANDLE)
            {
                break;
            }
            Sleep(1000);
            continue;
        }

        const DWORD events = state.dwEventState;
        state.dwCurrentState = events & ~SCARD_STATE_CHANGED;

        if (events & (SCARD_STATE_UNKNOWN | SCARD_STATE_UNAVAILABLE | SCARD_STATE_IGNORE))
        {
            log_.Info("Reader is no longer available. Worker exiting.");
            break;
        }

        const bool present = (events & SCARD_STATE_PRESENT) != 0;
        if (present && !cardPresent)
        {
            cardPresent = true;
            if (events & SCARD_STATE_MUTE)
            {
                log_.Info("Card detected but it is not responding. Please re-insert the card.");
            }
            else
            {
                ProcessCard(context, state);
                log_.Info("Remove the card to continue.");
            }
        }
        else if (!present && cardPresent)
        {
            cardPresent = false;
            log_.Info("Card removed. Waiting for a card...");
        }
    }

    context_ = 0;
    SCardReleaseContext(context);
    finished_ = true;
}

void ReaderWorker::ProcessCard(SCARDCONTEXT context, const SCARD_READERSTATEW& state)
{
    log_.Info("Card detected.");
    log_.Trace("  ATR: " + emv::ToHex(std::span<const uint8_t>(state.rgbAtr, state.cbAtr)));

    SCARDHANDLE card = 0;
    DWORD protocol = 0;
    const LONG rc = SCardConnectW(context, readerName_.c_str(), SCARD_SHARE_SHARED,
                                  SCARD_PROTOCOL_T0 | SCARD_PROTOCOL_T1, &card, &protocol);
    if (rc != SCARD_S_SUCCESS)
    {
        log_.Info("Could not connect to the card: " + ScardError(rc));
        return;
    }
    log_.Trace(std::format("  Connected using protocol {}", ProtocolName(protocol)));

    emv::EmvSession session(card, protocol, log_);
    const emv::CardResult result = session.Read();

    SCardDisconnect(card, SCARD_RESET_CARD);

    if (result.applicationSelected)
    {
        std::string description = result.scheme;
        if (!result.applicationLabel.empty())
        {
            description += ", " + result.applicationLabel;
        }
        log_.Info(std::format("ACCEPTED  AID: {} ({})", emv::ToHex(result.aid, false), description));

        if (result.cardholderName)
        {
            log_.Info(std::format("          Name: {}", *result.cardholderName));
        }
        else if (result.communicationError)
        {
            log_.Info("          Name: <card was removed before the name could be read>");
        }
        else
        {
            log_.Info("          Name: <not provided by this card>");
        }
    }
    else if (result.communicationError)
    {
        log_.Info("Could not finish reading the card. Please try again.");
    }
    else
    {
        log_.Info("REJECTED  This card does not contain a supported payment application.");
    }
}
