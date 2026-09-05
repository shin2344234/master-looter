#pragma once

namespace ml
{
    // Runtime flags shared by the render thread (menu) and the window thread
    // (input). Plain scalars only.
    struct State
    {
        bool menuOpen      = false;
        bool textCapture   = false; // an ImGui text field has focus: every key belongs to the menu
        bool rebindCapture = false; // the General tab is waiting for the next key to bind
        bool hooksOk       = false;
        bool overlayReady  = false;

        static State& Get()
        {
            static State s;
            return s;
        }
    };
}
