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
        // An important one is drawn whether or not the toggle notice is wanted:
        // someone who turned that off still wants to hear about a full bag.
        char  notice[96]      = "";
        DWORD noticeUntil     = 0;
        bool  noticeImportant = false;

        // When the overlay last actually drew a frame. The menu can only be
        // opened or closed from inside the render path, so if drawing stops
        // while it is open there is no way left to close it.
        DWORD lastDrawAt = 0;

        // True while the menu owns keyboard, mouse and pad.
        //
        // Gated on the overlay having drawn recently. Without that, an overlay
        // that stops drawing with the menu open leaves the pad zeroed for the
        // rest of the session and no key can undo it, because the key that
        // would close the menu is read from the render path that has stopped.
        // The symptom is a dead controller and no menu on screen, which looks
        // nothing like a mod problem and is entirely a mod problem.
        //
        // Half a second is far longer than a frame and far shorter than anyone
        // would spend wondering why the pad died.
        bool Captures() const
        {
            if (!menuOpen || menuWatch) return false;
            const DWORD now = GetTickCount();
            return lastDrawAt && (now - lastDrawAt) < 500;
        }

        void Notify(const char* text, DWORD ms = 2500, bool important = false)
        {
            strncpy(notice, text, sizeof notice - 1);
            notice[sizeof notice - 1] = 0;
            noticeImportant = important;
            noticeUntil = GetTickCount() + ms;
        }

        // The game window is in front. Hotkeys are read with GetAsyncKeyState,
        // which sees every window, so they are ignored while alt-tabbed.
        static bool ForegroundIsOurs()
        {
            DWORD pid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &pid);
            return pid == GetCurrentProcessId();
        }

        static State& Get()
        {
            static State s;
            return s;
        }
    };
}
