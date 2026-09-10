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
        else if (k == "WrapSwapChain")    c.wrapSwapChain = Flag(v);
        else if (k == "HookDX12")         c.hookDX12 = Flag(v);
        else if (k == "EnableDred")       c.enableDred = Flag(v);
        else if (k == "HookXInput")       c.hookXInput = Flag(v);
        else if (k == "OreBonus")         c.oreBonus = Clamp(v, 0, 10);
        else if (k == "Language")         c.language = v;
        else if (k == "PadMenu")          c.padMenu   = static_cast<unsigned>(strtoul(v.c_str(), nullptr, 0));
        else if (k == "PadToggle")        c.padToggle = static_cast<unsigned>(strtoul(v.c_str(), nullptr, 0));
        else if (k == "PadBurst")         c.padBurst  = static_cast<unsigned>(strtoul(v.c_str(), nullptr, 0));
        else if (k == "PadWatch")         c.padWatch  = static_cast<unsigned>(strtoul(v.c_str(), nullptr, 0));
        else if (k == "KeyToggle")        c.keyToggle = Key(v, 0x79);
        else if (k == "KeyBurst")         c.keyBurst = Key(v, 0x7A);
        else if (k == "KeyWatch")         c.keyWatch = Key(v, 0x24);
        else if (k == "KeyOwned")         c.keyOwned = Key(v, 0);
        else if (k == "PadOwned")         c.padOwned  = static_cast<unsigned>(strtoul(v.c_str(), nullptr, 0));
        else if (k == "ScansPerSec")      c.scansPerSec = Clamp(v, 1, 30);
        else if (k == "PerScan")          c.perScan = Clamp(v, 0, 64);
        else if (k == "BurstPerKey")      c.burstPerKey = Clamp(v, 0, 64);
        else if (k == "RetryAfterMs")     c.retryAfterMs = Clamp(v, 500, 60000);
        else if (k == "LootCorpses")      c.lootCorpses = Flag(v);
        else if (k == "PickUpItems")      c.pickUpItems = Flag(v);
        else if (k == "GatherPlants")     c.gatherPlants = Flag(v);
        else if (k == "GatherCrops")      c.gatherCrops = Flag(v);
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
        else if (k == "GatherVeins")      c.gatherVeins = Flag(v);
        else if (k == "BreakOre")         c.breakOre = Flag(v);
        else if (k == "DrawWells")        c.drawWells = Flag(v);
        else if (k == "ArmRange")         c.armRange = Range(v, 0, 60, 8);
        else if (k == "ArmContainers")    c.armContainers = Flag(v);
        else if (k == "LootOwned")        c.lootOwned = Flag(v);
        else if (k == "SkipQuestItems")   c.skipQuestItems = Flag(v);
        else if (k == "SkipNoSell")       c.skipNoSell = Flag(v);
        else if (k == "PetFilter")        c.petFilter = Flag(v);
        else if (k == "MinValueCopper")   c.minValueCopper = std::max(0, atoi(v.c_str()));
        else if (k == "TakeUnknownItems") c.takeUnknownItems = Flag(v);
        else if (k == "DebugLog")         c.debugLog = Flag(v);
        else if (k == "DeleteTestName")   c.deleteTestName = v;
        else if (k == "ConfigVersion")    c.configVersion = atoi(v.c_str());
    }

    static bool WriteText(const std::wstring& path, const std::string& text);
    static std::string Serialize(const Config& c);
    static bool ReadWhole(const std::wstring& path, std::string& out);

    // Backups live as one dated file each in MasterLooter.backups. One is
    // written every time the game starts, before this process can change
    // anything, and the oldest are dropped so the folder cannot grow forever.
    static constexpr int kKeepBackups = 12;
    static std::wstring BackupDir() { return Paths::File(L"MasterLooter.backups"); }

    // "20260906-0850" on disk, "2026-09-06 08:50" on screen.
    static std::string StampNow(const char* suffix)
    {
        SYSTEMTIME lt{};
        GetLocalTime(&lt);
        char b[48];
        snprintf(b, sizeof b, "%04d%02d%02d-%02d%02d%s", lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, suffix ? suffix : "");
        return b;
    }

    std::string BackupLabel(const char* stamp)
    {
        const std::string s(stamp ? stamp : "");
        if (s.size() < 13 || s[8] != '-') return s;
        return s.substr(0, 4) + "-" + s.substr(4, 2) + "-" + s.substr(6, 2) + " " +
               s.substr(9, 2) + ":" + s.substr(11, 2) + s.substr(13);
    }

    static std::wstring BackupPath(const std::string& stamp)
    {
        std::wstring w(stamp.begin(), stamp.end());
        return BackupDir() + L"\\" + w + L".ini";
    }

    static bool BackupText(const std::string& stamp, const std::string& text)
    {
        if (text.empty()) return false;
        CreateDirectoryW(BackupDir().c_str(), nullptr);
        return WriteText(BackupPath(stamp), text);
    }

    // Newest first, so the list reads the way a person looks for one.
    int ListBackups(std::string* out, int max)
    {
        WIN32_FIND_DATAW fd;
        const std::wstring pat = BackupDir() + L"\\*.ini";
        HANDLE h = FindFirstFileW(pat.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return 0;
        std::vector<std::string> all;
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring w = fd.cFileName;
            if (w.size() > 4) w.resize(w.size() - 4);
            std::string name;
            for (wchar_t c : w) name += (c < 128) ? static_cast<char>(c) : '?';
            all.push_back(name);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        std::sort(all.begin(), all.end(), std::greater<std::string>());
        const int n = static_cast<int>(all.size()) < max ? static_cast<int>(all.size()) : max;
        for (int i = 0; i < n; ++i) out[i] = all[i];
        return n;
    }

    static void PruneBackups()
    {
        std::string names[256];
        const int n = ListBackups(names, 256);
        for (int i = kKeepBackups; i < n; ++i) DeleteFileW(BackupPath(names[i]).c_str());
    }

    bool BackupExists(const char* stamp)
    {
        return GetFileAttributesW(BackupPath(stamp ? stamp : "").c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    std::string NextBackupStamp() { return StampNow(""); }

    bool BackupNow()
    {
        std::lock_guard<std::recursive_mutex> lk(g_mutex);
        if (!BackupText(StampNow(""), Serialize(g_cfg))) return false;
        PruneBackups();
        LOG("Settings backed up as %s.", BackupLabel(StampNow("").c_str()).c_str());
        return true;
    }

    bool DeleteBackup(const char* stamp)
    {
        if (!stamp || !*stamp) return false;
        if (!DeleteFileW(BackupPath(stamp).c_str())) return false;
        LOG("Backup %s deleted.", BackupLabel(stamp).c_str());
        return true;
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
        bool migrated = false;
        std::string text;
        const bool present = ReadFile(text);
        if (present) ParseInto(text, c);
        // Files written before version 2 carried a 20 m gather range the game
        // ignores and gathered unidentified nodes by default; bring both in line.
        if (present && c.configVersion < 2)
        {
            BackupText(StampNow("-v1"), text);
            c.gatherRange = std::min(c.gatherRange, 6.0f);
            c.gatherUnknown = false;
            c.configVersion = 2;
            migrated = true;
            LOG("Settings migrated to version 2: gather range %.0f m, unidentified nodes off. The file as it was is kept as a backup.", c.gatherRange);
        }
        if (!present) c.configVersion = 2;
        Clamp(c);
        g_cfg = c;
        g_knownTime = FileTime();
        // The migration used to set the dirty flag here and this line cleared it
        // three lines later, so version 2 was never written back. A v1 file was
        // migrated again on every launch: a hand-edited GatherUnknown=1 was
        // reverted every time rather than once, and each launch left another
        // "-v1" backup, so twelve slots of real backups were evicted in six
        // crash-and-relaunch cycles.
        g_dirty = migrated;
        if (migrated) g_dirtyAt = GetTickCount64();
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
        snprintf(b, sizeof b, "WrapSwapChain=%d\n", c.wrapSwapChain); s += b;
        snprintf(b, sizeof b, "HookDX12=%d\n", c.hookDX12); s += b;
        snprintf(b, sizeof b, "EnableDred=%d\n", c.enableDred); s += b;
        snprintf(b, sizeof b, "HookXInput=%d\n", c.hookXInput); s += b;
        snprintf(b, sizeof b, "OreBonus=%d\n", c.oreBonus); s += b;
        snprintf(b, sizeof b, "Language=%s\n", c.language.c_str()); s += b;
        snprintf(b, sizeof b, "KeyOwned=%d\nPadOwned=%u\n", c.keyOwned, c.padOwned); s += b;
        snprintf(b, sizeof b, "PadMenu=%u\nPadToggle=%u\nPadBurst=%u\nPadWatch=%u\n",
                 c.padMenu, c.padToggle, c.padBurst, c.padWatch); s += b;
        snprintf(b, sizeof b, "ScansPerSec=%d\nPerScan=%d\nBurstPerKey=%d\nRetryAfterMs=%d\n",
                 c.scansPerSec, c.perScan, c.burstPerKey, c.retryAfterMs); s += b;
        snprintf(b, sizeof b, "LootCorpses=%d\nPickUpItems=%d\nGatherPlants=%d\nGatherCrops=%d\nGatherOre=%d\nGatherStone=%d\nGatherWood=%d\nGatherUnknown=%d\n",
                 c.lootCorpses, c.pickUpItems, c.gatherPlants, c.gatherCrops, c.gatherOre, c.gatherStone, c.gatherWood, c.gatherUnknown); s += b;
        snprintf(b, sizeof b, "CatchInsects=%d\nCatchFish=%d\nCatchAnimals=%d\nLootContainers=%d\nLootFurniture=%d\n",
                 c.catchInsects, c.catchFish, c.catchAnimals, c.lootContainers, c.lootFurniture); s += b;
        snprintf(b, sizeof b, "ScanRange=%.1f\nLootRange=%.1f\nGatherRange=%.1f\nCatchRange=%.1f\nCorpseRange=%.1f\nMinRange=%.2f\n",
                 c.scanRange, c.lootRange, c.gatherRange, c.catchRange, c.corpseRange, c.minRange); s += b;
        snprintf(b, sizeof b, "AutoArm=%d\nArmRange=%.1f\nArmContainers=%d\nGatherVeins=%d\n", c.autoArm, c.armRange, c.armContainers, c.gatherVeins); s += b;
        snprintf(b, sizeof b, "BreakOre=%d\nDrawWells=%d\n", c.breakOre, c.drawWells); s += b;
        snprintf(b, sizeof b, "LootOwned=%d\nSkipQuestItems=%d\nSkipNoSell=%d\nMinValueCopper=%d\nTakeUnknownItems=%d\nPetFilter=%d\nDebugLog=%d\nConfigVersion=%d\n",
                 c.lootOwned, c.skipQuestItems, c.skipNoSell, c.minValueCopper, c.takeUnknownItems, c.petFilter, c.debugLog, c.configVersion); s += b;
        if (!c.deleteTestName.empty()) { snprintf(b, sizeof b, "DeleteTestName=%s\n", c.deleteTestName.c_str()); s += b; }
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

    bool PresetExists(const char* rawName)
    {
        const std::string name = CleanPresetName(rawName);
        if (name.empty()) return false;
        return GetFileAttributesW(PresetPath(name).c_str()) != INVALID_FILE_ATTRIBUTES;
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
        std::string text;
        if (!ReadWhole(PresetPath(name), text)) { LOG_ERR("Preset \"%s\" could not be read.", name.c_str()); return false; }
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
    static bool ReadWhole(const std::wstring& path, std::string& out)
    {
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f) return false;
        char buf[4096]; size_t got;
        while ((got = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, got);
        fclose(f);
        return !out.empty();
    }

    bool RestoreBackup(const char* stamp)
    {
        if (!stamp || !*stamp) return false;
        std::string text;
        if (!ReadWhole(BackupPath(stamp), text)) { LOG_ERR("Backup %s could not be read.", BackupLabel(stamp).c_str()); return false; }
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
        LOG("Settings restored from the backup of %s.", BackupLabel(stamp).c_str());
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
        if (ReadFile(text) && BackupText(StampNow(""), text))
        {
            PruneBackups();
            LOG("Settings backed up as %s before this session touched them.", BackupLabel(StampNow("").c_str()).c_str());
        }
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
