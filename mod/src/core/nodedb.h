#pragma once
#include <string>

namespace ml
{
    // What a gather node is, looked up by the prefab it was placed from. The
    // id a node reports is not stable between sessions, so the prefab path is
    // the only durable name it has; every path is a row of the game's gimmick
    // table, and that row's tags say what kind of gathering it is.
    struct NodeType
    {
        std::string prefab;   // basename, lower case, without _scenecollector
        std::string kind;     // plant, ore, stone, wood, item
        std::string itemKey;  // what it yields, when the name gives it; may be empty
        std::string name;     // for the log and the Nearby list
        // True when the kind is the game's own gimmick tag, false when the
        // generator guessed it from the prefab name. A guess decides which
        // switch the node answers to; only the game's word buys the long
        // arming reach, since a guess once claimed shop counters and pipework.
        bool tagged = false;
    };

    namespace NodeDb
    {
        bool Load();   // MasterLooter.nodes.tsv next to the plugin, else the copy compiled in
        bool Loaded();
        const char* Source();
        int  Count();
        // `path` is the full prefab path as the game holds it. The folder is
        // dropped and a _scenecollector wrapper normalised away before lookup.
        const NodeType* ByPrefab(const char* path);
    }
}
