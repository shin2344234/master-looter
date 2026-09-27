/* Private Storage Master, in-process interface.
 *
 * Another plugin in the game process (Master Looter's Storage tab) finds
 * PrivateStorageMaster.asi with GetModuleHandleW and each function below with
 * GetProcAddress. Check PsmApiVersion() first and use nothing if it differs
 * from PSM_API_VERSION. Every struct starts with its own size; set it before
 * passing one in, and a call with a size it does not know returns 0.
 *
 * Plain C, no allocation across the boundary, strings copied into the
 * caller's buffers. Everything is safe to call from any thread, every frame.
 */
#pragma once
#include <stdint.h>

#ifndef PSM_API
#define PSM_API   /* the plugin defines it as dllexport while building */
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define PSM_API_VERSION 1
#define PSM_STORAGES    9       /* see PsmStorageName for the order */
#define PSM_MAX_SLOTS   1460

#define PSM_MOD_CTRL  1
#define PSM_MOD_SHIFT 2
#define PSM_MOD_ALT   4

typedef struct PsmKey
{
    uint8_t vk;     /* Windows virtual-key code, 0 for none */
    uint8_t mods;   /* PSM_MOD_* that must be held, and no others */
} PsmKey;

typedef struct PsmPad
{
    uint16_t hold;  /* XINPUT_GAMEPAD_* held first */
    uint16_t press; /* the single button that fires, 0 for none */
} PsmPad;

typedef struct PsmSettings
{
    uint32_t size;                          /* sizeof(PsmSettings) */
    int32_t  enabled;                       /* next launch */
    int32_t  debugLog;
    PsmKey   keys[PSM_STORAGES];
    PsmPad   pads[PSM_STORAGES];
    PsmKey   dumpKey;
    int32_t  leaveCapacityAlone;            /* next launch */
    int32_t  slots[PSM_STORAGES];           /* next launch; 0 keeps the game's size */
    int32_t  privateStorageExpansions;      /* next launch; -1 learns them from the save */
} PsmSettings;

typedef struct PsmStatus
{
    uint32_t size;                          /* sizeof(PsmStatus) */
    int32_t  enabled;                       /* this launch */
    int32_t  storageReady;                  /* keys and combos are live */
    int32_t  capacityHooked;                /* sizes were applied this launch */
    int32_t  keyWindowFound;
    int32_t  padSlot;                       /* -1 with no controller */
    int32_t  openStorage;                   /* -1 when none */
    int32_t  oldModLoaded;                  /* PrivateStorageAnywhere.asi is in the process */
    int32_t  imported;                      /* settings came from PrivateStorageAnywhere.ini */
    int32_t  restartNeeded;                 /* a next-launch setting differs from this launch */
    int32_t  learnedExpansions;             /* Private Storage slots from expansions and story */
    char     version[32];
    char     gameVersion[16];
    char     lastError[256];
} PsmStatus;

typedef struct PsmSize
{
    uint32_t size;                          /* sizeof(PsmSize) */
    int32_t  known;                         /* the game has read this storage's record */
    int32_t  gameDefault;
    int32_t  gameMax;
    int32_t  appliedDefault;                /* this launch */
    int32_t  liveCapacity;                  /* -1 with no save loaded */
    int32_t  liveUsed;                      /* filled slots, -1 with no save loaded */
    int32_t  slotArray;
    int32_t  extras;                        /* expansions bought plus story slots */
} PsmSize;

PSM_API int         PsmApiVersion(void);
PSM_API const char* PsmStorageName(int storage);    /* in-game name, "Private Storage" */
PSM_API const char* PsmStorageKey(int storage);     /* ini prefix, "PrivateStorage" */
PSM_API int         PsmGetStatus(PsmStatus* out);
PSM_API int         PsmGetSettings(PsmSettings* out);
PSM_API int         PsmGetDefaults(PsmSettings* out);
/* Applies what can change live, saves the ini. 0 with a reason in why. */
PSM_API int         PsmApplySettings(const PsmSettings* in, char* why, int whyLen);
PSM_API int         PsmReloadSettings(void);
PSM_API int         PsmGetSize(int storage, PsmSize* out);
PSM_API void        PsmWriteDump(void);
/* Ignore storage keys and combos for this long; call it every frame a menu has input. */
PSM_API void        PsmPauseInput(uint32_t ms);
/* A size that is not a setting, or 0: the Collectibles Chest at 958, and Bird
 * Feed at 50 from PSM 1.1.6. Added after interface 1 shipped to Master Looter,
 * so look it up as optional. */
PSM_API int         PsmFixedSlots(int storage);
/* 1 while keys are held back from the game: HideKeysWithModifier is on and a
 * modifier some binding uses is down. Any thread, no lock, no logging, and it
 * lags the keyboard by up to one poll (16 ms). Optional, look it up by name. */
PSM_API int         PsmHidingKeys(void);

/* HideKeysWithModifier and its toggle key. They came after PsmSettings shipped,
 * which cannot grow, so they have their own struct and calls. Optional, look
 * them up by name. */
typedef struct PsmKeyBlock
{
    uint32_t size;                          /* sizeof(PsmKeyBlock) */
    int32_t  on;                            /* HideKeysWithModifier */
    PsmKey   toggleKey;                     /* HideKeysToggleKey, vk 0 for none */
} PsmKeyBlock;
/* The live values, or the defaults when defaults is nonzero. */
PSM_API int         PsmGetKeyBlock(PsmKeyBlock* out, int defaults);
/* Applies at once and saves the ini. 0 with a reason in why. A toggle key
 * that clashes with another binding is turned off; read it back to see. */
PSM_API int         PsmApplyKeyBlock(const PsmKeyBlock* in, char* why, int whyLen);

/* Loot straight into storage. Optional, look every one up by name.
 *
 * After a pickup lands in the bag, a looting mod calls PsmDeposit with the
 * item number (bag slot +0x08) and how many just arrived. On the game's main
 * thread, within a few frames, PSM offers the item to each storage that is on,
 * in a fixed order (Collectibles Chest, Abyss gear, Gatherables Chest, Kuku
 * Cooler, Bird Feed, Camp Straw, Wardrobe, then Private Storage), through the
 * game's own move check, and sends it to the first that takes it. PsmDeposit
 * refuses a pickup reported outside free play or while a storage is open, and a
 * queued one that play leaves free play before it moves waits for it to return. */
typedef struct PsmAutoStore
{
    uint32_t size;                          /* sizeof(PsmAutoStore) */
    int32_t  enabled;                       /* the master switch, off by default */
    int32_t  storages[PSM_STORAGES];        /* 1 = may receive loot; Camp Provisions is always 0 */
    int32_t  onlyGained;                    /* 1 = move what was picked up, 0 = the whole stack */
    int32_t  available;                     /* read only: the move and PSM's frame hook are both in */
} PsmAutoStore;
/* The live values, or the defaults when defaults is nonzero. */
PSM_API int         PsmGetAutoStore(PsmAutoStore* out, int defaults);
/* Applies at once and saves the ini. 0 with a reason in why. */
PSM_API int         PsmApplyAutoStore(const PsmAutoStore* in, char* why, int whyLen);

/* AutoStoreNeverMove: item numbers auto-store never moves. The default is every
 * currency (silver, the pouches, gold bars, camp funds, tokens and the rest). */
#define PSM_NEVER_MOVE_MAX 64
/* Copies the live list, or the default when defaults is nonzero, into items.
 * Returns how many were copied, at most max. */
PSM_API int         PsmGetNeverMove(uint16_t* items, int max, int defaults);
/* Replaces the list, sorted and without repeats, and saves the ini. count may be
 * 0 to move everything. 0 with a reason in why when count is over the maximum. */
PSM_API int         PsmApplyNeverMove(const uint16_t* items, int count, char* why, int whyLen);

/* Any thread. 1 when queued; 0 when auto-store is off, this game version lacks
 * the move, the player is not in free play (a shop, a craft, a menu, a load),
 * a storage is open, or the queue is full. Nothing is queued on 0, so call it
 * as the pickup lands. */
PSM_API int         PsmDeposit(uint16_t item, int64_t gained);
/* Any thread. 1 when the player is in free play right now, by the same test
 * PsmDeposit uses: the last frame, under a second old, was in free play (not a
 * shop, craft, menu, load or cutscene) and no PSM storage is open. 0 otherwise,
 * including when PSM's frame hook is not in. */
PSM_API int         PsmFreePlay(void);

#define PSM_DEPOSIT_STORED              1   /* moved is how many went into storage */
#define PSM_DEPOSIT_NO_STORAGE_TAKES_IT 2
#define PSM_DEPOSIT_STORAGE_FULL        3   /* every storage that takes it is full */
#define PSM_DEPOSIT_NEVER_MOVED         4   /* on AutoStoreNeverMove, silver by default */
#define PSM_DEPOSIT_LOCKED              5   /* the player locked that stack */
#define PSM_DEPOSIT_NOT_IN_BAG          6   /* it never showed up in the bag */
#define PSM_DEPOSIT_OFF                 7
#define PSM_DEPOSIT_BUSY                8   /* too many at once */

typedef struct PsmDepositResult
{
    uint16_t item;
    int16_t  storage;                       /* PSM storage index, -1 when it stayed in the bag */
    int32_t  reason;                        /* PSM_DEPOSIT_* */
    int64_t  moved;
} PsmDepositResult;
/* Copies out and forgets finished deposits, oldest first. Returns how many. */
PSM_API int         PsmDepositResults(PsmDepositResult* out, int max);
/* "Ctrl+F1", "LB+LS", "None". */
PSM_API int         PsmKeyText(PsmKey key, char* out, int outLen);
PSM_API int         PsmPadText(PsmPad pad, char* out, int outLen);

#ifdef __cplusplus
}
#endif
