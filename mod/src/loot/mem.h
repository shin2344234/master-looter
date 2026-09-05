#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>

// Guarded access to the game's memory, pattern scanning over the main module,
// and the MSVC RTTI walk. Everything that touches game memory in the loot
// engine goes through here so a stale pointer costs a skipped object, never a
// crash. Locals in the SEH functions stay POD.
namespace ml::mem
{
    struct Module { uintptr_t base = 0; size_t size = 0; };
    const Module& Game();                       // main executable, resolved once
    bool InImage(uintptr_t p);
    inline uintptr_t Rva(uintptr_t p) { return p ? p - Game().base : 0; }

    // Plausible user-mode pointer: above the null page, below the canonical limit.
    inline bool Plausible(uintptr_t a) { return a >= 0x10000 && (a >> 47) == 0; }

    // Committed + readable, via VirtualQuery with a small region cache. Use it
    // to vet pointers pulled out of large arrays where garbage is common (an
    // exception storm is far costlier than a cached query); the SEH readers
    // below are the second line of defence.
    bool Readable(uintptr_t p, size_t n);
    long FaultCount();

    bool Read8 (uintptr_t a, uint8_t*  out);
    bool Read16(uintptr_t a, uint16_t* out);
    bool Read32(uintptr_t a, uint32_t* out);
    bool Read64(uintptr_t a, uint64_t* out);
    bool ReadPtr(uintptr_t a, uintptr_t* out);  // also requires the value to be plausible
    bool ReadF32x3(uintptr_t a, float* out);
    bool ReadBytes(uintptr_t a, void* out, size_t n);
    // Printable ASCII C string, at most n-1 chars; false when not a string.
    bool ReadCString(uintptr_t a, char* out, size_t n);
    // Engine string: slot -> string object -> first qword = char*.
    bool ReadEngineString(uintptr_t slot, char* out, size_t n);
    // Pointer chain helper: *(p + off), plausible and readable.
    uintptr_t Deref(uintptr_t p, unsigned off);

    // IDA-style pattern ("48 8B ?? 05"). Scans the module's committed readable
    // spans. Find returns the first hit or 0; Count stops at maxCount.
    uintptr_t Find(const char* pattern);
    size_t    Count(const char* pattern, size_t maxCount = 8);
    // First hit for which visit() returns true, or 0.
    uintptr_t FindIf(const char* pattern, bool (*visit)(uintptr_t hit, void* ctx), void* ctx);
    // Unique hit or 0 (logs nothing; callers log).
    uintptr_t FindUnique(const char* pattern, size_t* hitsOut = nullptr);
    // Does the pattern match at exactly this address (guarded read)?
    bool MatchAt(uintptr_t addr, const char* pattern);
    // Resolve a rip-relative operand at the end of an instrLen-byte instruction.
    uintptr_t RipAt(uintptr_t instr, unsigned instrLen);

    // MSVC x64 RTTI: object -> vtable -> COL -> TypeDescriptor name (".?AV...").
    // Returns a pointer into the image or nullptr. RttiShort strips ".?AV".
    const char* RttiName(uintptr_t obj);
    const char* RttiShort(uintptr_t obj);
    // All vtables whose COL names exactly `decoratedName`; returns the count.
    int FindVtablesByName(const char* decoratedName, uintptr_t* out, int max);
    // Scan writable, non-executable sections for a qword pointing at an object
    // whose first qword is one of the vtables. Returns the global's address or 0.
    uintptr_t FindGlobalHoldingVtable(const uintptr_t* vtables, int n, long* candidates = nullptr);
}
