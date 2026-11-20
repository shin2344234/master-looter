#pragma once
#include <cstdint>

// Whether a creature is a spawner's replacement for one the mod just caught.
// Some creatures come from a spawner that puts a fresh one on the same spot the
// moment the last is taken, so catching from range never runs out: 154 rats in
// a few minutes at the Hernand teleport, a new id at 50.5 m every 0.6 s, each
// one paid. Issue #232.
//
// A creature counts as the replacement when it first entered the scan list at
// or after the catch and no more than kRespawnMs after it, within kRespawnM of
// where the caught one was. The scan list reaches out to the scan range, well
// past catch range, so a creature that walked up to the spot, or was already
// standing there, was listed before the catch and never matches. Whether the
// caught one is still listed is not asked: the game can leave it in the list
// for a pass after taking it, and a test on that let the loop run on.
namespace ml::loot
{
    constexpr float    kRespawnM  = 4.0f;
    constexpr uint32_t kRespawnMs = 20000;

    inline bool IsRespawnOf(uint32_t eid, uint32_t listedAt, uint32_t now,
                            uint32_t caughtEid, uint32_t caughtAt, float dist2)
    {
        if (eid == caughtEid) return false;
        if (now - caughtAt > kRespawnMs) return false;
        if (static_cast<int32_t>(listedAt - caughtAt) < 0) return false;   // listed before that catch
        return dist2 < kRespawnM * kRespawnM;
    }
}
