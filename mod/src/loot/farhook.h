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

    // For a target another mod has already detoured with a five-byte `jmp
    // rel32` over its entry. Install would refuse the relative branch, and
    // its twelve-byte patch would land where that mod's trampoline jumps back
    // to. Instead a fourteen-byte relay to the detour goes into a run of int3
    // padding inside the game image, which a rel32 always reaches, and only
    // the jump's four-byte offset changes to point at it. *original jumps on
    // to the other mod's detour, so both run and the original runs last.
    bool InstallOverJump(const char* name, uintptr_t target, void* detour, void** original, char* why, unsigned whyLen);

    // For an entry another mod has patched with an absolute jump: `mov rax,
    // imm64; jmp rax`, which is what CDAutoLoot writes and what Install here
    // writes too, or `jmp [rip+0]` with the address after it. Only the eight
    // address bytes change, to the detour, and *original jumps to the address
    // they held, so the other mod's detour still runs and the original last.
    bool InstallOverAbsJump(const char* name, uintptr_t target, void* detour, void** original, char* why, unsigned whyLen);

    // What InstallOverAbsJump would take over: the address such a patch jumps
    // to, or 0 when the entry is not one.
    uintptr_t AbsJumpTarget(uintptr_t target);

    // For one call instruction rather than a whole function: `site` is an E8
    // call rel32 in the game's code. A fourteen-byte relay to the detour goes
    // into int3 padding in reach, the same as InstallOverJump, and only the
    // call's four-byte offset changes to point at it. *original is the
    // function the call went to, so every other caller of that function runs
    // exactly as before and only this one call passes through the detour.
    bool InstallCallSite(const char* name, uintptr_t site, void* detour, void** original, char* why, unsigned whyLen);
    void RemoveAll();

    // The last patch's freeze: how many threads it suspended, and how many
    // handles it closed, all of them after every thread was running again.
    int LastFrozen();
    int LastClosed();
}
