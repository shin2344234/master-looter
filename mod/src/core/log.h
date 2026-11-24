#pragma once
#include <string>
#include <vector>

namespace ml::Log
{
    // printf-style. Lines are buffered until Claim(); after that they go to
    // MasterLooter.log next to the plugin, or in bin64\logs when UseFolder
    // asked for that. A copy of recent lines is always kept
    // for the Status tab.
    void Write(const char* level, const char* fmt, ...);
    // The same, but gives up rather than wait when another thread holds the
    // log. For the crash path, where the holder may be the thread that died.
    bool TryWrite(const char* level, const char* fmt, ...);
    // Before Claim: true writes the log into bin64\logs, made when missing,
    // and moves in the logs an earlier session left loose in bin64. Returns
    // how many it moved, or -1 when the folder could not be made, in which
    // case the log stays beside the plugin. Does nothing after Claim.
    int  UseFolder(bool logs);
    // The folder the log is written to, with a trailing backslash, and the
    // live log's full path.
    std::wstring Dir();
    std::wstring Path();
    void Claim();
    bool Claimed();
    void Shutdown();
    void Snapshot(std::vector<std::string>& out, int maxLines);
}

#define LOG(...)     ::ml::Log::Write("info ", __VA_ARGS__)
#define LOG_OK(...)  ::ml::Log::Write("ok   ", __VA_ARGS__)
#define LOG_ERR(...) ::ml::Log::Write("error", __VA_ARGS__)
