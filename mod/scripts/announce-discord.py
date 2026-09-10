"""Post a release to the Discord server's mod-releases channel.

    py -3 announce-discord.py            print what would be posted
    py -3 announce-discord.py --apply    post it

Reads the version from version.h and the changelog from
private/nexus/nexus-changelog-<version>.txt, and posts a header with the
Nexus files page and the GitHub release, followed by the changelog in
chunks under Discord's message limit. Headings become bold lines.

Needs DISCORD_RELEASES_WEBHOOK, a webhook for the mod-releases channel, in
the environment or in keys.local.env beside this script, the same file the
Nexus and VirusTotal scripts read. Server settings, Integrations, Webhooks.
The webhook posts as itself, so name it there.

Refuses to post a version twice: a marker is written under private/discord
after a successful post and checked before the next one.
"""

import argparse
import json
import os
import re
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
KEYFILE = os.path.join(HERE, "keys.local.env")
NEXUS_FILES = "https://www.nexusmods.com/crimsondesert/mods/3402?tab=files"
GITHUB_RELEASE = "https://github.com/shin2344234/master-looter/releases/tag/v%s"
LIMIT = 1900


def webhook():
    url = os.environ.get("DISCORD_RELEASES_WEBHOOK", "").strip()
    if not url and os.path.exists(KEYFILE):
        for line in open(KEYFILE, encoding="utf-8"):
            line = line.strip()
            if line.startswith("DISCORD_RELEASES_WEBHOOK="):
                url = line.split("=", 1)[1].strip().strip('"')
    return url


def version():
    header = open(os.path.join(ROOT, "mod", "src", "version.h"), encoding="utf-8").read()
    return re.search(r'ML_VERSION\s+"([^"]+)"', header).group(1)


def changelog(ver):
    path = os.path.join(ROOT, "private", "nexus", "nexus-changelog-%s.txt" % ver)
    text = open(path, encoding="utf-8").read().strip()
    out = []
    for para in re.split(r"\n\s*\n", text):
        para = para.strip()
        if para.startswith("## "):
            out.append("**%s**" % para[3:].strip())
        elif re.match(r"^[0-9a-f]{64}\s", para):
            out.append("```\n%s\n```" % para)
        else:
            out.append(re.sub(r"\s*\n\s*", " ", para))
    return out


def chunks(paras):
    cur, out = "", []
    for p in paras:
        piece = (cur + "\n\n" + p) if cur else p
        if len(piece) > LIMIT and cur:
            out.append(cur)
            cur = p
        else:
            cur = piece
    if cur:
        out.append(cur)
    return out


def post(url, content):
    req = urllib.request.Request(url, data=json.dumps({"content": content}).encode("utf-8"),
                                 headers={"Content-Type": "application/json", "User-Agent": "MasterLooter-announce"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.status


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true", help="post instead of printing")
    args = ap.parse_args()
    ver = version()
    marker = os.path.join(ROOT, "private", "discord", "announced-%s" % ver)
    if os.path.exists(marker):
        print("%s was already announced (%s). Delete the marker to post again." % (ver, marker))
        return 1
    head = "**Master Looter %s**\nNexus: <%s>\nGitHub: <%s>" % (ver, NEXUS_FILES, GITHUB_RELEASE % ver)
    messages = [head] + chunks(changelog(ver))
    for i, m in enumerate(messages, 1):
        print("--- message %d of %d (%d chars) ---" % (i, len(messages), len(m)))
        print(m)
    if not args.apply:
        print("\nREPORT ONLY - nothing was sent. Re-run with --apply to post.")
        return 0
    url = webhook()
    if not url:
        print("No DISCORD_RELEASES_WEBHOOK in the environment or keys.local.env.")
        return 2
    for m in messages:
        post(url, m)
    os.makedirs(os.path.dirname(marker), exist_ok=True)
    open(marker, "w").write("posted\n")
    print("\nPosted %d messages for %s." % (len(messages), ver))
    return 0


if __name__ == "__main__":
    sys.exit(main())
