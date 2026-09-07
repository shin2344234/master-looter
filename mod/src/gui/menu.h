#pragma once

namespace ml::gui
{
    // Called once when ImGui is created, before the DX12 backend builds the font.
    void InitStyle(float scale);
    // A language change brings in characters the atlas was not built with. The
    // render hook asks between frames and rebuilds once its own work has
    // retired, since the backend releases the font texture to do it.
    bool FontsNeedRebuild();
    void RebuildFonts();
    // Polled every frame from the render hook: menu key edge, Escape to close.
    void PollToggle();
    // Whether anything needs drawing this frame (menu or HUD).
    bool WantsDraw();
    // Draws the HUD line and, when open, the menu.
    void Render();
}
