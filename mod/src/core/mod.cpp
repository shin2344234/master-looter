#include "mod.h"

#include <MinHook.h>
#include <cstring>

#include "creaturedb.h"
#include "text.h"
#include "nodedb.h"
#include "itemdb.h"
#include "log.h"
#include "paths.h"
#include "settings.h"
#include "state.h"
#include "../hooks/dx12_hook.h"
#include "../loot/engine.h"
#include "../version.h"

namespace ml::Mod
{
    static bool g_initialized = false;

    static bool HostIsGame()
    {
        char exe[MAX_PATH] = "";
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        const char* name = strrchr(exe, '\\');
        name = name ? name + 1 : exe;
        return _stricmp(name, "CrimsonDesert.exe") == 0;
    }

    // Where a fatal fault happened, on whatever thread it happened on.
    //
    // The scan already runs under its own handler, and every memory read the
    // mod does is guarded and counted. Neither caught the crash on Seth's
    // second machine: three runs died at the same point and wrote nothing,
    // which means the fault is not in the scan and is not a read the mod
    // expected to fail. That leaves a thread this plugin does not own, most
    // likely one of the game threads inside a hook while the world is being
    // torn down and rebuilt.
    //
    // An unhandled filter is the one place that sees all of those. It runs
    // only when nothing else has handled the exception, so the deliberate
    // faults inside mem:: never reach it. It changes nothing: whatever filter
    // the game installed still runs afterwards and the process still dies the
    // way it would have. It just writes down where first.
    static LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;
    static HMODULE g_self = nullptr;

    // Naming a module from an address, for both handlers below.
    static void WhereIs(void* at, char* mod, size_t modN, unsigned long long* off, bool* isSelf)
    {
        strncpy(mod, "unknown", modN - 1); mod[modN - 1] = 0;
        *off = 0; *isSelf = false;
        HMODULE h = nullptr;
        if (!at || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       static_cast<LPCSTR>(at), &h) || !h)
            return;
        char full[MAX_PATH] = "";
        if (GetModuleFileNameA(h, full, MAX_PATH))
        {
            const char* slash = strrchr(full, '\\');
            strncpy(mod, slash ? slash + 1 : full, modN - 1);
            mod[modN - 1] = 0;
        }
        *off = static_cast<unsigned long long>(
                   reinterpret_cast<uintptr_t>(at) - reinterpret_cast<uintptr_t>(h));
        *isSelf = (h == g_self);
    }

    // A vectored handler, because the top-level filter did not fire.
    //
    // SetUnhandledExceptionFilter has one global slot. The game installs its
    // own crash reporting after this plugin loads, which replaces ours, so on
    // the machine where this crash reproduces our filter was never called. A
    // vectored handler sits on a chain instead and cannot be displaced.
    //
    // It sees every first-chance exception, including the ones mem:: raises on
    // purpose when it probes an address and expects to be refused. Those all
    // fault at an address inside this plugin, so faults inside this module are
    // skipped here; the scan's own handler and the filter below still cover
    // them. What is left is a fault in somebody else's code, which is exactly
    // what a swapchain being torn down under us would look like.
    static LONG CALLBACK FirstChance(EXCEPTION_POINTERS* xp)
    {
        static volatile LONG s_said = 0;
        if (!xp || !xp->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
        const EXCEPTION_RECORD* er = xp->ExceptionRecord;

        // Only the codes that kill a process. A game raises C++ exceptions and
        // other first-chance noise constantly and none of it is interesting.
        const DWORD c = er->ExceptionCode;
        if (c != EXCEPTION_ACCESS_VIOLATION &&
            c != EXCEPTION_ILLEGAL_INSTRUCTION &&
            c != EXCEPTION_PRIV_INSTRUCTION &&
            c != EXCEPTION_STACK_OVERFLOW &&
            c != EXCEPTION_INT_DIVIDE_BY_ZERO &&
            c != 0xC0000374 /* heap corruption */)
            return EXCEPTION_CONTINUE_SEARCH;

        char mod[MAX_PATH]; unsigned long long off = 0; bool self = false;
        WhereIs(er->ExceptionAddress, mod, sizeof mod, &off, &self);
        if (self) return EXCEPTION_CONTINUE_SEARCH;   // mem:: probing, as designed

        if (InterlockedIncrement(&s_said) <= 12)
        {
            char access[128] = "";
            if (c == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
            {
                const ULONG_PTR kind = er->ExceptionInformation[0];
                snprintf(access, sizeof access, ", %s %p",
                         kind == 0 ? "reading" : (kind == 1 ? "writing" : "executing"),
                         reinterpret_cast<void*>(er->ExceptionInformation[1]));
            }
            LOG_ERR("[fault] 0x%08X at %p (%s+0x%llX) thread %lu%s",
                    c, er->ExceptionAddress, mod, off,
                    static_cast<unsigned long>(GetCurrentThreadId()), access);
        }
        return EXCEPTION_CONTINUE_SEARCH;   // change nothing, only write it down
    }

    static LONG WINAPI LastChance(EXCEPTION_POINTERS* xp)
    {
        static volatile LONG s_once = 0;
        if (InterlockedIncrement(&s_once) == 1 && xp && xp->ExceptionRecord)
        {
            const EXCEPTION_RECORD* er = xp->ExceptionRecord;
            void* at = er->ExceptionAddress;

            char mod[MAX_PATH] = "unknown";
            unsigned long long off = 0;
            HMODULE h = nullptr;
            if (at && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                         static_cast<LPCSTR>(at), &h) && h)
            {
                char full[MAX_PATH] = "";
                if (GetModuleFileNameA(h, full, MAX_PATH))
                {
                    const char* slash = strrchr(full, '\\');
                    strncpy(mod, slash ? slash + 1 : full, sizeof mod - 1);
                    mod[sizeof mod - 1] = 0;
                }
                off = static_cast<unsigned long long>(
                          reinterpret_cast<uintptr_t>(at) - reinterpret_cast<uintptr_t>(h));
            }

            char access[128] = "";
            if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
            {
                const ULONG_PTR kind = er->ExceptionInformation[0];
                snprintf(access, sizeof access, ", %s %p",
                         kind == 0 ? "reading" : (kind == 1 ? "writing" : "executing"),
                         reinterpret_cast<void*>(er->ExceptionInformation[1]));
            }

            LOG_ERR("[crash] unhandled 0x%08X at %p (%s+0x%llX) on thread %lu%s",
                    er->ExceptionCode, at, mod, off,
                    static_cast<unsigned long>(GetCurrentThreadId()), access);

            // Whose thread it is matters as much as where it faulted. Ours are
            // the scan worker and anything the overlay runs on; everything else
            // is the game calling into a hook.
            LOG_ERR("[crash] this is the last line before the process goes. If the "
                    "module above is CrimsonDesert.exe the fault is in game code the "
                    "mod called into, not in the mod itself.");
        }
        return g_prevFilter ? g_prevFilter(xp) : EXCEPTION_CONTINUE_SEARCH;
    }

    void Initialize(HMODULE module)
    {
        if (g_initialized)
            return;

        Paths::Init(module);
        // The log goes to disk now, not at the first rendered frame. A crash
        // before anything is presented is exactly when someone needs the file,
        // and waiting meant that case left nothing behind at all. Only in the
        // game itself: this plugin gets loaded by other processes too, and one
        // of those must not truncate a real session's log.
        if (HostIsGame()) Log::Claim();
        LOG("Master Looter v%s for Crimson Desert %s starting (built %s %s).", ML_VERSION, ML_GAME_BUILD, __DATE__, __TIME__);
        g_self = module;
        g_prevFilter = SetUnhandledExceptionFilter(&LastChance);
        AddVectoredExceptionHandler(1 /* first */, &FirstChance);
        LOG("[crash] handlers armed: vectored plus top-level filter.");
        LOG("Mod page %s | source %s", ML_MOD_PAGE, ML_SOURCE_URL);

        Settings::Load();
        Text::Load(Settings::Get().language.c_str());

        if (MH_Initialize() != MH_OK)
        {
            LOG_ERR("MinHook initialization failed.");
            Log::Claim();   // in case this is not the game and nothing claimed it above
            return;
        }

        if (!hooks::InstallDX12Hooks())
        {
            LOG_ERR("DX12 hooks failed; no menu this session.");
            MH_Uninitialize();
            Log::Claim();
            return;
        }
        State::Get().hooksOk = true;

        g_initialized = true;
        LOG_OK("Hooks installed. The menu key (default Insert) opens the menu once the game renders.");

        // The loot engine used to start only from the first rendered frame,
        // which is fine right up until there are no frames. The overlay
        // arrives either through the swapchain wrapper or through the native
        // Present hook, and on a machine where another mod detoured Present
        // first and wrapping is off, neither exists. The plugin then loaded,
        // installed its hooks, said so, and did nothing at all for the rest of
        // the session without a word about why. Looting has nothing to do with
        // drawing, so it no longer waits for a frame that may never come.
        // Only in the game. This plugin is loaded into the game's crash
        // reporter as well, which never renders anything, so an unconditional
        // watchdog fires there every time, runs the whole late init in a
        // process that has no business doing it, and claims the log out from
        // under the real session.
        if (HostIsGame())
        {
            CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
                for (int i = 0; i < 100 && !State::Get().overlayReady; ++i)
                    Sleep(200);
                if (!State::Get().overlayReady)
                {
                    LOG_ERR("No frame has been rendered in twenty seconds. Nothing is drawing the "
                            "overlay: the swapchain is not wrapped and the present hook belongs to "
                            "another mod. Starting the loot engine anyway; the menu will not appear.");
                    OnRenderProcess();
                }
                return 0;
            }, nullptr, 0, nullptr);
        }
    }

    void OnRenderProcess()
    {
        static bool s_done = false;
        if (s_done)
            return;
        s_done = true;

        Log::Claim();
        Settings::Claim();
        if (ItemDb::Load())
            LOG_OK("Item database loaded (%s): %d items, %d classes, %d tags.", ItemDb::Source(), ItemDb::Count(),
                   static_cast<int>(ItemDb::Classes().size()), static_cast<int>(ItemDb::Tags().size()));
        else
            LOG_ERR("Item database unavailable: neither compiled into the plugin nor next to it as MasterLooter.items.tsv; class and tag rules will be empty.");
        if (NodeDb::Load()) LOG_OK("Gather node table loaded (%s): %d node kinds.", NodeDb::Source(), NodeDb::Count());
        else LOG_ERR("Gather node table unavailable: neither compiled into the plugin nor next to it as MasterLooter.nodes.tsv; gather nodes fall back to learning what they yield.");
        if (CreatureDb::Load()) LOG_OK("Creature table loaded (%s): %d creatures.", CreatureDb::Source(), CreatureDb::Count());
        else LOG_ERR("Creature table unavailable: neither compiled into the plugin nor next to it as MasterLooter.creatures.tsv; creatures cannot be told apart by species.");
        State::Get().overlayReady = true;

        // The loot engine touches game memory and game functions: only in the game itself.
        if (HostIsGame()) loot::Start();
        else LOG("Host is not CrimsonDesert.exe; loot engine not started.");
    }

    void Shutdown(bool terminating)
    {
        if (!g_initialized)
            return;
        g_initialized = false;
        if (terminating)
        {
            Settings::Save();
            Log::Shutdown();
            return;
        }
        loot::Stop();
        Settings::Save();
        hooks::RemoveDX12Hooks();
        MH_Uninitialize();
        Log::Shutdown();
    }
}
