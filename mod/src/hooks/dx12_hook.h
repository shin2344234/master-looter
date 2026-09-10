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
}
