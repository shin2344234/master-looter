#pragma once
#include <cstdint>
#include <string>

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
        bool Load();   // MasterLooter.creatures.tsv next to the plugin
        bool Loaded();
        int  Count();
        const Creature* ByKey(const char* stringKey);
        // The creature whose string key appears inside `text` (longest wins), e.g.
        // an animation or behaviour asset name that embeds it. Null when none.
        const Creature* InText(const char* text);
        // Classify by the species words in `text` ("cd_m0002_rat" -> animal, Rat).
        // Words come from the name and key of every creature that becomes an
        // item (monsters and mounts do not vote) plus a short generic list.
        // Returns the class ("insect", "fish", "seafood", "animal", "amphibian")
        // or null; `creature` gets a representative row when the word names a
        // specific one.
        const char* Classify(const char* text, const Creature** creature);
    }
}
