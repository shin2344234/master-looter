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
#include <vector>

#include "core/creaturedb.h"
#include "core/itemdb.h"
#include "core/paths.h"
#include "core/rules.h"
#include "core/settings.h"
#include "loot/breakjudge.h"
#include "loot/scanretry.h"
#include "loot/respawn.h"

using namespace ml;
using loot::BreakVerdict;
using loot::JudgeBreak;
using loot::NextScanRetryMs;
using loot::IsRespawnOf;

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

static void Respawns()
{
    // omegadragonr2, issue #232: at the Hernand teleport rat B010016A was
    // caught at 26.861 and B0100252 turned up on the same spot 3.4 s later,
    // then a new one every 0.6 s. The replacement is left alone.
    const uint32_t caught = 26861, next = 30268;
    Check(IsRespawnOf(0xB0100252, next, next, 0xB010016A, caught, 0.0f), "a rat listed on the caught one's spot after the catch is a respawn");
    Check(IsRespawnOf(0xB0100259, 30884, 30884, 0xB0100252, next - 1, 1.0f), "and so is the next one 0.6 s later");
    Check(IsRespawnOf(0xB0100252, caught, caught + 600, 0xB010016A, caught, 0.0f), "one listed in the same pass as the catch counts too");
    Check(!IsRespawnOf(0xB0100252, caught - 600, next, 0xB010016A, caught, 0.0f), "a creature listed before the catch was already there");
    Check(!IsRespawnOf(0xB010016A, next, next, 0xB010016A, caught, 0.0f), "the caught creature is not its own respawn");
    Check(!IsRespawnOf(0xB0100252, next, next, 0xB010016A, caught, 4.0f * 4.0f), "4 m away is a different creature");
    Check(!IsRespawnOf(0xB0100252, caught + 20001, caught + 20001, 0xB010016A, caught, 0.0f), "twenty seconds on, the catch no longer counts");
    const uint32_t w = 0xFFFFF000u;
    Check(IsRespawnOf(1, w + 0x2000, w + 0x2000, 2, w, 0.0f), "judged across the tick counter wrapping");
    Check(!IsRespawnOf(1, w - 1, w + 0x2000, 2, w, 0.0f), "and a creature listed before a catch near the wrap is still old");
}

// Issue #235. The sea creatures the game lets you ride are marked in the
// shipped creature table and nothing else is, so a registered manta ray or a
// wild shark is never caught while the small fish still are.
static void SeaMounts()
{
    Check(CreatureDb::Load(), "the creature table loads");
    const char* mounts[] = { "Animal_MantaRay_Wild_1", "Animal_Giant_MantaRay_Wild_1", "Animal_pink_MantaRay_Wild_1",
                             "Animal_EagleRay_Wild_1", "Animal_LightningRay_Wild_1", "Animal_Shark_Wild_1",
                             "Animal_hammerhead_shark_Wild_1", "Animal_Giant_Shark_Wild_1", "Animal_gold_Shark_Wild_1",
                             "Animal_Orca_Wild_1", "Animal_Dolphin_Wild_1", "Animal_Pink_Dolphin_Wild_1",
                             "Animal_OceanSunfish_Wild_1", "Animal_Big_Sturgeon_Wild_1", "Animal_PaddleFish_Wild_39101",
                             "Animal_SevenGill_Wild_39102" };
    int marked = 0;
    for (const char* k : mounts)
    {
        const Creature* cr = CreatureDb::ByKey(k);
        Check(cr && cr->mount, k);
        if (cr && cr->mount) ++marked;
    }
    Check(marked == 16, "all sixteen sea mounts are marked");
    // Small fish stay catchable, and the striped marlin, which carries no ride
    // interaction, is left to the switches. The cat carries Shark_Ride in its
    // record and is not a fish, so it is not marked either.
    const char* notMounts[] = { "Animal_Insect_Lobster", "Animal_Normal_Mackerel_Wild_1", "Animal_Middle_Red_Seabream_Wild_1",
                                "Animal_Middle_Pikefish_Wild_1", "Animal_Electric_Eel_Wild_35234", "Animal_ClownFish_Wild_1",
                                "Animal_VeryBig_Marlin_Wild_1", "Animal_Tricolor_Cat_Domestic" };
    for (const char* k : notMounts)
    {
        const Creature* cr = CreatureDb::ByKey(k);
        Check(cr && !cr->mount, k);
    }
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

// sherlucky1, Nexus posts tab, issue #129: SkipCarriedGear judges weapons,
// armour, bows, accessories and damaged gear that do not stack, and nothing else.
static void CarriedGearKinds()
{
    Check(ItemDb::Load(), "the item table loads");
    const Item* sword = ItemDb::ByStringKey("Taoria_OneHandSword");
    const Item* bow = ItemDb::ByStringKey("Robinhood_OneHandBow");
    const Item* ring = ItemDb::ByStringKey("Legendary_Titan_Ring");
    const Item* mail = ItemDb::ByStringKey("Luksa_ChainMail_Armor");
    const Item* bag = ItemDb::ByStringKey("Inventory_Expansion_1");
    const Item* copper = ItemDb::ByStringKey("Money_Copper");
    const Item* porridge = ItemDb::ByStringKey("Vegetable_Porridge");
    Check(sword && Rules::IsUnstackableGear(*sword), "a Tauria Curved Sword is gear the switch judges");
    Check(bow && Rules::IsUnstackableGear(*bow), "a bow is gear the switch judges");
    Check(ring && Rules::IsUnstackableGear(*ring), "a ring is gear the switch judges");
    Check(mail && Rules::IsUnstackableGear(*mail), "damaged chain mail is gear the switch judges");
    Check(bag && !Rules::IsUnstackableGear(*bag), "a bag stacks and is left to the other rules");
    Check(copper && !Rules::IsUnstackableGear(*copper), "money is not gear");
    Check(porridge && !Rules::IsUnstackableGear(*porridge), "food is not gear");
    Check(!Config{}.skipCarriedGear, "SkipCarriedGear is off by default");
}

// erikem007, Nexus bugs tab, issue #242: the chimney quest asks for the Broom
// to be taken by hand, and the Greymane camp cooking quest stalled on a looted
// Clear Soup recipe. Neither carries the game's quest tag, so the override
// table gives them one and the Quest items switch keeps the mod off them.
static void QuestStepItems()
{
    Check(ItemDb::Load(), "the item table loads");
    Config cfg;
    const Item* broom = ItemDb::ByStringKey("Equip_Broom");
    const Item* recipe = ItemDb::ByStringKey("Recipe_Meat_Porridge");
    const Item* sturdy = ItemDb::ByStringKey("Equip_Sturdy_Broom");
    const Item* skewers = ItemDb::ByStringKey("Recipe_Meat_Sanjeok");
    Check(broom && broom->HasTag("quest") && broom->klass == "tool", "the Broom carries the quest tag and stays a tool");
    Check(recipe && recipe->HasTag("quest") && recipe->klass == "recipe-food", "Recipe: Clear Soup carries the quest tag and stays a cooking recipe");
    Check(broom && std::strcmp(Rules::Decide(*broom, cfg).rule, "quest item") == 0, "the Broom is refused as a quest item by default");
    Check(recipe && std::strcmp(Rules::Decide(*recipe, cfg).rule, "quest item") == 0, "Recipe: Clear Soup is refused as a quest item by default");
    Check(sturdy && Rules::Decide(*sturdy, cfg).loot, "the Sturdy Broom is still taken");
    Check(skewers && Rules::Decide(*skewers, cfg).loot, "another cooking recipe is still taken");
    Config off; off.skipQuestItems = false;
    Check(broom && Rules::Decide(*broom, off).loot, "with Quest items off the Broom is taken again");
    Config lifted; if (broom) lifted.itemRule[broom->key] = 1;
    Check(broom && Rules::Decide(*broom, lifted).loot, "an Always on the Broom lifts it");
}

// Sov, Discord, 1 October 2026, issue #95: a floor on food by how much it
// heals, so the 400 health dishes come in and the 120 ones stay behind.
static void FoodHealthFloor()
{
    Check(ItemDb::Load(), "the item table loads");
    Check(ItemDb::HealCount() > 250, "the table carries a health value for the dishes");
    const Item* hearty = ItemDb::ByStringKey("Gungjung_Tteokbokki_XLarge");
    const Item* porridge = ItemDb::ByStringKey("Vegetable_Porridge");
    const Item* choc = ItemDb::ByStringKey("Item_Chocolate");
    const Item* copper = ItemDb::ByStringKey("Money_Copper");
    Check(hearty && hearty->heal == 860, "Hearty Pan-Fried Rice Cakes heal 860");
    Check(porridge && porridge->heal == 200, "Vegetable Porridge heals 200");
    Check(choc && choc->heal == 0, "Chocolate heals by a percentage and carries no health value");
    Check(copper && copper->heal == 0, "copper carries no health value");
    if (!hearty || !porridge || !choc || !copper) return;

    Config off;
    Check(off.minFoodHealth == 0, "MinFoodHealth is off by default");
    Check(Rules::Decide(*porridge, off).loot, "with the floor off the porridge is taken as before");

    Config cfg;
    cfg.minFoodHealth = 300;
    const Rules::Verdict low = Rules::Decide(*porridge, cfg);
    Check(!low.loot && strcmp(low.rule, "below health floor") == 0, "a 300 floor leaves the 200 porridge");
    Check(low.detail == "200 health, floor 300", "the refusal names the dish's health and the floor");
    Check(Rules::Decide(*hearty, cfg).loot, "a 300 floor takes the 860 rice cakes");
    Check(Rules::Decide(*choc, cfg).loot, "food with no health value is not judged by the floor");

    Config copperFloor = cfg;
    copperFloor.minValueCopper = 1000;
    Check(Rules::Decide(*hearty, copperFloor).loot, "the health floor replaces the copper floor for a dish");
    Check(!Rules::Decide(*choc, copperFloor).loot, "the copper floor still judges food with no health value");

    Config skipped = cfg;
    skipped.classRule[hearty->klass] = 0;
    Check(!Rules::Decide(*hearty, skipped).loot, "a skipped class still refuses a dish over the floor");
    Config always = cfg;
    always.itemRule[porridge->key] = 1;
    Check(Rules::Decide(*porridge, always).loot, "an item Always still takes a dish under the floor");
}

// LuxDragon, 9 November 2026: an Always on flower pots did nothing with
// Furniture off. stinkfire, bug 1172278: shop tables are housing-only and must
// never be taken, whatever the player sets.
static void AlwaysOverSwitch()
{
    const Config cfg;
    const Item* pot = ItemDb::ByStringKey("Collection_Prop_FlowerPot_0005");
    Check(pot && pot->HasTag("furniture"), "the Red Rose Flower Pot is in the table and tagged furniture");
    if (pot)
    {
        Check(!Rules::ExplicitAlways(*pot, cfg).loot, "no rule set: the Furniture switch still decides a pot");
        Config item = cfg; item.itemRule[pot->key] = 1;
        const Rules::Verdict a = Rules::ExplicitAlways(*pot, item);
        Check(a.loot && strcmp(a.rule, "item override") == 0, "an item Always on the pot lifts the switch");
        Config tag = cfg; tag.tagRule["flower-pot"] = 1;
        Check(Rules::ExplicitAlways(*pot, tag).loot, "a tag Always on flower-pot lifts the switch");
        Config cls = cfg; cls.classRule["flower-pot"] = 1;
        Check(!Rules::ExplicitAlways(*pot, cls).loot, "a ticked class does not lift the switch");
        Config veto = tag; veto.tagRule["collectible"] = -1;
        Check(!Rules::ExplicitAlways(*pot, veto).loot, "a never on another of its tags wins over the Always");
        Config never = cfg; never.itemRule[pot->key] = -1;
        Check(!Rules::ExplicitAlways(*pot, never).loot, "an item Never is not an Always");
    }
    static const char* fixtures[] = {
        "Item_cd_in_dff_standing_table_02_housing", "Item_Background_Breakable_Wood_176",
        "Item_Background_Breakable_Wood_259", "Item_gimmick_wood_bench_0001", "Item_me_00_str_sawmill_000_0001",
    };
    for (const char* key : fixtures)
    {
        const Item* it = ItemDb::ByStringKey(key);
        Check(it != nullptr, key);
        if (!it) continue;
        Config c = cfg;
        c.itemRule[it->key] = 1;
        c.tagRule["furniture"] = 1;
        c.tagRule["housing-only"] = 1;
        Check(!Rules::ExplicitAlways(*it, c).loot, "a housing-only shop fixture never lifts a switch, even with Always on it and its tags");
    }
    // Population: Always on the furniture tag, how many furnishings lift the
    // switch and how many stay behind it.
    Config fur = cfg; fur.tagRule["furniture"] = 1;
    int furn = 0, lifted = 0, held = 0, quest = 0;
    for (const Item& it : ItemDb::All())
    {
        const bool ok = Rules::ExplicitAlways(it, fur).loot;
        if (it.HasTag("quest") && ok) ++quest;
        if (!it.HasTag("furniture")) { Check(!ok, "an Always on furniture lifts nothing outside the tag"); continue; }
        ++furn;
        if (ok) ++lifted;
        else ++held;
        if (ok && it.HasTag("housing-only")) Check(false, it.stringKey.c_str());
    }
    printf("      Always on furniture: %d of %d furnishings lift the switch, %d stay behind it\n", lifted, furn, held);
    Check(quest == 0, "no quest item ever lifts a switch");
}

// Game 2.05.00: 8,379 rows, our 7,068 base rows in place and the 1,311
// DLC-only items after them, where the shipped table gives them no row. The
// live table is laid out the same way here, with one base row unreadable.
static void LiveRows()
{
    std::vector<std::string> keys;
    std::vector<const Item*> dlc;
    for (const Item& it : ItemDb::All())
    {
        if (it.row < 0) { dlc.push_back(&it); continue; }
        if (keys.size() <= static_cast<size_t>(it.row)) keys.resize(it.row + 1);
        keys[it.row] = it.stringKey;
    }
    const size_t base = keys.size();
    printf("      %zu base rows, %zu DLC-only items\n", base, dlc.size());
    Check(base == 7068 && dlc.size() == 1311, "the shipped table has 7,068 base rows and 1,311 DLC-only items");
    const Item* hidden = ItemDb::ByRow(5);
    keys[5].clear();
    for (const Item* it : dlc) keys.push_back(it->stringKey);
    Check(ItemDb::ByRow(static_cast<int>(base)) == nullptr, "before the live rows, the first DLC row names nothing");
    const ItemDb::LiveRows lr = ItemDb::SetLiveRows(keys);
    Check(lr.readable == 8378 && lr.sameRow == 7067 && lr.byKey == 1311 && lr.unknown == 0, "every readable row is found, the DLC ones by key");
    bool all = true;
    for (size_t i = 0; i < dlc.size(); ++i)
    {
        const int row = static_cast<int>(base + i);
        if (ItemDb::ByRow(row) != dlc[i] || ItemDb::GameRow(*dlc[i]) != row) all = false;
    }
    Check(all, "every DLC item is named by its live row, and its live row is what gets sent back");
    Check(hidden && ItemDb::ByRow(5) == hidden && ItemDb::GameRow(*hidden) == 5, "an unreadable row keeps the shipped mapping");
    const Item* copper = ItemDb::ByStringKey("Money_Copper");
    Check(copper && ItemDb::ByRow(copper->row) == copper && ItemDb::GameRow(*copper) == copper->row, "a base item keeps its row");
    Check(ItemDb::ByRow(static_cast<int>(base + dlc.size())) == nullptr, "a row past the table names nothing");
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
    Check(!Settings::Snapshot().pauseMounted, "PauseWhenMounted is off when the ini does not name it");
    Check(WriteText(ini, "[MasterLooter]\r\nConfigVersion=9\r\nPauseWhenMounted=1\r\n"), "the third test ini is written");
    Settings::Load();
    Check(Settings::Snapshot().pauseMounted, "PauseWhenMounted=1 in the ini turns it on");
    Check(!Settings::Snapshot().dropRefusedChests, "DropRefusedChests is off when the ini does not name it");
    Check(WriteText(ini, "[MasterLooter]\r\nConfigVersion=9\r\nDropRefusedChests=1\r\n"), "the DropRefusedChests test ini is written");
    Settings::Load();
    Check(Settings::Snapshot().dropRefusedChests, "DropRefusedChests=1 in the ini turns it on");
    Check(!Settings::Snapshot().dropRefused, "DropRefusedChests=1 leaves Drop refused loot from bodies off");
    Check(Settings::Snapshot().minFoodHealth == 0, "MinFoodHealth is 0 when the ini does not name it");
    Check(WriteText(ini, "[MasterLooter]\r\nConfigVersion=9\r\nMinFoodHealth=300\r\n"), "the MinFoodHealth test ini is written");
    Settings::Load();
    Check(Settings::Snapshot().minFoodHealth == 300, "MinFoodHealth=300 in the ini sets the floor");
    Check(WriteText(ini, "[MasterLooter]\r\nConfigVersion=9\r\nMinFoodHealth=-5\r\n"), "the negative MinFoodHealth test ini is written");
    Settings::Load();
    Check(Settings::Snapshot().minFoodHealth == 0, "a negative MinFoodHealth reads as off");
    Check(!Settings::Snapshot().hideItemLog, "HideItemLog is off when the ini does not name it");
    Check(WriteText(ini, "[MasterLooter]\r\nConfigVersion=9\r\nHideItemLog=1\r\n"), "the HideItemLog test ini is written");
    Settings::Load();
    Check(Settings::Snapshot().hideItemLog, "HideItemLog=1 in the ini turns it on");
    Check(WriteText(ini, "[MasterLooter]\r\nConfigVersion=9\r\n\r\n[Items]\r\nHideItemLog=1\r\n"), "the misplaced HideItemLog ini is written");
    Settings::Load();
    Check(!Settings::Snapshot().hideItemLog, "HideItemLog outside [MasterLooter] does nothing");
    Check(!Settings::Snapshot().skipCarriedGear, "SkipCarriedGear is off when the ini does not name it");
    Check(WriteText(ini, "[MasterLooter]\r\nConfigVersion=9\r\nSkipCarriedGear=1\r\n"), "the SkipCarriedGear test ini is written");
    Settings::Load();
    Check(Settings::Snapshot().skipCarriedGear, "SkipCarriedGear=1 in the ini turns it on");
    Check(!Settings::Snapshot().logFolder && !Settings::PeekLogFolder(), "LogFolder is off when the ini does not name it");
    Check(WriteText(ini, "[MasterLooter]\r\nConfigVersion=9\r\nLogFolder=1\r\n"), "the LogFolder test ini is written");
    Check(Settings::PeekLogFolder(), "the early peek reads LogFolder=1 before any load");
    Settings::Load();
    Check(Settings::Snapshot().logFolder, "LogFolder=1 in the ini turns it on");
    Check(WriteText(ini, "[MasterLooter]\r\nConfigVersion=9\r\n\r\n[Items]\r\nLogFolder=1\r\n"), "the misplaced LogFolder ini is written");
    Check(!Settings::PeekLogFolder(), "the early peek ignores LogFolder outside [MasterLooter], as Load does");
    DeleteFileW(ini.c_str());
}

static void ClassMultipliers()
{
    const std::wstring ini = Paths::File(L"MasterLooter.ini");
    Check(WriteText(ini, "[MasterLooter]\r\nConfigVersion=9\r\n"), "the multiplier-free ini is written");
    Settings::Load();
    Check(Settings::Snapshot().classMult.empty(), "no class is multiplied when the ini names none");
    Check(WriteText(ini,
        "[MasterLooter]\r\nConfigVersion=9\r\n\r\n"
        "[ClassMultipliers]\r\n"
        "ore=3\r\n"
        "stone=1\r\n"
        "jewel=50\r\n"
        "herb=0\r\n"), "the multiplier ini is written");
    Settings::Load();
    Config c = Settings::Snapshot();
    Check(c.classMult.count("ore") && c.classMult["ore"] == 3, "ore=3 loads as x3");
    Check(!c.classMult.count("stone"), "x1 is the same as off and is not kept");
    Check(c.classMult.count("jewel") && c.classMult["jewel"] == 10, "a multiplier above 10 is held at 10");
    Check(!c.classMult.count("herb"), "x0 is not a multiplier");

    // A preset is the whole config written out, so it shows what the ini
    // writer puts under [ClassMultipliers] and that the reader takes it back.
    Check(Settings::SavePreset("multtest"), "the preset is written");
    {
        std::lock_guard<std::recursive_mutex> lk(Settings::Mutex());
        Settings::Get().classMult.clear();
    }
    Check(Settings::LoadPreset("multtest"), "the preset loads back");
    c = Settings::Snapshot();
    Check(c.classMult.size() == 2 && c.classMult["ore"] == 3 && c.classMult["jewel"] == 10,
          "the multipliers survive a write and a read");
    Settings::DeletePreset("multtest");
    DeleteFileW(ini.c_str());
}

// LlexX, posts tab, 15 November 2026: the mod gathered a Greymane Dwelling
// Tent whatever the filter said. The node table has no row for the prefab, so
// the item table's placed_prefab column is what names it.
static void PlacedHousing()
{
    const Config cfg;
    Check(ItemDb::PlacedCount() == 572, "every housing-only item names the prefab it is placed as");
    const Item* tent = ItemDb::ByPlacedPrefab("gimmick_tent_graymane_01");
    Check(tent && tent->stringKey == "tent_graymane_01", "gimmick_tent_graymane_01 is the Greymane Dwelling Tent");
    if (tent)
    {
        Check(tent->HasTag("furniture"), "the tent answers to the Furniture switch");
        Check(Prot(Rules::Decide(*tent, cfg)), "the tent is protected as housing-only by default");
        Config item = cfg; item.itemRule[tent->key] = 1;
        Check(Rules::Decide(*tent, item).loot, "an item Always on the tent still takes it");
    }
    Check(!ItemDb::ByPlacedPrefab("gimmick_tent_bandit_01"), "a bandit camp tent places no item");
    Check(!ItemDb::ByPlacedPrefab(""), "an empty prefab names nothing");
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
    Respawns();
    SeaMounts();
    ProtectedTags();
    FoodHealthFloor();
    QuestStepItems();
    CarriedGearKinds();
    AlwaysOverSwitch();
    PlacedHousing();
    LiveRows();
    NotVeins();
    ClassMultipliers();

    printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
