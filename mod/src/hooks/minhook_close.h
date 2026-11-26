#pragma once
// Forced into every MinHook source file by CMakeLists.txt, ahead of its own
// includes. MinHook's Freeze closes each thread handle right after suspending
// that thread, and its Unfreeze closes each one right after resuming it, so
// both call CloseHandle while other threads are still frozen. OptiScaler's
// d3d12.dll detours CloseHandle and takes a mutex inside it, and its input
// thread holds that mutex during startup: if MinHook froze that thread and
// then called CloseHandle, the game hung for good (issue #241). NtClose is
// what CloseHandle ends in for a thread or snapshot handle, and it is imported
// from ntdll at load, so nothing is resolved inside the frozen window.
#include <windows.h>
#include <winternl.h>

extern volatile LONG ml_minhook_closes;

static __inline BOOL MlMinHookClose(HANDLE h)
{
    InterlockedIncrement(&ml_minhook_closes);
    return NtClose(h) >= 0;
}

#define CloseHandle(h) MlMinHookClose(h)
