#include "Console.h"

#include <windows.h>

#include <array>
#include <cstdio>
#include <mutex>

namespace console
{
    namespace
    {
        std::mutex g_outputMutex;
        bool g_ansiEnabled = false;

        constexpr std::string_view Reset = "\x1b[0m";

        // Red is deliberately left out so it stays reserved for program-level errors.
        constexpr std::array<std::string_view, 10> WorkerPalette =
        {
            "\x1b[92m", // bright green
            "\x1b[96m", // bright cyan
            "\x1b[93m", // bright yellow
            "\x1b[95m", // bright magenta
            "\x1b[94m", // bright blue
            "\x1b[97m", // bright white
            "\x1b[32m", // green
            "\x1b[36m", // cyan
            "\x1b[33m", // yellow
            "\x1b[35m", // magenta
        };
    }

    bool Initialize()
    {
        SetConsoleOutputCP(CP_UTF8);

        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (out == nullptr || out == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        DWORD mode = 0;
        if (!GetConsoleMode(out, &mode))
        {
            return false; // Output is redirected to a file or pipe.
        }

        mode |= ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        g_ansiEnabled = SetConsoleMode(out, mode) != FALSE;
        return g_ansiEnabled;
    }

    void WriteLine(std::string_view color, std::string_view text)
    {
        std::string line;
        line.reserve(text.size() + 16);

        const bool useColor = g_ansiEnabled && !color.empty();
        if (useColor)
        {
            line.append(color);
        }
        line.append(text);
        if (useColor)
        {
            line.append(Reset);
        }
        line.push_back('\n');

        std::lock_guard lock(g_outputMutex);
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fflush(stdout);
    }

    std::string_view ColorForWorker(size_t index)
    {
        return WorkerPalette[index % WorkerPalette.size()];
    }

    std::string ToUtf8(std::wstring_view text)
    {
        if (text.empty())
        {
            return {};
        }

        const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                             nullptr, 0, nullptr, nullptr);
        std::string result(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                            result.data(), size, nullptr, nullptr);
        return result;
    }
}
