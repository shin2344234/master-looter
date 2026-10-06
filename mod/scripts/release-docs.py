"""Draft, stamp and check the release documents for the current version.

    py -3 release-docs.py new      create the four drafts from private/templates
    py -3 release-docs.py stamp    fill checksums in from private/checksums/<version>.json
    py -3 release-docs.py check    everything that must be true before publishing

Synced from release-kit. Do not edit it here; the next sync overwrites it.

The four documents every release writes:

    private/github/release-<version>.md          GitHub release notes, the full account
    private/nexus/nexus-changelog-<version>.txt  Nexus changelog, one line per change
    private/nexus/nexus-post-<version>.bbcode    Nexus update post
    private/discord/release-<version>.txt        Discord announcement

new never overwrites a file that exists. Template lines that start with TODO
are guidance, and check refuses while any are left.

stamp replaces {{SHA256_DMM}}, {{SHA256_MANUAL}} and {{SHA256_PLUGIN}}, and
rewrites any line that pairs a checksum with one of this mod's release file
names, in the four documents and in the files listed under evergreen in
release.json (the Nexus description and the root README by default). A file
that ships inside the manual archive is never stamped: its checksum is part of
the archive it would describe.

A VirusTotal link keeps pointing at the file it named. stamp looks its hash up
in the previous version's private/checksums record: the previous plugin's hash
becomes this plugin's, the previous DMM zip's becomes this DMM zip's, and the
same for the manual zip. A link to a hash on neither record is left alone and
listed. The antivirus step still rewrites the scores once they are in.

check refuses when a document is missing, a placeholder or TODO is left, the
checksums on record no longer match dist, two VirusTotal links with different
labels in one file carry the same hash, the Discord text lacks a link, the
in-game verification record is missing, or prose_check.py fails.

The verification record is private/verified/<version>.txt. Its first line is
"tested" followed by the log lines that show the change working, or
"untested", in which case the GitHub notes and the Discord text must both
say the build was not tested in play.
"""
import hashlib
import io
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import release_config as rc

CFG = rc.load()
TEMPLATES = {
    "github": "github-release.md",
    "changelog": "nexus-changelog.txt",
    "post": "nexus-post.bbcode",
    "discord": "discord-release.txt",
}
UNTESTED = "not tested in play"
HEX = re.compile(r"\b[0-9a-f]{64}\b")
SHA_HEADING = re.compile(r"(SHA-256 for )\d+\.\d+\.\d+")
VT = r"https?://(?:www\.)?virustotal\.com/(?:gui/)?file/"
VT_HASH = re.compile(r"(%s)([0-9a-f]{64})\b" % VT)
# A labelled link, BBCode or Markdown: the label and the hash.
VT_LINKS = (
    re.compile(r"\[url=%s(?P<hash>[0-9a-f]{64})[^\]]*\](?P<label>.*?)\[/url\]" % VT, re.I | re.S),
    re.compile(r"\[(?P<label>[^\]]+)\]\(%s(?P<hash>[0-9a-f]{64})[^)]*\)" % VT, re.I),
)


def read(path):
    return io.open(path, encoding="utf-8").read()


def write(path, text):
    d = os.path.dirname(path)
    if not os.path.isdir(d):
        os.makedirs(d)
    io.open(path, "w", encoding="utf-8", newline="").write(text)


def rel(path):
    return os.path.relpath(path, CFG.root)


def tokens():
    return {
        "NAME": CFG.name,
        "FILEBASE": CFG.file_base,
        "VERSION": CFG.version,
        "NEXUS_FILES_URL": CFG.nexus_files_url or "{{NEXUS_FILES_URL}}",
        "GITHUB_RELEASE_URL": CFG.github_release_url(),
        "GITHUB_REPO_URL": CFG.github_repo_url,
        "GITHUB_REPO": CFG.github,
    }


def fill(text, values):
    for k, v in values.items():
        text = text.replace("{{%s}}" % k, v)
    return text


def load_checksums():
    p = CFG.checksums_path()
    if not os.path.exists(p):
        return None
    data = json.loads(read(p))
    if data.get("version") != CFG.version:
        raise SystemExit("%s is for %s, not %s." % (rel(p), data.get("version"), CFG.version))
    return data["files"]


def previous_hashes():
    """Each hash on the latest checksum record older than this version, mapped
    to its file's role, and that record's version. ({}, None) without one."""
    d = CFG.path("private", "checksums")
    if not os.path.isdir(d):
        return {}, None
    now = version_tuple(CFG.version)
    best = None
    for name in os.listdir(d):
        if not name.endswith(".json"):
            continue
        data = json.loads(read(os.path.join(d, name)))
        v = data.get("version") or name[:-5]
        if version_tuple(v) < now and (best is None or version_tuple(v) > version_tuple(best[0])):
            best = (v, data)
    if not best:
        return {}, None
    files = best[1].get("files", {})
    return dict((files[r]["sha256"], r) for r in ("dmm", "manual", "plugin") if r in files), best[0]


def sha_tokens(files):
    return {
        "SHA256_DMM": files["dmm"]["sha256"],
        "SHA256_MANUAL": files["manual"]["sha256"],
        "SHA256_PLUGIN": files["plugin"]["sha256"],
    }


def cmd_new():
    tdir = CFG.path("private", "templates")
    if not os.path.isdir(tdir):
        raise SystemExit("No %s. Run release-kit's sync.ps1 for this mod." % rel(tdir))
    values = tokens()
    files = load_checksums()
    if files:
        values.update(sha_tokens(files))
    for role, path in CFG.release_docs():
        if os.path.exists(path):
            print("exists   %s" % rel(path))
            continue
        write(path, fill(read(os.path.join(tdir, TEMPLATES[role])), values))
        print("created  %s" % rel(path))
    ver = CFG.path("private", "verified", "%s.txt" % CFG.version)
    if not os.path.exists(ver):
        print("\nStill needed: %s, first line 'tested' plus the log lines, or 'untested'." % rel(ver))
    return 0


def evergreen():
    """The documents outside this version's four that stamp keeps current."""
    return [CFG.path(p) for p in CFG.data.get("evergreen", ["private/nexus/nexus-description.bbcode", "README.md"])]


def role_patterns():
    b = re.escape(CFG.file_base)
    return {
        "dmm": re.compile(r"%s-\d[\w.]*-DMM\.zip" % b),
        "manual": re.compile(r"%s-\d[\w.]*\.zip" % b),
        "plugin": re.compile(r"%s\.asi" % b),
    }


def line_role(line, pats):
    """The one release file a line names, or None when it names none or several."""
    found = set()
    for m in pats["dmm"].finditer(line):
        found.add(("dmm", m.group(0)))
    for m in pats["manual"].finditer(line):   # never matches a -DMM name: [\w.] has no hyphen
        found.add(("manual", m.group(0)))
    for m in pats["plugin"].finditer(line):
        found.add(("plugin", m.group(0)))
    roles = set(r for r, _ in found)
    return roles.pop() if len(roles) == 1 else None


def stamp_text(text, files, pats, previous):
    text = fill(text, sha_tokens(files))
    current = set(f["sha256"] for f in files.values())
    out, touched, leftover, unknown = [], 0, [], []

    # A VirusTotal link names its file by its label, not by anything stamp can
    # read, so it follows its own hash from the previous record instead. The
    # line around it can name the plugin and still hold a link to an archive.
    def vt(m):
        h = m.group(2)
        if h in previous:
            return m.group(1) + files[previous[h]]["sha256"]
        if h not in current:
            unknown.append(h)
        return m.group(0)

    for line in text.splitlines(True):
        before = line
        links = set(m.start(2) for m in VT_HASH.finditer(line))
        plain = [m for m in HEX.finditer(line) if m.start() not in links]
        role = line_role(line, pats) if plain else None
        if role:
            line = HEX.sub(lambda m: m.group(0) if m.start() in links else files[role]["sha256"], line)
            if role in ("dmm", "manual"):
                line = pats[role].sub(files[role]["name"], line)
        elif plain:
            leftover.append(line.strip()[:110])
        line = VT_HASH.sub(vt, line)
        # The heading over the checksum block names the version.
        line = SHA_HEADING.sub(lambda m: m.group(1) + CFG.version, line)
        if line != before:
            touched += 1
        out.append(line)
    return "".join(out), touched, leftover, unknown


def cmd_stamp():
    files = load_checksums()
    if not files:
        raise SystemExit("No %s. Run package.ps1 first." % rel(CFG.checksums_path()))
    pats = role_patterns()
    previous, prev_version = previous_hashes()
    if prev_version:
        print("VirusTotal links on %s files move to their %s counterparts" % (prev_version, CFG.version))
    else:
        print("no checksum record older than %s, so VirusTotal links stay as they are" % CFG.version)
    shipped = set(os.path.normcase(CFG.path(p))
                  for p in CFG.archive_files("manualZip") + CFG.archive_files("dmmZip"))
    targets = [p for _, p in CFG.release_docs()] + evergreen()
    for path in targets:
        if not os.path.exists(path):
            print("missing  %s" % rel(path))
            continue
        if os.path.normcase(path) in shipped:
            print("skipped  %s ships in an archive, so it carries no checksums" % rel(path))
            continue
        before = read(path)
        after, touched, leftover, unknown = stamp_text(before, files, pats, previous)
        if after != before:
            write(path, after)
        print("%-8s %s (%d lines changed)" % ("stamped" if after != before else "current", rel(path), touched))
        for l in leftover:
            print("         left alone, check it by hand: %s" % l)
        for h in unknown:
            print("         VirusTotal link to %s, on neither checksum record, left alone" % h)
    return 0


def rehash(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def version_tuple(v):
    return tuple(int(x) for x in re.findall(r"\d+", v)[:3])


def check_requires(problems):
    for req in CFG.data.get("requires", []):
        other = CFG.path(req["dir"])
        try:
            tags = subprocess.run(["git", "-C", other, "tag", "-l", "v*"], capture_output=True,
                                  text=True, check=True).stdout.split()
        except Exception as e:
            problems.append("could not read the tags of %s: %s" % (req["name"], e))
            continue
        released = [t[1:] for t in tags if re.match(r"^v\d+\.\d+\.\d+$", t)]
        if not any(version_tuple(t) >= version_tuple(req["minVersion"]) for t in released):
            problems.append("%s must be at %s or later before this ships (%s)"
                            % (req["name"], req["minVersion"], req.get("why", "see release.json")))


def cmd_check():
    problems, notes = [], []
    docs = CFG.release_docs()
    for role, path in docs:
        if not os.path.exists(path):
            problems.append("missing %s (release-docs.py new)" % rel(path))
            continue
        text = read(path)
        if re.search(r"\{\{[A-Z0-9_]+\}\}", text):
            problems.append("%s still has a {{PLACEHOLDER}} (release-docs.py stamp)" % rel(path))
        todo = [l for l in text.splitlines() if l.strip().startswith("TODO")]
        if todo:
            problems.append("%s still has %d TODO line(s)" % (rel(path), len(todo)))

    files = load_checksums()
    if not files:
        problems.append("no %s (package.ps1)" % rel(CFG.checksums_path()))
    else:
        for role, path in (("dmm", CFG.zip_dmm), ("manual", CFG.zip_manual), ("plugin", CFG.asi)):
            if not os.path.exists(path):
                problems.append("missing %s" % rel(path))
            elif rehash(path) != files[role]["sha256"]:
                problems.append("%s changed since package.ps1 recorded it; package and stamp again" % rel(path))
        # A file that goes into the manual archive and changed after packaging
        # means the archive on record is not what the repo now describes.
        if os.path.exists(CFG.zip_manual):
            built = os.path.getmtime(CFG.zip_manual)
            for p in CFG.archive_files("manualZip") + CFG.archive_files("dmmZip"):
                full = CFG.path(p)
                if os.path.exists(full) and os.path.getmtime(full) > built:
                    problems.append("%s changed after the archives were built; package and stamp again" % p)
        for role, path in docs[:2]:
            if os.path.exists(path):
                text = read(path)
                for r in ("dmm", "manual", "plugin"):
                    if files[r]["sha256"] not in text:
                        problems.append("%s lacks the %s checksum" % (rel(path), r))
        # stamp only rewrites checksum lines that are already there, so a
        # description that never had a line for one of the files stays without
        # it. The first release with two archives hit that.
        desc = CFG.path("private", "nexus", "nexus-description.bbcode")
        if os.path.exists(desc):
            text = read(desc)
            for r in ("dmm", "manual", "plugin"):
                if files[r]["sha256"] not in text:
                    problems.append("%s lacks the %s checksum. If it has no line for %s at all, add "
                                    "'<sha256>  %s' to its checksum block, then run release-docs.py stamp"
                                    % (rel(desc), r, files[r]["name"], files[r]["name"]))

    # Two differently labelled VirusTotal links on one hash means one of them
    # reports on the wrong file. stamp did that to 1.1.4 of Private Storage
    # Master, whose "the archive" link came out with the plugin's hash.
    for path in [p for _, p in docs] + evergreen():
        if not os.path.exists(path):
            continue
        labels = {}
        text = read(path)
        for pat in VT_LINKS:
            for m in pat.finditer(text):
                label = re.sub(r"\s+", " ", m.group("label")).strip().lower()
                label = re.sub(r"^the ", "", label)
                labels.setdefault(m.group("hash"), set()).add(label)
        for h, names in sorted(labels.items()):
            if len(names) > 1:
                problems.append("%s has VirusTotal links %s on the same hash %s; each should carry its own file's hash"
                                % (rel(path), " and ".join('"%s"' % n for n in sorted(names)), h))

    discord = dict(docs)["discord"]
    if os.path.exists(discord):
        body = read(discord)
        for link in (CFG.nexus_files_url, CFG.github_release_url()):
            if link and link not in body:
                problems.append("%s lacks the link %s" % (rel(discord), link))

    vpath = CFG.path("private", "verified", "%s.txt" % CFG.version)
    if not os.path.exists(vpath):
        problems.append("no %s: first line 'tested' plus the log lines, or 'untested'" % rel(vpath))
    else:
        lines = [l for l in read(vpath).splitlines() if l.strip()]
        head = lines[0].strip().lower() if lines else ""
        if head == "tested":
            if len(lines) < 2:
                problems.append("%s says tested but quotes no log lines" % rel(vpath))
        elif head == "untested":
            for role in ("github", "discord"):
                p = dict(docs)[role]
                if os.path.exists(p) and UNTESTED not in read(p).lower():
                    problems.append("%s must say the build was %s" % (rel(p), UNTESTED))
            notes.append("this release goes out untested in play, and says so")
        else:
            problems.append("%s must start with 'tested' or 'untested'" % rel(vpath))

    check_requires(problems)

    checker = CFG.path("scripts", "prose_check.py")
    prose = [p for _, p in docs if os.path.exists(p)]
    desc = CFG.path("private", "nexus", "nexus-description.bbcode")
    if os.path.exists(desc):
        prose.append(desc)
    if not os.path.exists(checker):
        problems.append("no scripts/prose_check.py (sync.ps1)")
    elif prose:
        r = subprocess.run([sys.executable, checker] + prose, cwd=CFG.root)
        if r.returncode != 0:
            problems.append("prose_check.py fails; rewrite until it is clean")

    print()
    for n in notes:
        print("note     %s" % n)
    if problems:
        for p in problems:
            print("REFUSED  %s" % p)
        print("\n%s %s is not ready to publish." % (CFG.name, CFG.version))
        return 1
    print("%s %s is ready to publish." % (CFG.name, CFG.version))
    return 0


def main():
    cmds = {"new": cmd_new, "stamp": cmd_stamp, "check": cmd_check}
    if len(sys.argv) < 2 or sys.argv[1] not in cmds:
        print(__doc__)
        return 2
    return cmds[sys.argv[1]]()


if __name__ == "__main__":
    sys.exit(main())
