#pragma once
#include <Windows.h>

namespace ml::input
{
    // Subclasses the game window. While the menu is open, keyboard and mouse
    // messages go to ImGui and are withheld from the game (key releases still
    // pass so nothing sticks); raw input is dropped so the camera stays put.
    void Init(HWND hwnd);
    void Shutdown();
}
