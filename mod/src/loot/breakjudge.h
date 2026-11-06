#pragma once
#include <cstdint>

// How a break the mod drove at an ore node is judged. Kept apart from the
// engine so the timing can be checked without a game (tests/ml_tests.cpp).
namespace ml::loot
{
    // A vein drops its ore within a frame or two of the break running.
    constexpr uint32_t kBreakAnswerMs = 1500;
    // A break that never ran inside this long was dropped from the queue or
    // refused because its area unloaded. That says nothing about the prefab.
    constexpr uint32_t kBreakNeverRanMs = 10000;
    // A prefab learned as not a vein is taken back if its drop still turns up
    // inside this window, counted from the verdict.
    constexpr uint32_t kLateDropMs = 15000;

    enum class BreakVerdict { Wait, Vein, NotVein, NeverRan };

    // now, queued and ran are GetTickCount values. ran is 0 when the break has
    // not run on the game thread yet. The silence is timed from ran: a queued
    // break waits for the next game tick, and in LuxDragon's 28 October 2026
    // log that tick came 1712 ms after the queue, so a clock started at the
    // queue judged two copper veins silent 134 ms before they dropped.
    inline BreakVerdict JudgeBreak(uint32_t now, uint32_t queued, uint32_t ran, bool dropSeen)
    {
        // A time from an earlier break of the same entity is not this one's.
        if (ran && static_cast<int32_t>(ran - queued) < 0) ran = 0;
        if (dropSeen) return BreakVerdict::Vein;
        if (!ran) return now - queued < kBreakNeverRanMs ? BreakVerdict::Wait : BreakVerdict::NeverRan;
        return now - ran < kBreakAnswerMs ? BreakVerdict::Wait : BreakVerdict::NotVein;
    }
}
