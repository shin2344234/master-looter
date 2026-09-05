#include <Windows.h>
#include "core/mod.h"

static HMODULE g_module = nullptr;

static DWORD WINAPI MainThread(LPVOID)
{
    ml::Mod::Initialize(g_module);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        g_module = module;
        DisableThreadLibraryCalls(module);
        // Real work happens off the loader lock.
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
        break;
    case DLL_PROCESS_DETACH:
        ml::Mod::Shutdown();
        break;
    }
    return TRUE;
}
