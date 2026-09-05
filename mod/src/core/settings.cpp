#include "settings.h"

#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "log.h"
#include "paths.h"

namespace ml::Settings
{
    static Config       g_cfg;
    static std::wstring g_path;
    static bool         g_claimed   = false;
    static bool         g_dirty     = false;
    static ULONGLONG    g_dirtyAt   = 0;
    static ULONGLONG    g_lastCheck = 0;
    static ULONGLONG    g_knownTime = 0; // last write time we loaded or wrote
    static int          g_generation = 0;

    Config& Get() { return g_cfg; }
    const std::wstring& Path() { if (g_path.empty()) g_path = Paths::File(L"MasterLooter.ini"); return g_path; }
    int Generation() { return g_generation; }

    static ULONGLONG FileTime()
    {
        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        if (!GetFileAttributesExW(Path().c_str(), GetFileExInfoStandard, &fad))
            return 0;
        ULARGE_INTEGER u;
        u.LowPart  = fad.ftLastWriteTime.dwLowDateTime;
        u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
        return u.QuadPart;
    }

    static std::string Trim(const std::string& s)
    {
        size_t a = 0, b = s.size();
        while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
        return s.substr(a, b - a);
    }

    static bool ReadFile(std::string& out)
    {
        FILE* f = _wfopen(Path().c_str(), L"rb");
        if (!f) return false;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
        fclose(f);
        return true;
    }

    static void ApplyGeneral(Config& c, const std::string& k, const std::string& v)
    {
        const int   i = atoi(v.c_str());
        const float f = static_cast<float>(atof(v.c_str()));
        if      (k == "Enabled")        c.enabled = i != 0;
        else if (k == "MenuKey")        c.menuKey = (i > 0 && i < 256) ? i : 0x2D;
        else if (k == "ShowHud")        c.showHud = i != 0;
        else if (k == "LootRange")      c.lootRange = (f >= 1.0f && f <= 100.0f) ? f : 15.0f;
        else if (k == "MaxLootsPerSec") c.maxLootsPerSec = (i >= 1 && i <= 60) ? i : 5;
        else if (k == "LootCorpses")    c.lootCorpses = i != 0;
        else if (k == "PickUpItems")    c.pickUpItems = i != 0;
        else if (k == "GatherPlants")   c.gatherPlants = i != 0;
        else if (k == "CatchInsects")   c.catchInsects = i != 0;
        else if (k == "LootContainers") c.lootContainers = i != 0;
        else if (k == "MinValueCopper") c.minValueCopper = i >= 0 ? i : 0;
        else if (k == "SkipQuestItems") c.skipQuestItems = i != 0;
        else if (k == "SkipNoSell")     c.skipNoSell = i != 0;
    }

    void Load()
    {
        Config c; // defaults
        std::string text;
        const bool present = ReadFile(text);
        if (present)
        {
            std::string section;
            size_t pos = 0;
            while (pos < text.size())
            {
                size_t nl = text.find('\n', pos);
                if (nl == std::string::npos) nl = text.size();
                std::string line = Trim(text.substr(pos, nl - pos));
                pos = nl + 1;
                if (line.empty() || line[0] == ';' || line[0] == '#') continue;
                if (line.front() == '[' && line.back() == ']') { section = line.substr(1, line.size() - 2); continue; }
                const size_t eq = line.find('=');
                if (eq == std::string::npos) continue;
                const std::string k = Trim(line.substr(0, eq));
                const std::string v = Trim(line.substr(eq + 1));
                if (k.empty()) continue;
                if (section == "MasterLooter")      ApplyGeneral(c, k, v);
                else if (section == "Classes")      c.classRule[k] = atoi(v.c_str()) != 0 ? 1 : 0;
                else if (section == "Tags")         { const int r = atoi(v.c_str()); if (r == 1 || r == -1) c.tagRule[k] = r; }
                else if (section == "Items")        { const int r = atoi(v.c_str()); const unsigned long key = strtoul(k.c_str(), nullptr, 10); if (key && (r == 1 || r == -1)) c.itemRule[static_cast<uint32_t>(key)] = r; }
            }
        }
        g_cfg = c;
        g_knownTime = FileTime();
        g_dirty = false;
        ++g_generation;
        LOG("Settings %s: %d class rules, %d tag rules, %d item rules.", present ? "loaded" : "defaulted (no ini yet)",
            static_cast<int>(c.classRule.size()), static_cast<int>(c.tagRule.size()), static_cast<int>(c.itemRule.size()));
    }

    void Save()
    {
        g_dirty = false;
        if (!g_claimed) return;
        const Config& c = g_cfg;
        std::string s;
        s += "; Master Looter settings. Edited live from the in-game menu (Insert by default);\n";
        s += "; hand edits are picked up within a second while the game runs.\n";
        s += "[MasterLooter]\n";
        char b[256];
        snprintf(b, sizeof b, "Enabled=%d\nMenuKey=%d\nShowHud=%d\nLootRange=%.1f\nMaxLootsPerSec=%d\n",
                 c.enabled ? 1 : 0, c.menuKey, c.showHud ? 1 : 0, c.lootRange, c.maxLootsPerSec);
        s += b;
        snprintf(b, sizeof b, "LootCorpses=%d\nPickUpItems=%d\nGatherPlants=%d\nCatchInsects=%d\nLootContainers=%d\n",
                 c.lootCorpses ? 1 : 0, c.pickUpItems ? 1 : 0, c.gatherPlants ? 1 : 0, c.catchInsects ? 1 : 0, c.lootContainers ? 1 : 0);
        s += b;
        snprintf(b, sizeof b, "MinValueCopper=%d\nSkipQuestItems=%d\nSkipNoSell=%d\n", c.minValueCopper, c.skipQuestItems ? 1 : 0, c.skipNoSell ? 1 : 0);
        s += b;
        s += "\n; class -> 1 loot, 0 skip (classes not listed are looted)\n[Classes]\n";
        for (const auto& kv : c.classRule) { s += kv.first; s += kv.second ? "=1\n" : "=0\n"; }
        s += "\n; tag -> 1 always loot, -1 never loot (wins over the class rule)\n[Tags]\n";
        for (const auto& kv : c.tagRule) { s += kv.first; s += kv.second > 0 ? "=1\n" : "=-1\n"; }
        s += "\n; item key -> 1 always loot, -1 never loot (wins over everything)\n[Items]\n";
        for (const auto& kv : c.itemRule) { snprintf(b, sizeof b, "%u=%d\n", kv.first, kv.second); s += b; }

        FILE* f = _wfopen(Path().c_str(), L"wb");
        if (!f) { LOG_ERR("Could not write %ls", Path().c_str()); return; }
        fwrite(s.data(), 1, s.size(), f);
        fclose(f);
        g_knownTime = FileTime();
    }

    void Claim()
    {
        g_claimed = true;
        if (FileTime() == 0)
        {
            Save();
            LOG("Wrote default settings to %ls", Path().c_str());
        }
    }

    void MarkDirty()
    {
        g_dirty = true;
        g_dirtyAt = GetTickCount64();
    }

    void Poll()
    {
        const ULONGLONG now = GetTickCount64();
        if (g_dirty && now - g_dirtyAt > 600)
            Save();
        if (now - g_lastCheck > 1000)
        {
            g_lastCheck = now;
            const ULONGLONG t = FileTime();
            if (t != 0 && t != g_knownTime && !g_dirty)
            {
                Load();
                LOG("MasterLooter.ini changed on disk; reloaded.");
            }
        }
    }

    const char* KeyName(int vk)
    {
        static char name[64];
        switch (vk)
        {
        case VK_INSERT: return "Insert";
        case VK_DELETE: return "Delete";
        case VK_HOME:   return "Home";
        case VK_END:    return "End";
        case VK_PRIOR:  return "Page Up";
        case VK_NEXT:   return "Page Down";
        case VK_PAUSE:  return "Pause";
        case VK_SCROLL: return "Scroll Lock";
        default: break;
        }
        if (vk >= VK_F1 && vk <= VK_F24) { snprintf(name, sizeof name, "F%d", vk - VK_F1 + 1); return name; }
        UINT sc = MapVirtualKeyA(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
        if (sc && GetKeyNameTextA(static_cast<LONG>(sc << 16), name, sizeof name) > 0) return name;
        snprintf(name, sizeof name, "key 0x%02X", vk);
        return name;
    }
}
