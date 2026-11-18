#include "storage_link.h"

#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <mutex>

#include "../core/log.h"

namespace ml::psm
{
    namespace
    {
        // The menu asks from the render thread and the loot engine from its
        // own, so the lookup is done under a lock and published once. g_api is
        // written before g_ok is set and never again.
        std::mutex        g_lock;
        Api               g_api{};
        std::atomic<bool> g_ok{false};
        bool  g_refused = false;   // found but unusable; that does not change while the game runs
        DWORD g_nextTry = 0;
        char  g_why[96] = "not installed";

        bool Bind(HMODULE h)
        {
            Api a{};
            bool all = true;
            const auto get = [&](const char* name) {
                const FARPROC p = GetProcAddress(h, name);
                if (!p) all = false;
                return p;
            };
            a.apiVersion     = reinterpret_cast<int (*)(void)>(get("PsmApiVersion"));
            a.storageName    = reinterpret_cast<const char* (*)(int)>(get("PsmStorageName"));
            a.storageKey     = reinterpret_cast<const char* (*)(int)>(get("PsmStorageKey"));
            a.getStatus      = reinterpret_cast<int (*)(PsmStatus*)>(get("PsmGetStatus"));
            a.getSettings    = reinterpret_cast<int (*)(PsmSettings*)>(get("PsmGetSettings"));
            a.getDefaults    = reinterpret_cast<int (*)(PsmSettings*)>(get("PsmGetDefaults"));
            a.applySettings  = reinterpret_cast<int (*)(const PsmSettings*, char*, int)>(get("PsmApplySettings"));
            a.reloadSettings = reinterpret_cast<int (*)(void)>(get("PsmReloadSettings"));
            a.getSize        = reinterpret_cast<int (*)(int, PsmSize*)>(get("PsmGetSize"));
            a.writeDump      = reinterpret_cast<void (*)(void)>(get("PsmWriteDump"));
            a.pauseInput     = reinterpret_cast<void (*)(uint32_t)>(get("PsmPauseInput"));
            a.keyText        = reinterpret_cast<int (*)(PsmKey, char*, int)>(get("PsmKeyText"));
            a.padText        = reinterpret_cast<int (*)(PsmPad, char*, int)>(get("PsmPadText"));
            a.fixedSlots     = reinterpret_cast<int (*)(int)>(GetProcAddress(h, "PsmFixedSlots"));   // optional
            a.getKeyBlock    = reinterpret_cast<int (*)(PsmKeyBlock*, int)>(GetProcAddress(h, "PsmGetKeyBlock"));   // optional
            a.applyKeyBlock  = reinterpret_cast<int (*)(const PsmKeyBlock*, char*, int)>(GetProcAddress(h, "PsmApplyKeyBlock"));   // optional
            if (!a.getKeyBlock || !a.applyKeyBlock) { a.getKeyBlock = nullptr; a.applyKeyBlock = nullptr; }   // both or neither
            a.getAutoStore   = reinterpret_cast<int (*)(PsmAutoStore*, int)>(GetProcAddress(h, "PsmGetAutoStore"));   // optional
            a.applyAutoStore = reinterpret_cast<int (*)(const PsmAutoStore*, char*, int)>(GetProcAddress(h, "PsmApplyAutoStore"));   // optional
            a.deposit        = reinterpret_cast<int (*)(uint16_t, int64_t)>(GetProcAddress(h, "PsmDeposit"));   // optional
            a.depositResults = reinterpret_cast<int (*)(PsmDepositResult*, int)>(GetProcAddress(h, "PsmDepositResults"));   // optional
            if (!a.getAutoStore || !a.applyAutoStore || !a.deposit || !a.depositResults)   // all four or none
                a.getAutoStore = nullptr, a.applyAutoStore = nullptr, a.deposit = nullptr, a.depositResults = nullptr;
            a.getNeverMove   = reinterpret_cast<int (*)(uint16_t*, int, int)>(GetProcAddress(h, "PsmGetNeverMove"));   // optional
            a.applyNeverMove = reinterpret_cast<int (*)(const uint16_t*, int, char*, int)>(GetProcAddress(h, "PsmApplyNeverMove"));   // optional
            if (!a.getNeverMove || !a.applyNeverMove || !a.getAutoStore)   // both or neither, and only beside auto-store
                a.getNeverMove = nullptr, a.applyNeverMove = nullptr;
            a.freePlay       = reinterpret_cast<int (*)(void)>(GetProcAddress(h, "PsmFreePlay"));   // optional
            a.getWeaponsPot   = reinterpret_cast<int (*)(PsmWeaponsPot*, int)>(GetProcAddress(h, "PsmGetWeaponsPot"));   // optional
            a.applyWeaponsPot = reinterpret_cast<int (*)(const PsmWeaponsPot*, char*, int)>(GetProcAddress(h, "PsmApplyWeaponsPot"));   // optional
            if (!a.getWeaponsPot || !a.applyWeaponsPot || !a.getAutoStore)   // both or neither, and only beside auto-store
                a.getWeaponsPot = nullptr, a.applyWeaponsPot = nullptr;
            if (!a.apiVersion)
            {
                snprintf(g_why, sizeof g_why, "found, but it has no interface (an older build)");
                return false;
            }
            const int v = a.apiVersion();
            if (v != PSM_API_VERSION)
            {
                snprintf(g_why, sizeof g_why, "interface %d, this build of Master Looter speaks %d", v, PSM_API_VERSION);
                return false;
            }
            if (!all)
            {
                snprintf(g_why, sizeof g_why, "interface %d but a function is missing", v);
                return false;
            }
            g_api = a;
            return true;
        }
    }

    const Api* Get()
    {
        if (g_ok.load(std::memory_order_acquire)) return &g_api;
        std::lock_guard<std::mutex> lk(g_lock);
        if (g_ok.load(std::memory_order_relaxed)) return &g_api;
        if (g_refused) return nullptr;
        const DWORD now = GetTickCount();
        if (g_nextTry && static_cast<LONG>(now - g_nextTry) < 0) return nullptr;
        g_nextTry = now + 2000;
        const HMODULE h = GetModuleHandleW(L"PrivateStorageMaster.asi");
        if (!h) { snprintf(g_why, sizeof g_why, "not installed"); return nullptr; }
        const bool ok = Bind(h);
        if (ok)
        {
            g_why[0] = 0;
            LOG("[storage] Private Storage Master found, interface %d%s", PSM_API_VERSION,
                g_api.deposit ? ", with auto-store" : "");
            g_ok.store(true, std::memory_order_release);
        }
        else { g_refused = true; LOG_ERR("[storage] Private Storage Master %s", g_why); }
        return ok ? &g_api : nullptr;
    }

    bool Deposit(uint16_t item, long long gained)
    {
        const Api* a = Get();
        return a && a->deposit && gained > 0 && a->deposit(item, static_cast<int64_t>(gained)) != 0;
    }

    int DepositResults(PsmDepositResult* out, int max)
    {
        const Api* a = Get();
        return a && a->depositResults ? a->depositResults(out, max) : 0;
    }

    int FreePlay()
    {
        const Api* a = Get();
        return a && a->freePlay ? (a->freePlay() != 0 ? 1 : 0) : -1;
    }

    bool AutoStoreOn()
    {
        const Api* a = Get();
        if (!a || !a->getAutoStore) return false;
        PsmAutoStore s{};
        s.size = sizeof s;
        return a->getAutoStore(&s, 0) && s.enabled && s.available;
    }

    const char* Why() { return g_why; }

    bool Installed()
    {
        Get();
        std::lock_guard<std::mutex> lk(g_lock);
        return g_ok.load(std::memory_order_relaxed) || g_refused;
    }
}
