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

    // Suspends every other thread; refuses (returns false) while one of them
    // sits inside the bytes about to change, so the caller can retry.
    static int SuspendOthers(HANDLE* handles, int max, uintptr_t lo, uintptr_t hi, bool* inside)
    {
        *inside = false;
        int n = 0;
        const DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;
        THREADENTRY32 te; te.dwSize = sizeof te;
        if (Thread32First(snap, &te))
        {
            do
            {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == me || n >= max) continue;
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
                if (!h) continue;
                if (SuspendThread(h) == static_cast<DWORD>(-1)) { CloseHandle(h); continue; }
                CONTEXT ctx; ctx.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(h, &ctx) && ctx.Rip >= lo && ctx.Rip < hi) *inside = true;
                handles[n++] = h;
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
        return n;
    }
    static void ResumeAll(HANDLE* handles, int n)
    {
        for (int i = 0; i < n; ++i) { ResumeThread(handles[i]); CloseHandle(handles[i]); }
    }

    // Fails rather than patch under a thread that keeps sitting inside the
    // bytes: a thread resumed halfway through a rewritten instruction crashes
    // the game, while a missing hook only costs a fallback or one feature.
    static bool WriteCode(uintptr_t dst, const void* src, unsigned n, char* why, unsigned whyLen)
    {
        DWORD old;
        if (!VirtualProtect(reinterpret_cast<void*>(dst), n, PAGE_EXECUTE_READWRITE, &old)) { snprintf(why, whyLen, "VirtualProtect failed"); return false; }
        HANDLE handles[512]; bool inside = false; int cnt = 0;
        for (int attempt = 0; attempt < 40; ++attempt)
        {
            cnt = SuspendOthers(handles, 512, dst, dst + n, &inside);
            if (!inside) break;
            ResumeAll(handles, cnt);
            cnt = 0;
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
        ResumeAll(handles, cnt);
        VirtualProtect(reinterpret_cast<void*>(dst), n, old, &old);
        return true;
    }

    bool Install(const char* name, uintptr_t target, void* detour, void** original, char* why, unsigned whyLen)
    {
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

    void RemoveAll()
    {
        char why[64];
        for (int i = g_n - 1; i >= 0; --i) WriteCode(g_entries[i].target, g_entries[i].orig, g_entries[i].stolen, why, sizeof why);
        g_n = 0;
        // The trampoline page stays: a game thread may still be running through it.
    }
}
