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
        // The other half of a pack and what it holds, 0 for nearly everything.
        // A Delysian machine drops Cog_Pack, which turns into Cog the moment it
        // is taken; both are called Cogwheel, so the Items tab lists two rows
        // with one name and an override on the wrong one did nothing. Fyreon87
        // said on the posts tab on 29 October 2026 that Cogwheel and Small
        // Battery set to Always were still left on Delysian enemies, with his
        // kuku-currency class off. Rules::Decide reads the twin's override
        // when the item has none of its own.
        uint32_t    twin  = 0;
        bool HasTag(const std::string& tag) const { return tags.find(" " + tag + " ") != std::string::npos; }
        const char* Label() const { return name.empty() ? stringKey.c_str() : name.c_str(); }
    };

    namespace ItemDb
    {
        bool Load(); // MasterLooter.items.tsv next to the plugin, else the copy compiled in
        bool Loaded();
        const char* Source(); // "built into the plugin" or "file next to the plugin"
        int  Count();
        // One past the highest runtime row in the table. Smaller than Count()
        // since 1.0.0.3392: the DLC's own items carry no row and are found by
        // string key only.
        int  RowCount();
        const Item* Find(uint32_t key);
        const Item* ByRow(int row);                       // runtime type id -> item

        // The live table's own row order, read from each row's string key.
        // keyByRow[row] is the key the game's row holds, empty where the row
        // was unbuilt or unreadable. After this, ByRow answers from the live
        // order: a row with a key maps to the item with that key whatever our
        // row column says, and a row without one keeps the shipped mapping.
        // Game 1.0.0.3663 puts the 1,311 DLC-only items after the base rows,
        // and the shipped table gives those no row, so until this ran ByRow
        // named none of them. Call once; later calls are ignored.
        struct LiveRows { int readable = 0, sameRow = 0, byKey = 0, unknown = 0; };
        LiveRows SetLiveRows(const std::vector<std::string>& keyByRow);
        bool HasLiveRows();
        // The game's runtime row for this item: the live row once
        // SetLiveRows has run, the shipped row before that. -1 when the item
        // has none, which before the live rows is every DLC-only item.
        int GameRow(const Item& item);
        const Item* ByStringKey(const char* stringKey);   // engine key ("Money_Copper") -> item
        const std::vector<Item>& All();
        // (class, item count) sorted by count, largest first
        const std::vector<std::pair<std::string, int>>& Classes();
        // (tag, item count) sorted by name
        const std::vector<std::pair<std::string, int>>& Tags();
    }
}
