#pragma once

#include <string>
#include <string_view>

namespace console
{
    // Switches the console to UTF-8 and turns on ANSI escape sequence processing.
    // Returns false if ANSI colors are not available (for example, when output is redirected).
    bool Initialize();

    // Writes one complete line atomically so output from different threads never interleaves.
    void WriteLine(std::string_view color, std::string_view text);

    // Returns the ANSI color assigned to a worker thread.
    std::string_view ColorForWorker(size_t index);

    std::string ToUtf8(std::wstring_view text);

    inline constexpr std::string_view NoColor   = "";
    inline constexpr std::string_view Bold      = "\x1b[1m";
    inline constexpr std::string_view ErrorRed  = "\x1b[91m";
}
