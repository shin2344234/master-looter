#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ml
{
    struct Item
    {
        uint32_t    key   = 0;
        std::string stringKey;
        std::string name;       // English, may be empty for dev items
        std::string klass;      // first tag in the priority order (see build_item_db.py)
        std::string tags;       // space separated, with a leading and trailing space for fast lookup
        int         tier  = 0;
        long long   value = -1; // copper sell value, -1 when unknown
        bool HasTag(const std::string& tag) const { return tags.find(" " + tag + " ") != std::string::npos; }
    };

    namespace ItemDb
    {
        bool Load(); // MasterLooter.items.tsv next to the plugin
        bool Loaded();
        int  Count();
        const Item* Find(uint32_t key);
        const std::vector<Item>& All();
        // (class, item count) sorted by count, largest first
        const std::vector<std::pair<std::string, int>>& Classes();
        // (tag, item count) sorted by name
        const std::vector<std::pair<std::string, int>>& Tags();
    }
}
