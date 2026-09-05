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
        uintptr_t descMask = 0, queue = 0;                  // globals
        uintptr_t itemTableGlobal = 0, gimmickTableGlobal = 0;
    };

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

    // --- entities -----------------------------------------------------------
    inline constexpr uint8_t kTagPlayer = 0xA0;
    inline constexpr uint8_t kTagWorld  = 0xB0;
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
}
