"""Check prose for AI hallmarks before it goes out as Seth's words.

    py -3 scripts/prose_check.py private/nexus/nexus-post-1.6.3.txt
    py -3 scripts/prose_check.py --all

Exit code is 1 if anything in the HARD list is present. Those are mechanical
and there is never a reason to ship one.

The WARN list is heuristic. A hit is not proof, but every hit has to be looked
at and consciously kept, because these are the shapes that survive a clean
vocabulary sweep. Three times now the banned words were all absent and the
structure gave it away anyway.

What no script can check is printed at the end. Read it and do it by eye. The
rule in CLAUDE.md is "the grep is not the sweep", and this file is the grep.

Code blocks, bbcode [code] sections, checksum lines and URLs are stripped
before checking, because those are exempt and full of false positives.
"""
import io
import os
import re
import sys
import unicodedata

# ---------------------------------------------------------------- hard bans
DASHES = [
    ("—", "em dash"),
    ("– ", "spaced en dash"),
    (" –", "spaced en dash"),
]
QUOTES = [
    ("‘", "curly apostrophe"), ("’", "curly apostrophe"),
    ("“", "curly quote"), ("”", "curly quote"),
]

VOCAB = """delve leverage utilize harness foster unlock unleash elevate empower
streamline showcase underscore underscores spearhead embark bolster illuminate
unpack revolutionize boast boasts crucial pivotal vital robust seamless
seamlessly vibrant bustling meticulous meticulously intricate multifaceted
holistic invaluable transformative groundbreaking cutting-edge state-of-the-art
ever-evolving fast-paced commendable noteworthy profound nestled tapestry realm
beacon cornerstone testament synergy game-changer game-changing myriad plethora
""".split()

# Words with a real technical sense in this project. Flagged softly.
SOFT_VOCAB = "navigate landscape ecosystem journey dive deep-dive".split()

PHRASES = [
    "in today's fast-paced world", "in an era of", "have you ever wondered",
    "it's important to note", "it is important to note", "it's worth noting",
    "it is worth noting", "keep in mind", "needless to say", "at its core",
    "at the end of the day", "when it comes to", "that being said",
    "plays a crucial role", "plays a vital role", "plays a pivotal role",
    "plays a significant role", "a testament to", "underscores the importance",
    "leaves a lasting impact", "watershed moment", "key turning point",
    "deeply rooted", "in conclusion", "in summary", "in essence",
    "the bottom line", "but here's the thing", "but here's the kicker",
]

OPENERS = ("furthermore moreover additionally consequently notably "
           "importantly interestingly").split()

# My own tics, added as they get caught. Seth has asked four times now, so the
# ones that keep coming back stop being a judgement call and become a failure.
# Every one of these was written more than once in a single day.
#
# Matched against whitespace-collapsed text, because these span line breaks in a
# wrapped document and a pattern with a space in it silently misses otherwise.
# The first version of this list also had every \b turned into a literal
# backspace by a shell heredoc, so it matched nothing at all and reported a
# clean run. Build the patterns in a file, never in a heredoc.
TICS = [
    (r"which is what .{0,40} looks like", 'which is what X looks like'),
    (r"\bis the tell\b", 'is the tell'),
    (r"\bthe giveaway (?:was|is)\b", 'the giveaway was'),
    (r"\bI would rather\b", 'performed honesty: I would rather'),
    (r"\b(?:my own fault|my fault|I was wrong|I had it wrong)\b", 'narrating my own error'),
    (r"\bwhich is worse than\b", 'which is worse than'),
    (r"\brather than a theory\b", 'rather than a theory'),
    (r"\bthe whole (?:design|point|trick|of it)\b", 'the whole X'),
    (r"\bthat is the (?:whole|entire) \w+", 'that is the whole X'),
    (r"\bwhat actually happened\b", 'what actually happened'),
    (r"\bnot a theory\b", 'not a theory'),
    (r"\boff a log\b", 'off a log'),
    (r"\bworth (?:saying|having|knowing) (?:plainly|rather than)\b", 'worth saying plainly'),
    (r"\bhonest(?:ly)? (?:state|answer|position|about)\b", 'announcing my own honesty'),
]

# Saying "I got this wrong" once is candour. Five times in one document is a
# mannerism, and it reads as performance.
CONFESSION = r"\b(?:my own|my fault|I was wrong|I had assumed|I never checked|I should have|I failed)\b"

# Product and proper names that are capitalised legitimately, so the Title Case
# heading test does not fire on "With Definitive Mod Manager (DMM)".
PROPER = [
    "Definitive Mod Manager", "Crimson Desert", "Master Looter",
    "Ultimate ASI Loader", "Dear ImGui", "Nexus Mods", "Vortex",
    "Mod Organizer", "Direct3D", "DirectX", "Windows", "Steam", "GitHub",
    "VirusTotal", "Microsoft", "Symantec", "CrowdStrike Falcon",
    "Deep Instinct", "Take-or-Steal", "Damiane", "Oongka", "Kliff",
]

# ---------------------------------------------------------------- heuristics
NEG_PARALLEL = [
    r"\bnot just \w+[^.]{0,40}, (?:it's|it is|but)\b",
    r",\s*not (?:a|an|the|just|only)\b",
    r"\bit is not \w+[^.]{0,30}, it is\b",
    r"\bisn't \w+[^.]{0,30}, it's\b",
]
TRAILING_PARTICIPLE = (r",\s+(highlighting|ensuring|underscoring|reflecting|"
                       r"showcasing|emphasizing|demonstrating)\b")
SERVES_AS = r"\b(serves as|stands as|functions as)\b"
VAGUE_ATTRIB = r"\b(experts say|studies show|research shows)\b"

CLAUSE_VERB = (r"\b(is|are|was|were|has|have|does|do|can|never|carries|names|"
               r"walks|goes|reads|pays|drops|lands|gets|takes)\b")


def strip_exempt(text):
    """Drop what the rules exempt, so the checks only see prose."""
    text = re.sub(r"```.*?```", " ", text, flags=re.S)
    text = re.sub(r"\[code\].*?\[/code\]", " ", text, flags=re.S | re.I)
    text = re.sub(r"\[url=[^\]]*\]", " ", text, flags=re.I)
    text = re.sub(r"https?://\S+", " ", text)
    text = re.sub(r"^\s{4,}\S.*$", " ", text, flags=re.M)
    text = re.sub(r"^[0-9a-f]{64}\s+\S+$", " ", text, flags=re.M)
    text = re.sub(r"`[^`\n]*`", " ", text)
    # Quoted reporter text is exempt, and reporters write however they like.
    # Markdown blockquotes and bbcode [quote] both carry someone else's words.
    text = re.sub(r"^\s*>.*$", " ", text, flags=re.M)
    text = re.sub(r"\[quote.*?\[/quote\]", " ", text, flags=re.S | re.I)
    return text


def sentences(text):
    flat = re.sub(r"\s+", " ", text)
    return [s.strip() for s in re.split(r"(?<=[.?])\s+", flat) if s.strip()]


def check(path):
    raw = io.open(path, encoding="utf-8").read()
    body = strip_exempt(raw)
    low = body.lower()
    flat = re.sub(r"\s+", " ", low)
    hard, warn = [], []

    for ch, what in DASHES + QUOTES:
        if ch in body:
            hard.append("%s present" % what)
    for ch in body:
        if ord(ch) > 0x2100 and unicodedata.category(ch) == "So":
            hard.append("emoji or pictograph %r" % ch)
            break
    if "!" in re.sub(r"!\w", "", body):
        hard.append("exclamation point")

    for w in VOCAB:
        if re.search(r"\b%s\b" % re.escape(w), low):
            hard.append("banned word: %s" % w)
    for p in PHRASES:
        if p in flat:
            hard.append("stock phrase: %s" % p)
    for s in sentences(body):
        first = s.split(" ")[0].strip(",").lower()
        if first in OPENERS:
            hard.append("sentence opens with %s" % first.capitalize())

    for line in body.splitlines():
        m = re.match(r"^\s*(?:#{1,6}\s+|\[b\])(.+?)(?:\[/b\])?\s*$", line)
        if not m:
            continue
        head = m.group(1)
        for name in PROPER:
            head = head.replace(name, " ")
        words = [w for w in re.findall(r"[A-Za-z']+", head) if len(w) > 3]
        if len(words) >= 3 and sum(w[0].isupper() for w in words) > len(words) * 0.6:
            hard.append("Title Case heading: %s" % m.group(1)[:50])

    for pat, why in TICS:
        for m in re.finditer(pat, flat):
            hard.append("tic: %s -> ...%s..." % (why, m.group(0)[:60]))

    n_conf = len(re.findall(CONFESSION, flat))
    if n_conf > 2:
        warn.append("%d self-corrections in one piece; once is honest, five is a mannerism" % n_conf)

    for w in SOFT_VOCAB:
        if re.search(r"\b%s\b" % re.escape(w), low):
            warn.append("soft word, fine if literal: %s" % w)
    for pat in NEG_PARALLEL:
        for m in re.finditer(pat, low):
            warn.append("negative parallelism: ...%s..." % m.group(0)[:60])
    for m in re.finditer(TRAILING_PARTICIPLE, low):
        warn.append("trailing participle: %s" % m.group(0).strip())
    for m in re.finditer(SERVES_AS, low):
        warn.append("%s instead of is" % m.group(0))
    for m in re.finditer(VAGUE_ATTRIB, low):
        warn.append("vague attribution: %s" % m.group(0))

    n = len(re.findall(r"\brather than\b", low))
    if n > 1:
        warn.append('"rather than" %d times, check it is contrast not habit' % n)

    for s in sentences(body):
        parts = [p for p in s.split(",") if p.strip()]
        if len(parts) >= 3 and re.search(r",\s*and\b", s):
            if sum(bool(re.search(CLAUSE_VERB, p)) for p in parts) >= 3:
                warn.append("possible triad: %s" % s[:90])

    ss = sentences(body)
    for a, b in zip(ss, ss[1:]):
        if abs(len(a) - len(b)) <= 4 and len(a) > 45:
            warn.append("same-shape pair: %r / %r" % (a[:45], b[:45]))

    return hard, warn


# Files the repeated-sentence rule does not apply to.
#
# The rule exists because the same person reads a release's post and its
# changelog back to back, so a sentence in both reads as copy and paste. That
# reasoning does not reach these:
#
#   nexus-changelog.txt is a deliberate verbatim copy of the current release's
#   changelog, so every sentence in it is a duplicate by construction.
#
#   README.md, mod/README.md, the mod page description and the short
#   description are long-lived reference documents about one mod. They are
#   supposed to say the same things, and thirteen shared sentences between the
#   README and the description are thirteen places that are correctly in step,
#   not thirteen mistakes. Flagging them trained me to skim the output, which
#   is how the check stops working.
SKIP_DUP = {
    "nexus-changelog.txt",
    "README.md",
    "nexus-description.bbcode",
    "nexus-short-description.txt",
}


def release_of(path):
    """The x.y.z in a filename, so only documents of one release are compared."""
    m = re.search(r"(\d+\.\d+\.\d+)", os.path.basename(path))
    return m.group(1) if m else ""


def duplicates(paths):
    """A sentence used in two documents of the same release.

    Readers of a Nexus release see the post and the changelog together, so a
    sentence in both reads as copy and paste. Three constraints keep this from
    crying wolf, which would get the whole check ignored:

      - nexus-changelog.txt is skipped. It is a deliberate verbatim copy of the
        current release's changelog, so every sentence in it is a duplicate.
      - Only documents of the same release are compared. The 1.2.0 and the
        1.6.3 changelogs sharing a line is not copy and paste.
      - A sentence in three or more files is house boilerplate, not laziness.
        "For Crimson Desert 2.01.00 (exe 1.0.0.2760)." heads every changelog.
    """
    paths = [p for p in paths if os.path.basename(p) not in SKIP_DUP]
    everywhere = {}
    per_release = {}
    for p in paths:
        rel = release_of(p)
        for s in sentences(strip_exempt(io.open(p, encoding="utf-8").read())):
            if len(s) < 30:
                continue
            everywhere.setdefault(s, set()).add(os.path.basename(p))
            per_release.setdefault((rel, s), set()).add(os.path.basename(p))
    out = {}
    for (rel, s), files in per_release.items():
        if len(files) > 1 and len(everywhere[s]) < 3:
            out[s] = files
    return out


BY_EYE = """
Now the part no script can do. Reread the whole piece and answer these:

  1. Does the opening line say something, or does it announce the shape of the
     piece? Cut it if it is announcing.
  2. Does the last paragraph add anything, or restate and then moralise? End on
     what the reader does next.
  3. Read the previous two documents of this kind. Does this one share their
     skeleton with the labels swapped?
  4. Any move that is becoming a signature? Performed honesty, the
     self-deprecating fix introduction, the same joke shape twice.
  5. Read it aloud. Two sentences in a row with the same rhythm get rewritten.
"""


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if "--all" in sys.argv or not args:
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        d = os.path.join(root, "docs")
        args = [os.path.join(d, f) for f in sorted(os.listdir(d))
                if f.endswith((".txt", ".bbcode")) and "template" not in f]
    failed = False
    for p in args:
        hard, warn = check(p)
        if hard or warn:
            print("=== %s ===" % os.path.basename(p))
        for h in sorted(set(hard)):
            print("  HARD  %s" % h)
            failed = True
        for w in sorted(set(warn)):
            print("  warn  %s" % w)
        if hard or warn:
            print()
    if len(args) > 1:
        dup = duplicates(args)
        if dup:
            print("=== repeated across documents ===")
            for s, f in sorted(dup.items()):
                print("  HARD  %s" % ", ".join(sorted(f)))
                print("        %s" % s[:100])
                failed = True
            print()
    print(BY_EYE)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
