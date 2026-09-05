#include "input.h"

#include <imgui.h>
#include <imgui_impl_win32.h>

#include "../core/state.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace ml::input
{
    static WNDPROC g_original = nullptr;
    static HWND    g_hwnd     = nullptr;

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
