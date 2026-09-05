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
        if (alwaysTag) { v.loot = true; v.rule = "tag always"; v.detail = alwaysTag; return v; }

        if (item.klass == "dev") { v.loot = false; v.rule = "dev item"; return v; }
        if (cfg.skipQuestItems && item.HasTag("quest")) { v.loot = false; v.rule = "quest item"; return v; }
        if (cfg.skipNoSell && item.HasTag("no-sell")) { v.loot = false; v.rule = "unsellable"; return v; }
        if (cfg.minValueCopper > 0 && item.value >= 0 && item.value < cfg.minValueCopper)
        {
            v.loot = false; v.rule = "below value floor"; v.detail = std::to_string(item.value) + " copper"; return v;
        }

        auto co = cfg.classRule.find(item.klass);
        if (co != cfg.classRule.end() && co->second == 0)
        {
            v.loot = false; v.rule = "class skipped"; v.detail = item.klass; return v;
        }
        v.loot = true;
        v.rule = co != cfg.classRule.end() ? "class" : "default";
        v.detail = item.klass;
        return v;
    }
}
