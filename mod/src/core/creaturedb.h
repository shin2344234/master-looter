#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ml
{
    // A creature the player can catch, keyed by its CharacterInfo string key
    // ("Animal_Insect_Butterfly"), with the class of the item it becomes.
    struct Creature
    {
        uint32_t    key = 0;
        std::string stringKey;
        std::string name;
        std::string klass;      // insect, fish, seafood, animal, amphibian
        int         itemRow = -1;
    };

    namespace CreatureDb
    {
        bool Load();   // MasterLooter.creatures.tsv next to the plugin, else the copy compiled in
        bool Loaded();
        const char* Source();
        int  Count();
        // Every row, for the Creatures tab to search. Empty until Load().
        const std::vector<Creature>& All();
        const Creature* ByKey(const char* stringKey);
        // The creature whose string key appears inside `text` (longest wins), e.g.
        // an animation or behaviour asset name that embeds it. Null when none.
        const Creature* InText(const char* text);
        // Classify by a species word in `text` ("cd_m0002_rat" -> animal, Rat).
        // Words are the head noun of every catchable creature's English name
        // ("Red Tonguesole" -> tonguesole, "Tree Frog" -> frog) plus a short
        // generic list; key tokens do not take part (see BuildWords). `klass`
        // is null when nothing matched. `row` is the creature the word names
        // when `exact`, otherwise a representative of several sharing the word
        // (every butterfly says "butterfly").
        struct Match { const char* klass = nullptr; const Creature* row = nullptr; bool exact = false; std::string word; };
        Match Classify(const char* text);
    }
}
