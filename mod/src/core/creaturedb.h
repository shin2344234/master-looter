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
        std::string klass;      // insect, fish, animal, amphibian
        int         itemRow = -1;
    };

    namespace CreatureDb
    {
        bool Load();   // MasterLooter.creatures.tsv next to the plugin
        bool Loaded();
        int  Count();
        const Creature* ByKey(const char* stringKey);
    }
}
