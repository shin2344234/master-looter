#pragma once
#include <string>
#include <vector>

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
        // Whether the game will break this node when the mod drives the swing
        // and the break at it. The row says so itself: a vein carries
        // SelfForceBreakImpulse or a BreakProjectileKey, and the ore chunks a
        // vein drops carry neither, because they are picked up. Driving a
        // break at one of those does nothing at all, which is what made
        // bismuth look intermittent. A table without the column leaves this
        // true, so the old behaviour and the learning fallback still apply.
        bool breaks = true;
        // Everything the gimmick row says this node can hand over, as item
        // string keys. The itemKey above is one item the generator could name
        // from the prefab, and 835 of the 966 rows have none; this is read out
        // of the row's own drop entries instead and covers 302 of them.
        //
        // It is a set of possibilities, not a promise of one item, so the only
        // safe use is refusing a node when the rules refuse every entry. A
        // spurious entry then makes the mod more willing to touch the node, and
        // a missing one is what the learned [NodeYields] net covers.
        std::vector<std::string> yields;
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
