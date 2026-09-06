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

    // species word -> (class, representative row or -1)
    struct Word { std::string klass; int row; int votes; };
    static std::unordered_map<std::string, Word> g_words;
    static const char* kStop[] = { "animal", "wild", "baby", "small", "large", "giant", "common", "red", "gray", "grey", "brown", "white",
                                   "black", "blue", "golden", "gold", "young", "old", "the", "and", "of", "big", "little", "great",
                                   "insect", "fish", "queen", "king", "mimic", "spotted", "striped", "eastern", "western", "northern", "southern" };
    static bool Stop(const std::string& w)
    {
        if (w.size() < 3) return true;
        bool digit = false; for (char c : w) if (c >= '0' && c <= '9') digit = true;
        if (digit) return true;
        for (const char* s : kStop) if (w == s) return true;
        return false;
    }
    static void AddWord(const std::string& w, const std::string& klass, int row)
    {
        if (Stop(w)) return;
        auto it = g_words.find(w);
        if (it == g_words.end()) { g_words[w] = { klass, row, 1 }; return; }
        if (it->second.klass == klass) { ++it->second.votes; if (it->second.row < 0) it->second.row = row; }
        else if (--it->second.votes <= 0) { it->second = { klass, row, 1 }; }   // contested word: last majority wins
    }
    static void Tokens(const std::string& text, std::vector<std::string>& out)
    {
        std::string w;
        for (char ch : text)
        {
            const bool alpha = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
            if (alpha) { w += static_cast<char>((ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch); }
            else if (!w.empty()) { out.push_back(w); w.clear(); }
        }
        if (!w.empty()) out.push_back(w);
    }
    static void BuildWords()
    {
        g_words.clear();
        std::vector<std::string> toks;
        for (size_t i = 0; i < g_rows.size(); ++i)
        {
            toks.clear(); Tokens(g_rows[i].name, toks); Tokens(g_rows[i].stringKey, toks);
            for (const std::string& w : toks) AddWord(w, g_rows[i].klass, static_cast<int>(i));
        }
        // Generic words the assets use that no single row may carry.
        struct G { const char* w; const char* k; };
        static const G generic[] = {
            { "fish", "fish" }, { "carp", "fish" }, { "trout", "fish" }, { "bass", "fish" }, { "salmon", "fish" }, { "catfish", "fish" }, { "eel", "fish" }, { "perch", "fish" }, { "pike", "fish" },
            { "butterfly", "insect" }, { "beetle", "insect" }, { "dragonfly", "insect" }, { "moth", "insect" }, { "bee", "insect" }, { "bug", "insect" }, { "mantis", "insect" },
            { "grasshopper", "insect" }, { "cicada", "insect" }, { "firefly", "insect" }, { "ladybug", "insect" }, { "cricket", "insect" }, { "locust", "insect" }, { "wasp", "insect" }, { "fly", "insect" },
            { "frog", "amphibian" }, { "toad", "amphibian" }, { "salamander", "amphibian" }, { "newt", "amphibian" },
            { "bird", "animal" }, { "goose", "animal" }, { "duck", "animal" }, { "chicken", "animal" }, { "hen", "animal" }, { "rooster", "animal" }, { "coot", "animal" },
            { "rat", "animal" }, { "mouse", "animal" }, { "squirrel", "animal" }, { "rabbit", "animal" }, { "hare", "animal" }, { "hedgehog", "animal" }, { "turtle", "animal" }, { "lizard", "animal" }, { "crab", "animal" },
        };
        for (const G& g : generic) { auto it = g_words.find(g.w); if (it == g_words.end()) g_words[g.w] = { g.k, -1, 1 }; }
    }

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
        if (g_loaded) BuildWords();
        return g_loaded;
    }

    const char* Classify(const char* text, const Creature** creature)
    {
        if (creature) *creature = nullptr;
        if (!text || !*text || g_words.empty()) return nullptr;
        std::vector<std::string> toks;
        Tokens(text, toks);
        const Word* best = nullptr;
        for (const std::string& w : toks)
        {
            auto it = g_words.find(w);
            if (it == g_words.end()) continue;
            if (!best || it->second.row >= 0 && best->row < 0) best = &it->second;   // a word naming a row beats a generic one
        }
        if (!best) return nullptr;
        if (creature && best->row >= 0) *creature = &g_rows[static_cast<size_t>(best->row)];
        static const char* kNames[] = { "insect", "fish", "animal", "amphibian" };
        for (const char* k : kNames) if (best->klass == k) return k;
        return nullptr;
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
