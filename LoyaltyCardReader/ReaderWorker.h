#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winscard.h>

#include "Logger.h"

#include <atomic>
#include <string>
#include <thread>

// Owns one worker thread that monitors a single smart card reader.
class ReaderWorker
{
public:
    ReaderWorker(size_t index, std::wstring readerName, bool verbose);
    ~ReaderWorker();

    ReaderWorker(const ReaderWorker&) = delete;
    ReaderWorker& operator=(const ReaderWorker&) = delete;

    void Start();
    void RequestStop();
    void Join();
    bool IsFinished() const { return finished_; }

private:
    void Run();
    void ProcessCard(SCARDCONTEXT context, const SCARD_READERSTATEW& state);

    std::wstring readerName_;
    Logger log_;
    std::thread thread_;
    std::atomic<SCARDCONTEXT> context_{ 0 };
    std::atomic<bool> stopRequested_{ false };
    std::atomic<bool> finished_{ false };
};
