"""Post a release to the Discord server's mod-releases channel.

    py -3 announce-discord.py            print what would be posted
    py -3 announce-discord.py --apply    post it

Reads the version from version.h and posts private/discord/release-<version>.txt
as it is: two or three plain lines saying what changed, then the Nexus files
page and the GitHub release. Write that file first and run prose_check.py on
it; the script refuses without it, and refuses if either link is missing.

Posts as the bot, using DISCORD_BOT_TOKEN from the environment or from
keys.local.env beside this script, the same file the Nexus and VirusTotal
scripts read. It was written against a webhook that this project has never
had, which is why every announcement so far has gone out by hand.

The channel is mod-releases, and its id is below rather than in the key file
because a channel id is not a secret. DISCORD_RELEASES_CHANNEL overrides it.

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
RELEASES_CHANNEL = "1547305058922668072"   # mod-releases
LIMIT = 1900


def from_keys(name):
    v = os.environ.get(name, "").strip()
    if not v and os.path.exists(KEYFILE):
        for line in open(KEYFILE, encoding="utf-8"):
            line = line.strip()
            if line.startswith(name + "="):
                v = line.split("=", 1)[1].strip().strip('"').strip("'")
    return v


def version():
    header = open(os.path.join(ROOT, "mod", "src", "version.h"), encoding="utf-8").read()
    return re.search(r'ML_VERSION\s+"([^"]+)"', header).group(1)



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


def post(token, channel, content):
    url = "https://discord.com/api/v10/channels/%s/messages" % channel
    req = urllib.request.Request(url, data=json.dumps({"content": content}).encode("utf-8"),
                                 headers={"Content-Type": "application/json",
                                          "Authorization": "Bot " + token,
                                          "User-Agent": "MasterLooter-announce"})
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
    # One short post, written by hand for the release and gated like every
    # other piece of prose: private/discord/release-<version>.txt, posted as
    # it is. The first 1.6.7 announcement went out as the whole changelog in
    # three messages, the second as the changelog's opening paragraph, and
    # neither read as an announcement. Two plain lines saying what changed,
    # then the two links. Without the file the script refuses rather than
    # inventing one.
    hand = os.path.join(ROOT, "private", "discord", "release-%s.txt" % ver)
    if not os.path.exists(hand):
        print("No %s. Write the announcement there, two or three plain lines and the links, run prose_check.py on it, then run this again." % hand)
        return 3
    body = open(hand, encoding="utf-8").read().strip()
    if NEXUS_FILES not in body or (GITHUB_RELEASE % ver) not in body:
        print("The announcement must carry both links:\n  %s\n  %s" % (NEXUS_FILES, GITHUB_RELEASE % ver))
        return 3
    messages = chunks([body]) if len(body) > LIMIT else [body]
    for i, m in enumerate(messages, 1):
        print("--- message %d of %d (%d chars) ---" % (i, len(messages), len(m)))
        print(m)
    if not args.apply:
        print("\nREPORT ONLY - nothing was sent. Re-run with --apply to post.")
        return 0
    token = from_keys("DISCORD_BOT_TOKEN")
    if not token:
        print("No DISCORD_BOT_TOKEN in the environment or keys.local.env.")
        return 2
    channel = from_keys("DISCORD_RELEASES_CHANNEL") or RELEASES_CHANNEL
    for m in messages:
        post(token, channel, m)
    os.makedirs(os.path.dirname(marker), exist_ok=True)
    open(marker, "w").write("posted\n")
    print("\nPosted %d messages for %s." % (len(messages), ver))
    return 0


if __name__ == "__main__":
    sys.exit(main())
