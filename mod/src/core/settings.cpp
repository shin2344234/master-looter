#include "settings.h"

#include <Windows.h>
#include <algorithm>
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
    static ULONGLONG    g_knownTime = 0;
    static int          g_generation = 0;
    static std::recursive_mutex g_mutex;

    Config& Get() { return g_cfg; }
    std::recursive_mutex& Mutex() { return g_mutex; }
    Config Snapshot() { std::lock_guard<std::recursive_mutex> lk(g_mutex); return g_cfg; }
    const std::wstring& Path() { if (g_path.empty()) g_path = Paths::File(L"MasterLooter.ini"); return g_path; }
    int Generation() { return g_generation; }

    static ULONGLONG FileTime()
    {
        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        if (!GetFileAttributesExW(Path().c_str(), GetFileExInfoStandard, &fad)) return 0;
        ULARGE_INTEGER u; u.LowPart = fad.ftLastWriteTime.dwLowDateTime; u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
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
        char buf[4096]; size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
        fclose(f);
        return true;
    }

    static int  Key(const std::string& v, int def) { const int i = atoi(v.c_str()); return (i > 0 && i < 256) ? i : def; }
    static float Range(const std::string& v, float lo, float hi, float def) { const float f = static_cast<float>(atof(v.c_str())); return (f >= lo && f <= hi) ? f : def; }
    static int  Clamp(const std::string& v, int lo, int hi) { return std::clamp(atoi(v.c_str()), lo, hi); }
    static bool Flag(const std::string& v) { return atoi(v.c_str()) != 0; }

    static void ApplyGeneral(Config& c, const std::string& k, const std::string& v)
    {
        if      (k == "Enabled")          c.enabled = Flag(v);
        else if (k == "MenuKey")          c.menuKey = Key(v, 0x2D);
        else if (k == "ShowHud")          c.showHud = Flag(v);
        else if (k == "NotifyBagFull")    c.notifyBagFull = Flag(v);
        else if (k == "KeyToggle")        c.keyToggle = Key(v, 0x79);
        else if (k == "KeyBurst")         c.keyBurst = Key(v, 0x7A);
        else if (k == "KeyWatch")         c.keyWatch = Key(v, 0x24);
        else if (k == "ScansPerSec")      c.scansPerSec = Clamp(v, 1, 30);
        else if (k == "PerScan")          c.perScan = Clamp(v, 0, 64);
        else if (k == "BurstPerKey")      c.burstPerKey = Clamp(v, 0, 64);
        else if (k == "RetryAfterMs")     c.retryAfterMs = Clamp(v, 500, 60000);
        else if (k == "LootCorpses")      c.lootCorpses = Flag(v);
        else if (k == "PickUpItems")      c.pickUpItems = Flag(v);
        else if (k == "GatherPlants")     c.gatherPlants = Flag(v);
        else if (k == "GatherOre")        c.gatherOre = Flag(v);
        else if (k == "GatherStone")      c.gatherStone = Flag(v);
        else if (k == "GatherWood")       c.gatherWood = Flag(v);
        else if (k == "GatherUnknown")    c.gatherUnknown = Flag(v);
        else if (k == "CatchCreatures")   { c.catchInsects = Flag(v); c.catchAnimals = Flag(v); } // pre-0.3.4 key
        else if (k == "CatchInsects")     c.catchInsects = Flag(v);
        else if (k == "CatchFish")        c.catchFish = Flag(v);
        else if (k == "CatchAnimals")     c.catchAnimals = Flag(v);
        else if (k == "LootContainers")   c.lootContainers = Flag(v);
        else if (k == "LootFurniture")    c.lootFurniture = Flag(v);
        else if (k == "ScanRange")        c.scanRange = Range(v, 5, 200, 40);
        else if (k == "LootRange")        c.lootRange = Range(v, 0, 200, 15);
        else if (k == "GatherRange")      c.gatherRange = Range(v, 0, 200, 6);
        else if (k == "CatchRange")       c.catchRange = Range(v, 0, 200, 8);
        else if (k == "CorpseRange")      c.corpseRange = Range(v, 0, 200, 12);
        else if (k == "MinRange")         c.minRange = Range(v, 0, 5, 0.35f);
        else if (k == "AutoArm")          c.autoArm = Flag(v);
        else if (k == "ArmRange")         c.armRange = Range(v, 0, 60, 8);
        else if (k == "ArmContainers")    c.armContainers = Flag(v);
        else if (k == "LootOwned")        c.lootOwned = Flag(v);
        else if (k == "SkipQuestItems")   c.skipQuestItems = Flag(v);
        else if (k == "SkipNoSell")       c.skipNoSell = Flag(v);
        else if (k == "MinValueCopper")   c.minValueCopper = std::max(0, atoi(v.c_str()));
        else if (k == "TakeUnknownItems") c.takeUnknownItems = Flag(v);
        else if (k == "DebugLog")         c.debugLog = Flag(v);
        else if (k == "ConfigVersion")    c.configVersion = atoi(v.c_str());
    }

    static bool WriteText(const std::wstring& path, const std::string& text);
    static std::string Serialize(const Config& c);

    // A copy of the settings file kept aside, so a bad edit, a migration or a
    // new version's defaults can be undone. Never overwrites with nothing.
    static bool BackupTo(const wchar_t* name, const std::string& text)
    {
        if (text.empty()) return false;
        return WriteText(Paths::File(name), text);
    }

    // Reads an ini's text into `c`. Used for the live file, for a preset and
    // for a backup, so all three understand exactly the same format.
    static void ParseInto(const std::string& text, Config& c)
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
            if (section == "MasterLooter") ApplyGeneral(c, k, v);
            else if (section == "Classes") c.classRule[k] = atoi(v.c_str()) != 0 ? 1 : 0;
            else if (section == "Tags") { const int r = atoi(v.c_str()); if (r == 1 || r == -1) c.tagRule[k] = r; }
            else if (section == "Items") { const int r = atoi(v.c_str()); const unsigned long key = strtoul(k.c_str(), nullptr, 10); if (key && (r == 1 || r == -1)) c.itemRule[static_cast<uint32_t>(key)] = r; }
        }
    }

    // Ranges beyond the scan range can never trigger, so they are pulled in
    // wherever a config arrives from.
    static void Clamp(Config& c)
    {
        c.lootRange = std::min(c.lootRange, c.scanRange); c.gatherRange = std::min(c.gatherRange, c.scanRange);
        c.catchRange = std::min(c.catchRange, c.scanRange); c.corpseRange = std::min(c.corpseRange, c.scanRange);
    }

    void Load()
    {
        Config c;
        c.configVersion = 1; // a file that predates the version key
        std::string text;
        const bool present = ReadFile(text);
        if (present) ParseInto(text, c);
        // Files written before version 2 carried a 20 m gather range the game
        // ignores and gathered unidentified nodes by default; bring both in line.
        if (present && c.configVersion < 2)
        {
            BackupTo(L"MasterLooter.ini.v1.bak", text);
            c.gatherRange = std::min(c.gatherRange, 6.0f);
            c.gatherUnknown = false;
            c.configVersion = 2;
            g_dirty = true; g_dirtyAt = GetTickCount64();
            LOG("Settings migrated to version 2: gather range %.0f m, unidentified nodes off. The file as it was is saved as MasterLooter.ini.v1.bak.", c.gatherRange);
        }
        if (!present) c.configVersion = 2;
        Clamp(c);
        g_cfg = c;
        g_knownTime = FileTime();
        g_dirty = false;
        ++g_generation;
        LOG("Settings %s: %d class rules, %d tag rules, %d item rules.", present ? "loaded" : "defaulted (no ini yet)",
            static_cast<int>(c.classRule.size()), static_cast<int>(c.tagRule.size()), static_cast<int>(c.itemRule.size()));
    }

    // The whole config as ini text: the live file, a preset and a backup are
    // all this same thing.
    static std::string Serialize(const Config& c)
    {
        std::string s;
        char b[512];
        s += "; Master Looter settings. Edited live from the in-game menu (Insert by default);\n";
        s += "; hand edits are picked up within a second while the game runs.\n";
        s += "[MasterLooter]\n";
        snprintf(b, sizeof b, "Enabled=%d\nMenuKey=%d\nShowHud=%d\nNotifyBagFull=%d\nKeyToggle=%d\nKeyBurst=%d\nKeyWatch=%d\n",
                 c.enabled, c.menuKey, c.showHud, c.notifyBagFull, c.keyToggle, c.keyBurst, c.keyWatch); s += b;
        snprintf(b, sizeof b, "ScansPerSec=%d\nPerScan=%d\nBurstPerKey=%d\nRetryAfterMs=%d\n",
                 c.scansPerSec, c.perScan, c.burstPerKey, c.retryAfterMs); s += b;
        snprintf(b, sizeof b, "LootCorpses=%d\nPickUpItems=%d\nGatherPlants=%d\nGatherOre=%d\nGatherStone=%d\nGatherWood=%d\nGatherUnknown=%d\n",
                 c.lootCorpses, c.pickUpItems, c.gatherPlants, c.gatherOre, c.gatherStone, c.gatherWood, c.gatherUnknown); s += b;
        snprintf(b, sizeof b, "CatchInsects=%d\nCatchFish=%d\nCatchAnimals=%d\nLootContainers=%d\nLootFurniture=%d\n",
                 c.catchInsects, c.catchFish, c.catchAnimals, c.lootContainers, c.lootFurniture); s += b;
        snprintf(b, sizeof b, "ScanRange=%.1f\nLootRange=%.1f\nGatherRange=%.1f\nCatchRange=%.1f\nCorpseRange=%.1f\nMinRange=%.2f\n",
                 c.scanRange, c.lootRange, c.gatherRange, c.catchRange, c.corpseRange, c.minRange); s += b;
        snprintf(b, sizeof b, "AutoArm=%d\nArmRange=%.1f\nArmContainers=%d\n", c.autoArm, c.armRange, c.armContainers); s += b;
        snprintf(b, sizeof b, "LootOwned=%d\nSkipQuestItems=%d\nSkipNoSell=%d\nMinValueCopper=%d\nTakeUnknownItems=%d\nDebugLog=%d\nConfigVersion=%d\n",
                 c.lootOwned, c.skipQuestItems, c.skipNoSell, c.minValueCopper, c.takeUnknownItems, c.debugLog, c.configVersion); s += b;
        s += "\n; class -> 1 loot, 0 skip (classes not listed are looted)\n[Classes]\n";
        for (const auto& kv : c.classRule) { s += kv.first; s += kv.second ? "=1\n" : "=0\n"; }
        s += "\n; tag -> 1 always loot, -1 never loot (wins over the class rule)\n[Tags]\n";
        for (const auto& kv : c.tagRule) { s += kv.first; s += kv.second > 0 ? "=1\n" : "=-1\n"; }
        s += "\n; item key -> 1 always loot, -1 never loot (wins over everything)\n[Items]\n";
        for (const auto& kv : c.itemRule) { snprintf(b, sizeof b, "%u=%d\n", kv.first, kv.second); s += b; }
        return s;
    }

    static bool WriteText(const std::wstring& path, const std::string& text)
    {
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (!f) { LOG_ERR("Could not write %ls", path.c_str()); return false; }
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
        return true;
    }

    void Save()
    {
        g_dirty = false;
        if (!g_claimed) return;
        if (WriteText(Path(), Serialize(g_cfg))) g_knownTime = FileTime();
    }

    // --------------------------------------------------------- presets ----
    // A preset is the whole config written to MasterLooter.presets\<name>.ini,
    // so swapping between "everything" and "ore run" is two clicks, and so a
    // set of rules survives anything that happens to the live file.

    static std::wstring PresetDir() { return Paths::File(L"MasterLooter.presets"); }

    // Preset names become file names, so only what is safe in one is kept.
    std::string CleanPresetName(const char* raw)
    {
        std::string out;
        for (const char* p = raw ? raw : ""; *p && out.size() < 40; ++p)
        {
            const unsigned char ch = static_cast<unsigned char>(*p);
            if (isalnum(ch) || ch == ' ' || ch == '-' || ch == '_') out += static_cast<char>(ch);
        }
        while (!out.empty() && out.front() == ' ') out.erase(out.begin());
        while (!out.empty() && out.back() == ' ') out.pop_back();
        return out;
    }

    static std::wstring PresetPath(const std::string& name)
    {
        std::wstring w(name.begin(), name.end());
        return PresetDir() + L"\\" + w + L".ini";
    }

    int ListPresets(std::string* out, int max)
    {
        WIN32_FIND_DATAW fd;
        const std::wstring pat = PresetDir() + L"\\*.ini";
        HANDLE h = FindFirstFileW(pat.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return 0;
        int n = 0;
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring w = fd.cFileName;
            if (w.size() > 4) w.resize(w.size() - 4);   // drop .ini
            std::string name;
            for (wchar_t c : w) name += (c < 128) ? static_cast<char>(c) : '?';
            if (n < max) out[n++] = name;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        std::sort(out, out + n);
        return n;
    }

    bool SavePreset(const char* rawName)
    {
        const std::string name = CleanPresetName(rawName);
        if (name.empty()) { LOG_ERR("A preset needs a name of letters, digits, spaces, dashes or underscores."); return false; }
        CreateDirectoryW(PresetDir().c_str(), nullptr);
        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        if (!WriteText(PresetPath(name), Serialize(g_cfg))) return false;
        LOG("Preset \"%s\" saved.", name.c_str());
        return true;
    }

    // Everything a preset holds replaces what is loaded, rules included, and
    // the live file is rewritten from it so the swap survives a restart.
    bool LoadPreset(const char* rawName)
    {
        const std::string name = CleanPresetName(rawName);
        if (name.empty()) return false;
        FILE* f = _wfopen(PresetPath(name).c_str(), L"rb");
        if (!f) { LOG_ERR("Preset \"%s\" could not be opened.", name.c_str()); return false; }
        std::string text;
        char buf[4096]; size_t got;
        while ((got = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, got);
        fclose(f);
        if (text.empty()) { LOG_ERR("Preset \"%s\" is empty.", name.c_str()); return false; }
        Config c;
        c.configVersion = 1;
        ParseInto(text, c);
        Clamp(c);
        {
            std::lock_guard<std::recursive_mutex> lk(g_mutex);
            g_cfg = c;
            ++g_generation;
        }
        MarkDirty();
        LOG("Preset \"%s\" loaded: %d class rules, %d tag rules, %d item rules.", name.c_str(),
            static_cast<int>(c.classRule.size()), static_cast<int>(c.tagRule.size()), static_cast<int>(c.itemRule.size()));
        return true;
    }

    bool DeletePreset(const char* rawName)
    {
        const std::string name = CleanPresetName(rawName);
        if (name.empty()) return false;
        if (!DeleteFileW(PresetPath(name).c_str())) return false;
        LOG("Preset \"%s\" deleted.", name.c_str());
        return true;
    }

    // ---------------------------------------------------------- backup ----
    bool BackupExists(char* whenOut, size_t n)
    {
        if (whenOut && n) whenOut[0] = '\0';
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(Paths::File(L"MasterLooter.ini.bak").c_str(), GetFileExInfoStandard, &fad)) return false;
        if (whenOut && n)
        {
            SYSTEMTIME st{}, lt{};
            if (FileTimeToSystemTime(&fad.ftLastWriteTime, &st) && SystemTimeToTzSpecificLocalTime(nullptr, &st, &lt))
                snprintf(whenOut, n, "%04d-%02d-%02d %02d:%02d", lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute);
        }
        return true;
    }

    bool RestoreBackup()
    {
        FILE* f = _wfopen(Paths::File(L"MasterLooter.ini.bak").c_str(), L"rb");
        if (!f) { LOG_ERR("There is no MasterLooter.ini.bak to restore."); return false; }
        std::string text;
        char buf[4096]; size_t got;
        while ((got = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, got);
        fclose(f);
        if (text.empty()) { LOG_ERR("MasterLooter.ini.bak is empty; nothing restored."); return false; }
        Config c;
        c.configVersion = 1;
        ParseInto(text, c);
        Clamp(c);
        {
            std::lock_guard<std::recursive_mutex> lk(g_mutex);
            g_cfg = c;
            ++g_generation;
        }
        MarkDirty();
        LOG("Settings restored from MasterLooter.ini.bak.");
        return true;
    }

    void Claim()
    {
        g_claimed = true;
        if (FileTime() == 0) { Save(); LOG("Wrote default settings to %ls", Path().c_str()); return; }
        // Before this session can write anything, keep the file as it was. A
        // version that changes a default, or an afternoon of fiddling, is then
        // one button away from being undone.
        std::string text;
        if (ReadFile(text) && BackupTo(L"MasterLooter.ini.bak", text))
            LOG("Settings backed up to MasterLooter.ini.bak before this session touched them.");
    }

    void MarkDirty() { g_dirty = true; g_dirtyAt = GetTickCount64(); }

    void Poll()
    {
        const ULONGLONG now = GetTickCount64();
        if (g_dirty && now - g_dirtyAt > 600) { std::lock_guard<std::recursive_mutex> lk(g_mutex); Save(); }
        if (now - g_lastCheck > 1000)
        {
            g_lastCheck = now;
            const ULONGLONG t = FileTime();
            if (t != 0 && t != g_knownTime && !g_dirty)
            {
                std::lock_guard<std::recursive_mutex> lk(g_mutex);
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
        case VK_MBUTTON: return "Mouse 3";
        case VK_XBUTTON1: return "Mouse 4";
        case VK_XBUTTON2: return "Mouse 5";
        default: break;
        }
        if (vk >= VK_F1 && vk <= VK_F24) { snprintf(name, sizeof name, "F%d", vk - VK_F1 + 1); return name; }
        UINT sc = MapVirtualKeyA(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
        if (sc && GetKeyNameTextA(static_cast<LONG>(sc << 16), name, sizeof name) > 0) return name;
        snprintf(name, sizeof name, "key 0x%02X", vk);
        return name;
    }
}
