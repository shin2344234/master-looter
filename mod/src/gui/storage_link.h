#pragma once
#include "psm_api.h"

// Private Storage Master, when it is installed beside this mod.
//
// Nothing here links against it. The plugin is looked up by module name and
// each function by name, and all of it is used only when the interface version
// matches the header this build was compiled with. Without it the Storage tab
// is simply not drawn.
namespace ml::psm
{
    struct Api
    {
        int         (*apiVersion)(void);
        const char* (*storageName)(int);
        const char* (*storageKey)(int);
        int         (*getStatus)(PsmStatus*);
        int         (*getSettings)(PsmSettings*);
        int         (*getDefaults)(PsmSettings*);
        int         (*applySettings)(const PsmSettings*, char*, int);
        int         (*reloadSettings)(void);
        int         (*getSize)(int, PsmSize*);
        void        (*writeDump)(void);
        void        (*pauseInput)(uint32_t);
        int         (*keyText)(PsmKey, char*, int);
        int         (*padText)(PsmPad, char*, int);
        int         (*fixedSlots)(int);   // optional, null on a plugin built before it
        int         (*getKeyBlock)(PsmKeyBlock*, int);                // optional, 1.0.1 and later
        int         (*applyKeyBlock)(const PsmKeyBlock*, char*, int); // optional, 1.0.1 and later
        // Auto-store, optional and all four or none.
        int         (*getAutoStore)(PsmAutoStore*, int);
        int         (*applyAutoStore)(const PsmAutoStore*, char*, int);
        int         (*deposit)(uint16_t, int64_t);
        int         (*depositResults)(PsmDepositResult*, int);
        // The never-move list, optional and both or neither.
        int         (*getNeverMove)(uint16_t*, int, int);
        int         (*applyNeverMove)(const uint16_t*, int, char*, int);
        // Whether the player is in free play now, optional.
        int         (*freePlay)(void);
        // The Weapon's Pot (MJ7L's Weapon's Pot mod) as a store-loot target, optional
        // and both or neither, and only beside auto-store.
        int         (*getWeaponsPot)(PsmWeaponsPot*, int);
        int         (*applyWeaponsPot)(const PsmWeaponsPot*, char*, int);
    };

    // Null until the plugin is found with a matching interface. Cheap to call
    // every frame: a miss is retried every two seconds.
    const Api* Get();

    // What the lookup found, for the Status line: "not installed", "interface 2,
    // this build speaks 1", and so on. Empty when Get() succeeds.
    const char* Why();

    // Found in the process, usable or not. The Storage tab shows either way,
    // with the reason when it cannot be used.
    bool Installed();

    // For the loot engine, from its own thread. False when the plugin or its
    // auto-store exports are missing, or it did not queue the deposit (auto-store
    // off, the move missing on this game version, or its queue full).
    bool Deposit(uint16_t item, long long gained);
    // Finished deposits, oldest first; 0 when there is nothing or no plugin.
    int  DepositResults(PsmDepositResult* out, int max);
    // 1 in free play, 0 in a shop, a menu, a storage or anything else, and -1
    // when Private Storage Master does not say.
    int  FreePlay();
    // Auto-store switched on and able to move things on this game version.
    bool AutoStoreOn();
}
