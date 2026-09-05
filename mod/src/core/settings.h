#pragma once
#include <cstdint>
#include <map>
#include <string>

namespace ml
{
    struct Config
    {
        // [MasterLooter]
        bool  enabled        = true;
        int   menuKey        = 0x2D;   // VK_INSERT
        bool  showHud        = true;
        float lootRange      = 15.0f;  // metres
        int   maxLootsPerSec = 5;
        bool  lootCorpses    = true;
        bool  pickUpItems    = true;
        bool  gatherPlants   = true;
        bool  catchInsects   = true;
        bool  lootContainers = true;
        int   minValueCopper = 0;      // 0 = no value floor
        bool  skipQuestItems = true;
        bool  skipNoSell     = false;

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
    }
}
