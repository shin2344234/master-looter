#include <Windows.h>
#include "core/mod.h"

static HMODULE g_module = nullptr;

static DWORD WINAPI MainThread(LPVOID)
{
    ml::Mod::Initialize(g_module);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        g_module = module;
        DisableThreadLibraryCalls(module);
        // Real work happens off the loader lock.
        if (HANDLE h = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr)) CloseHandle(h);
        break;
    case DLL_PROCESS_DETACH:
        // reserved is non-null when the process is terminating.
        ml::Mod::Shutdown(reserved != nullptr);
        break;
    }
    return TRUE;
}
