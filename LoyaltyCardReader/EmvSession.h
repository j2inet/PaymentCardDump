#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winscard.h>

#include "Logger.h"
#include "Tlv.h"

#include <optional>
#include <string>
#include <string_view>

namespace emv
{
    struct ApduResponse
    {
        Bytes data;
        uint8_t sw1 = 0;
        uint8_t sw2 = 0;

        uint16_t Sw() const { return static_cast<uint16_t>((sw1 << 8) | sw2); }
        bool Ok() const { return sw1 == 0x90 && sw2 == 0x00; }
    };

    struct CardResult
    {
        bool communicationError = false;
        bool applicationSelected = false;
        Bytes aid;                      // Full AID (DF name) reported by the card
        std::string scheme;             // Scheme for the RID that matched, e.g. "Visa"
        std::string applicationLabel;   // Tag 50, if present
        std::optional<std::string> cardholderName;
        std::optional<std::string> bin;  // First 6 digits of the PAN (issuer identification)
    };

    // Runs the EMV read flow against a connected card:
    //   SELECT (by RID priority) -> GET PROCESSING OPTIONS -> READ RECORD (per AFL)
    // and stops as soon as both the cardholder name and the BIN have been found.
    class EmvSession
    {
    public:
        EmvSession(SCARDHANDLE card, DWORD protocol, const Logger& log);

        CardResult Read();

    private:
        bool Transmit(std::string_view label, const Bytes& command, ApduResponse& response);
        bool SelectApplication(CardResult& result, Bytes& pdol);
        bool GetProcessingOptions(std::span<const uint8_t> pdol, ApduResponse& response, bool& succeeded);
        bool ReadRecord(uint8_t sfi, uint8_t record, ApduResponse& response);
        bool ScanRecordsWithoutAfl(CardResult& result);

        SCARDHANDLE card_;
        DWORD protocol_;
        const Logger& log_;
    };
}
