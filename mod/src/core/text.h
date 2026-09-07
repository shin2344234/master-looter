#pragma once
#include <string>

namespace ml
{
    // Menu text, translatable.
    //
    // A string is its own key: T("Load") returns the translation of "Load" if
    // one is loaded, and "Load" itself if not. That has two consequences worth
    // the design. A half-finished translation still works, every untranslated
    // line simply staying English rather than showing a missing-key marker. And
    // with no language selected nothing is looked up differently from before,
    // so the English build behaves exactly as it did.
    //
    // Every string asked for is remembered, so the mod can write out a template
    // holding precisely what the interface uses, rather than a translator
    // working from a list someone maintained by hand and got wrong.
    namespace Text
    {
        // MasterLooter.<lang>.txt, tab separated, one record per line: the
        // English on the left, the translation on the right, with \n for a
        // line break. Lines starting with # are comments. An empty right-hand
        // side means not translated yet.
        //
        // Read from beside the plugin when a file of that name is there, and
        // otherwise from the copy built into the plugin, so a contributed
        // language works under a mod manager that deploys the .asi on its own
        // while a translator can still iterate without a rebuild.
        bool Load(const char* lang);          // "" or "en" unloads and returns to English
        const char* Language();               // what is loaded, empty when English
        int  Count();                         // translated strings in use
        int  Rejected();                      // dropped because their placeholders did not match
        bool FromFile();                      // read from disk rather than from inside the plugin

        // The languages that ship inside the plugin, so the menu can offer
        // them instead of expecting someone to know the code. Named in the
        // language itself: a reader who needs the Chinese cannot necessarily
        // read the word "Chinese".
        struct Lang { const char* code; const char* name; const char* credit; };
        const Lang* BuiltIn(int& count);
        const Lang* Find(const char* code);   // null when not a built-in one

        const char* Get(const char* english);

        // Every translated string currently loaded, handed over one at a time,
        // so the font atlas can be asked for exactly the characters they use.
        // The English keys are not passed: they are ASCII and the default
        // range already covers them.
        void ForEachTranslation(void (*fn)(const char*, void*), void* user);

        // Writes MasterLooter.template.txt: every string the interface has
        // asked for this session, ready to be translated. Open every tab first,
        // since a string is only listed once it has been drawn.
        bool WriteTemplate();
        int  Seen();
    }
}

// Wraps a string for translation. Short, because it appears a few hundred times
// in the menu and anything longer would bury the text it wraps. Not T: the
// Windows headers use that as a template parameter name and a macro of that
// name breaks winnt.h.
#define TR(s) ::ml::Text::Get(s)
