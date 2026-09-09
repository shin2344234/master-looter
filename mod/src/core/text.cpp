#include "text.h"

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

#include "log.h"
#include "paths.h"

namespace ml::Text
{
    static std::unordered_map<std::string, std::string> g_map;
    static std::set<std::string>  g_seen;      // ordered, so the template comes out stable
    static std::string            g_lang;
    static int                    g_rejected = 0;
    static bool                   g_fromFile = false;
    static std::recursive_mutex   g_mu;

    // Kept in step with the RCDATA entries in resources.rc. Adding a
    // language means a line in each.
    static const Lang kBuiltIn[] = {
        { "pt-br", "Português (Brasil)", "Kyo-70" },
        { "zh-cn", "简体中文", "dofo7777" },
        { "zh-tw", "繁體中文", "dofo7777" },
    };

    // A translation is used verbatim in printf-style calls, so one that does
    // not carry the same placeholders in the same order would read the wrong
    // arguments off the stack. That is a crash, and it would be blamed on the
    // mod rather than on the translation, so such a line is dropped at load.
    static bool SamePlaceholders(const std::string& a, const std::string& b)
    {
        auto specs = [](const std::string& s) {
            std::string out;
            for (size_t i = 0; i + 1 < s.size(); ++i)
            {
                if (s[i] != '%') continue;
                if (s[i + 1] == '%') { ++i; continue; }
                size_t j = i + 1;
                while (j < s.size() && !strchr("diouxXeEfgGaAcspn%", s[j])) ++j;
                if (j < s.size()) { out += s.substr(i, j - i + 1); i = j; }
            }
            return out;
        };
        return specs(a) == specs(b);
    }

    // The language comes from the ini and from a text box in the menu, and
    // it is pasted into a file name and a resource name. Anything outside
    // this set could walk out of the plugin's folder, so it is refused
    // rather than sanitised into something the reader did not ask for.
    static bool Nameable(const char* lang)
    {
        size_t n = 0;
        for (const char* p = lang; *p; ++p, ++n)
        {
            const unsigned char c = static_cast<unsigned char>(*p);
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_';
            if (!ok) return false;
        }
        return n > 0 && n < 16;
    }

    static std::string Unescape(const std::string& s)
    {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == '\\' && i + 1 < s.size())
            {
                if (s[i + 1] == 'n') { out += '\n'; ++i; continue; }
                if (s[i + 1] == 't') { out += '\t'; ++i; continue; }
                if (s[i + 1] == '\\') { out += '\\'; ++i; continue; }
                if (s[i + 1] == '"') { out += '"'; ++i; continue; }
            }
            out += s[i];
        }
        return out;
    }

    static std::string Escape(const std::string& s)
    {
        std::string out;
        for (char c : s)
        {
            if (c == '\n') out += "\\n";
            else if (c == '\t') out += "\\t";
            else if (c == '\\') out += "\\\\";
            else out += c;
        }
        return out;
    }

    bool Load(const char* lang)
    {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        g_map.clear();
        g_rejected = 0;
        g_lang.clear();
        g_fromFile = false;
        if (!lang || !*lang || _stricmp(lang, "en") == 0) { LOG("Menu language: English."); return true; }
        if (!Nameable(lang)) { LOG_ERR("Menu language \"%s\": not a language name, staying in English.", lang); return false; }

        wchar_t name[64], res[64];
        _snwprintf_s(name, _TRUNCATE, L"MasterLooter.%hs.txt", lang);
        _snwprintf_s(res, _TRUNCATE, L"ML_LANG_%hs", lang);
        for (wchar_t* w = res; *w; ++w)
        {
            if (*w >= L'a' && *w <= L'z') *w = static_cast<wchar_t>(*w - L'a' + L'A');
            else if (*w == L'-') *w = L'_';   // zh-cn is the file, ML_LANG_ZH_CN the resource
        }

        std::string text;
        if (!Paths::ReadDataText(name, res, text, &g_fromFile))
        {
            LOG_ERR("Menu language \"%s\": no %ls beside the plugin and none built in, staying in English.", lang, name);
            return false;
        }

        size_t pos = 0;
        while (pos < text.size())
        {
            size_t nl = text.find('\n', pos);
            if (nl == std::string::npos) nl = text.size();
            std::string line = text.substr(pos, nl - pos);
            pos = nl + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            const size_t tab = line.find('\t');
            if (tab == std::string::npos) continue;
            const std::string key = Unescape(line.substr(0, tab));
            const std::string val = Unescape(line.substr(tab + 1));
            if (key.empty() || val.empty()) continue;      // not translated yet
            if (!SamePlaceholders(key, val)) { ++g_rejected; continue; }
            g_map[key] = val;
        }
        g_lang = lang;
        if (g_rejected)
            LOG_ERR("Menu language \"%s\": %d line(s) ignored because their %% placeholders did not match the English, which would read the wrong values.", lang, g_rejected);
        LOG("Menu language \"%s\": %d string(s) translated, from %s.", lang, static_cast<int>(g_map.size()),
            g_fromFile ? "the file beside the plugin" : "the copy inside the plugin");
        return !g_map.empty();
    }

    const char* Language() { return g_lang.c_str(); }
    int Count() { std::lock_guard<std::recursive_mutex> lk(g_mu); return static_cast<int>(g_map.size()); }
    int Rejected() { return g_rejected; }
    bool FromFile() { return g_fromFile; }

    const Lang* BuiltIn(int& count)
    {
        count = static_cast<int>(sizeof kBuiltIn / sizeof kBuiltIn[0]);
        return kBuiltIn;
    }

    const Lang* Find(const char* code)
    {
        if (!code || !*code) return nullptr;
        for (const Lang& l : kBuiltIn)
            if (_stricmp(l.code, code) == 0) return &l;
        return nullptr;
    }
    int Seen() { std::lock_guard<std::recursive_mutex> lk(g_mu); return static_cast<int>(g_seen.size()); }

    const char* Get(const char* english)
    {
        if (!english || !*english) return english;
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        // Remembering what the interface asks for is what makes the template
        // complete; it costs one set insertion per string per session, since
        // the same pointer is asked for every frame but the set already holds
        // it after the first.
        if (g_seen.size() < 4096) g_seen.insert(english);
        if (g_map.empty()) return english;
        auto it = g_map.find(english);
        return it == g_map.end() ? english : it->second.c_str();
    }

    void ForEachTranslation(void (*fn)(const char*, void*), void* user)
    {
        if (!fn) return;
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        for (const auto& kv : g_map) fn(kv.second.c_str(), user);
    }

    bool WriteTemplate()
    {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        FILE* f = _wfopen(Paths::File(L"MasterLooter.template.txt").c_str(), L"wb");
        if (!f) { LOG_ERR("Could not write MasterLooter.template.txt."); return false; }
        fputs("# Master Looter menu text.\n", f);
        fputs("# One string per line: the English, a tab, then your translation.\n", f);
        fputs("# \\n is a line break. Lines starting with # are ignored. A line with\n", f);
        fputs("# nothing after the tab is left in English.\n", f);
        fputs("# Keep every %d, %s and %.1f exactly as they appear and in the same\n", f);
        fputs("# order: they are replaced with numbers and names at runtime, and a\n", f);
        fputs("# line that changes them is ignored rather than risked.\n", f);
        fputs("# Save as MasterLooter.<language>.txt beside the plugin, for example\n", f);
        fputs("# MasterLooter.de.txt, then set Language=de in MasterLooter.ini.\n", f);
        fputs("#\n", f);
        fputs("# Only what has been on screen this session is listed, so open every\n", f);
        fputs("# tab before writing this out.\n\n", f);
        for (const auto& s : g_seen)
        {
            const std::string esc = Escape(s);
            fputs(esc.c_str(), f);
            fputc('\t', f);
            auto it = g_map.find(s);
            if (it != g_map.end()) fputs(Escape(it->second).c_str(), f);
            fputc('\n', f);
        }
        fclose(f);
        LOG("Wrote MasterLooter.template.txt with %d string(s).", static_cast<int>(g_seen.size()));
        return true;
    }
}
