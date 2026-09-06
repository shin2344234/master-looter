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
        int   keyToggle      = 0x79;   // VK_F10: auto-loot on/off
        int   keyBurst       = 0x7A;   // VK_F11: loot everything in range once
        int   keyWatch       = 0x24;   // VK_HOME: watch mode, menu stays up while you play
        // pace
        int   scansPerSec    = 5;      // how often the scene is read (1..30)
        int   perScan        = 0;      // objects taken per scan, 0 = no limit
        int   burstPerKey    = 0;      // objects per burst press, 0 = all
        int   retryAfterMs   = 6000;   // before the same object may be tried again
        // what to collect
        bool  lootCorpses    = true;
        bool  pickUpItems    = true;
        bool  gatherPlants   = true;   // herbs, flowers, mushrooms and seeds; crops follow pickUpItems
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
        // safety and filters
        bool  lootOwned      = false;  // take goods the game would call stealing
        bool  skipQuestItems = true;
        bool  skipNoSell     = false;
        int   minValueCopper = 0;      // 0 = no value floor
        bool  takeUnknownItems = true; // items our database cannot name
        bool  debugLog       = false;
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
