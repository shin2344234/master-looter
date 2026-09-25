// Adapted for Master Looter from Trinity (https://github.com/XeTrinityz/Trinity),
// MIT License, Copyright (c) 2026 XeTrinityz. See THIRD_PARTY_NOTICES.md.
// Changes: Master Looter namespaces, logger, settings and menu hooks; icon atlas code removed.
#pragma once

namespace ml::hooks
{
    // Grabs the DX12 present/queue vtable pointers from a throwaway device and
    // detours them with MinHook. Must be called after MH_Initialize().
    bool InstallDX12Hooks();
    void RemoveDX12Hooks();

    // True once a frame has reached this mod and the menu renderer is up.
    // Until then the menu cannot open, and the menu key is read nowhere else.
    bool MenuHasFrame();

    // Written when no frame has arrived in twenty seconds: who owns each way
    // the game's factory can make a swapchain, and whether this mod's
    // creation detour is still in line and has ever run.
    void LogSwapChainCreationPath();
}
