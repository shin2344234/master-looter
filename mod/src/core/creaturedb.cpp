#include "creaturedb.h"

#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "paths.h"

namespace ml::CreatureDb
{
    static std::vector<Creature> g_rows;
    static std::unordered_map<std::string, size_t> g_byKey;
    static bool g_loaded = false;

    bool Load()
    {
        FILE* f = _wfopen(Paths::File(L"MasterLooter.creatures.tsv").c_str(), L"rb");
        if (!f) return false;
        char line[512];
        bool header = true;
        while (fgets(line, sizeof line, f))
        {
            if (header) { header = false; continue; }
            std::string cols[5]; int c = 0;
            for (char* p = line; *p && c < 5; ++p)
            {
                if (*p == '\t') { ++c; continue; }
                if (*p == '\r' || *p == '\n') break;
                cols[c] += *p;
            }
            if (c < 4 || cols[1].empty()) continue;
            Creature cr;
            cr.key = static_cast<uint32_t>(strtoul(cols[0].c_str(), nullptr, 10));
            cr.stringKey = cols[1]; cr.name = cols[2];
            cr.itemRow = atoi(cols[3].c_str());
            cr.klass = cols[4];
            g_byKey[cr.stringKey] = g_rows.size();
            g_rows.push_back(std::move(cr));
        }
        fclose(f);
        g_loaded = !g_rows.empty();
        return g_loaded;
    }

    bool Loaded() { return g_loaded; }
    int  Count()  { return static_cast<int>(g_rows.size()); }

    const Creature* InText(const char* text)
    {
        if (!text || !*text) return nullptr;
        const Creature* best = nullptr;
        for (const Creature& c : g_rows)
        {
            if (c.stringKey.size() < 8) continue;
            if (!strstr(text, c.stringKey.c_str())) continue;
            if (!best || c.stringKey.size() > best->stringKey.size()) best = &c;
        }
        return best;
    }

    const Creature* ByKey(const char* stringKey)
    {
        if (!stringKey || !*stringKey) return nullptr;
        auto it = g_byKey.find(stringKey);
        return it == g_byKey.end() ? nullptr : &g_rows[it->second];
    }
}
