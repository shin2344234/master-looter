#pragma once
#include <Windows.h>
#include <cstring>

namespace ml
{
    // Runtime flags shared by the render thread (menu), the window thread
    // (input) and the loot worker. Plain scalars only.
    struct State
    {
        bool menuOpen      = false;
        bool menuWatch     = false; // menu drawn but every input goes to the game (watch mode)
        bool textCapture   = false; // an ImGui text field has focus: every key belongs to the menu
        bool rebindCapture = false; // the General tab is waiting for the next key to bind
        bool hooksOk       = false;
        bool overlayReady  = false;
        DWORD renderTid    = 0;     // thread that presents frames (set by the menu each frame)

        // A short on-screen notice ("auto-loot on"), drawn until `noticeUntil`.
        char  notice[96]   = "";
        DWORD noticeUntil  = 0;

        // True while the menu owns keyboard, mouse and pad.
        bool Captures() const { return menuOpen && !menuWatch; }

        void Notify(const char* text, DWORD ms = 2500)
        {
            strncpy(notice, text, sizeof notice - 1);
            notice[sizeof notice - 1] = 0;
            noticeUntil = GetTickCount() + ms;
        }

        static State& Get()
        {
            static State s;
            return s;
        }
    };
}
