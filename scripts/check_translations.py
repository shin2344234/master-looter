"""Check every menu translation against the template.

Reads docs/MasterLooter.template.txt for the set of English strings the menu
uses, then holds each mod/data/MasterLooter.<lang>.txt up against it the way
Text::Load in the plugin does: unescape both sides, split on the first tab,
and refuse any line whose printf placeholders differ from the English.

Reports, per file: strings missing or left empty, lines whose English no longer
matches anything in the template (stale), lines the plugin would drop for a
placeholder mismatch, and a few advisories (a translation identical to its
English, a different count of line breaks, a byte order mark). Exits non-zero
when any file has a missing string or a line the plugin would reject, so it
can gate a commit.

    py -3 scripts/check_translations.py            every language
    py -3 scripts/check_translations.py de fr      only these
    py -3 scripts/check_translations.py --gaps de  print what de still needs,
                                                   as template lines, plus its
                                                   stale lines for reference
"""

import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TEMPLATE = os.path.join(HERE, "..", "docs", "MasterLooter.template.txt")
DATA = os.path.join(HERE, "..", "mod", "data")

SPEC_END = "diouxXeEfgGaAcspn%"


def unescape(s):
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            n = s[i + 1]
            if n == "n":
                out.append("\n"); i += 2; continue
            if n == "t":
                out.append("\t"); i += 2; continue
            if n == "\\":
                out.append("\\"); i += 2; continue
            if n == '"':
                out.append('"'); i += 2; continue
        out.append(c)
        i += 1
    return "".join(out)


def specs(s):
    """The placeholder sequence Text::SamePlaceholders compares."""
    out = []
    i = 0
    while i + 1 < len(s):
        if s[i] != "%":
            i += 1
            continue
        if s[i + 1] == "%":
            i += 2
            continue
        j = i + 1
        while j < len(s) and s[j] not in SPEC_END:
            j += 1
        if j < len(s):
            out.append(s[i:j + 1])
            i = j
        i += 1
    return out


def read_records(path):
    """(raw_key, raw_val, lineno) for every record line, plus flags."""
    raw = open(path, "rb").read()
    notes = []
    if raw.startswith(b"\xef\xbb\xbf"):
        notes.append("starts with a byte order mark; the plugin reads the first line as a record")
        raw = raw[3:]
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as e:
        return None, ["not valid UTF-8: %s" % e]
    if "\r" in text:
        notes.append("has CR line endings; the plugin copes, but keep the file LF")
    if not text.endswith("\n"):
        notes.append("does not end with a newline")
    recs = []
    for n, line in enumerate(text.split("\n"), 1):
        line = line.rstrip("\r")
        if not line or line[0] == "#":
            continue
        if "\t" not in line:
            notes.append("line %d has no tab and is ignored: %s" % (n, line[:60]))
            continue
        k, v = line.split("\t", 1)
        recs.append((k, v, n))
    return recs, notes


def template_keys():
    recs, notes = read_records(TEMPLATE)
    keys = {}
    for k, v, n in recs:
        keys[unescape(k)] = k
    return keys


def check(path, keys, want_gaps=False):
    lang = os.path.basename(path)[len("MasterLooter."):-len(".txt")]
    recs, notes = read_records(path)
    if recs is None:
        print("%s: %s" % (lang, notes[0]))
        return False
    seen = {}
    stale, rejected, identical, breaks, dupes, tabs = [], [], [], [], [], []
    for k, v, n in recs:
        uk, uv = unescape(k), unescape(v)
        if uk not in keys:
            stale.append((k, v, n))
            continue
        if uk in seen:
            dupes.append((n, k))
        seen[uk] = uv
        if not uv:
            continue
        if specs(uk) != specs(uv):
            rejected.append((n, k, v))
        if uv == uk:
            identical.append(k)
        if uk.count("\n") != uv.count("\n"):
            breaks.append(k)
        if "\t" in uv:
            tabs.append(k)
    missing = [raw for uk, raw in keys.items() if not seen.get(uk)]
    translated = sum(1 for uk in keys if seen.get(uk))

    if want_gaps:
        print("# %s: %d of %d translated, %d to do" % (lang, translated, len(keys), len(missing)))
        for raw in sorted(missing, key=lambda x: (x.lower(), x)):
            print(raw + "\t")
        if stale:
            print("\n# stale lines, whose English is no longer in the template; the")
            print("# translation may carry over to the reworded string above")
            for k, v, n in stale:
                print("# line %d: %s\t%s" % (n, k, v))
        return not missing and not rejected

    ok = not missing and not rejected
    print("%s: %d of %d translated%s" % (lang, translated, len(keys), "" if ok else "  <-- needs work"))
    for m in notes:
        print("  %s" % m)
    if missing:
        print("  %d missing or empty" % len(missing))
        for raw in sorted(missing, key=lambda x: (x.lower(), x))[:8]:
            print("    %s" % raw[:90])
        if len(missing) > 8:
            print("    ...")
    if rejected:
        print("  %d would be rejected by the plugin, placeholders differ:" % len(rejected))
        for n, k, v in rejected:
            print("    line %d: %s\n      -> %s" % (n, k[:90], v[:90]))
    if stale:
        print("  %d stale, English not in the template:" % len(stale))
        for k, v, n in stale[:8]:
            print("    line %d: %s" % (n, k[:90]))
        if len(stale) > 8:
            print("    ...")
    if dupes:
        print("  %d duplicate keys, the last one wins:" % len(dupes))
        for n, k in dupes[:5]:
            print("    line %d: %s" % (n, k[:90]))
    if identical:
        print("  %d identical to the English (fine for names and numbers): %s" %
              (len(identical), ", ".join(x[:30] for x in identical[:6]) + (", ..." if len(identical) > 6 else "")))
    if breaks:
        print("  %d with a different number of \n line breaks: %s" %
              (len(breaks), ", ".join(x[:30] for x in breaks[:4]) + (", ..." if len(breaks) > 4 else "")))
    if tabs:
        print("  %d with a tab inside the translation: %s" % (len(tabs), ", ".join(x[:30] for x in tabs)))
    return ok


def main(argv):
    want_gaps = False
    if argv and argv[0] == "--gaps":
        want_gaps = True
        argv = argv[1:]
    keys = template_keys()
    paths = sorted(glob.glob(os.path.join(DATA, "MasterLooter.*.txt")))
    if argv:
        paths = [os.path.join(DATA, "MasterLooter.%s.txt" % a) for a in argv]
    if not want_gaps:
        print("template: %d strings" % len(keys))
    ok = True
    for p in paths:
        if not os.path.exists(p):
            print("%s: no such file" % os.path.relpath(p, HERE))
            ok = False
            continue
        ok &= check(p, keys, want_gaps)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
