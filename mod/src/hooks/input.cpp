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

    // --- cursor diagnostics -------------------------------------------------
    // Nothing about the menu cursor has ever been written down, so a cursor
    // that does not move leaves a log that says only "menu open". These count
    // what reaches the window and report what ImGui ended up with.
    typedef BOOL (WINAPI *FnGetCursorPos)(LPPOINT);
    static FnGetCursorPos oGetCursorPos = nullptr;
    static volatile LONG g_dbgInput  = 0;   // WM_INPUT seen by our procedure
    static volatile LONG g_dbgMove   = 0;   // legacy WM_MOUSEMOVE seen
    static volatile LONG g_dbgButton = 0;   // legacy button and wheel messages
    static bool    g_gcpHooked = false;
    static int     g_dbgLines  = 0;         // per-second lines used this opening
    static DWORD   g_dbgNext   = 0;
    static WNDPROC g_ours      = nullptr;

    // A plugin that subclasses the game window after us sits in front of our
    // procedure, and if it does not pass mouse messages on the menu never sees
    // a move (YeOldeKnob, #186: Quick Save and Reload, 0 raw, 0 legacy, 0
    // button messages with the menu open). So when the menu opens and someone
    // else is outermost, a second procedure of ours goes on top of theirs. It
    // does the captured-input work and hands everything else down the chain,
    // which reaches the first one again through the other plugin. t_below
    // tells the first one a message already went through the top, so it is
    // not handled twice. Installed once at most: putting it on top a second
    // time would make it its own successor.
    static WNDPROC g_next    = nullptr;   // what the top procedure hands down to
    static bool    g_topOn   = false;     // the top procedure is somewhere in the chain
    static thread_local int t_below = 0;
    static bool    g_topSaid = false;     // the top procedure has logged its first catch this opening

    static void Lock()   { EnterCriticalSection(&g_cs); }
    static void Unlock() { LeaveCriticalSection(&g_cs); }

    // Zero until the render path has a swapchain, which means no scaling.
    static float g_ovW = 0.0f, g_ovH = 0.0f;

    void SetOverlaySize(float w, float h) { g_ovW = w; g_ovH = h; }

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
        int cw = 0, chh = 0;
        ClientSize(&cw, &chh);
        const float sx = (g_ovW > 0.0f && cw > 0) ? g_ovW / static_cast<float>(cw) : 1.0f;
        const float sy = (g_ovH > 0.0f && chh > 0) ? g_ovH / static_cast<float>(chh) : 1.0f;
        Lock();
        io.AddMousePosEvent(g_vx * sx, g_vy * sy);
        for (int b = 0; b < 5; ++b)
        {
            for (int i = 0; i < g_pendingButtons[b][0]; ++i) io.AddMouseButtonEvent(b, true);
            for (int i = 0; i < g_pendingButtons[b][1]; ++i) io.AddMouseButtonEvent(b, false);
            g_pendingButtons[b][0] = g_pendingButtons[b][1] = 0;
        }
        if (g_pendingWheel != 0) { io.AddMouseWheelEvent(0, g_pendingWheel); g_pendingWheel = 0; }
        const float vx = g_vx, vy = g_vy;
        Unlock();

        // The first few seconds of each opening, which is where a cursor fault
        // shows. Three lines rather than one: the first can catch the menu
        // mid-open. Read the raw move count first, then whether the position
        // ImGui settled on follows the virtual cursor or the OS one.
        const DWORD now = GetTickCount();
        if (g_dbgLines < 3 && now >= g_dbgNext)
        {
            g_dbgNext = now + 1000;
            ++g_dbgLines;
            POINT os = {};
            if (oGetCursorPos) oGetCursorPos(&os);
            LOG("[input] cursor: %ld raw moves, %ld legacy moves, %ld button messages | virtual %.0f,%.0f fed as %.0f,%.0f | ImGui %.0f,%.0f of %.0fx%.0f | client %dx%d | OS cursor %ld,%ld",
                InterlockedExchange(&g_dbgInput, 0), InterlockedExchange(&g_dbgMove, 0), InterlockedExchange(&g_dbgButton, 0),
                vx, vy, vx * sx, vy * sy, io.MousePos.x, io.MousePos.y, io.DisplaySize.x, io.DisplaySize.y, cw, chh, os.x, os.y);
        }
    }

    // ImGui's Win32 backend polls GetCursorPos every frame as a fallback for
    // the mouse position. On the render thread, while the menu is open, that
    // poll gets the virtual cursor so the two never fight.
    static BOOL WINAPI hkGetCursorPos(LPPOINT p)
    {
        const State& st = State::Get();
        if (p && st.Captures() && st.renderTid && GetCurrentThreadId() == st.renderTid && g_hwnd)
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

    static void LogRawTarget(const char* when, HWND target)
    {
        if (!target)
        {
            LOG("[input] %s raw mouse input has no target window, so it follows keyboard focus.", when);
            return;
        }
        if (target == g_hwnd)
        {
            LOG("[input] %s raw mouse input goes to the window we subclassed (%p), so WM_INPUT reaches the menu.", when, (void*)g_hwnd);
            return;
        }
        char cls[64] = "";
        GetClassNameA(target, cls, sizeof cls);
        DWORD pid = 0;
        GetWindowThreadProcessId(target, &pid);
        LOG_ERR("[input] %s raw mouse input goes to %p (class \"%s\", pid %lu) and not to the window we subclassed (%p). WM_INPUT never reaches the menu, so its cursor cannot move.",
                when, (void*)target, cls, pid, (void*)g_hwnd);
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
                LogRawTarget("at startup", devs[i].hwndTarget);
                return;
            }
        }
        RAWINPUTDEVICE rid = { 0x01, 0x02, 0, g_hwnd };
        g_weRegistered = RegisterRawInputDevices(&rid, 1, sizeof rid) != 0;
        LOG("[input] raw mouse input %s", g_weRegistered ? "registered for the menu cursor" : "registration FAILED; the menu cursor will not move");
    }

    static LRESULT CALLBACK TopProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    static void ModulePath(const void* addr, wchar_t (&path)[MAX_PATH])
    {
        path[0] = L'\0';
        HMODULE owner = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addr), &owner);
        if (owner) GetModuleFileNameW(owner, path, MAX_PATH);
    }

    // Runs at each opening of the menu, on the render thread. Subclassing
    // from another thread of the same process is allowed.
    static void PutTopOn()
    {
        const WNDPROC live = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(g_hwnd, GWLP_WNDPROC));
        if (live == g_ours)
        {
            g_topOn = false; // whoever was above us has unhooked, taking the top procedure with them
            LOG("[input] our window procedure is still the outermost one.");
            return;
        }
        if (live == &TopProc)
        {
            LOG("[input] our top window procedure is still the outermost one, above %p.", (void*)g_next);
            return;
        }
        wchar_t path[MAX_PATH];
        ModulePath(reinterpret_cast<const void*>(live), path);
        LOG("[input] something subclassed the window after us: procedure %p in %S. Messages reach us only if it passes them on.",
            (void*)live, path[0] ? path : L"an unknown module");
        if (g_topOn)
        {
            LOG("[input] our top window procedure is already below that one, so it stays where it is.");
            return;
        }
        g_next = live;
        const LONG_PTR prev = SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&TopProc));
        if (prev == reinterpret_cast<LONG_PTR>(live))
        {
            g_topOn = true;
            LOG("[input] put our top window procedure above %S, so the menu sees the mouse before it does.",
                path[0] ? path : L"that procedure");
        }
        else if (!prev)
        {
            const DWORD err = GetLastError();
            g_next = nullptr;
            LOG_ERR("[input] could not put our window procedure on top (error %lu); the menu may not see the mouse.", err);
        }
        else
        {
            // Something swapped it between the read and the write. Put back
            // what was there and try again at the next opening.
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, prev);
            g_next = nullptr;
            LOG_ERR("[input] could not put our window procedure on top: it changed under us, so it is left as it was until the next opening.");
        }
    }

    // Everything about the cursor that can be read from outside, written once
    // per opening. A frozen cursor and a cursor that lands somewhere else are
    // different faults and this tells them apart.
    static void ReportOnOpen(int w, int h)
    {
        InterlockedExchange(&g_dbgInput, 0);
        InterlockedExchange(&g_dbgMove, 0);
        InterlockedExchange(&g_dbgButton, 0);
        g_dbgLines = 0;
        g_dbgNext  = GetTickCount() + 1000;

        RECT wr = {};
        GetWindowRect(g_hwnd, &wr);
        LOG("[input] menu opened. Window %p, client %dx%d, frame at %ld,%ld size %ldx%ld, unicode %d.",
            (void*)g_hwnd, w, h, wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top, IsWindowUnicode(g_hwnd) ? 1 : 0);

        const HWND fg = GetForegroundWindow();
        LOG("[input] foreground %p%s, focus %p, active %p.",
            (void*)fg, fg == g_hwnd ? " (ours)" : " (not the window we subclassed)", (void*)GetFocus(), (void*)GetActiveWindow());

        PutTopOn();

        UINT n = 0;
        GetRegisteredRawInputDevices(nullptr, &n, sizeof(RAWINPUTDEVICE));
        std::vector<RAWINPUTDEVICE> devs(n);
        if (n) GetRegisteredRawInputDevices(devs.data(), &n, sizeof(RAWINPUTDEVICE));
        bool found = false;
        for (UINT i = 0; i < n; ++i)
            if (devs[i].usUsagePage == 0x01 && devs[i].usUsage == 0x02)
            {
                found = true;
                LOG("[input] raw mouse registration right now: flags 0x%X.", devs[i].dwFlags);
                LogRawTarget("right now", devs[i].hwndTarget);
            }
        if (!found)
            LOG_ERR("[input] nothing in this process is registered for raw mouse input any more, so the menu cursor cannot move.");

        POINT os = {};
        if (oGetCursorPos) oGetCursorPos(&os);
        POINT osc = os;
        ScreenToClient(g_hwnd, &osc);
        RECT clip = {};
        GetClipCursor(&clip);
        Lock();
        const float vx = g_vx, vy = g_vy;
        Unlock();
        LOG("[input] GetCursorPos detour %s. OS cursor screen %ld,%ld client %ld,%ld, clipped to %ld,%ld..%ld,%ld. Virtual cursor starts at %.0f,%.0f.",
            g_gcpHooked ? "installed" : "MISSING, so ImGui polls the real cursor and fights the virtual one",
            os.x, os.y, osc.x, osc.y, clip.left, clip.top, clip.right, clip.bottom, vx, vy);
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
        g_topSaid = false;
        ReportOnOpen(w, h);
    }
    void MenuClosed() {}

    // --- window procedure ---------------------------------------------------
    static bool IsMouse(UINT m)    { return m >= WM_MOUSEFIRST && m <= WM_MOUSELAST; }
    static bool IsKeyboard(UINT m) { return m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP || m == WM_CHAR || m == WM_SYSCHAR; }

    static LRESULT PassDown(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        ++t_below;
        const LRESULT r = CallWindowProc(g_next, hwnd, msg, wParam, lParam);
        --t_below;
        return r;
    }

    static void Count(UINT msg)
    {
        if (msg == WM_INPUT)          InterlockedIncrement(&g_dbgInput);
        else if (msg == WM_MOUSEMOVE) InterlockedIncrement(&g_dbgMove);
        else if (IsMouse(msg))        InterlockedIncrement(&g_dbgButton);
    }

    // The menu's share of a message while it holds the input. True when the
    // message is finished with and *out is its result. top says which of our
    // two procedures is asking, which decides where a key release goes on to.
    static bool Captured(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, bool top, LRESULT* out)
    {
        {
            if (msg == WM_INPUT)
            {
                // The game registers the mouse with RIDEV_INPUTSINK, so WM_INPUT
                // keeps arriving while another window is in front. Those moves
                // belong to that window: the menu cursor stays put and the OS
                // cursor is left alone. The keyboard is gated the same way.
                if (GET_RAWINPUT_CODE_WPARAM(wParam) == RIM_INPUT && State::ForegroundIsOurs())
                {
                    OnRawInput(reinterpret_cast<HRAWINPUT>(lParam));
                    SetCursor(nullptr); // the menu draws its own cursor; the OS one stays hidden
                }
                *out = DefWindowProcW(hwnd, msg, wParam, lParam); // the game never sees mouse look; DefWindowProc frees the buffer
                return true;
            }
            if (msg == WM_SETCURSOR) { SetCursor(nullptr); *out = TRUE; return true; }
            if (msg == WM_MOUSEMOVE || msg == WM_NCMOUSEMOVE || msg == WM_MOUSELEAVE || msg == WM_NCMOUSELEAVE)
            {
                *out = 0; // the OS cursor position is meaningless here; the virtual cursor is fed each frame
                return true;
            }
            if (IsMouse(msg))
            {
                if (!g_rawButtons) ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam); // buttons and wheel
                *out = 0;
                return true;
            }
            if (IsKeyboard(msg))
            {
                ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
                // Releases reach the game so a key held when the menu opened does not stick.
                if (msg == WM_KEYUP || msg == WM_SYSKEYUP)
                    *out = top ? PassDown(hwnd, msg, wParam, lParam) : CallWindowProc(g_original, hwnd, msg, wParam, lParam);
                else
                    *out = 0;
                return true;
            }
        }
        return false;
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        const bool captures = State::Get().Captures();
        // Under the top procedure every message has been counted and judged
        // already, so this one only passes it on.
        if (t_below == 0)
        {
            Count(msg);
            LRESULT r = 0;
            if (captures && Captured(hwnd, msg, wParam, lParam, false, &r)) return r;
        }
        if (!captures && msg == WM_INPUT && g_weRegistered)
        {
            // Our own registration: the game did not ask for these, so do not hand them over.
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        return CallWindowProc(g_original, hwnd, msg, wParam, lParam);
    }

    static LRESULT CALLBACK TopProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        Count(msg);
        if (State::Get().Captures())
        {
            if (!g_topSaid && (msg == WM_INPUT || IsMouse(msg)))
            {
                g_topSaid = true;
                LOG("[input] the top window procedure caught mouse input for the menu.");
            }
            LRESULT r = 0;
            if (Captured(hwnd, msg, wParam, lParam, true, &r)) return r;
        }
        if (!g_next) return DefWindowProcW(hwnd, msg, wParam, lParam);
        return PassDown(hwnd, msg, wParam, lParam);
    }

    void Init(HWND hwnd)
    {
        if (g_original) return;
        if (!g_csReady) { InitializeCriticalSection(&g_cs); g_csReady = true; }
        g_hwnd = hwnd;
        g_ours = &WndProc;
        g_original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc)));
        EnsureRawInput();
        if (HMODULE user32 = GetModuleHandleW(L"user32.dll"))
        {
            void* target = reinterpret_cast<void*>(GetProcAddress(user32, "GetCursorPos"));
            if (!target || MH_CreateHook(target, reinterpret_cast<void*>(&hkGetCursorPos), reinterpret_cast<void**>(&oGetCursorPos)) != MH_OK || MH_EnableHook(target) != MH_OK)
                LOG_ERR("[input] could not hook GetCursorPos; the menu cursor may jump");
            else
                g_gcpHooked = true;
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
