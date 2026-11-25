// LoyaltyCardReader
//
// Watches every PC/SC smart card reader on the system (one worker thread per reader).
// When a payment card is presented, the worker selects the first supported EMV
// application and reads the cardholder name so the customer can be registered
// in the loyalty program without typing.
//
// Usage: LoyaltyCardReader.exe [-v | --verbose]

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winscard.h>

#include "Console.h"
#include "ReaderWorker.h"

#include <format>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "winscard.lib")

namespace
{
    HANDLE g_shutdownEvent = nullptr;

    BOOL WINAPI ConsoleControlHandler(DWORD controlType)
    {
        switch (controlType)
        {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            SetEvent(g_shutdownEvent);
            return TRUE;
        default:
            return FALSE;
        }
    }

    void PrintUsage()
    {
        console::WriteLine(console::NoColor,
            "Usage: LoyaltyCardReader.exe [options]\n"
            "\n"
            "  -v, --verbose   Show the APDUs exchanged with each card.\n"
            "                  (PAN, track data and expiry date are masked.)\n"
            "  -h, --help      Show this help.\n"
            "\n"
            "Press Ctrl+C to exit.");
    }

    void PrintError(std::string_view message)
    {
        console::WriteLine(console::ErrorRed, message);
    }

    bool EnumerateReaders(std::vector<std::wstring>& readers)
    {
        SCARDCONTEXT context = 0;
        LONG rc = SCardEstablishContext(SCARD_SCOPE_USER, nullptr, nullptr, &context);
        if (rc == SCARD_E_NO_SERVICE || rc == SCARD_E_SERVICE_STOPPED)
        {
            PrintError("The Smart Card service is not running. Connect a reader and try again.");
            return false;
        }
        if (rc != SCARD_S_SUCCESS)
        {
            PrintError(std::format("SCardEstablishContext failed: 0x{:08X}", static_cast<uint32_t>(rc)));
            return false;
        }

        LPWSTR readerList = nullptr;
        DWORD length = SCARD_AUTOALLOCATE;
        rc = SCardListReadersW(context, nullptr, reinterpret_cast<LPWSTR>(&readerList), &length);

        bool ok = true;
        if (rc == SCARD_S_SUCCESS)
        {
            // The result is a multi-string: names separated by '\0', terminated by "\0\0".
            for (const wchar_t* name = readerList; *name != L'\0'; name += wcslen(name) + 1)
            {
                readers.emplace_back(name);
            }
            SCardFreeMemory(context, readerList);
        }
        else if (rc != SCARD_E_NO_READERS_AVAILABLE)
        {
            PrintError(std::format("SCardListReaders failed: 0x{:08X}", static_cast<uint32_t>(rc)));
            ok = false;
        }

        SCardReleaseContext(context);
        return ok;
    }
}

int wmain(int argc, wchar_t* argv[])
{
    console::Initialize();

    bool verbose = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring_view arg = argv[i];
        if (arg == L"-v" || arg == L"--verbose" || arg == L"/v")
        {
            verbose = true;
        }
        else if (arg == L"-h" || arg == L"--help" || arg == L"/?")
        {
            PrintUsage();
            return 0;
        }
        else
        {
            PrintError("Unknown option: " + console::ToUtf8(arg));
            PrintUsage();
            return 1;
        }
    }

    g_shutdownEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_shutdownEvent == nullptr)
    {
        PrintError("Could not create shutdown event.");
        return 1;
    }
    SetConsoleCtrlHandler(ConsoleControlHandler, TRUE);

    console::WriteLine(console::Bold, "Loyalty Card Reader");
    if (verbose)
    {
        console::WriteLine(console::NoColor, "Verbose mode: APDUs will be displayed (sensitive fields masked).");
    }

    std::vector<std::wstring> readers;
    if (!EnumerateReaders(readers))
    {
        CloseHandle(g_shutdownEvent);
        return 1;
    }
    if (readers.empty())
    {
        PrintError("No smart card readers were found.");
        CloseHandle(g_shutdownEvent);
        return 1;
    }

    console::WriteLine(console::NoColor, std::format("Found {} reader(s):", readers.size()));
    for (size_t i = 0; i < readers.size(); ++i)
    {
        console::WriteLine(console::ColorForWorker(i),
                           std::format("  [Reader {}] {}", i + 1, console::ToUtf8(readers[i])));
    }
    console::WriteLine(console::NoColor, "Press Ctrl+C to exit.\n");

    std::vector<std::unique_ptr<ReaderWorker>> workers;
    workers.reserve(readers.size());
    for (size_t i = 0; i < readers.size(); ++i)
    {
        workers.push_back(std::make_unique<ReaderWorker>(i, readers[i], verbose));
        workers.back()->Start();
    }

    // Run until Ctrl+C, or until every reader has gone away.
    for (;;)
    {
        if (WaitForSingleObject(g_shutdownEvent, 500) == WAIT_OBJECT_0)
        {
            console::WriteLine(console::NoColor, "Shutting down...");
            break;
        }

        bool allFinished = true;
        for (const auto& worker : workers)
        {
            allFinished = allFinished && worker->IsFinished();
        }
        if (allFinished)
        {
            PrintError("No readers remain active.");
            break;
        }
    }

    for (auto& worker : workers)
    {
        worker->RequestStop();
    }
    for (auto& worker : workers)
    {
        worker->Join();
    }

    SetConsoleCtrlHandler(ConsoleControlHandler, FALSE);
    CloseHandle(g_shutdownEvent);
    return 0;
}
