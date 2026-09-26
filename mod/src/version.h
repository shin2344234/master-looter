#pragma once
// Keep these three in step with ML_VERSION. The resource block below is
// built from them, so the file properties Windows shows cannot drift away
// from the version the plugin reports. They did: every release from 1.4.0
// to 1.6.1 shipped a PE version resource still reading 1.3.0.
#define ML_VERSION_MAJOR 1
#define ML_VERSION_MINOR 6
#define ML_VERSION_PATCH 42
#define ML_VERSION "1.6.42"
// What a test build calls itself. Every log opens with this, so a log from a
// build handed to one reporter cannot be read as a log from the release.
// Empty on anything that ships, and set from the build line rather than by
// editing it here, because package.py, vtscan.py, publish-nexus.ps1 and
// announce-discord.py all read ML_VERSION out of this file with a regex and
// must keep seeing exactly the released number.
//   mod\build.bat tag equipcheck   ->  Master Looter v1.6.17-equipcheck
#ifndef ML_BUILD_TAG
#define ML_BUILD_TAG ""
#endif
#define ML_VERSION_FULL ML_VERSION ML_BUILD_TAG
#define ML_GAME_BUILD "2.03.00"
#define ML_MOD_PAGE "https://www.nexusmods.com/crimsondesert/mods/3402"
#define ML_SOURCE_URL "https://github.com/shin2344234/master-looter"
