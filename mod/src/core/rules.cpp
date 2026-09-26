#include "rules.h"

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
        static const char* protectedTags[] = { "memory-fragment", "gimmick" };
        for (const char* tag : protectedTags)
        {
            if (!item.HasTag(tag)) continue;
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
        if (cfg.minValueCopper > 0 && item.value >= 0 && item.value < cfg.minValueCopper)
        {
            v.loot = false; v.rule = "below value floor"; v.detail = std::to_string(item.value) + " copper"; return v;
        }

        return DecideClass(item.klass, cfg);
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
}
