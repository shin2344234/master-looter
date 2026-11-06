// Checks that need no game: the break verdict's timing, the protected-tag
// rule against the shipped item table, and how [NotVeins] lines load.
//
//   ninja -C build ml_tests && build\ml_tests.exe
//
// Exit code 0 when every check passes. Settings::Load refreshes the settings
// copy under LOCALAPPDATA, so the test points LOCALAPPDATA at a folder of its
// own before anything reads it, and the player's real copy is never touched.
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/itemdb.h"
#include "core/paths.h"
#include "core/rules.h"
#include "core/settings.h"
#include "loot/breakjudge.h"
#include "loot/scanretry.h"

using namespace ml;
using loot::BreakVerdict;
using loot::JudgeBreak;
using loot::NextScanRetryMs;

static int g_failed = 0, g_passed = 0;

static void Check(bool ok, const char* what)
{
    if (ok) { ++g_passed; return; }
    ++g_failed;
    printf("FAIL  %s\n", what);
}

static bool Prot(const Rules::Verdict& v) { return v.rule && strcmp(v.rule, "protected") == 0; }

static void BreakTiming()
{
    // LuxDragon, 28 October 2026, rock_b_0011 eid B01000D0, times from the
    // queue: the old clock judged it at +1634, the break ran at +1712 and the
    // drop landed at +1768.
    const uint32_t q = 1000000;
    Check(JudgeBreak(q + 1634, q, 0, false) == BreakVerdict::Wait, "a queued break that has not run yet is not judged");
    Check(JudgeBreak(q + 1740, q, q + 1712, false) == BreakVerdict::Wait, "28 ms after the break ran is too soon to judge");
    Check(JudgeBreak(q + 1768, q, q + 1712, true) == BreakVerdict::Vein, "the drop at +1768 makes it a vein");
    Check(JudgeBreak(q + 3211, q, q + 1712, false) == BreakVerdict::Wait, "1499 ms after the run is still inside the window");
    Check(JudgeBreak(q + 3212, q, q + 1712, false) == BreakVerdict::NotVein, "1500 ms of silence after the run is not a vein");
    Check(JudgeBreak(q + 9999, q, 0, false) == BreakVerdict::Wait, "a break still queued at 9999 ms is waited on");
    Check(JudgeBreak(q + 10000, q, 0, false) == BreakVerdict::NeverRan, "a break that never ran in 10 s teaches nothing");
    Check(JudgeBreak(q + 10000, q, 0, true) == BreakVerdict::Vein, "a drop with no recorded run still counts");
    Check(JudgeBreak(q + 2000, q, q - 50, false) == BreakVerdict::Wait, "a run time from an earlier break of the same entity is ignored");
    // GetTickCount wraps every 49.7 days.
    const uint32_t w = 0xFFFFFF00u;
    Check(JudgeBreak(w + 1549, w, w + 50, false) == BreakVerdict::Wait, "wrap: 1499 ms after the run is still waited on");
    Check(JudgeBreak(w + 1550, w, w + 50, false) == BreakVerdict::NotVein, "wrap: judged across the tick counter wrapping");
}

static void ScanRetry()
{
    // SoulCaliburSK, bug 1166149: one fault used to leave looting off until a
    // restart. The wait after a fault never runs past five minutes now.
    Check(NextScanRetryMs(0, 0) == 5000, "the first fault waits 5 s");
    Check(NextScanRetryMs(5000, 6000) == 10000, "a second fault soon after doubles the wait");
    Check(NextScanRetryMs(160000, 170000) == 300000, "the wait stops at 5 minutes");
    Check(NextScanRetryMs(300000, 310000) == 300000, "and stays there while faults keep coming");
    Check(NextScanRetryMs(300000, 600000) == 5000, "ten clean minutes put it back to 5 s");
    uint32_t w = 0, total = 0;
    for (int i = 0; i < 40; ++i) { w = NextScanRetryMs(w, w + 1); total += w; }
    Check(w == 300000 && total < 40u * 300000u, "forty faults in a row never wait longer than 5 minutes each");
}

static void ProtectedTags()
{
    Check(ItemDb::Load(), "the item table loads");
    const Config cfg;   // defaults, no tag or item rules

    const Item* tw = ItemDb::ByStringKey("Item_gimmick_tumbleweed_0001");
    Check(tw != nullptr, "Item_gimmick_tumbleweed_0001 is in the table");
    if (tw)
    {
        Check(tw->HasTag("gimmick") && tw->HasTag("material"), "the Tumbleweed carries both gimmick and material");
        const Rules::Verdict v = Rules::Decide(*tw, cfg);
        Check(!Prot(v), "the Tumbleweed is not refused as protected");
        Check(v.loot, "the Tumbleweed is looted on default settings");
        Config never = cfg;
        never.tagRule["material"] = -1;
        Check(!Rules::Decide(*tw, never).loot, "a never on material still refuses the Tumbleweed");
    }

    int gimmick = 0, letThrough = 0, protectedGimmick = 0;
    for (const Item& it : ItemDb::All())
    {
        if (!it.HasTag("gimmick")) continue;
        ++gimmick;
        const Rules::Verdict v = Rules::Decide(it, cfg);
        if (Prot(v)) ++protectedGimmick;
        else if (it.stringKey != "Item_gimmick_tumbleweed_0001") ++letThrough;
    }
    printf("      %d gimmick items, %d protected\n", gimmick, protectedGimmick);
    Check(gimmick > 400, "the table holds the gimmick items");
    Check(letThrough == 0, "no gimmick item other than the Tumbleweed loses its protection");

    Config always = cfg;
    always.tagRule["gimmick"] = 1;
    const Item* core = nullptr;
    for (const Item& it : ItemDb::All())
        if (it.HasTag("gimmick") && !it.HasTag("material") && !it.HasTag("memory-fragment") && !it.HasTag("housing-only"))
        { core = &it; break; }
    if (core) printf("      gimmick part used: %s\n", core->stringKey.c_str());
    Check(core && Prot(Rules::Decide(*core, cfg)), "a gimmick part without material stays protected");
    Check(core && !Prot(Rules::Decide(*core, always)), "an always on gimmick still lifts the protection");
}

static bool WriteText(const std::wstring& path, const char* text)
{
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    fputs(text, f);
    fclose(f);
    return true;
}

static void NotVeins()
{
    const std::wstring ini = Paths::File(L"MasterLooter.ini");
    Check(WriteText(ini,
        "[MasterLooter]\r\nConfigVersion=9\r\n\r\n"
        "[NotVeins]\r\n"
        "object/gimmick/mine/gimmick_mine_copperstone_place_01.prefab=1\r\n"
        "object/gimmick/mine/rock_b_0011.prefab=2\r\n"
        "object/gimmick/mine/rock_b_0012.prefab=0\r\n"), "the test ini is written");
    Settings::Load();
    const Config c = Settings::Snapshot();
    Check(!c.notVeins.count("object/gimmick/mine/gimmick_mine_copperstone_place_01.prefab"),
          "a =1 line from an older build is forgotten, so copper gets another break");
    Check(c.notVeins.count("object/gimmick/mine/rock_b_0011.prefab") == 1, "a =2 line is kept");
    Check(!c.notVeins.count("object/gimmick/mine/rock_b_0012.prefab"), "a =0 line is ignored");
    Check(!c.pauseAtHome, "PauseAtHome is off when the ini does not name it");
    Check(WriteText(ini, "[MasterLooter]\r\nConfigVersion=9\r\nPauseAtHome=1\r\n"), "the second test ini is written");
    Settings::Load();
    Check(Settings::Snapshot().pauseAtHome, "PauseAtHome=1 in the ini turns it on");
    DeleteFileW(ini.c_str());
}

int main()
{
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring appdata = std::wstring(tmp) + L"ml_tests_appdata";
    CreateDirectoryW(appdata.c_str(), nullptr);
    SetEnvironmentVariableW(L"LOCALAPPDATA", appdata.c_str());
    Paths::Init(GetModuleHandleW(nullptr));

    BreakTiming();
    ScanRetry();
    ProtectedTags();
    NotVeins();

    printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
