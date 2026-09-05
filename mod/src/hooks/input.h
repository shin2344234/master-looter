#pragma once
#include <Windows.h>

namespace ml::input
{
    // Subclasses the game window. While the menu is open, keyboard and mouse
    // messages go to ImGui and are withheld from the game (key releases still
    // pass so nothing sticks); raw input is dropped so the camera stays put.
    // Also hooks the cursor API: the game clips the cursor to a point and
    // recentres it every frame, which would leave the menu's cursor frozen.
    void Init(HWND hwnd);
    void Shutdown();

    // Called by the menu on open/close transitions (render thread): free the
    // cursor for the menu, then give the game its clip and position back.
    void MenuOpened();
    void MenuClosed();
}
