#pragma once
#include <cstdint>

// Function hooks with absolute 64-bit jumps. MinHook needs a scratch page
// within 1 GB of the target and the game's image is surrounded by its own
// DLLs with nothing free nearby, so MH_CreateHook fails there with
// MH_ERROR_MEMORY_ALLOC. These hooks steal the first instructions (measured
// with MinHook's HDE decoder, rejected if any is rip-relative), put them in a
// trampoline anywhere in memory, and patch `mov rax, detour; jmp rax` over the
// entry. Other threads are suspended around the write.
namespace ml::farhook
{
    // Installs a detour. *original receives the trampoline (callable as the
    // original function). Returns false and fills `why` on failure.
    bool Install(const char* name, uintptr_t target, void* detour, void** original, char* why, unsigned whyLen);
    void RemoveAll();
}
