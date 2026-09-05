#include "input.h"

#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_win32.h>

#include "../core/log.h"
#include "../core/state.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace ml::input
{
    static WNDPROC g_original = nullptr;
    static HWND    g_hwnd     = nullptr;

    // --- cursor API ---------------------------------------------------------
    // The game confines the cursor with ClipCursor and parks it with
    // SetCursorPos each frame. While the menu is open both are swallowed for
    // the game (the last requested clip and position are remembered and put
    // back on close), and GetCursorPos hands the game the parked position so
    // any camera code that polls it sees no movement. The render thread, which
    // is where ImGui reads the cursor, always gets the truth.
    typedef BOOL (WINAPI *FnClipCursor)(const RECT*);
    typedef BOOL (WINAPI *FnSetCursorPos)(int, int);
    typedef BOOL (WINAPI *FnGetCursorPos)(LPPOINT);
    static FnClipCursor   oClipCursor   = nullptr;
    static FnSetCursorPos oSetCursorPos = nullptr;
    static FnGetCursorPos oGetCursorPos = nullptr;
    static RECT  g_gameClip = {};
    static bool  g_gameClipValid = false;
    static POINT g_gamePos = {};
    static bool  g_gamePosValid = false;
    static bool  g_cursorHooks = false;

    static bool GameThreadCall()
    {
        const State& st = State::Get();
        return st.menuOpen && st.renderTid != 0 && GetCurrentThreadId() != st.renderTid;
    }

    static BOOL WINAPI hkClipCursor(const RECT* r)
    {
        if (r) { g_gameClip = *r; g_gameClipValid = true; } else g_gameClipValid = false;
        if (State::Get().menuOpen) return TRUE;
        return oClipCursor(r);
    }
    static BOOL WINAPI hkSetCursorPos(int x, int y)
    {
        if (GameThreadCall()) { g_gamePos.x = x; g_gamePos.y = y; g_gamePosValid = true; return TRUE; }
        if (!State::Get().menuOpen) { g_gamePos.x = x; g_gamePos.y = y; g_gamePosValid = true; }
        return oSetCursorPos(x, y);
    }
    static BOOL WINAPI hkGetCursorPos(LPPOINT p)
    {
        if (GameThreadCall() && g_gamePosValid && p) { *p = g_gamePos; return TRUE; }
        return oGetCursorPos(p);
    }

    static void HookCursorApi()
    {
        if (g_cursorHooks) return;
        g_cursorHooks = true;
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (!user32) return;
        struct { const char* name; void* detour; void** original; } hooks[] = {
            { "ClipCursor",   reinterpret_cast<void*>(&hkClipCursor),   reinterpret_cast<void**>(&oClipCursor) },
            { "SetCursorPos", reinterpret_cast<void*>(&hkSetCursorPos), reinterpret_cast<void**>(&oSetCursorPos) },
            { "GetCursorPos", reinterpret_cast<void*>(&hkGetCursorPos), reinterpret_cast<void**>(&oGetCursorPos) },
        };
        int ok = 0;
        for (auto& h : hooks)
        {
            void* target = reinterpret_cast<void*>(GetProcAddress(user32, h.name));
            if (!target) continue;
            if (MH_CreateHook(target, h.detour, h.original) == MH_OK && MH_EnableHook(target) == MH_OK) ++ok;
            else LOG_ERR("[input] could not hook %s; the mouse may stay locked while the menu is open", h.name);
        }
        LOG("[input] cursor API hooks: %d of 3", ok);
    }

    void MenuOpened()
    {
        if (oClipCursor) oClipCursor(nullptr); // let the cursor roam the whole screen
    }
    void MenuClosed()
    {
        if (oClipCursor && g_gameClipValid) oClipCursor(&g_gameClip);
        if (oSetCursorPos && g_gamePosValid) oSetCursorPos(g_gamePos.x, g_gamePos.y);
    }

    // --- window procedure ---------------------------------------------------
    static bool IsMouse(UINT m)    { return m >= WM_MOUSEFIRST && m <= WM_MOUSELAST; }
    static bool IsKeyboard(UINT m) { return m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP || m == WM_CHAR || m == WM_SYSCHAR; }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (State::Get().menuOpen)
        {
            if (IsMouse(msg) || IsKeyboard(msg))
            {
                ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
                // Releases reach the game so a key held when the menu opened does not stick.
                if (msg == WM_KEYUP || msg == WM_SYSKEYUP)
                    return CallWindowProc(g_original, hwnd, msg, wParam, lParam);
                return 0;
            }
            if (msg == WM_INPUT)
                return DefWindowProcW(hwnd, msg, wParam, lParam); // swallow raw mouse look; DefWindowProc frees the buffer
        }
        return CallWindowProc(g_original, hwnd, msg, wParam, lParam);
    }

    void Init(HWND hwnd)
    {
        if (g_original) return;
        g_hwnd = hwnd;
        g_original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc)));
        HookCursorApi();
    }

    void Shutdown()
    {
        if (g_original && g_hwnd)
        {
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original));
            g_original = nullptr;
            g_hwnd = nullptr;
        }
        // Cursor hooks are MinHook hooks; the DX12 layer disables all hooks on shutdown.
    }
}
