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
        uintptr_t invHolder = 0;     // GetInventoryHolder(actor), called on the game thread
        uintptr_t descMask = 0, queue = 0;                  // globals
        uintptr_t itemTableGlobal = 0, gimmickTableGlobal = 0;
        uintptr_t controlGlobal = 0;                        // see ControlledActor
        unsigned  ctlController = 0, ctlActor = 0;
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
    // A unique match for a pattern that starts at a function entry, found
    // under another mod's jump over that entry when it is not found plain.
    uintptr_t FindEntry(const char* pattern, size_t* hits);
    uintptr_t ActorManager();
    // The actor the game's controller is driving, by the chain Crimson Route
    // uses: services global, +0x10, the controller, its actor. Read without
    // calling the accessor, every hop guarded. 0 when any hop is missing.
    uintptr_t ControlledActor();
    bool ActorManagerFound();
    // Address of the global the manager pointer is read from, for the log.
    uintptr_t ActorManagerSlot();

    // --- entities -----------------------------------------------------------
    inline constexpr uint8_t kTagPlayer = 0xA0;
    inline constexpr uint8_t kTagWorld  = 0xB0;
    // The holder of the bag an actor uses, as the game's own GetInventoryHolder
    // last answered for it on the game thread, or the actor's own holder when
    // there is no answer for that actor yet. Every inventory read goes through
    // this. RefreshHolder is the game-thread half and must only run there.
    uintptr_t Holder(uintptr_t actor);
    void      RefreshHolder(uintptr_t actor);
    // Drops the cached answer. It is a raw game pointer, so it goes in the
    // world-change purge with the others.
    void      ForgetHolder();
    // The holder the last InventoryRefresh read from, 0 when it read nothing.
    // A change means every sample taken before it was of a different bag.
    uintptr_t LastInventoryHolder();

    bool     Eid(uintptr_t e, uint32_t* out);
    uint32_t Route(uintptr_t e);
    uint8_t  TypeTag(uintptr_t e);              // 0xFF when unreadable; 6 item, 7 plant/ore
    uintptr_t Comps(uintptr_t e);               // component slot array
    uintptr_t CompByClass(uintptr_t comps, const char* cls); // RTTI name contains cls
    uint8_t  Cat2(uintptr_t e);                 // status category byte; 0x11 worn item, 0 when unreadable
    uintptr_t Transform(uintptr_t comps);
    bool     WorldPos(uintptr_t e, Vec3* out);  // parent-relative position resolved to world
    uint32_t ParentEid(uintptr_t e);            // 0 when none

    // --- inventory ----------------------------------------------------------
    void InventoryRefresh(uintptr_t player, bool force);
    bool InventoryHas(uint32_t instanceId);
    struct InvEntry { uint32_t iid; uint16_t tid; int bucket, slot; long long count; uintptr_t addr; };
    // A fresh walk, every bucket. holderOut, when given, is the holder this
    // walk read, from the same lookup, so a caller never labels a sample with
    // a holder looked up separately and published over in between.
    int  InventoryEntries(uintptr_t player, InvEntry* out, int max, uintptr_t* holderOut = nullptr);
    // The same walk over a holder given outright, for the server side's own
    // holder, which is a different object from the client's. DROP.md.
    int  HolderEntries(uintptr_t holder, InvEntry* out, int max);
    // GetInventoryHolder called now, on this thread. The answer depends on
    // the thread: the server thread gets the server's holder.
    uintptr_t HolderNow(uintptr_t actor);
    // Issue #32 probe. Every inventory on the holder with the u16 at +0x10
    // that the delete matches its key's type against.
    struct BucketInfo { uintptr_t addr; uint16_t type, slotN, capC, used, cap; uint32_t exclN; };
    int  InventoryBuckets(uintptr_t player, BucketInfo* out, int max);
    int  HolderBuckets(uintptr_t holder, BucketInfo* out, int max);
    // The inventory-type table, found among the static tables by its first
    // two row keys, Money and Character: rows by index, and the key a payload
    // carries turned into the row the buckets name (0xFFFF on a miss).
    struct InvTypeRow { uint16_t id, idx; char name[40]; };
    int      InvTypeRows(InvTypeRow* out, int max, uint32_t* countOut);
    uint16_t InvTypeLookup(uint16_t key);
    int      InvTypeKeysFor(uint16_t value, uint16_t* out, int max);   // the key of the type at row `value`
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
    // The same, counted in the carried bag alone. -1 when the last refresh could
    // not read the bag.
    int  BagTypes(uint16_t* types, long long* qty, int max);

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
    // The row of the named table whose string key is `key`, or -1 when the
    // table is not among EnumTables or no row carries that key.
    int  RowByKey(const char* table, const char* key);
    // The characterinfo table, resolved once. Several static tables carry that
    // name and only one is the real one, so this takes the largest: the real
    // one has 7250 rows and the impostor that cost a probe round has four.
    // Returns 0 when it cannot be found.
    uintptr_t CharacterInfoTable(uint32_t* rowsOut);
}
