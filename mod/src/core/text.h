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
        // MasterLooter.<lang>.txt next to the plugin, tab separated, one record
        // per line: the English on the left, the translation on the right, with
        // \n for a line break. Lines starting with # are comments. An empty
        // right-hand side means not translated yet.
        bool Load(const char* lang);          // "" or "en" unloads and returns to English
        const char* Language();               // what is loaded, empty when English
        int  Count();                         // translated strings in use
        int  Rejected();                      // dropped because their placeholders did not match

        const char* Get(const char* english);

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
