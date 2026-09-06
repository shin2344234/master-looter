#include "mod.h"

#include <MinHook.h>
#include <cstring>

#include "creaturedb.h"
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

    void Initialize(HMODULE module)
    {
        if (g_initialized)
            return;

        Paths::Init(module);
        LOG("Master Looter v%s for Crimson Desert %s starting (built %s %s).", ML_VERSION, ML_GAME_BUILD, __DATE__, __TIME__);

        Settings::Load();

        if (MH_Initialize() != MH_OK)
        {
            LOG_ERR("MinHook initialization failed.");
            return;
        }

        if (!hooks::InstallDX12Hooks())
        {
            LOG_ERR("DX12 hooks failed; no menu this session.");
            MH_Uninitialize();
            return;
        }
        State::Get().hooksOk = true;

        g_initialized = true;
        LOG_OK("Hooks installed. The menu key (default Insert) opens the menu once the game renders.");
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
            LOG_OK("Item database loaded: %d items, %d classes, %d tags.", ItemDb::Count(),
                   static_cast<int>(ItemDb::Classes().size()), static_cast<int>(ItemDb::Tags().size()));
        else
            LOG_ERR("Item database not found next to the plugin (MasterLooter.items.tsv); class and tag rules will be empty.");
        if (CreatureDb::Load()) LOG_OK("Creature table loaded: %d creatures.", CreatureDb::Count());
        else LOG_ERR("Creature table not found next to the plugin (MasterLooter.creatures.tsv); creatures cannot be told apart by species.");
        State::Get().overlayReady = true;

        // The loot engine touches game memory and game functions: only in the game itself.
        if (HostIsGame()) loot::Start();
        else LOG("Host is not CrimsonDesert.exe; loot engine not started.");
    }

    void Shutdown()
    {
        if (!g_initialized)
            return;
        loot::Stop();
        Settings::Save();
        hooks::RemoveDX12Hooks();
        MH_Uninitialize();
        Log::Shutdown();
        g_initialized = false;
    }
}
