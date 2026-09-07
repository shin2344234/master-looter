#include "log.h"

#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>

#include "paths.h"

namespace ml::Log
{
    static std::mutex               g_mu;
    static std::deque<std::string>  g_recent;   // for the Status tab
    static std::deque<std::string>  g_pending;  // not yet on disk
    static FILE*                    g_file    = nullptr;
    static bool                     g_claimed = false;
    static constexpr size_t         kKeep     = 400;

    static std::string Stamp()
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        char b[32];
        snprintf(b, sizeof b, "%02d:%02d:%02d.%03d", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        return b;
    }

    void Write(const char* level, const char* fmt, ...)
    {
        char msg[2048];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof msg, fmt, ap);
        va_end(ap);

        std::string line = "[" + Stamp() + "] [" + level + "] " + msg;

        std::lock_guard<std::mutex> lk(g_mu);
        g_recent.push_back(line);
        if (g_recent.size() > kKeep) g_recent.pop_front();
        if (g_file)
        {
            fputs(line.c_str(), g_file);
            fputc('\n', g_file);
            fflush(g_file);
        }
        else
        {
            g_pending.push_back(line);
            if (g_pending.size() > kKeep) g_pending.pop_front();
        }
    }

    // Keep the last dozen sessions instead of one. A log is the only evidence
    // a bug report ever carries, and every launch used to destroy the previous
    // one: a session worth reading was routinely gone before anyone thought to
    // ask for it, including the capture that answered how a vein breaks.
    //
    // Plain text, not compressed, on two counts. The first thing anyone does
    // with one of these is paste it into a comment or a paste site, and an
    // archive is a barrier to that. And the plugin's import list is short by
    // design (the README's antivirus note leans on it), so pulling in a
    // compressor would lengthen the very list that argument rests on. Twelve
    // sessions come to a few megabytes, against a saved game of any size.
    static constexpr int kArchives = 11;   // plus the live one, so twelve in all

    static void Rotate()
    {
        wchar_t from[64], to[64];
        // Oldest out first, then each one shuffles up a place, so the numbers
        // read as age: 01 is the session before this one, 11 the furthest back.
        _snwprintf_s(to, _countof(to), _TRUNCATE, L"MasterLooter.%02d.log", kArchives);
        DeleteFileW(Paths::File(to).c_str());
        for (int i = kArchives - 1; i >= 1; --i)
        {
            _snwprintf_s(from, _countof(from), _TRUNCATE, L"MasterLooter.%02d.log", i);
            _snwprintf_s(to,   _countof(to),   _TRUNCATE, L"MasterLooter.%02d.log", i + 1);
            MoveFileExW(Paths::File(from).c_str(), Paths::File(to).c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        MoveFileExW(Paths::File(L"MasterLooter.log").c_str(),
                    Paths::File(L"MasterLooter.01.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    void Claim()
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_claimed) return;
        g_claimed = true;
        Rotate();
        g_file = _wfopen(Paths::File(L"MasterLooter.log").c_str(), L"w");
        if (!g_file) return;
        for (const auto& l : g_pending)
        {
            fputs(l.c_str(), g_file);
            fputc('\n', g_file);
        }
        g_pending.clear();
        fflush(g_file);
    }

    bool Claimed()
    {
        std::lock_guard<std::mutex> lk(g_mu);
        return g_claimed;
    }

    void Shutdown()
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_file) { fclose(g_file); g_file = nullptr; }
    }

    void Snapshot(std::vector<std::string>& out, int maxLines)
    {
        std::lock_guard<std::mutex> lk(g_mu);
        out.clear();
        const size_t n = g_recent.size();
        const size_t start = (maxLines > 0 && n > static_cast<size_t>(maxLines)) ? n - maxLines : 0;
        for (size_t i = start; i < n; ++i) out.push_back(g_recent[i]);
    }
}
