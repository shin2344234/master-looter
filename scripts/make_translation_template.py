"""Build the menu translation template from the source.

The plugin can write a template itself, from the General tab, but it only lists
what has actually been drawn, so anything on a tab nobody opened is missing. This
reads the strings straight out of the code instead, which gives a translator the
whole menu in one go and cannot miss a corner of it.

Three places matter. Most menu labels and tooltips are wrapped in TR() by hand.
Some are not: the collect switches and the class groups are struct literals, and
the key, pad and status rows pass a bare literal to a helper that translates it,
which keeps the call sites readable but hides the string from a TR() search.
Finally the Nearby table's Decision column shows text the engine produced, which
is passed through TR() at the point it is drawn, so those literals are collected
as well.

Writes docs/MasterLooter.template.txt, which is the copy a translator is given.
"""

import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "mod", "src")
# docs/, not mod/data/: this is for a translator to pick up, not something the
# plugin compiles in, and the three tables next to it are the latter.
OUT = os.path.join(HERE, "..", "docs", "MasterLooter.template.txt")

# One literal, or several adjacent ones the compiler joins before TR sees them.
LIT = r'"(?:[^"\\]|\\.)*"(?:\s*"(?:[^"\\]|\\.)*")*'


def join_literals(run):
    """"a" "b" -> a b, keeping the escapes a translator has to preserve."""
    return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', run))


def from_tr(s):
    """The ordinary case: TR("...") written at the point of use."""
    return {join_literals(m) for m in re.findall(r"TR\(\s*(" + LIT + r")\s*\)", s)}


def from_helpers(s):
    """Rows whose label stays a bare literal at the call site.

    Help, OnOff, KeyRow and PadRow translate the label themselves. The third and
    fourth arguments of OnOff are the words it prints for on and off, and most of
    those are written only at the call site.
    """
    out = set()
    for m in re.findall(r"\b(?:Help|OnOff|KeyRow|PadRow)\(\s*(" + LIT + r")", s):
        out.add(join_literals(m))
    for call in re.findall(r"\bOnOff\((.*?)\)\s*;", s):
        for lit in re.findall(LIT, call)[1:]:
            out.add(join_literals(lit))
    out |= {"yes", "no"}          # OnOff's defaults
    return out


def from_tables(s):
    """The two tables of switches, which are struct literals.

    Toggle is {label, &setting, help}; ClassGroup is {name, classes, help}. A
    group's middle field lists class names from the item database rather than
    English, so it is left alone.
    """
    out = set()
    toggle = r"\{\s*(" + LIT + r")\s*,\s*&[^,]+,\s*(" + LIT + r")\s*\}"
    group = r"\{\s*(" + LIT + r")\s*,\s*" + LIT + r"\s*,\s*(" + LIT + r")\s*\}"
    for pat in (toggle, group):
        for entry in re.findall(pat, s):
            out |= {join_literals(x) for x in entry}
    return out


def from_menu(path):
    s = open(path, encoding="utf-8").read()
    return from_tr(s) | from_helpers(s) | from_tables(s) | from_tabs(s)


def from_tabs(s):
    """The tab bar, a table of {name, function}."""
    return {join_literals(m) for m in re.findall(r"\{\s*(" + LIT + r")\s*,\s*(?:Tab\w+|\[\])", s)}


def from_engine(path):
    """The verdicts the Nearby table shows, which reach TR() at draw time."""
    s = open(path, encoding="utf-8").read()
    out = set()
    # skip("..."), v.why = "...", and the switch names GatherSwitchOff hands
    # back as `on ? nullptr : "..."`, which reach skip() through a variable.
    for pat in (r"\bskip\(\s*\"((?:[^\"\\]|\\.)*)\"",
                r"v\.why\s*=\s*\"((?:[^\"\\]|\\.)*)\"",
                r"\?\s*nullptr\s*:\s*\"((?:[^\"\\]|\\.)*)\""):
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

    # Font file names are not menu text. The menu names the faces it tries in
    # order, segoeui.ttf then tahoma.ttf then arial.ttf and the script
    # fallbacks, and those reached translators as if they were something to
    # translate. Nobody translated tahoma.ttf, but it sat in every one of the
    # 28 files as a line to skip past.
    fonts = {s for s in strings if re.fullmatch(r"[A-Za-z0-9_-]+\.tt[cf]", s)}
    strings -= fonts

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
    if fonts:
        print("  %d skipped as font file names: %s" % (len(fonts), ", ".join(sorted(fonts))))


if __name__ == "__main__":
    main()
