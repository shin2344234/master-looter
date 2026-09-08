#pragma once
#include <cstddef>
#include <cstdint>

// What the loot engine knows about the running game: resolved functions and
// globals, the actor manager, entity accessors, the player's inventory, and
// the item and gimmick name tables. Everything reads through mem.h guards.
namespace ml::game
{
    struct Vec3 { float x = 0, y = 0, z = 0; };

    struct SigResult
    {
        const char* name;
        uintptr_t   addr;   // 0 when unresolved
        size_t      hits;
        bool        required;
    };

    struct Fns
    {
        uintptr_t tlsInit = 0, descLookup = 0, allocEvent = 0, enqueue = 0;
        uintptr_t moveUpdate = 0;    // primary game-thread pump
        uintptr_t areaSweepHit = 0;  // fallback pump (hook at the match itself)
        uintptr_t ownCheck = 0, armFn = 0;
        uintptr_t stateDriver = 0;   // gimmick transition driver, used to break veins
        uintptr_t descMask = 0, queue = 0;                  // globals
        uintptr_t itemTableGlobal = 0, gimmickTableGlobal = 0;
    };

    // The game hashes gimmick state and event names with Jenkins lookup3
    // (hashlittle), seeded length + 0xDEBA1DCD, over the LOWERCASE name. The
    // routine is at RVA 0x12D5630 in build 2.01.00, but there is no reason to
    // call it: reimplementing it means the mod names what it wants in source
    // ("onbreak") instead of carrying a magic number that says nothing.
    // Verified against ids captured live: wait 0x866C7489, gimmickon
    // 0x150B14D0, break 0x353C1CAD, onbreak 0xFD8F7D2C.
    uint32_t NameId(const char* lowercaseName);

    // Runs every pattern scan (seconds; call from a worker thread). Returns
    // true when the required set resolved: event functions, both globals and
    // at least one pump.
    bool ResolveAll();
    const Fns& F();
    int SigCount();
    const SigResult& Sig(int i);
    bool RequiredOk();

    // --- actor manager ------------------------------------------------------
    // The one ClientActorManager, found by RTTI class name and then by the
    // global that points at it. 0 until the world exists; retries every 3 s.
    uintptr_t ActorManager();
    bool ActorManagerFound();
    // Address of the global the manager pointer is read from, for the log.
    uintptr_t ActorManagerSlot();

    // --- entities -----------------------------------------------------------
    inline constexpr uint8_t kTagPlayer = 0xA0;
    inline constexpr uint8_t kTagWorld  = 0xB0;
    // The actor the game itself treats as the one being played, read from its
    // own global rather than guessed at. 0 when it cannot be followed.
    uintptr_t LocalPlayer();

    bool     Eid(uintptr_t e, uint32_t* out);
    uint32_t Route(uintptr_t e);
    uint8_t  TypeTag(uintptr_t e);              // 0xFF when unreadable; 6 item, 7 plant/ore
    uintptr_t Comps(uintptr_t e);               // component slot array
    uintptr_t CompByClass(uintptr_t comps, const char* cls); // RTTI name contains cls
    uintptr_t Transform(uintptr_t comps);
    bool     WorldPos(uintptr_t e, Vec3* out);  // parent-relative position resolved to world
    uint32_t ParentEid(uintptr_t e);            // 0 when none

    // --- inventory ----------------------------------------------------------
    void InventoryRefresh(uintptr_t player, bool force);
    bool InventoryHas(uint32_t instanceId);
    int  InventoryCount();
    // Slots across every bucket the reader can see, 0 when it cannot tell.
    int  InventoryCapacity();
    // How full the carried bag is. False when the fields do not read sanely,
    // in which case there is nothing to report and the behaviour check is all
    // there is.
    bool BagSlots(int* used, int* cap);
    // Samples the inventory holder. This is how the two fields above were
    // found; keep it for doing the same again after a game patch.
    void DumpInventoryShape(uintptr_t me, bool bagFull);
    // Quantity per item type id across every storage, sorted by type. Returns
    // the number of entries copied.
    int  InventoryTypes(uint16_t* types, long long* qty, int max);

    // --- tables -------------------------------------------------------------
    // Probes the live iteminfo table against our item database to learn the
    // def-array offset and whether the runtime type id is our row index.
    // State: 0 not probed, 1 rows verified, 2 names readable but rows differ,
    // -1 unavailable. Call after ItemDb::Load and once the game has loaded.
    int  ProbeItemTable();
    int  ItemTableState();
    uint32_t ItemTableCount();
    bool ItemKeyForType(uint16_t typeId, char* out, size_t n);
    bool GimmickKeyForType(uint16_t typeId, char* out, size_t n);
    bool NodeName(uintptr_t gimmickComp, char* out, size_t n);
    // The path of the prefab the node was placed from, e.g.
    // ".../collect/socket/gimmick_socket_collection_peony_01.prefab".
    bool NodePrefab(uintptr_t gimmickComp, char* out, size_t n);

    // Every static table in the image, found by the shared index prologue.
    // `name` is the table's own name string when one is reachable from the
    // resolver, otherwise empty. Scanned once and cached.
    struct TableRef { uintptr_t global; uint32_t count; char name[32]; };
    int  EnumTables(const TableRef** out);
    // Reads the string key of one row, trying both def-array offsets.
    bool KeyInTable(uintptr_t global, uint32_t row, char* out, size_t n);
}
