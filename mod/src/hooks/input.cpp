#include "input.h"

#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <vector>

#include "../core/log.h"
#include "../core/state.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace ml::input
{
    static WNDPROC g_original = nullptr;
    static HWND    g_hwnd     = nullptr;

    // --- virtual cursor -----------------------------------------------------
    static CRITICAL_SECTION g_cs;
    static bool  g_csReady = false;
    static float g_vx = 0, g_vy = 0;         // client coordinates
    static bool  g_centered = false;
    static bool  g_rawButtons = false;       // the game asked for no legacy button messages
    static int   g_pendingButtons[5][2];     // per button: down count, up count since last feed
    static float g_pendingWheel = 0;
    static bool  g_weRegistered = false;

    static void Lock()   { EnterCriticalSection(&g_cs); }
    static void Unlock() { LeaveCriticalSection(&g_cs); }

    static void ClientSize(int* w, int* h)
    {
        RECT rc = {};
        if (g_hwnd && GetClientRect(g_hwnd, &rc)) { *w = rc.right - rc.left; *h = rc.bottom - rc.top; }
        else { *w = 1920; *h = 1080; }
    }

    static void OnRawInput(HRAWINPUT h)
    {
        UINT size = 0;
        if (GetRawInputData(h, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 || size == 0 || size > 1024) return;
        alignas(8) unsigned char buf[1024];
        if (GetRawInputData(h, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) != size) return;
        const RAWINPUT* ri = reinterpret_cast<const RAWINPUT*>(buf);
        if (ri->header.dwType != RIM_TYPEMOUSE) return;
        const RAWMOUSE& m = ri->data.mouse;
        int w, hgt; ClientSize(&w, &hgt);
        Lock();
        if (m.usFlags & MOUSE_MOVE_ABSOLUTE)
        {
            // Absolute devices (tablets, remote desktop) report 0..65535 over the desktop.
            const bool virt = (m.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
            const int sw = GetSystemMetrics(virt ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
            const int sh = GetSystemMetrics(virt ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
            POINT p = { static_cast<LONG>(m.lLastX * sw / 65535.0), static_cast<LONG>(m.lLastY * sh / 65535.0) };
            ScreenToClient(g_hwnd, &p);
            g_vx = static_cast<float>(p.x); g_vy = static_cast<float>(p.y);
        }
        else
        {
            g_vx += static_cast<float>(m.lLastX);
            g_vy += static_cast<float>(m.lLastY);
        }
        if (g_vx < 0) g_vx = 0; if (g_vy < 0) g_vy = 0;
        if (g_vx > w - 1) g_vx = static_cast<float>(w - 1);
        if (g_vy > hgt - 1) g_vy = static_cast<float>(hgt - 1);
        if (g_rawButtons)
        {
            static const USHORT down[5] = { RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_5_DOWN };
            static const USHORT up[5]   = { RI_MOUSE_LEFT_BUTTON_UP,   RI_MOUSE_RIGHT_BUTTON_UP,   RI_MOUSE_MIDDLE_BUTTON_UP,   RI_MOUSE_BUTTON_4_UP,   RI_MOUSE_BUTTON_5_UP };
            for (int b = 0; b < 5; ++b)
            {
                if (m.usButtonFlags & down[b]) ++g_pendingButtons[b][0];
                if (m.usButtonFlags & up[b])   ++g_pendingButtons[b][1];
            }
            if (m.usButtonFlags & RI_MOUSE_WHEEL) g_pendingWheel += static_cast<short>(m.usButtonData) / static_cast<float>(WHEEL_DELTA);
        }
        Unlock();
    }

    void FeedMouse(ImGuiIO& io)
    {
        Lock();
        io.AddMousePosEvent(g_vx, g_vy);
        for (int b = 0; b < 5; ++b)
        {
            for (int i = 0; i < g_pendingButtons[b][0]; ++i) io.AddMouseButtonEvent(b, true);
            for (int i = 0; i < g_pendingButtons[b][1]; ++i) io.AddMouseButtonEvent(b, false);
            g_pendingButtons[b][0] = g_pendingButtons[b][1] = 0;
        }
        if (g_pendingWheel != 0) { io.AddMouseWheelEvent(0, g_pendingWheel); g_pendingWheel = 0; }
        Unlock();
    }

    // ImGui's Win32 backend polls GetCursorPos every frame as a fallback for
    // the mouse position. On the render thread, while the menu is open, that
    // poll gets the virtual cursor so the two never fight.
    typedef BOOL (WINAPI *FnGetCursorPos)(LPPOINT);
    static FnGetCursorPos oGetCursorPos = nullptr;
    static BOOL WINAPI hkGetCursorPos(LPPOINT p)
    {
        const State& st = State::Get();
        if (p && st.menuOpen && st.renderTid && GetCurrentThreadId() == st.renderTid && g_hwnd)
        {
            Lock();
            POINT c = { static_cast<LONG>(g_vx), static_cast<LONG>(g_vy) };
            Unlock();
            ClientToScreen(g_hwnd, &c);
            *p = c;
            return TRUE;
        }
        return oGetCursorPos(p);
    }

    static void EnsureRawInput()
    {
        // If the game registered the mouse for raw input we ride along; if not,
        // register it ourselves (per process, delivered to the game window).
        UINT n = 0;
        GetRegisteredRawInputDevices(nullptr, &n, sizeof(RAWINPUTDEVICE));
        std::vector<RAWINPUTDEVICE> devs(n);
        if (n) GetRegisteredRawInputDevices(devs.data(), &n, sizeof(RAWINPUTDEVICE));
        for (UINT i = 0; i < n; ++i)
        {
            if (devs[i].usUsagePage == 0x01 && devs[i].usUsage == 0x02)
            {
                g_rawButtons = (devs[i].dwFlags & RIDEV_NOLEGACY) != 0;
                LOG("[input] game registered raw mouse input (flags 0x%X)%s", devs[i].dwFlags, g_rawButtons ? ", no legacy button messages" : "");
                return;
            }
        }
        RAWINPUTDEVICE rid = { 0x01, 0x02, 0, g_hwnd };
        g_weRegistered = RegisterRawInputDevices(&rid, 1, sizeof rid) != 0;
        LOG("[input] raw mouse input %s", g_weRegistered ? "registered for the menu cursor" : "registration FAILED; the menu cursor will not move");
    }

    void MenuOpened()
    {
        int w, h; ClientSize(&w, &h);
        Lock();
        g_vx = w * 0.5f; g_vy = h * 0.5f;
        for (auto& b : g_pendingButtons) b[0] = b[1] = 0;
        g_pendingWheel = 0;
        Unlock();
        g_centered = true;
    }
    void MenuClosed() {}

    // --- window procedure ---------------------------------------------------
    static bool IsMouse(UINT m)    { return m >= WM_MOUSEFIRST && m <= WM_MOUSELAST; }
    static bool IsKeyboard(UINT m) { return m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP || m == WM_CHAR || m == WM_SYSCHAR; }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (State::Get().menuOpen)
        {
            if (msg == WM_INPUT)
            {
                OnRawInput(reinterpret_cast<HRAWINPUT>(lParam));
                return DefWindowProcW(hwnd, msg, wParam, lParam); // the game never sees mouse look; DefWindowProc frees the buffer
            }
            if (msg == WM_MOUSEMOVE || msg == WM_NCMOUSEMOVE || msg == WM_MOUSELEAVE || msg == WM_NCMOUSELEAVE)
                return 0; // the OS cursor position is meaningless here; the virtual cursor is fed each frame
            if (IsMouse(msg))
            {
                if (!g_rawButtons) ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam); // buttons and wheel
                return 0;
            }
            if (IsKeyboard(msg))
            {
                ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
                // Releases reach the game so a key held when the menu opened does not stick.
                if (msg == WM_KEYUP || msg == WM_SYSKEYUP)
                    return CallWindowProc(g_original, hwnd, msg, wParam, lParam);
                return 0;
            }
        }
        else if (msg == WM_INPUT && g_weRegistered)
        {
            // Our own registration: the game did not ask for these, so do not hand them over.
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        return CallWindowProc(g_original, hwnd, msg, wParam, lParam);
    }

    void Init(HWND hwnd)
    {
        if (g_original) return;
        if (!g_csReady) { InitializeCriticalSection(&g_cs); g_csReady = true; }
        g_hwnd = hwnd;
        g_original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc)));
        EnsureRawInput();
        if (HMODULE user32 = GetModuleHandleW(L"user32.dll"))
        {
            void* target = reinterpret_cast<void*>(GetProcAddress(user32, "GetCursorPos"));
            if (!target || MH_CreateHook(target, reinterpret_cast<void*>(&hkGetCursorPos), reinterpret_cast<void**>(&oGetCursorPos)) != MH_OK || MH_EnableHook(target) != MH_OK)
                LOG_ERR("[input] could not hook GetCursorPos; the menu cursor may jump");
        }
    }

    void Shutdown()
    {
        if (g_original && g_hwnd)
        {
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original));
            g_original = nullptr;
            g_hwnd = nullptr;
        }
    }
}
