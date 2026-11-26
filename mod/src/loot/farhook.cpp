#include "farhook.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <cstdio>
#include <cstring>

extern "C" {
#include <hde64.h>
}

namespace ml::farhook
{
    struct Entry { uintptr_t target; unsigned stolen; unsigned char orig[32]; };
    static Entry g_entries[24];
    static int   g_n = 0;
    static unsigned char* g_page = nullptr;
    static unsigned g_used = 0;

    // The loot hooks go in from the engine thread and the swapchain creation
    // watch in dx12_hook.cpp can reclaim its detour from a thread of its own,
    // and with no frame both run inside the same few seconds. Everything
    // below shares one page, one table and one count, so the public calls
    // take this first.
    static SRWLOCK g_lock = SRWLOCK_INIT;
    struct Held
    {
        Held() { AcquireSRWLockExclusive(&g_lock); }
        ~Held() { ReleaseSRWLockExclusive(&g_lock); }
    };

    static unsigned char* Alloc(unsigned n)
    {
        if (!g_page || g_used + n > 4096)
        {
            g_page = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            g_used = 0;
            if (!g_page) return nullptr;
        }
        unsigned char* p = g_page + g_used;
        g_used += (n + 15) & ~15u;
        return p;
    }

    // Bytes to steal: whole instructions totalling at least 12, none of them
    // rip-relative (a jump, call or memory operand that would point elsewhere
    // once moved), none ending the function early.
    static unsigned Measure(uintptr_t target, char* why, unsigned whyLen)
    {
        unsigned len = 0;
        while (len < 12)
        {
            hde64s hs;
            const unsigned l = hde64_disasm(reinterpret_cast<const void*>(target + len), &hs);
            if (hs.flags & F_ERROR) { snprintf(why, whyLen, "undecodable instruction at +%u", len); return 0; }
            if (hs.flags & F_RELATIVE) { snprintf(why, whyLen, "relative branch at +%u", len); return 0; }
            if ((hs.flags & F_MODRM) && hs.modrm_mod == 0 && hs.modrm_rm == 5) { snprintf(why, whyLen, "rip-relative operand at +%u", len); return 0; }
            if (hs.opcode == 0xC3 || hs.opcode == 0xC2 || hs.opcode == 0xCC) { snprintf(why, whyLen, "function ends before 12 bytes"); return 0; }
            len += l;
            if (len > 31) { snprintf(why, whyLen, "prologue too long"); return 0; }
        }
        return len;
    }

    // Nothing but OpenThread, SuspendThread, GetThreadContext and ResumeThread
    // may run while another thread is suspended. Any call that can take a
    // lock can wait on a thread this one has just frozen, and then neither
    // ever moves. CloseHandle is the one that did it: OptiScaler's d3d12.dll
    // detours it and takes a mutex inside, and its input thread holds that
    // mutex during startup, so a patch that closed the snapshot or a thread
    // handle with that thread frozen hung the game for good (hasewo2010, from
    // a full dump, issue #241). So every thread id is read and the snapshot
    // closed before the first suspend, and every handle is closed only once
    // every thread is running again.
    struct Frozen
    {
        HANDLE held[512];    // suspended, to resume
        int    n = 0;
        HANDLE spare[512];   // opened, never suspended, to close afterwards
        int    nSpare = 0;
    };
    static volatile LONG g_lastFrozen = 0, g_lastClosed = 0;

    // Suspends every other thread. Sets *inside while one of them sits inside
    // the bytes about to change, so the caller can resume and retry.
    static void SuspendOthers(Frozen& f, uintptr_t lo, uintptr_t hi, bool* inside)
    {
        *inside = false;
        f.n = f.nSpare = 0;
        const DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
        static DWORD ids[1024];   // only touched under g_lock
        int nIds = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) return;
        THREADENTRY32 te; te.dwSize = sizeof te;
        if (Thread32First(snap, &te))
        {
            do
            {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == me || nIds >= 1024) continue;
                ids[nIds++] = te.th32ThreadID;
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
        for (int i = 0; i < nIds && f.n < 512 && f.nSpare < 512; ++i)
        {
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, ids[i]);
            if (!h) continue;
            if (SuspendThread(h) == static_cast<DWORD>(-1)) { f.spare[f.nSpare++] = h; continue; }
            CONTEXT ctx; ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(h, &ctx) && ctx.Rip >= lo && ctx.Rip < hi) *inside = true;
            f.held[f.n++] = h;
        }
    }
    static void ResumeAll(Frozen& f)
    {
        for (int i = 0; i < f.n; ++i) ResumeThread(f.held[i]);
        // Every thread runs again before the first handle is closed.
        for (int i = 0; i < f.n; ++i) CloseHandle(f.held[i]);
        for (int i = 0; i < f.nSpare; ++i) CloseHandle(f.spare[i]);
        InterlockedExchange(&g_lastFrozen, f.n);
        InterlockedExchange(&g_lastClosed, f.n + f.nSpare);
        f.n = f.nSpare = 0;
    }

    int LastFrozen() { return static_cast<int>(g_lastFrozen); }
    int LastClosed() { return static_cast<int>(g_lastClosed); }

    // Fails rather than patch under a thread that keeps sitting inside the
    // bytes: a thread resumed halfway through a rewritten instruction crashes
    // the game, while a missing hook only costs a fallback or one feature.
    static bool WriteCode(uintptr_t dst, const void* src, unsigned n, char* why, unsigned whyLen)
    {
        DWORD old;
        if (!VirtualProtect(reinterpret_cast<void*>(dst), n, PAGE_EXECUTE_READWRITE, &old)) { snprintf(why, whyLen, "VirtualProtect failed"); return false; }
        static Frozen frozen;   // 8 KB, only touched under g_lock
        bool inside = false;
        for (int attempt = 0; attempt < 40; ++attempt)
        {
            SuspendOthers(frozen, dst, dst + n, &inside);
            if (!inside) break;
            ResumeAll(frozen);
            Sleep(1);
        }
        if (inside)
        {
            VirtualProtect(reinterpret_cast<void*>(dst), n, old, &old);
            snprintf(why, whyLen, "a thread stayed inside the prologue");
            return false;
        }
        memcpy(reinterpret_cast<void*>(dst), src, n);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(dst), n);
        ResumeAll(frozen);
        VirtualProtect(reinterpret_cast<void*>(dst), n, old, &old);
        return true;
    }

    bool Install(const char* name, uintptr_t target, void* detour, void** original, char* why, unsigned whyLen)
    {
        Held held;
        (void)name;
        why[0] = 0;
        if (!target) { snprintf(why, whyLen, "no target"); return false; }
        if (g_n >= 24) { snprintf(why, whyLen, "hook table full"); return false; }
        const unsigned stolen = Measure(target, why, whyLen);
        if (!stolen) return false;

        // Trampoline: the stolen bytes, then `jmp [rip+0]` to the rest of the
        // function. That form keeps every register, which matters for entries
        // like `mov rax, rsp` whose value the function still needs.
        unsigned char* tramp = Alloc(stolen + 14);
        if (!tramp) { snprintf(why, whyLen, "trampoline page allocation failed"); return false; }
        memcpy(tramp, reinterpret_cast<const void*>(target), stolen);
        unsigned char* p = tramp + stolen;
        p[0] = 0xFF; p[1] = 0x25; p[2] = p[3] = p[4] = p[5] = 0;
        const uintptr_t back = target + stolen;
        memcpy(p + 6, &back, 8);

        // Entry patch: `mov rax, detour; jmp rax`, padded with int3. rax is
        // volatile and carries nothing at a call boundary.
        unsigned char patch[32];
        memset(patch, 0xCC, sizeof patch);
        patch[0] = 0x48; patch[1] = 0xB8;
        memcpy(patch + 2, &detour, 8);
        patch[10] = 0xFF; patch[11] = 0xE0;

        Entry& e = g_entries[g_n];
        e.target = target; e.stolen = stolen;
        memcpy(e.orig, reinterpret_cast<const void*>(target), stolen);

        // Publish the trampoline before the entry patch, never after.
        //
        // The patch is what sends callers to the detour, and every detour calls
        // its original straight away: hkMove opens with oMove(...) and nothing
        // checks it first. Writing the patch first leaves a window, a few
        // instructions wide, in which a game thread already inside the process
        // can enter the detour and call through a null pointer. The movement
        // tick runs thousands of times a second and these hooks go in during
        // world load, so the window is small but it is aimed at a firehose.
        //
        // Publishing early is safe in a way publishing late is not: the
        // trampoline is complete here, and until the patch lands nothing can
        // reach the detour that would use it.
        *original = tramp;
        if (!WriteCode(target, patch, stolen, why, whyLen)) { *original = nullptr; return false; }
        ++g_n;
        return true;
    }

    // Sixteen bytes of int3 padding in the game's code, 16-aligned, inside a
    // run of at least 48, so the block sits between functions and no
    // instruction's immediate can be what it is made of. Each block is handed
    // out once, and the middle of the run is taken so a neighbour's own use of
    // the edge is left alone.
    static uintptr_t g_caves[8];
    static int g_caveN = 0;
    static uintptr_t FindCave(uintptr_t from)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec)
        {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            const auto* p = reinterpret_cast<const unsigned char*>(base + sec->VirtualAddress);
            const size_t n = sec->Misc.VirtualSize;
            size_t run = 0;
            for (size_t i = 0; i < n; ++i)
            {
                if (p[i] != 0xCC) { run = 0; continue; }
                if (++run < 48) continue;
                // i is the 48th int3 of a run; take the 16-aligned block in its middle.
                const uintptr_t start = (reinterpret_cast<uintptr_t>(p + i - 47) + 16 + 15) & ~static_cast<uintptr_t>(15);
                bool used = false;
                for (int c = 0; c < g_caveN; ++c) if (g_caves[c] == start) used = true;
                const int64_t d = static_cast<int64_t>(start) - static_cast<int64_t>(from);
                if (used || d > 0x7FFF0000LL || d < -0x7FFF0000LL) { run = 0; continue; }
                return start;
            }
        }
        return 0;
    }

    bool InstallOverJump(const char* name, uintptr_t target, void* detour, void** original, char* why, unsigned whyLen)
    {
        Held held;
        (void)name;
        why[0] = 0;
        if (!target) { snprintf(why, whyLen, "no target"); return false; }
        if (g_n >= 24 || g_caveN >= 8) { snprintf(why, whyLen, "hook table full"); return false; }
        const auto* at = reinterpret_cast<const unsigned char*>(target);
        if (at[0] != 0xE9) { snprintf(why, whyLen, "the entry is not a jmp rel32"); return false; }
        int32_t rel;
        memcpy(&rel, at + 1, 4);
        const uintptr_t theirs = target + 5 + static_cast<int64_t>(rel);

        // The original: straight on to the other mod's detour.
        unsigned char* tramp = Alloc(14);
        if (!tramp) { snprintf(why, whyLen, "trampoline page allocation failed"); return false; }
        tramp[0] = 0xFF; tramp[1] = 0x25; tramp[2] = tramp[3] = tramp[4] = tramp[5] = 0;
        memcpy(tramp + 6, &theirs, 8);

        const uintptr_t cave = FindCave(target + 5);
        if (!cave) { snprintf(why, whyLen, "no int3 padding in reach for a relay"); return false; }
        unsigned char relay[16];
        memset(relay, 0xCC, sizeof relay);
        relay[0] = 0xFF; relay[1] = 0x25; relay[2] = relay[3] = relay[4] = relay[5] = 0;
        memcpy(relay + 6, &detour, 8);
        // Nothing jumps into padding, so the relay can be written before the
        // entry points at it; the same order as Install, trampoline first.
        *original = tramp;
        if (!WriteCode(cave, relay, sizeof relay, why, whyLen)) { *original = nullptr; return false; }
        g_caves[g_caveN++] = cave;

        unsigned char entry[5];
        entry[0] = 0xE9;
        const int32_t toCave = static_cast<int32_t>(static_cast<int64_t>(cave) - static_cast<int64_t>(target + 5));
        memcpy(entry + 1, &toCave, 4);
        Entry& e = g_entries[g_n];
        e.target = target; e.stolen = 5;
        memcpy(e.orig, at, 5);
        if (!WriteCode(target, entry, 5, why, whyLen)) { *original = nullptr; return false; }
        ++g_n;
        return true;
    }

    // `mov rax, imm64; jmp rax` keeps the address at +2; `jmp [rip+0]` at +6.
    static unsigned AbsJumpAt(const unsigned char* at)
    {
        if (at[0] == 0x48 && at[1] == 0xB8 && at[10] == 0xFF && at[11] == 0xE0) return 2;
        if (at[0] == 0xFF && at[1] == 0x25 && at[2] == 0 && at[3] == 0 && at[4] == 0 && at[5] == 0) return 6;
        return 0;
    }

    uintptr_t AbsJumpTarget(uintptr_t target)
    {
        if (!target) return 0;
        __try
        {
            const auto* at = reinterpret_cast<const unsigned char*>(target);
            const unsigned off = AbsJumpAt(at);
            if (!off) return 0;
            uintptr_t to;
            memcpy(&to, at + off, 8);
            return to;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    bool InstallOverAbsJump(const char* name, uintptr_t target, void* detour, void** original, char* why, unsigned whyLen)
    {
        Held held;
        (void)name;
        why[0] = 0;
        if (!target) { snprintf(why, whyLen, "no target"); return false; }
        if (g_n >= 24) { snprintf(why, whyLen, "hook table full"); return false; }
        const auto* at = reinterpret_cast<const unsigned char*>(target);
        const unsigned off = AbsJumpAt(at);
        if (!off) { snprintf(why, whyLen, "the entry is not an absolute jump"); return false; }
        uintptr_t theirs;
        memcpy(&theirs, at + off, 8);

        // The original: straight on to where the other mod's patch went.
        unsigned char* tramp = Alloc(14);
        if (!tramp) { snprintf(why, whyLen, "trampoline page allocation failed"); return false; }
        tramp[0] = 0xFF; tramp[1] = 0x25; tramp[2] = tramp[3] = tramp[4] = tramp[5] = 0;
        memcpy(tramp + 6, &theirs, 8);

        // Trampoline first, as in Install. The eight bytes are the operand of
        // one instruction and no thread can be stopped inside them, so a
        // thread before the write takes ours and one past it takes theirs.
        Entry& e = g_entries[g_n];
        e.target = target + off; e.stolen = 8;
        memcpy(e.orig, at + off, 8);
        *original = tramp;
        if (!WriteCode(target + off, &detour, 8, why, whyLen)) { *original = nullptr; return false; }
        ++g_n;
        return true;
    }

    bool InstallCallSite(const char* name, uintptr_t site, void* detour, void** original, char* why, unsigned whyLen)
    {
        Held held;
        (void)name;
        why[0] = 0;
        if (!site) { snprintf(why, whyLen, "no call site"); return false; }
        if (g_n >= 24 || g_caveN >= 8) { snprintf(why, whyLen, "hook table full"); return false; }
        const auto* at = reinterpret_cast<const unsigned char*>(site);
        if (at[0] != 0xE8) { snprintf(why, whyLen, "the site is not a call rel32"); return false; }
        int32_t rel;
        memcpy(&rel, at + 1, 4);
        const uintptr_t callee = site + 5 + static_cast<int64_t>(rel);

        const uintptr_t cave = FindCave(site + 5);
        if (!cave) { snprintf(why, whyLen, "no int3 padding in reach for a relay"); return false; }
        unsigned char relay[16];
        memset(relay, 0xCC, sizeof relay);
        relay[0] = 0xFF; relay[1] = 0x25; relay[2] = relay[3] = relay[4] = relay[5] = 0;
        memcpy(relay + 6, &detour, 8);
        // The callee is the original and needs no trampoline: nothing of it
        // is overwritten. Published before the call is pointed at the relay.
        *original = reinterpret_cast<void*>(callee);
        if (!WriteCode(cave, relay, sizeof relay, why, whyLen)) { *original = nullptr; return false; }
        g_caves[g_caveN++] = cave;

        // Only the operand changes. A thread cannot be stopped halfway through
        // one instruction, so it calls either the old target or the relay.
        const int32_t toCave = static_cast<int32_t>(static_cast<int64_t>(cave) - static_cast<int64_t>(site + 5));
        Entry& e = g_entries[g_n];
        e.target = site + 1; e.stolen = 4;
        memcpy(e.orig, at + 1, 4);
        if (!WriteCode(site + 1, &toCave, 4, why, whyLen)) { *original = nullptr; return false; }
        ++g_n;
        return true;
    }

    void RemoveAll()
    {
        Held held;
        char why[64];
        for (int i = g_n - 1; i >= 0; --i) WriteCode(g_entries[i].target, g_entries[i].orig, g_entries[i].stolen, why, sizeof why);
        g_n = 0;
        // The trampoline page stays: a game thread may still be running through it.
    }
}
