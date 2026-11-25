#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace emv
{
    using Bytes = std::vector<uint8_t>;

    // A BER-TLV node. The value is not copied; offset/length refer to the parsed buffer.
    struct Tlv
    {
        uint32_t tag = 0;
        bool constructed = false;
        size_t offset = 0;
        size_t length = 0;
        std::vector<Tlv> children;
    };

    // Data Object List entry (tag + expected length, no value), as used by the PDOL.
    struct DolEntry
    {
        uint32_t tag = 0;
        size_t length = 0;
    };

    bool ParseTlv(std::span<const uint8_t> buffer, std::vector<Tlv>& out);
    bool ParseDol(std::span<const uint8_t> dol, std::vector<DolEntry>& out);

    // Depth-first search for the first node with the given tag.
    const Tlv* FindTag(const std::vector<Tlv>& nodes, uint32_t tag);

    std::span<const uint8_t> ValueOf(std::span<const uint8_t> buffer, const Tlv& node);

    std::string ToHex(std::span<const uint8_t> data, bool spaced = true);

    // Hex dump for verbose logging with PAN, track data and expiry date replaced by "**".
    std::string ToMaskedHex(std::span<const uint8_t> data);
}
