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
    static const char* g_source = "none";

    // species word -> (class, representative row or -1)
    struct Word { std::string klass; int row; int votes; bool shared; };
    static std::unordered_map<std::string, Word> g_words;
    static const char* kStop[] = { "animal", "wild", "baby", "small", "large", "giant", "common", "red", "gray", "grey", "brown", "white",
                                   "black", "blue", "golden", "gold", "young", "old", "the", "and", "of", "big", "little", "great",
                                   "insect", "fish", "queen", "king", "mimic", "spotted", "striped", "eastern", "western", "northern", "southern",
                                   "normal", "middle", "medium", "tiny", "huge", "verybig", "unique", "female", "male", "kid", "dead", "fat",
                                   "skinny", "sick", "trained", "domestic", "battle", "ride", "boss", "swarm", "colony", "cluster", "pack" };
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
        if (it == g_words.end()) { g_words[w] = { klass, row, 1, false }; return; }
        if (it->second.klass == klass)
        {
            ++it->second.votes;
            if (it->second.row < 0) it->second.row = row;
            else if (g_rows[static_cast<size_t>(it->second.row)].name != g_rows[static_cast<size_t>(row)].name) it->second.shared = true;
        }
        else if (--it->second.votes <= 0) { it->second = { klass, row, 1, false }; }   // contested word: last majority wins
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
    // Only the head noun of each English name votes ("Red Tonguesole" ->
    // tonguesole, "Tree Frog" -> frog). Key tokens are left out on purpose:
    // "Animal_Normal_Dover_Sole" taught the map that "normal" meant fish, and
    // every creature's effect asset is called cd_effectmonster_normal, so a
    // whole meadow of butterflies came up as Red Tonguesole (v0.3.8).
    static void BuildWords()
    {
        g_words.clear();
        std::vector<std::string> toks;
        for (size_t i = 0; i < g_rows.size(); ++i)
        {
            if (g_rows[i].itemRow < 0) continue;   // rows without a catch item (monsters, mounts, wildlife) do not vote
            toks.clear(); Tokens(g_rows[i].name, toks);
            for (size_t t = toks.size(); t-- > 0;)
                if (!Stop(toks[t])) { AddWord(toks[t], g_rows[i].klass, static_cast<int>(i)); break; }
        }
        // Generic words the assets use that no single row may carry.
        struct G { const char* w; const char* k; };
        static const G generic[] = {
            { "fish", "fish" }, { "carp", "fish" }, { "trout", "fish" }, { "bass", "fish" }, { "salmon", "fish" }, { "catfish", "fish" }, { "eel", "fish" }, { "perch", "fish" }, { "pike", "fish" },
            // Model keys and movement flags the engine strings carry. The small bugs have
            // no character model: their key is cd_effectmonster_normal and they move AirOnly.
            { "effectmonster", "insect" }, { "aironly", "insect" },
            { "underwateronly", "fish" }, { "animalwallupwalldownnowater", "animal" },
            { "butterfly", "insect" }, { "beetle", "insect" }, { "dragonfly", "insect" }, { "moth", "insect" }, { "bee", "insect" }, { "bug", "insect" }, { "mantis", "insect" },
            { "grasshopper", "insect" }, { "cicada", "insect" }, { "firefly", "insect" }, { "ladybug", "insect" }, { "cricket", "insect" }, { "locust", "insect" }, { "wasp", "insect" }, { "fly", "insect" },
            { "crab", "seafood" }, { "shrimp", "seafood" }, { "crayfish", "seafood" }, { "lobster", "seafood" }, { "squid", "seafood" }, { "starfish", "seafood" }, { "seahorse", "seafood" }, { "clam", "seafood" },
            { "frog", "amphibian" }, { "toad", "amphibian" }, { "salamander", "amphibian" }, { "axolotl", "amphibian" }, { "newt", "amphibian" },
            { "bird", "animal" }, { "smallbird", "animal" }, { "embriza", "animal" }, { "goose", "animal" }, { "duck", "animal" }, { "chicken", "animal" }, { "hen", "animal" }, { "rooster", "animal" }, { "coot", "animal" },
            { "rat", "animal" }, { "squirrel", "animal" }, { "crow", "animal" }, { "pigeon", "animal" }, { "rabbit", "animal" }, { "hare", "animal" }, { "hedgehog", "animal" }, { "turtle", "animal" }, { "lizard", "animal" }, { "snake", "animal" },
        };
        for (const G& g : generic) { auto it = g_words.find(g.w); if (it == g_words.end()) g_words[g.w] = { g.k, -1, 1, false }; }
    }

    bool Load()
    {
        std::string text;
        bool fromFile = false;
        if (!Paths::ReadDataText(L"MasterLooter.creatures.tsv", L"ML_CREATURES_TSV", text, &fromFile)) return false;
        g_source = fromFile ? "file next to the plugin" : "built into the plugin";
        bool header = true;
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t nl = text.find('\n', pos);
            if (nl == std::string::npos) nl = text.size();
            const std::string line = text.substr(pos, nl - pos);
            pos = nl + 1;
            if (header) { header = false; continue; }
            std::string cols[5]; int c = 0;
            for (const char* p = line.c_str(); *p && c < 5; ++p)
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
        g_loaded = !g_rows.empty();
        if (g_loaded) BuildWords();
        return g_loaded;
    }

    Match Classify(const char* text)
    {
        Match m;
        if (!text || !*text || g_words.empty()) return m;
        std::vector<std::string> toks;
        Tokens(text, toks);
        const Word* best = nullptr;
        const std::string* word = nullptr;
        for (const std::string& w : toks)
        {
            auto it = g_words.find(w);
            if (it == g_words.end()) continue;
            if (!best || (it->second.row >= 0 && best->row < 0)) { best = &it->second; word = &it->first; }   // a word naming a row beats a generic one
        }
        if (!best) return m;
        static const char* kNames[] = { "insect", "fish", "seafood", "animal", "amphibian" };
        for (const char* k : kNames) if (best->klass == k) m.klass = k;
        if (!m.klass) return m;
        m.word = *word;
        if (best->row >= 0) { m.row = &g_rows[static_cast<size_t>(best->row)]; m.exact = !best->shared; }
        return m;
    }

    bool Loaded() { return g_loaded; }
    const char* Source() { return g_source; }
    int  Count()  { return static_cast<int>(g_rows.size()); }
    const std::vector<Creature>& All() { return g_rows; }

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
