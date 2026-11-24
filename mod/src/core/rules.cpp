#include "rules.h"

#include <cstring>

namespace ml::Rules
{
    Verdict Decide(const Item& item, const Config& cfg)
    {
        Verdict v;

        auto io = cfg.itemRule.find(item.key);
        if (io != cfg.itemRule.end())
        {
            v.loot = io->second > 0;
            v.rule = "item override";
            v.detail = item.stringKey;
            return v;
        }
        // A pack follows the override on what it holds, and the other way
        // round, since the Items tab shows both under one name (see Item::twin).
        if (item.twin && (io = cfg.itemRule.find(item.twin)) != cfg.itemRule.end())
        {
            v.loot = io->second > 0;
            v.rule = "item override";
            const Item* other = ItemDb::Find(item.twin);
            v.detail = other ? other->stringKey : item.stringKey;
            return v;
        }

        // Tags: a "never" on any tag wins over an "always" on another.
        const char* alwaysTag = nullptr;
        for (const auto& kv : cfg.tagRule)
        {
            if (!item.HasTag(kv.first)) continue;
            if (kv.second < 0) { v.loot = false; v.rule = "tag never"; v.detail = kv.first; return v; }
            if (kv.second > 0 && !alwaysTag) alwaysTag = kv.first.c_str();
        }
        // Protected by default: memory fragments start a memory scene when
        // taken, and mechanism parts (puzzle pillars, power cores) break the
        // puzzle they belong to. Only an explicit "always" on that very tag
        // lifts the protection; an "always" on a broader tag does not.
        //
        // housing-only joined them on 10 October 2026. GG1313 and Ahplla on
        // the posts tab: with Furniture on, the camp tables went, the cups and
        // plates on them were left floating, and characters in later cutscenes
        // lay on a table that was no longer there. The table never reached the
        // bag, only the housing decoration storage, which is what the tag
        // means. 108 items carry it, 81 of them furniture: the camp Table
        // (Item_Background_Breakable_Wood_10 and _11), beds, wardrobes,
        // cabinets, the house chairs and tables, the Cooking Pot, Griddle and
        // Cauldron, and a dozen jars. Each one taken is a fixture gone from the
        // scene it furnishes, so none is taken unless the tag is set to always.
        //
        // A gimmick item that is also a material is let through. LuxDragon, 28
        // October 2026: tumbleweeds can be picked up by hand and the mod passed
        // every one over as protected. Of the 486 items tagged gimmick, the
        // Tumbleweed (Item_gimmick_tumbleweed_0001) is the only one that is
        // also tagged material, so no puzzle part or power core is let through
        // with it. It is a crafting material and the Plants switch decides it
        // like any other plant. An "always" or "never" on material or gimmick
        // still wins, since the tag rules above run first.
        static const char* protectedTags[] = { "memory-fragment", "gimmick", "housing-only" };
        for (const char* tag : protectedTags)
        {
            if (!item.HasTag(tag)) continue;
            if (tag == protectedTags[1] && item.HasTag("material")) continue;
            auto t = cfg.tagRule.find(tag);
            if (t != cfg.tagRule.end() && t->second > 0) continue;
            v.loot = false; v.rule = "protected"; v.detail = tag; return v;
        }
        if (alwaysTag) { v.loot = true; v.rule = "tag always"; v.detail = alwaysTag; return v; }

        if (item.klass == "dev") { v.loot = false; v.rule = "dev item"; return v; }
        if (cfg.skipQuestItems && item.HasTag("quest")) { v.loot = false; v.rule = "quest item"; return v; }
        // LuxDragon, 13 September 2026: a Fertilizer Sprayer taken off the
        // floor of a castle put a House Celeste quest step out of order, and
        // he had to drop it and pick it up again to clear it. It carries no
        // quest tag, so the switch above never saw it. What it does carry is
        // important and no-sell together, and that pair is a good description
        // of quest equipment: of the 180 items it covers beyond the quest tag,
        // 179 sell for a single copper. No sellable weapon is caught, because a
        // weapon you can sell fails the second half.
        //
        // The 180th is the Abyss Artifact at 2,850 copper, and it does change
        // behaviour: gimmick_catched_flowerbutterfly_0001 names it as its yield,
        // so that node is now passed over where 1.6.16 gathered it. Left as it
        // stands rather than carved out. The artifact is no-sell and no-discard,
        // so those 2,850 copper can never be realised, and a butterfly socket
        // naming an abyss artifact reads like a marker that matched rather than
        // a drop anybody has seen. If a report says otherwise, the fix is in
        // that row and not in this rule. An earlier version of this comment
        // claimed OffLimits refuses the artifact by name; it does not, because
        // OffLimits reads the node's prefab path and that prefab carries none
        // of its words.
        if (cfg.skipQuestGear && item.HasTag("important") && item.HasTag("no-sell"))
        { v.loot = false; v.rule = "quest equipment"; return v; }
        // Money is no-sell because it is money already. Sov1737's log of 26
        // September 2026 has this switch refusing Copper and its pouches
        // twelve times in one session. All 28 currency items that carry the
        // tag are coins, pouches, camp resources, contributions, tokens or
        // Pearl, so the class is left out whole. The Classes tab still
        // decides about currency like any other class.
        if (cfg.skipNoSell && item.HasTag("no-sell") && item.klass != "currency")
        { v.loot = false; v.rule = "unsellable"; return v; }
        // A class with a floor of its own uses that one in place of the global
        // floor, so food can sit at one copper while weapons want three hundred.
        // The rule name stays the global floor's on purpose: the delete paths
        // spare anything refused under it, since a floor says what is not worth
        // carrying and not what is worth losing, and that holds for this too.
        // Sov, Discord, 1 October 2026, issue #95: keep the 400 health dishes
        // and leave the 120 ones. A dish the table gives a heal value is
        // judged by health in place of copper while MinFoodHealth is set, so a
        // strong dish worth little still comes in under a copper floor. The
        // delete paths spare it the way they spare the copper floor.
        if (cfg.minFoodHealth > 0 && item.heal > 0)
        {
            if (item.heal >= cfg.minFoodHealth) return DecideClass(item.klass, cfg);
            v.loot = false; v.rule = "below health floor";
            v.detail = std::to_string(item.heal) + " health, floor " + std::to_string(cfg.minFoodHealth);
            return v;
        }
        int floor = cfg.minValueCopper;
        const auto cf = cfg.classFloor.find(item.klass);
        const bool ownFloor = cf != cfg.classFloor.end() && cf->second > 0;
        if (ownFloor) floor = cf->second;
        if (floor > 0 && item.value >= 0 && item.value < floor)
        {
            v.loot = false; v.rule = "below value floor"; v.detail = std::to_string(item.value) + " copper";
            if (ownFloor) v.detail += ", " + item.klass + " floor " + std::to_string(floor);
            return v;
        }

        return DecideClass(item.klass, cfg);
    }

    // LuxDragon, Discord, 9 November 2026: an Always on flower pots did
    // nothing while Furniture was off. The kind switch is asked in the engine
    // before Decide is ever reached, so the Always was never read. 309
    // refusals of flower pots in the saved logs read "furniture off".
    Verdict ExplicitAlways(const Item& item, const Config& cfg)
    {
        Verdict v;
        v.loot = false;
        v.rule = "no always";
        // The protected tags and quest outrank every rule when the question is
        // a switch. Decide lets an item override past them, and that is left
        // as it was, but a switch the player turned off is a second refusal,
        // and stinkfire's shop tables (bug 1172278) are housing-only furniture
        // that must stay put with Furniture on or off.
        static const char* never[] = { "memory-fragment", "gimmick", "housing-only", "quest" };
        for (const char* tag : never)
        {
            if (!item.HasTag(tag)) continue;
            if (tag == never[1] && item.HasTag("material")) continue;
            v.rule = "protected"; v.detail = tag; return v;
        }
        if (item.klass == "dev") { v.rule = "dev item"; return v; }
        if (item.HasTag("important") && item.HasTag("no-sell")) { v.rule = "quest equipment"; return v; }
        const Verdict d = Decide(item, cfg);
        if (!d.loot) { v.rule = d.rule; v.detail = d.detail; return v; }
        if (std::strcmp(d.rule, "item override") == 0 || std::strcmp(d.rule, "tag always") == 0) return d;
        return v;
    }

    Verdict DecideClass(const std::string& klass, const Config& cfg)
    {
        Verdict v;
        auto co = cfg.classRule.find(klass);
        if (co != cfg.classRule.end() && co->second == 0)
        {
            v.loot = false; v.rule = "class skipped"; v.detail = klass; return v;
        }
        v.loot = true;
        v.rule = co != cfg.classRule.end() ? "class" : "default";
        v.detail = klass;
        return v;
    }

    bool IsUnstackableGear(const Item& item)
    {
        // sherlucky1, issue #129: weapons, armour, bows and accessories, the
        // things that cannot stack. Bows are class weapon. A bag is class bag
        // and stacks, so the stackable test leaves it out; no other item in
        // these classes carries the tag in the 3732 table.
        static const char* gear[] = { "weapon", "shield", "helm", "body-armor", "gloves", "boots", "cloak", "armor",
                                      "mask", "eyewear", "damaged-gear", "accessory", "necklace", "ring", "earring", "bag" };
        if (item.HasTag("stackable")) return false;
        for (const char* k : gear) if (item.klass == k) return true;
        return false;
    }
}
