#pragma once
#include <string>
#include <vector>

namespace ml::Log
{
    // printf-style. Lines are buffered until Claim(); after that they go to
    // MasterLooter.log next to the plugin. A copy of recent lines is always kept
    // for the Status tab.
    void Write(const char* level, const char* fmt, ...);
    void Claim();
    bool Claimed();
    void Shutdown();
    void Snapshot(std::vector<std::string>& out, int maxLines);
}

#define LOG(...)     ::ml::Log::Write("info ", __VA_ARGS__)
#define LOG_OK(...)  ::ml::Log::Write("ok   ", __VA_ARGS__)
#define LOG_ERR(...) ::ml::Log::Write("error", __VA_ARGS__)
