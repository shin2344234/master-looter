#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ml
{
    struct Item
    {
        int         row   = -1; // position in iteminfo, the game's runtime item type id
        uint32_t    key   = 0;
        std::string stringKey;
        std::string name;       // English, may be empty for dev items
        std::string klass;      // first tag in the priority order (see build_item_db.py)
        std::string tags;       // space separated, with a leading and trailing space for fast lookup
        int         tier  = 0;
        long long   value = -1; // copper sell value, -1 when unknown
        bool HasTag(const std::string& tag) const { return tags.find(" " + tag + " ") != std::string::npos; }
        const char* Label() const { return name.empty() ? stringKey.c_str() : name.c_str(); }
    };

    namespace ItemDb
    {
        bool Load(); // MasterLooter.items.tsv next to the plugin, else the copy compiled in
        bool Loaded();
        const char* Source(); // "built into the plugin" or "file next to the plugin"
        int  Count();
        const Item* Find(uint32_t key);
        const Item* ByRow(int row);                       // runtime type id -> item
        const Item* ByStringKey(const char* stringKey);   // engine key ("Money_Copper") -> item
        const std::vector<Item>& All();
        // (class, item count) sorted by count, largest first
        const std::vector<std::pair<std::string, int>>& Classes();
        // (tag, item count) sorted by name
        const std::vector<std::pair<std::string, int>>& Tags();
    }
}
