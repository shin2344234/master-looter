#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace ml
{
    struct Config
    {
        // [MasterLooter]
        bool  enabled        = true;   // auto-loot on
        int   menuKey        = 0x2D;   // VK_INSERT
        bool  showHud        = true;   // brief on-screen notice when auto-loot is toggled
        bool  notifyBagFull  = true;   // say so on screen when things stop reaching the bag
        // Wrap the swapchain so the overlay draws under DLSS frame generation.
        // Off falls back to the plain present hook, which costs the overlay only
        // when frame generation is on, and takes this mod off a path other
        // overlay mods also patch. Read once at startup.
        bool  wrapSwapChain  = true;
        // Install the DirectX hooks at all. Off means no menu, no overlay and
        // no ExecuteCommandLists detour, with looting untouched, so a load-order
        // collision can be split into "the render layer" and "everything else"
        // in one run. Added to bisect issue 34: with the wrapper off the game
        // still hung alongside Crimson Route and RenoDX, and loaded with this
        // plugin absent, so the difference had to be in what was left.
        bool  hookDX12       = true;
        // Arm D3D12 DRED (breadcrumbs and page-fault reporting on device
        // removal). Off by default since 1.6.5: it is diagnostic only, it runs
        // before the game has made a device, and nothing in looting needs it.
        bool  enableDred     = false;
        // Menu language: empty or "en" is English, otherwise the suffix of a
        // MasterLooter.<lang>.txt file next to the plugin.
        std::string language;
        int   keyToggle      = 0x79;   // VK_F10: auto-loot on/off
        int   keyBurst       = 0x7A;   // VK_F11: loot everything in range once
        int   keyWatch       = 0x24;   // VK_HOME: watch mode, menu stays up while you play
        // Take-owned is the one switch worth a key. Leaving it on earns a
        // bounty, so it wants turning on for a moment and off again, and the
        // menu is the wrong shape for that. Unbound by default: every spare key
        // already does something in this game and picking one for you is worse
        // than making you choose. Issue #37.
        int   keyOwned       = 0;
        // Pad shortcuts: an XINPUT button mask each, two buttons or more, 0 when
        // unbound. Two at once because every single button is already the game's.
        // Hook XInput so the menu can take the pad while it is open, and so a
        // two-button shortcut can be spotted. Turning this off gives up both
        // and leaves the controller entirely alone.
        //
        // Here because it is the only part of this mod that touches the pad,
        // and when a controller misbehaves the first question is whether this
        // is why. Answering that should not need a custom build.
        bool  hookXInput     = true;
        unsigned padMenu     = 0;
        unsigned padToggle   = 0;
        unsigned padOwned    = 0;
        unsigned padBurst    = 0;
        unsigned padWatch    = 0;
        // pace
        int   scansPerSec    = 5;      // how often the scene is read (1..30)
        int   perScan        = 0;      // objects taken per scan, 0 = no limit
        int   burstPerKey    = 0;      // objects per burst press, 0 = all
        int   retryAfterMs   = 6000;   // before the same object may be tried again
        // what to collect
        bool  lootCorpses    = true;
        bool  pickUpItems    = true;
        bool  gatherPlants   = true;   // herbs, flowers, mushrooms and seeds
        // Vegetables, fruit and grain, on the plant or lying loose. Their own
        // switch since 1.4.0: they used to answer to pickUpItems, which meant
        // turning Plants off still left 44 of the game's 72 collection sockets
        // being harvested, sweet potatoes and barley among them.
        bool  gatherCrops    = true;
        bool  gatherOre      = true;
        bool  gatherStone    = true;
        bool  gatherWood     = true;
        bool  gatherUnknown  = false;  // nodes whose yield has not been seen yet (learned from what you gather by hand)
        bool  catchInsects   = true;
        bool  catchFish      = true;
        bool  catchAnimals   = true;   // chickens, coots, frogs and other small animals
        bool  lootContainers = false;
        bool  lootFurniture  = false;
        // ranges in metres
        float scanRange      = 40.0f;
        float lootRange      = 15.0f;
        float gatherRange    = 6.0f;   // the game ignores gathers from far away
        float catchRange     = 8.0f;
        float corpseRange    = 12.0f;
        float minRange       = 0.35f;
        // node arming
        bool  autoArm        = true;
        float armRange       = 8.0f;
        bool  armContainers  = true;
        // Mine an ore vein where it stands instead of waiting for it to be
        // broken. Off leaves veins alone and picks up only the chunks.
        bool  gatherVeins    = true;
        // Draw water by driving the well's own winch. Off by default: it is a
        // replay of a captured transition sequence, not an event the game
        // offers, and a well it gets wrong is a well left visibly broken.
        bool  drawWells      = false;
        // Break an ore vein rather than gathering it. Breaking makes the game
        // spill the contents through its own drop path and retire the node, so
        // the vein disappears the way it does by hand. That is the reason to
        // keep it on.
        //
        // It does NOT pay an equipped tool's Mining Yield Up. This comment used
        // to claim it did, which was never verified and is wrong: lsimo measured
        // ten ore by hand with a refined Knuckledrill against five auto-mined at
        // the same multiplier. The tool's share travels a path the game runs
        // from a real weapon swing, and the mod swings nothing. Issue #31.
        bool  breakOre       = true;
        // Extra ore from a vein the mod breaks, 0 to 10, off by default.
        //
        // A vein pays more when a good mining tool swings at it, and the mod
        // does not swing: it drives the node's own break sequence, and that
        // makes the game walk one drop row where a real swing walks two.
        // Measured on a refined drill: three ore by hand, one from the mod,
        // every time, with both inputs to the game's own count byte-identical.
        //
        // Rather than forge a swing, this tells the game the vein is worth more
        // and lets its own drop path spawn the difference. Nothing here copies
        // an item or writes to a save. It only ever applies to a vein this mod
        // broke, so mining by hand is untouched and the two cannot stack.
        //
        // Set it to what your tool pays by hand, minus one. The hook is
        // installed either way and reads this on every call, so the slider
        // takes effect without a restart.
        int   oreBonus       = 0;
        // safety and filters
        bool  lootOwned      = false;  // take goods the game would call stealing
        bool  skipQuestItems = true;
        bool  skipNoSell     = false;
        int   minValueCopper = 0;      // 0 = no value floor
        bool  takeUnknownItems = true; // items our database cannot name
        // A pet or a companion loots whatever the game lets it, and nothing in
        // the game looks at the item. On, whatever one of them picks up that
        // the item rules would have refused is deleted as it lands. Issue #32.
        bool  petFilter      = false;
        bool  debugLog       = false;
        // With the verbose log on, delete two of this item once per session
        // through the same path the pet filter uses, and log the inventory
        // layout on the way. This is how the delete was proved on 2760 and
        // how it is checked again after a game patch. Not in the menu.
        std::string deleteTestName;
        int   configVersion  = 2;      // bumps when a default changes in a way old files should follow

        // [Classes] class -> 1 loot / 0 skip. Absent means loot.
        std::map<std::string, int> classRule;
        // [Tags] tag -> 1 always loot / -1 never loot. Absent means no opinion.
        std::map<std::string, int> tagRule;
        // [Items] item key -> 1 always / -1 never.
        std::map<uint32_t, int> itemRule;
    };

    namespace Settings
    {
        Config& Get();
        void Load();                 // read MasterLooter.ini (defaults when missing)
        void Save();                 // write it; a no-op before Claim()
        void Claim();                // this process owns the file; writes defaults if missing
        void MarkDirty();            // save soon (debounced)
        void Poll();                 // once per frame: debounced save, reload after an external edit

        // Presets: the whole config, rules included, kept as a named file in
        // MasterLooter.presets next to the plugin. Loading one replaces
        // everything and rewrites the live file, so a swap survives a restart.
        int  ListPresets(std::string* out, int max);   // names, sorted; returns how many
        bool SavePreset(const char* name);
        bool LoadPreset(const char* name);
        bool DeletePreset(const char* name);
        std::string CleanPresetName(const char* raw);  // what the name will become as a file

        bool PresetExists(const char* name);

        // Backups: one dated file each in MasterLooter.backups next to the
        // plugin. One is written every time the game starts, before anything
        // can change the settings, and a migration leaves one of its own. The
        // oldest are dropped once there are more than a dozen.
        int  ListBackups(std::string* out, int max);   // stamps, newest first
        std::string BackupLabel(const char* stamp);    // "20260906-0850" -> "2026-09-06 08:50"
        std::string NextBackupStamp();                 // what BackupNow would write
        bool BackupExists(const char* stamp);
        bool BackupNow();
        bool RestoreBackup(const char* stamp);
        bool DeleteBackup(const char* stamp);
        const std::wstring& Path();
        int  Generation();           // bumps on every load
        const char* KeyName(int vk); // human name for a virtual-key code

        // The live Config is edited on the render thread (menu, hot reload) and
        // read on the loot worker. The render thread holds this while it draws;
        // the worker copies under it once per pass instead of reading live maps.
        std::recursive_mutex& Mutex();
        Config Snapshot();
    }
}
