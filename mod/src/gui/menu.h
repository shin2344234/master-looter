#pragma once

namespace ml::gui
{
    // Called once when ImGui is created, before the DX12 backend builds the font.
    void InitStyle(float scale);
    // Polled every frame from the render hook: menu key edge, Escape to close.
    void PollToggle();
    // Whether anything needs drawing this frame (menu or HUD).
    bool WantsDraw();
    // Draws the HUD line and, when open, the menu.
    void Render();
}
