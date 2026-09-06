// Adapted for Master Looter from Trinity (https://github.com/XeTrinityz/Trinity),
// MIT License, Copyright (c) 2026 XeTrinityz. See THIRD_PARTY_NOTICES.md.
// Changes: Master Looter namespaces, logger, settings and menu hooks; icon atlas code removed.
#pragma once
#include <Windows.h>
#include <Xinput.h>

namespace ml::hooks
{
    // Detours XInputGetState so the pad is neutralised for the GAME while the
    // menu is open - stops nav buttons (A/B, d-pad, RB+X) leaking through. The
    // menu reads the real pad via XInputReadReal, so its own navigation still
    // works while it blocks the game.
    //
    // Safe to call every frame: it hooks whichever xinput module has since
    // loaded (the game often inits its input system after we do) and becomes a
    // no-op once every known module is accounted for.
    void EnsureXInputHooks();
    void RemoveXInputHooks();

    // Real pad state, bypassing the menu-open neutralisation applied to the
    // game. Falls back to the plain export until the hooks are up.
    DWORD XInputReadReal(DWORD userIndex, XINPUT_STATE* state);

    // --- pad shortcuts ------------------------------------------------------
    // A shortcut on the pad is two buttons at once, never one. Every single
    // button already does something in this game, so a one-button shortcut
    // would fire while playing; a pair that the game does not use itself will
    // not. The mod only reads the pad, so the game still sees both buttons.
    // That is why the pair matters: pick two that do nothing together.

    // Buttons held right now, across whichever pad is connected. 0 when none.
    unsigned PadButtons();
    // True while every button in `mask` is held. A mask with fewer than two
    // buttons counts as unbound and is never held.
    bool PadChordHeld(unsigned mask);
    // "LB + RB", or "not set" when unbound. Points at a static buffer.
    const char* PadChordName(unsigned mask);
}
