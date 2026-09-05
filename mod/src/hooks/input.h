#pragma once
#include <Windows.h>

struct ImGuiIO;

namespace ml::input
{
    // Subclasses the game window. While the menu is open, keyboard and mouse
    // messages go to ImGui and are withheld from the game (key releases still
    // pass so nothing sticks).
    //
    // The menu's cursor is virtual. The game clips the OS cursor to a point,
    // hides it and recentres it every frame, and it does so from more than one
    // thread, so the OS cursor is useless for a menu. Instead the raw mouse
    // deltas the game already receives (WM_INPUT) move a cursor of our own,
    // clamped to the window, and that is what ImGui is fed. The OS cursor is
    // left alone, so nothing has to be restored when the menu closes.
    void Init(HWND hwnd);
    void Shutdown();

    void MenuOpened();   // centres the virtual cursor
    void MenuClosed();
    // Render thread, once per frame while the menu is open: hands ImGui the
    // virtual cursor position and any raw-input button state.
    void FeedMouse(ImGuiIO& io);
}
