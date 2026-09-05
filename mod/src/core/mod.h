#pragma once
#include <Windows.h>

namespace ml::Mod
{
    void Initialize(HMODULE module);
    void Shutdown();

    // Called from the render hook the first time this process presents a frame.
    // Only that process (not a launcher that also loaded the ASI) owns the log,
    // the INI and the item database.
    void OnRenderProcess();
}
