"""Build the menu translation template from the source.

The plugin can write a template itself, from the General tab, but it only lists
what has actually been drawn, so anything on a tab nobody opened is missing. This
reads the strings straight out of the code instead, which gives a translator the
whole menu in one go and cannot miss a corner of it.

Two places matter. Menu labels and tooltips are wrapped in TR() by hand. The
Nearby table's Decision column shows text the engine produced, which is passed
through TR() at the point it is drawn, so those literals are collected as well.

Writes mod/data/MasterLooter.template.txt.
"""

import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "mod", "src")
OUT = os.path.join(HERE, "..", "mod", "data", "MasterLooter.template.txt")

# One literal, or several adjacent ones the compiler joins before TR sees them.
LIT = r'"(?:[^"\\]|\\.)*"(?:\s*"(?:[^"\\]|\\.)*")*'


def join_literals(run):
    """"a" "b" -> a b, keeping the escapes a translator has to preserve."""
    return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', run))


def from_menu(path):
    s = open(path, encoding="utf-8").read()
    return {join_literals(m) for m in re.findall(r"TR\(\s*(" + LIT + r")\s*\)", s)}


def from_engine(path):
    """The verdicts the Nearby table shows, which reach TR() at draw time."""
    s = open(path, encoding="utf-8").read()
    out = set()
    for pat in (r"\bskip\(\s*\"((?:[^\"\\]|\\.)*)\"", r"v\.why\s*=\s*\"((?:[^\"\\]|\\.)*)\""):
        out |= set(re.findall(pat, s))
    # what Label() falls back to when an object has no name of its own
    for word in ("corpse", "creature", "object", "entity", "gather node"):
        out.add(word)
    return out


def main():
    strings = set()
    strings |= from_menu(os.path.join(SRC, "gui", "menu.cpp"))
    strings |= from_engine(os.path.join(SRC, "loot", "engine.cpp"))
    strings = {s for s in strings if s.strip() and not s.startswith("##")}
    # Anything that is only placeholders and punctuation has nothing to
    # translate and would just be noise in front of a translator. The length
    # modifiers matter here: "%lld" reads as two letters without them.
    spec = re.compile(r"%[-+ #0]*[0-9*]*(?:\.[0-9*]+)?(?:hh|h|ll|l|j|z|t|L)?[diouxXeEfgGaAcspn%]")
    dropped = {s for s in strings if not re.search(r"[A-Za-z]{2}", spec.sub("", s))}
    strings -= dropped

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("# Master Looter menu text.\n")
        f.write("# One string per line: the English, a tab, then your translation.\n")
        f.write("# \\n is a line break. Lines starting with # are ignored. A line with\n")
        f.write("# nothing after the tab is left in English, so a part-finished file is\n")
        f.write("# perfectly usable.\n")
        f.write("# Keep every %d, %s and %.1f exactly as they appear and in the same\n")
        f.write("# order: they are replaced with numbers and names at runtime, and a\n")
        f.write("# line that changes them is ignored rather than risked.\n")
        f.write("# Save as MasterLooter.<language>.txt beside the plugin, for example\n")
        f.write("# MasterLooter.de.txt, then set Language=de in MasterLooter.ini or type\n")
        f.write("# the language into General, Language.\n\n")
        for s in sorted(strings, key=lambda x: (x.lower(), x)):
            f.write(s + "\t\n")

    with_fmt = sum(1 for s in strings if re.search(r"%[-+ #0-9.]*[a-zA-Z]", s))
    print("wrote %d strings to %s" % (len(strings), os.path.relpath(OUT, HERE)))
    print("  %d carry %% placeholders that must be kept" % with_fmt)
    print("  %d skipped as format specifiers with no words in them" % len(dropped))


if __name__ == "__main__":
    main()
