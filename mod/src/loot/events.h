#pragma once
#include <cstdint>

// The game's own loot events, built and queued exactly as the game does it.
// Building and sending must happen on the game thread (the event allocator is
// thread-local and the queue is drained there), so anything decided elsewhere
// is queued and drained by the pump hook.
namespace ml::events
{
    enum class Action : int { Search = 0, Take = 1, Catch = 2, Gather = 3 };
    const char* ActionName(Action a);

    // Called by the pump hook: remembers the game thread and, once, runs the
    // self test that resolves descriptors and permits sending.
    void OnGameThreadTick();
    bool OnGameThread();

    bool SendAllowed();
    int  DescriptorsFound();   // out of 3
    bool SelfTested();
    bool RouteKnown();
    uint32_t Route();          // learned from the game's own events; 0 until seen

    // Break a vein by driving the game's own state machine instead: the swing
    // landing, then the break. The transition is what drops the ore, so this
    // replaces BreakGimmick rather than joining it. Queued off-thread like the
    // rest; the two events go out back to back on the game thread.
    // Drive one named transition at one gimmick. DriveBreak is the vein's pair
    // of these; a well needs a longer sequence and the ids for it are numbers
    // rather than names, since eight of them appear nowhere in the game's
    // strings. A gimmick whose chart has no transition for an id ignores it.
    //
    // targetEnt is the entity the component was read from. Just before the
    // drive runs on the game thread, that entity has to still carry targetEid
    // and still own gimmickComp, or the drive is refused: a queued drive can
    // outlive the area it was aimed at.
    bool DriveEvent(uintptr_t gimmickComp, uint32_t eventId, uint32_t playerEid,
                    uintptr_t playerActor, uint32_t targetEid, uintptr_t targetEnt);

    bool DriveBreak(uintptr_t gimmickComp, uint32_t playerEid, uintptr_t playerActor,
                    uint32_t targetEid, uintptr_t targetEnt, float x, float y, float z);

    // Send (or queue when off the game thread). Returns false when refused.
    // A search given a skin interaction and a character key is followed on the
    // game thread by the reward a hand skin raises, which is where the game
    // grants the creature's knowledge. Issue #70.
    bool Send(Action a, uint32_t targetEid, uint32_t playerEid, uint32_t route, uint8_t mode,
              uint32_t skinInteraction = 0, uint32_t skinCharacter = 0);
    // Whether this mod sent a search for that body in the last ten seconds.
    // Safe from any thread; the server's parse of a search asks it.
    bool SearchedRecently(uint32_t targetEid);
    // The same for a body a pet or a companion searched, as the event queue
    // saw it raised under that companion's own id.
    bool CompanionSearchedRecently(uint32_t targetEid);
    // Remove `amount` of one inventory slot's stack through the game's own
    // TrocTrDeleteItemFromInventoryOnceTimer. The payload names the slot's
    // instance (its u64), the inventory by a table key, and the slot index,
    // so it cannot touch anything but the stack it was aimed at. Queued for
    // the game thread. Issue #32.
    bool DeleteItem(uint64_t instance, uint16_t typeKey, uint16_t slot, uint64_t amount, uint16_t count, uint32_t player, uint32_t route);
    unsigned long LastDeleteSentAt();   // GetTickCount of the last delete that left, 0 if none

    // A pick up or a body search raised by something that is not the player:
    // a pet, a companion. A dog looting a corpse raises the search, the same
    // 0x07E8 this mod sends for a carcass, and never a pick up. The engine
    // drains these to judge what lands.
    struct PetPickup { uint32_t pet, item; unsigned long at; bool search; };
    int  DrainPetPickups(PetPickup* out, int max);
    // GetTickCount of the last event raised by anyone but the player:
    // a pet, a mercenary, a companion. Zero until one acts.
    uint32_t CompanionActiveAt();
    // The same for a hired mercenary alone: player-tagged like you, with its
    // own id. Nobody has seen whether a mercenary asks the pet-looting
    // question, so while one is out the filter still watches the bag.
    uint32_t MercenaryActiveAt();
    // GetTickCount of the last body search raised by a pet or a companion.
    uint32_t PetSearchAt();
    // Arm a gimmick node so the game fills its interaction data. Queued off-thread.
    bool Arm(uintptr_t gimmickComp, uintptr_t mode, uintptr_t arg3, uintptr_t ctx); // arg3 0 = a zeroed scratch buffer
    // Game thread only: run queued sends and arms.
    void Drain();
    // Throw away everything queued and say how many went. For a world change:
    // a queued arm or drive holds a raw component pointer captured before it,
    // and the game frees those across a teleport. Issue #35.
    int DropPending();
    // How much is queued, right now, without taking the lock. For the crash
    // handler, which runs on whatever thread faulted and may be holding it.
    // A torn read here costs a wrong number in one log line; a deadlock in an
    // exception handler costs the report.
    void PendingCounts(int* act, int* arm, int* drive);
    // Enqueue hook feeds every event here to learn the player's route id.
    void SpyEnqueue(uintptr_t ev);

    long SentCount();
    long QueuedCount();

    // The player's own interactions, seen on the game's event queue (not ours):
    // gather/pick-up and catch targets. The engine drains these to learn what
    // a node yields from what the player does by hand.
    struct Seen { uint32_t eid; Action act; unsigned long at; };
    int  DrainSeen(Seen* out, int max);
}
