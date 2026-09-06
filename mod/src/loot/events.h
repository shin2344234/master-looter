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

    // Send (or queue when off the game thread). Returns false when refused.
    bool Send(Action a, uint32_t targetEid, uint32_t playerEid, uint32_t route, uint8_t mode);
    // Arm a gimmick node so the game fills its interaction data. Queued off-thread.
    bool Arm(uintptr_t gimmickComp, uintptr_t mode, uintptr_t ctx);
    // Game thread only: run queued sends and arms.
    void Drain();
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
