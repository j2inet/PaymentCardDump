#pragma once

#include "Console.h"

#include <string>
#include <string_view>

// Per-worker output channel. Every line is prefixed and printed in the worker's color.
class Logger
{
public:
    Logger(std::string prefix, std::string_view color, bool verbose)
        : prefix_(std::move(prefix)), color_(color), verbose_(verbose)
    {
    }

    void Info(std::string_view message) const
    {
        std::string line = prefix_;
        line.push_back(' ');
        line.append(message);
        console::WriteLine(color_, line);
    }

    // Only printed in verbose mode (APDU traces and diagnostic details).
    void Trace(std::string_view message) const
    {
        if (verbose_)
        {
            Info(message);
        }
    }

    bool IsVerbose() const { return verbose_; }

private:
    std::string prefix_;
    std::string color_;
    bool verbose_;
};
