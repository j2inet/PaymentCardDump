#include "Tlv.h"

#include <array>
#include <format>

namespace emv
{
    namespace
    {
        constexpr int MaxDepth = 8;

        bool ReadTag(std::span<const uint8_t> b, size_t& pos, size_t end, uint32_t& tag, bool& constructed)
        {
            if (pos >= end)
            {
                return false;
            }

            const uint8_t first = b[pos++];
            tag = first;
            constructed = (first & 0x20) != 0;

            if ((first & 0x1F) == 0x1F)
            {
                // Multi-byte tag: subsequent bytes continue while bit 8 is set.
                int extra = 0;
                for (;;)
                {
                    if (pos >= end || ++extra > 3)
                    {
                        return false;
                    }
                    const uint8_t next = b[pos++];
                    tag = (tag << 8) | next;
                    if ((next & 0x80) == 0)
                    {
                        break;
                    }
                }
            }
            return true;
        }

        bool ReadLength(std::span<const uint8_t> b, size_t& pos, size_t end, size_t& length)
        {
            if (pos >= end)
            {
                return false;
            }

            const uint8_t first = b[pos++];
            if (first < 0x80)
            {
                length = first;
                return true;
            }

            const size_t count = first & 0x7F;
            if (count == 0 || count > 3 || end - pos < count)
            {
                return false;
            }

            length = 0;
            for (size_t i = 0; i < count; ++i)
            {
                length = (length << 8) | b[pos++];
            }
            return true;
        }

        bool ParseRange(std::span<const uint8_t> b, size_t pos, size_t end, std::vector<Tlv>& out, int depth)
        {
            if (depth > MaxDepth)
            {
                return false;
            }

            while (pos < end)
            {
                // EMV allows 0x00 / 0xFF padding between data objects.
                if (b[pos] == 0x00 || b[pos] == 0xFF)
                {
                    ++pos;
                    continue;
                }

                Tlv node;
                if (!ReadTag(b, pos, end, node.tag, node.constructed) ||
                    !ReadLength(b, pos, end, node.length) ||
                    node.length > end - pos)
                {
                    return false;
                }

                node.offset = pos;
                if (node.constructed && !ParseRange(b, pos, pos + node.length, node.children, depth + 1))
                {
                    return false;
                }

                pos += node.length;
                out.push_back(std::move(node));
            }
            return true;
        }

        bool IsSensitiveTag(uint32_t tag)
        {
            switch (tag)
            {
            case 0x5A:   // Application PAN
            case 0x56:   // Track 1 data
            case 0x57:   // Track 2 equivalent data
            case 0x5F24: // Application expiration date
            case 0x9F1F: // Track 1 discretionary data
            case 0x9F20: // Track 2 discretionary data
            case 0x9F6B: // Track 2 data (Mastercard contactless)
                return true;
            default:
                return false;
            }
        }

        void MarkSensitive(const std::vector<Tlv>& nodes, std::vector<bool>& mask)
        {
            for (const Tlv& node : nodes)
            {
                if (IsSensitiveTag(node.tag))
                {
                    for (size_t i = node.offset; i < node.offset + node.length; ++i)
                    {
                        mask[i] = true;
                    }
                }
                else if (node.constructed)
                {
                    MarkSensitive(node.children, mask);
                }
            }
        }
    }

    bool ParseTlv(std::span<const uint8_t> buffer, std::vector<Tlv>& out)
    {
        out.clear();
        return ParseRange(buffer, 0, buffer.size(), out, 0);
    }

    bool ParseDol(std::span<const uint8_t> dol, std::vector<DolEntry>& out)
    {
        out.clear();
        size_t pos = 0;
        while (pos < dol.size())
        {
            DolEntry entry;
            bool constructed = false;
            if (!ReadTag(dol, pos, dol.size(), entry.tag, constructed) ||
                !ReadLength(dol, pos, dol.size(), entry.length))
            {
                return false;
            }
            out.push_back(entry);
        }
        return true;
    }

    const Tlv* FindTag(const std::vector<Tlv>& nodes, uint32_t tag)
    {
        for (const Tlv& node : nodes)
        {
            if (node.tag == tag)
            {
                return &node;
            }
            if (node.constructed)
            {
                if (const Tlv* found = FindTag(node.children, tag))
                {
                    return found;
                }
            }
        }
        return nullptr;
    }

    std::span<const uint8_t> ValueOf(std::span<const uint8_t> buffer, const Tlv& node)
    {
        return buffer.subspan(node.offset, node.length);
    }

    std::string ToHex(std::span<const uint8_t> data, bool spaced)
    {
        std::string result;
        result.reserve(data.size() * 3);
        for (size_t i = 0; i < data.size(); ++i)
        {
            if (spaced && i > 0)
            {
                result.push_back(' ');
            }
            result += std::format("{:02X}", data[i]);
        }
        return result;
    }

    std::string ToMaskedHex(std::span<const uint8_t> data)
    {
        if (data.empty())
        {
            return {};
        }

        std::vector<Tlv> nodes;
        if (!ParseTlv(data, nodes))
        {
            // If we cannot tell where the sensitive fields are, do not show any of it.
            return std::format("[{} bytes withheld - not valid TLV]", data.size());
        }

        std::vector<bool> mask(data.size(), false);
        MarkSensitive(nodes, mask);

        std::string result;
        result.reserve(data.size() * 3);
        for (size_t i = 0; i < data.size(); ++i)
        {
            if (i > 0)
            {
                result.push_back(' ');
            }
            result += mask[i] ? std::string("**") : std::format("{:02X}", data[i]);
        }
        return result;
    }
}
