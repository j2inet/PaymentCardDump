#include "EmvSession.h"

#include <algorithm>
#include <array>
#include <format>
#include <random>

namespace emv
{
    namespace
    {
        struct PaymentRid
        {
            Bytes rid;
            const char* scheme;
        };

        // Selection order, highest priority first.
        const std::array<PaymentRid, 7> PreferredRids =
        {{
            { { 0xA0, 0x00, 0x00, 0x00, 0x03 }, "Visa" },
            { { 0xA0, 0x00, 0x00, 0x00, 0x04 }, "Mastercard" },
            { { 0xA0, 0x00, 0x00, 0x00, 0x25 }, "American Express" },
            { { 0xA0, 0x00, 0x00, 0x01, 0x52 }, "Discover" },
            { { 0xA0, 0x00, 0x00, 0x00, 0x65 }, "JCB" },
            { { 0xA0, 0x00, 0x00, 0x03, 0x33 }, "UnionPay" },
            { { 0xA0, 0x00, 0x00, 0x05, 0x24 }, "RuPay" },
        }};

        constexpr uint32_t TagFciTemplate          = 0x6F;
        constexpr uint32_t TagDfName               = 0x84;
        constexpr uint32_t TagApplicationLabel     = 0x50;
        constexpr uint32_t TagPdol                 = 0x9F38;
        constexpr uint32_t TagGpoFormat1           = 0x80;
        constexpr uint32_t TagAfl                  = 0x94;
        constexpr uint32_t TagCardholderName       = 0x5F20;
        constexpr uint32_t TagCardholderNameExt    = 0x9F0B;
        constexpr uint32_t TagTrack1               = 0x56;
        constexpr uint32_t TagPan                  = 0x5A;
        constexpr uint32_t TagTrack2               = 0x57;
        constexpr uint32_t TagTrack2Mastercard     = 0x9F6B;

        constexpr size_t BinLength = 6;

        uint8_t ToBcd(int value)
        {
            return static_cast<uint8_t>((((value / 10) % 10) << 4) | (value % 10));
        }

        // Terminal-side values used to fill in the card's PDOL. These describe a
        // generic attended terminal and a zero-amount transaction; GET PROCESSING
        // OPTIONS on its own does not authorize or charge anything.
        Bytes TerminalValueFor(uint32_t tag)
        {
            SYSTEMTIME now{};
            GetLocalTime(&now);

            switch (tag)
            {
            case 0x9F66: return { 0x36, 0x80, 0x40, 0x00 };             // Terminal Transaction Qualifiers
            case 0x9F02: return { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }; // Amount, authorized
            case 0x9F03: return { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }; // Amount, other
            case 0x9F1A: return { 0x08, 0x40 };                         // Terminal country code (USA)
            case 0x5F2A: return { 0x08, 0x40 };                         // Transaction currency code (USD)
            case 0x95:   return { 0x00, 0x00, 0x00, 0x00, 0x00 };       // Terminal verification results
            case 0x9A:   return { ToBcd(now.wYear % 100), ToBcd(now.wMonth), ToBcd(now.wDay) };
            case 0x9F21: return { ToBcd(now.wHour), ToBcd(now.wMinute), ToBcd(now.wSecond) };
            case 0x9C:   return { 0x00 };                               // Transaction type: purchase
            case 0x9F35: return { 0x22 };                               // Terminal type: attended, offline w/ online capability
            case 0x9F33: return { 0xE0, 0xB0, 0xC8 };                   // Terminal capabilities
            case 0x9F40: return { 0x60, 0x00, 0xF0, 0xA0, 0x01 };       // Additional terminal capabilities
            case 0x9F37:                                                // Unpredictable number
            {
                std::random_device rd;
                return { static_cast<uint8_t>(rd()), static_cast<uint8_t>(rd()),
                         static_cast<uint8_t>(rd()), static_cast<uint8_t>(rd()) };
            }
            default:
                return {};
            }
        }

        Bytes BuildPdolData(std::span<const uint8_t> pdol)
        {
            std::vector<DolEntry> entries;
            if (!ParseDol(pdol, entries))
            {
                return {};
            }

            Bytes data;
            for (const DolEntry& entry : entries)
            {
                Bytes value = TerminalValueFor(entry.tag);
                value.resize(entry.length, 0x00); // Unknown tags are zero-filled.
                data.insert(data.end(), value.begin(), value.end());
            }
            return data;
        }

        std::string Trim(std::string_view text)
        {
            const size_t first = text.find_first_not_of(' ');
            if (first == std::string_view::npos)
            {
                return {};
            }
            const size_t last = text.find_last_not_of(' ');
            return std::string(text.substr(first, last - first + 1));
        }

        // Card names are stored as "SURNAME/FIRST MIDDLE"; return "FIRST MIDDLE SURNAME".
        std::optional<std::string> FormatName(std::span<const uint8_t> raw)
        {
            std::string text;
            for (uint8_t c : raw)
            {
                if (c >= 0x20 && c <= 0x7E)
                {
                    text.push_back(static_cast<char>(c));
                }
            }

            text = Trim(text);
            const size_t slash = text.find('/');
            if (slash != std::string::npos)
            {
                const std::string surname = Trim(std::string_view(text).substr(0, slash));
                const std::string given = Trim(std::string_view(text).substr(slash + 1));
                text = given.empty() ? surname : (surname.empty() ? given : given + " " + surname);
            }

            if (text.empty())
            {
                return std::nullopt;
            }
            return text;
        }

        // Track 1 format: B<PAN>^<NAME>^<expiry/service code/discretionary>
        std::optional<std::string> NameFromTrack1(std::span<const uint8_t> track1)
        {
            const auto first = std::find(track1.begin(), track1.end(), static_cast<uint8_t>('^'));
            if (first == track1.end())
            {
                return std::nullopt;
            }
            const auto second = std::find(first + 1, track1.end(), static_cast<uint8_t>('^'));
            if (second == track1.end())
            {
                return std::nullopt;
            }
            return FormatName(std::span<const uint8_t>(first + 1, second));
        }

        // First 6 digits of a BCD-encoded PAN (tag 5A) or Track 2 image (tags 57 / 9F6B).
        // Track 2 uses nibble 'D' as the field separator, so stop at the first non-digit.
        std::optional<std::string> BinFromBcd(std::span<const uint8_t> value)
        {
            std::string digits;
            for (uint8_t byte : value)
            {
                for (const uint8_t nibble : { static_cast<uint8_t>(byte >> 4), static_cast<uint8_t>(byte & 0x0F) })
                {
                    if (nibble > 9)
                    {
                        return std::nullopt;
                    }
                    digits.push_back(static_cast<char>('0' + nibble));
                    if (digits.size() == BinLength)
                    {
                        return digits;
                    }
                }
            }
            return std::nullopt;
        }

        // Track 1 format: [%]B<PAN>^NAME^...
        std::optional<std::string> BinFromTrack1(std::span<const uint8_t> track1)
        {
            size_t pos = 0;
            if (pos < track1.size() && track1[pos] == '%')
            {
                ++pos;
            }
            if (pos < track1.size() && track1[pos] == 'B')
            {
                ++pos;
            }

            std::string digits;
            for (; pos < track1.size() && digits.size() < BinLength; ++pos)
            {
                if (track1[pos] < '0' || track1[pos] > '9')
                {
                    return std::nullopt;
                }
                digits.push_back(static_cast<char>(track1[pos]));
            }
            return digits.size() == BinLength ? std::optional<std::string>(digits) : std::nullopt;
        }

        std::optional<std::string> ExtractBin(std::span<const uint8_t> buffer, const std::vector<Tlv>& nodes)
        {
            for (const uint32_t tag : { TagPan, TagTrack2, TagTrack2Mastercard })
            {
                if (const Tlv* node = FindTag(nodes, tag))
                {
                    if (auto bin = BinFromBcd(ValueOf(buffer, *node)))
                    {
                        return bin;
                    }
                }
            }
            if (const Tlv* node = FindTag(nodes, TagTrack1))
            {
                return BinFromTrack1(ValueOf(buffer, *node));
            }
            return std::nullopt;
        }

        std::optional<std::string> ExtractCardholderName(std::span<const uint8_t> buffer, const std::vector<Tlv>& nodes)
        {
            if (const Tlv* node = FindTag(nodes, TagCardholderName))
            {
                if (auto name = FormatName(ValueOf(buffer, *node)))
                {
                    return name;
                }
            }
            if (const Tlv* node = FindTag(nodes, TagCardholderNameExt))
            {
                if (auto name = FormatName(ValueOf(buffer, *node)))
                {
                    return name;
                }
            }
            if (const Tlv* node = FindTag(nodes, TagTrack1))
            {
                if (auto name = NameFromTrack1(ValueOf(buffer, *node)))
                {
                    return name;
                }
            }
            return std::nullopt;
        }

        // Pulls whatever is still missing (name, BIN) out of a response.
        // Returns true once both have been found and no more records need to be read.
        // Only the first 6 digits of the PAN are kept; the full PAN is never stored.
        bool HarvestCardData(std::span<const uint8_t> buffer, CardResult& result)
        {
            std::vector<Tlv> nodes;
            if (ParseTlv(buffer, nodes))
            {
                if (!result.cardholderName)
                {
                    result.cardholderName = ExtractCardholderName(buffer, nodes);
                }
                if (!result.bin)
                {
                    result.bin = ExtractBin(buffer, nodes);
                }
            }
            return result.cardholderName && result.bin;
        }

        bool IsCase4(const Bytes& command)
        {
            return command.size() > 5 && command.size() == 5u + command[4] + 1u;
        }
    }

    EmvSession::EmvSession(SCARDHANDLE card, DWORD protocol, const Logger& log)
        : card_(card), protocol_(protocol), log_(log)
    {
    }

    bool EmvSession::Transmit(std::string_view label, const Bytes& original, ApduResponse& response)
    {
        response = {};

        Bytes command = original;
        if (protocol_ == SCARD_PROTOCOL_T0 && IsCase4(command))
        {
            // T=0 has no Le for case 4 commands; the card answers 61xx and we GET RESPONSE.
            command.pop_back();
        }

        const SCARD_IO_REQUEST* pci = (protocol_ == SCARD_PROTOCOL_T1) ? SCARD_PCI_T1 : SCARD_PCI_T0;

        for (int attempt = 0; attempt < 16; ++attempt)
        {
            log_.Trace(std::format("  >> {:<16} {}", label, ToHex(command)));

            std::array<uint8_t, 4096> buffer{};
            DWORD received = static_cast<DWORD>(buffer.size());
            const LONG rc = SCardTransmit(card_, pci, command.data(), static_cast<DWORD>(command.size()),
                                          nullptr, buffer.data(), &received);
            if (rc != SCARD_S_SUCCESS)
            {
                log_.Info(std::format("Card communication failed during {} (0x{:08X}).", label,
                                      static_cast<uint32_t>(rc)));
                return false;
            }
            if (received < 2)
            {
                log_.Info(std::format("Card returned a malformed response to {}.", label));
                return false;
            }

            const uint8_t sw1 = buffer[received - 2];
            const uint8_t sw2 = buffer[received - 1];
            const std::span<const uint8_t> data(buffer.data(), received - 2);

            log_.Trace(std::format("  << {:<16} {}{}SW={:02X}{:02X}", label, ToMaskedHex(data),
                                   data.empty() ? "" : "  ", sw1, sw2));

            response.data.insert(response.data.end(), data.begin(), data.end());

            if (sw1 == 0x61)
            {
                // More data available: fetch it with GET RESPONSE.
                command = { 0x00, 0xC0, 0x00, 0x00, sw2 };
                label = "GET RESPONSE";
                continue;
            }
            if (sw1 == 0x6C)
            {
                // Wrong Le: resend the same command with the exact length the card wants.
                command = original;
                command.back() = sw2;
                continue;
            }

            response.sw1 = sw1;
            response.sw2 = sw2;
            return true;
        }

        log_.Info(std::format("Too many response-chaining steps during {}.", label));
        return false;
    }

    bool EmvSession::SelectApplication(CardResult& result, Bytes& pdol)
    {
        for (const PaymentRid& candidate : PreferredRids)
        {
            Bytes command = { 0x00, 0xA4, 0x04, 0x00, static_cast<uint8_t>(candidate.rid.size()) };
            command.insert(command.end(), candidate.rid.begin(), candidate.rid.end());
            command.push_back(0x00);

            ApduResponse response;
            if (!Transmit("SELECT", command, response))
            {
                result.communicationError = true;
                return false;
            }

            if (!response.Ok())
            {
                log_.Trace(std::format("  {} ({}) not selectable, SW={:04X}", ToHex(candidate.rid, false),
                                       candidate.scheme, response.Sw()));
                continue;
            }

            result.applicationSelected = true;
            result.scheme = candidate.scheme;
            result.aid = candidate.rid;

            std::vector<Tlv> nodes;
            if (ParseTlv(response.data, nodes))
            {
                const std::span<const uint8_t> fci(response.data);
                if (const Tlv* dfName = FindTag(nodes, TagDfName))
                {
                    const auto value = ValueOf(fci, *dfName);
                    result.aid.assign(value.begin(), value.end());
                }
                if (const Tlv* label = FindTag(nodes, TagApplicationLabel))
                {
                    const auto value = ValueOf(fci, *label);
                    result.applicationLabel = Trim(std::string(value.begin(), value.end()));
                }
                if (const Tlv* pdolNode = FindTag(nodes, TagPdol))
                {
                    const auto value = ValueOf(fci, *pdolNode);
                    pdol.assign(value.begin(), value.end());
                }
                if (!FindTag(nodes, TagFciTemplate))
                {
                    log_.Trace("  Warning: SELECT response has no FCI template (6F).");
                }
            }
            return true;
        }
        return false;
    }

    bool EmvSession::GetProcessingOptions(std::span<const uint8_t> pdol, ApduResponse& response, bool& succeeded)
    {
        succeeded = false;

        const Bytes pdolData = BuildPdolData(pdol);
        if (pdolData.size() > 0x7F)
        {
            log_.Trace("  PDOL data too large; skipping GET PROCESSING OPTIONS.");
            return true;
        }

        Bytes command = { 0x80, 0xA8, 0x00, 0x00, static_cast<uint8_t>(pdolData.size() + 2),
                          0x83, static_cast<uint8_t>(pdolData.size()) };
        command.insert(command.end(), pdolData.begin(), pdolData.end());
        command.push_back(0x00);

        if (!Transmit("GPO", command, response))
        {
            return false;
        }

        succeeded = response.Ok();
        if (!succeeded)
        {
            log_.Trace(std::format("  GET PROCESSING OPTIONS refused, SW={:04X}", response.Sw()));
        }
        return true;
    }

    bool EmvSession::ReadRecord(uint8_t sfi, uint8_t record, ApduResponse& response)
    {
        const Bytes command = { 0x00, 0xB2, record, static_cast<uint8_t>((sfi << 3) | 0x04), 0x00 };
        return Transmit(std::format("READ REC {}/{}", sfi, record), command, response);
    }

    bool EmvSession::ScanRecordsWithoutAfl(CardResult& result)
    {
        // Fallback for cards that refuse GPO: many still allow records to be read directly.
        log_.Trace("  No AFL available; scanning SFI 1-10 directly.");
        for (uint8_t sfi = 1; sfi <= 10; ++sfi)
        {
            for (uint8_t record = 1; record <= 16; ++record)
            {
                ApduResponse response;
                if (!ReadRecord(sfi, record, response))
                {
                    result.communicationError = true;
                    return false;
                }
                if (!response.Ok())
                {
                    break; // 6A83 (no such record) or 6A82 (no such file): next SFI.
                }
                if (HarvestCardData(response.data, result))
                {
                    return true;
                }
            }
        }
        return true;
    }

    CardResult EmvSession::Read()
    {
        CardResult result;

        Bytes pdol;
        if (!SelectApplication(result, pdol))
        {
            return result;
        }

        ApduResponse gpo;
        bool gpoSucceeded = false;
        if (!GetProcessingOptions(pdol, gpo, gpoSucceeded))
        {
            result.communicationError = true;
            return result;
        }

        Bytes afl;
        if (gpoSucceeded)
        {
            // Contactless cards often return Track 2 (and sometimes the name) directly in GPO.
            if (HarvestCardData(gpo.data, result))
            {
                return result;
            }

            std::vector<Tlv> nodes;
            if (ParseTlv(gpo.data, nodes) && !nodes.empty())
            {
                const std::span<const uint8_t> buffer(gpo.data);
                if (nodes.front().tag == TagGpoFormat1)
                {
                    // Format 1: AIP (2 bytes) followed by the AFL.
                    const auto value = ValueOf(buffer, nodes.front());
                    if (value.size() > 2)
                    {
                        afl.assign(value.begin() + 2, value.end());
                    }
                }
                else if (const Tlv* aflNode = FindTag(nodes, TagAfl))
                {
                    const auto value = ValueOf(buffer, *aflNode);
                    afl.assign(value.begin(), value.end());
                }
            }
        }

        if (afl.size() < 4)
        {
            ScanRecordsWithoutAfl(result);
            return result;
        }

        // Each AFL entry is 4 bytes: SFI<<3, first record, last record, # offline-auth records.
        for (size_t i = 0; i + 4 <= afl.size(); i += 4)
        {
            const uint8_t sfi = static_cast<uint8_t>(afl[i] >> 3);
            const uint8_t first = afl[i + 1];
            const uint8_t last = afl[i + 2];

            for (unsigned record = first; record <= last && record != 0; ++record)
            {
                ApduResponse response;
                if (!ReadRecord(sfi, static_cast<uint8_t>(record), response))
                {
                    result.communicationError = true;
                    return result;
                }
                if (!response.Ok())
                {
                    continue;
                }
                if (HarvestCardData(response.data, result))
                {
                    return result;
                }
            }
        }
        return result;
    }
}
