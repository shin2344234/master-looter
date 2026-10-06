"""What every release script needs to know about the mod it is running in.

The scripts in this folder are the same in every mod. They are synced from
release-kit and must not be edited here, because the next sync overwrites
them. Everything that differs between mods lives in release.json at the repo
root, and this module is the one place that reads it.

    import release_config as rc
    cfg = rc.load()
    cfg.version          "1.0.2", read from the version header, or from any
                         file by version.pattern, whose first group it is
    cfg.path("private", "github", "release-%s.md" % cfg.version)
    cfg.zip_manual       mod/dist/StaminaMaster-1.0.2.zip
    rc.read_key("NEXUS_API_KEY")
"""
import io
import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
KEYFILE = os.path.join(HERE, "keys.local.env")


def find_root(start=HERE):
    d = start
    while True:
        if os.path.exists(os.path.join(d, "release.json")):
            return d
        up = os.path.dirname(d)
        if up == d:
            raise SystemExit("No release.json above %s. Every mod keeps one at its repo root." % start)
        d = up


class Config(object):
    def __init__(self, root, data):
        self.root = root
        self.data = data
        self.name = data["name"]
        self.file_base = data["fileBase"]
        self.dist = self.path(data["dist"])
        self.asi = os.path.join(self.dist, self.file_base + ".asi")
        self.github = data["github"]
        self.nexus = data.get("nexus") or {}
        self.discord = data.get("discord") or {}
        self._version = None

    def path(self, *parts):
        return os.path.normpath(os.path.join(self.root, *parts))

    @property
    def version(self):
        if self._version is None:
            v = self.data["version"]
            header = self.path(v["header"])
            text = io.open(header, encoding="utf-8").read()
            if v.get("pattern"):
                m = re.search(v["pattern"], text, re.M)
                if not m:
                    raise SystemExit("No line in %s matches %s" % (header, v["pattern"]))
                self._version = m.group(1)
                return self._version
            m = re.search(r'#define\s+%s\s+"([^"]+)"' % re.escape(v["macro"]), text)
            if not m:
                raise SystemExit("No %s in %s" % (v["macro"], header))
            self._version = m.group(1)
        return self._version

    def archive_files(self, key):
        """The paths under manualZip or dmmZip, {version} filled in."""
        return [p.replace("{version}", self.version) for p in self.data.get(key, [])]

    def set_version(self, version):
        self._version = version

    @property
    def zip_manual(self):
        return os.path.join(self.dist, "%s-%s.zip" % (self.file_base, self.version))

    @property
    def zip_dmm(self):
        return os.path.join(self.dist, "%s-%s-DMM.zip" % (self.file_base, self.version))

    @property
    def nexus_files_url(self):
        page = str(self.nexus.get("page", "")).strip()
        if not page:
            return ""
        return "https://www.nexusmods.com/%s/mods/%s?tab=files" % (self.nexus.get("game", "crimsondesert"), page)

    @property
    def github_repo_url(self):
        return "https://github.com/%s" % self.github

    def github_release_url(self, version=None):
        return "%s/releases/tag/v%s" % (self.github_repo_url, version or self.version)

    def release_docs(self, version=None):
        """The four files every release writes, in the order they are drafted."""
        v = version or self.version
        return [
            ("github", self.path("private", "github", "release-%s.md" % v)),
            ("changelog", self.path("private", "nexus", "nexus-changelog-%s.txt" % v)),
            ("post", self.path("private", "nexus", "nexus-post-%s.bbcode" % v)),
            ("discord", self.path("private", "discord", "release-%s.txt" % v)),
        ]

    def checksums_path(self, version=None):
        return self.path("private", "checksums", "%s.json" % (version or self.version))


def load():
    root = find_root()
    with io.open(os.path.join(root, "release.json"), encoding="utf-8") as f:
        return Config(root, json.load(f))


def read_key(name):
    """Environment first, then keys.local.env beside the scripts. Never printed."""
    got = os.environ.get(name, "").strip()
    if got:
        return got
    if not os.path.exists(KEYFILE):
        return None
    for line in io.open(KEYFILE, encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, _, v = line.partition("=")
        if k.strip() == name:
            return v.strip().strip('"').strip("'") or None
    return None
