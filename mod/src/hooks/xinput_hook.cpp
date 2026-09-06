// Adapted for Master Looter from Trinity (https://github.com/XeTrinityz/Trinity),
// MIT License, Copyright (c) 2026 XeTrinityz. See THIRD_PARTY_NOTICES.md.
// Changes: Master Looter namespaces, logger, settings and menu hooks; icon atlas code removed.
#include "xinput_hook.h"
#include <cstdio>

#include <MinHook.h>

#include "../core/state.h"

#pragma comment(lib, "xinput9_1_0.lib")

namespace ml::hooks
{
    using XInputGetState_t = DWORD (WINAPI*)(DWORD, XINPUT_STATE*);

    // One trampoline per known module. The game and our own overlay may link
    // different xinput versions, and each hook must forward to its own original.
    static XInputGetState_t o_1_4   = nullptr;
    static XInputGetState_t o_1_3   = nullptr;
    static XInputGetState_t o_9_1_0 = nullptr;

    // Any real-state trampoline, used by the menu to read the pad it's blocking.
    static XInputGetState_t g_read = nullptr;

    static void Neutralize(XINPUT_STATE* s)
    {
        const DWORD packet = s->dwPacketNumber;
        ZeroMemory(&s->Gamepad, sizeof s->Gamepad);
        s->dwPacketNumber = packet + 1;
    }

    // A distinct detour per module so each can call the matching trampoline
    // (MinHook can't tell us which target a shared detour was invoked for).
    #define ML_XINPUT_DETOUR(NAME, ORIG)                              \
        static DWORD WINAPI NAME(DWORD i, XINPUT_STATE* s)                 \
        {                                                                  \
            const DWORD r = ORIG(i, s);                                    \
            if (r == ERROR_SUCCESS && s && State::Get().Captures())       \
                Neutralize(s);                                             \
            return r;                                                      \
        }
    ML_XINPUT_DETOUR(hk_1_4,   o_1_4)
    ML_XINPUT_DETOUR(hk_1_3,   o_1_3)
    ML_XINPUT_DETOUR(hk_9_1_0, o_9_1_0)
    #undef ML_XINPUT_DETOUR

    struct Target
    {
        const wchar_t*    dll;
        void*             detour;
        XInputGetState_t* original;
        bool              done; // hooked, or confirmed nothing to hook here
    };

    static Target g_targets[] = {
        { L"xinput1_4.dll",   reinterpret_cast<void*>(&hk_1_4),   &o_1_4,   false },
        { L"xinput1_3.dll",   reinterpret_cast<void*>(&hk_1_3),   &o_1_3,   false },
        { L"xinput9_1_0.dll", reinterpret_cast<void*>(&hk_9_1_0), &o_9_1_0, false },
    };

    void EnsureXInputHooks()
    {
        static bool s_allDone = false;
        if (s_allDone)
            return;

        bool anyPending = false;
        for (auto& t : g_targets)
        {
            if (t.done)
                continue;

            HMODULE mod = GetModuleHandleW(t.dll);
            if (!mod)
            {
                anyPending = true; // may still load (game inits input after us)
                continue;
            }

            FARPROC proc = GetProcAddress(mod, "XInputGetState");
            if (!proc)
            {
                t.done = true; // module present but no export - never retry it
                continue;
            }

            if (MH_CreateHook(reinterpret_cast<void*>(proc), t.detour,
                              reinterpret_cast<void**>(t.original)) != MH_OK)
            {
                anyPending = true; // transient; try again next frame
                continue;
            }

            // Publish the reader before enabling so a concurrent XInputReadReal
            // never falls through to a now-patched plain export.
            if (!g_read)
                g_read = *t.original;

            if (MH_EnableHook(reinterpret_cast<void*>(proc)) == MH_OK)
            {
                t.done = true;
            }
            else
            {
                anyPending = true;
            }
        }

        if (!anyPending)
            s_allDone = true;
    }

    void RemoveXInputHooks()
    {
        // The detours are torn down by MH_DisableHook(MH_ALL_HOOKS) /
        // MH_Uninitialize during shutdown; just reset our bookkeeping.
        for (auto& t : g_targets)
        {
            t.done      = false;
            *t.original = nullptr;
        }
        g_read = nullptr;
    }

    DWORD XInputReadReal(DWORD userIndex, XINPUT_STATE* state)
    {
        if (g_read)
            return g_read(userIndex, state);
        return XInputGetState(userIndex, state); // hooks not up yet
    }

    // --- pad shortcuts ------------------------------------------------------
    // Reading a disconnected slot costs about a millisecond, so the pad that
    // answered last is asked first and the others are only swept occasionally.
    static DWORD g_padIndex = 0;
    static DWORD g_padProbeAt = 0;

    unsigned PadButtons()
    {
        XINPUT_STATE st{};
        if (XInputReadReal(g_padIndex, &st) == ERROR_SUCCESS) return st.Gamepad.wButtons;
        const DWORD now = GetTickCount();
        if (now - g_padProbeAt < 2000) return 0;
        g_padProbeAt = now;
        for (DWORD i = 0; i < 4; ++i)
        {
            if (i == g_padIndex) continue;
            if (XInputReadReal(i, &st) == ERROR_SUCCESS) { g_padIndex = i; return st.Gamepad.wButtons; }
        }
        return 0;
    }

    static int PopCount(unsigned v)
    {
        int n = 0;
        while (v) { v &= v - 1; ++n; }
        return n;
    }

    bool PadChordHeld(unsigned mask)
    {
        if (PopCount(mask) < 2) return false;
        const unsigned held = PadButtons();
        return (held & mask) == mask;
    }

    const char* PadChordName(unsigned mask)
    {
        static char name[96];
        name[0] = '\0';
        if (PopCount(mask) < 2) return "not set";
        static const struct { unsigned bit; const char* text; } kButtons[] = {
            { XINPUT_GAMEPAD_A, "A" }, { XINPUT_GAMEPAD_B, "B" },
            { XINPUT_GAMEPAD_X, "X" }, { XINPUT_GAMEPAD_Y, "Y" },
            { XINPUT_GAMEPAD_LEFT_SHOULDER, "LB" }, { XINPUT_GAMEPAD_RIGHT_SHOULDER, "RB" },
            { XINPUT_GAMEPAD_BACK, "Back" }, { XINPUT_GAMEPAD_START, "Start" },
            { XINPUT_GAMEPAD_LEFT_THUMB, "LS" }, { XINPUT_GAMEPAD_RIGHT_THUMB, "RS" },
            { XINPUT_GAMEPAD_DPAD_UP, "D-pad up" }, { XINPUT_GAMEPAD_DPAD_DOWN, "D-pad down" },
            { XINPUT_GAMEPAD_DPAD_LEFT, "D-pad left" }, { XINPUT_GAMEPAD_DPAD_RIGHT, "D-pad right" },
        };
        int w = 0;
        for (const auto& b : kButtons)
        {
            if (!(mask & b.bit)) continue;
            w += snprintf(name + w, sizeof name - w, "%s%s", w ? " + " : "", b.text);
        }
        return name[0] ? name : "not set";
    }
}
