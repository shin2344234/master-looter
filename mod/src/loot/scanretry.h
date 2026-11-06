#pragma once
#include <cstdint>

// How long the scan waits after a fault before it runs again. The first fault,
// or one ten clean minutes after the last, waits five seconds; each fault after
// that inside the ten minutes doubles the wait, up to five minutes.
namespace ml::loot
{
    constexpr uint32_t kScanRetryFirstMs = 5000;
    constexpr uint32_t kScanRetryMaxMs   = 300000;
    constexpr uint32_t kScanCleanMs      = 600000;

    // prevMs is the last wait, 0 before the first fault. sinceLast is the time
    // from the previous fault to this one, and is ignored when prevMs is 0.
    inline uint32_t NextScanRetryMs(uint32_t prevMs, uint32_t sinceLast)
    {
        if (!prevMs || sinceLast >= kScanCleanMs) return kScanRetryFirstMs;
        const uint32_t twice = prevMs * 2;
        return twice > kScanRetryMaxMs ? kScanRetryMaxMs : twice;
    }
}
