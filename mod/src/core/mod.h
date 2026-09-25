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

    // Polled by the loot engine. The first time a second copy of the plugin
    // has been refused in this process, logs it and puts it on screen.
    void ReportSecondCopy();

    // Polled by the loot engine. The first time, looks for another loot mod
    // loaded in this process, and logs it and puts it on screen if there is
    // one. OtherLootMod returns its file name afterwards, or nullptr.
    void ReportOtherLootMod();
    const char* OtherLootMod();

    // A loaded plugin known to keep this mod's menu from ever getting a frame,
    // by file name, or nullptr. Named in the log where the menu fails to draw.
    const char* OverlayConflict();
}
