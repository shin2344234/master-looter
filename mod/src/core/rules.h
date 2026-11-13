#pragma once
#include <string>

#include "itemdb.h"
#include "settings.h"

namespace ml::Rules
{
    struct Verdict
    {
        bool        loot   = true;
        const char* rule   = "default"; // which rule decided
        std::string detail;             // the class, tag or key it matched
    };

    // Order: item override, tag never, protected tags (memory-fragment, gimmick,
    // housing-only),
    // tag always, quest/no-sell/dev filters, value floor, class rule, then loot.
    Verdict Decide(const Item& item, const Config& cfg);

    // The class rule alone, for something that has a class and no item: a
    // creature the game gives no item of its own is judged by the class the
    // creature table gives it.
    Verdict DecideClass(const std::string& klass, const Config& cfg);

    // Whether the player set Always on this very item (or its pack twin) or
    // on one of its tags, so that the Always outranks a kind switch such as
    // Furniture or Plants. loot is true only then. Never true for an item
    // carrying a protected tag (memory-fragment, gimmick unless a material,
    // housing-only), the quest tag, the dev class, or the quest equipment
    // pair: those stay refused by the switch whatever the Always says. A
    // class rule never lifts a switch, since 1 there is only a ticked box
    // and Loot all ticks every one.
    Verdict ExplicitAlways(const Item& item, const Config& cfg);
}
