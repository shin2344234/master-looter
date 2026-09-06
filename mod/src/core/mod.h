#pragma once
#include <Windows.h>

namespace ml::Mod
{
    void Initialize(HMODULE module);
    // terminating: the process is exiting (DllMain lpReserved != null); only
    // flush files then, other threads are already gone and unhooking is unsafe.
    void Shutdown(bool terminating);

    // Called from the render hook the first time this process presents a frame.
    // Only that process (not a launcher that also loaded the ASI) owns the log,
    // the INI and the item database.
    void OnRenderProcess();
}
