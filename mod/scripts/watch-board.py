"""Watch the Nexus posts tab, the Nexus bugs tab, the GitHub issues and the
Discord help-n-bug-reports forum for anything new, and print one line per new
thing. Silence means nothing changed.

    py -3 watch-board.py --once      one pass, then exit
    py -3 watch-board.py             poll every ten minutes for ever

State lives in private/watch-state.json so a restart does not replay what
was already seen. The first run seeds the state and prints only a summary.

What it sees: every comment on page one of the posts tab (a new comment
anywhere bumps its thread to page one), every row on the bugs tab with its
status, and every issue and issue comment on GitHub since the last pass.
What it cannot see: replies inside a bugs-tab row, which load through a
script call the page does not expose to a plain fetch.

The Discord side needs the bot's token in DISCORD_BOT_TOKEN, in the
environment or in keys.local.env beside this script; without it the forum
is skipped and the seed line says so. It watches the active threads of the
forum: a new thread prints its title and opening post, and a reply in a
known thread prints the reply. Seth's own messages and the bot's are not
news. Archived threads are left alone.
"""
import io, json, os, re, subprocess, sys, time, html, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
STATE = os.path.join(REPO, "private", "watch-state.json")
MOD = "https://www.nexusmods.com/crimsondesert/mods/3402"
GH = "shin2344234/master-looter"
UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/128 Safari/537.36"
INTERVAL = 600
KEYFILE = os.path.join(HERE, "keys.local.env")
DISCORD_GUILD = "1547304303646089296"
DISCORD_FORUM = "1547305334945615922"      # help-n-bug-reports
DISCORD_FORUM_NAME = "help-n-bug-reports"
DISCORD_SELF = {"355497711568551947", "1547307453107150979"}   # Seth, the bot


def fetch(url):
    # curl, because the site's edge answers urllib with a 403 and curl with
    # the page, on the same user agent.
    r = subprocess.run(["curl", "-s", "-f", "-A", UA, "--max-time", "60", url],
                       capture_output=True)
    if r.returncode != 0:
        raise RuntimeError("curl exit %d" % r.returncode)
    return r.stdout.decode("utf-8", "replace")


def clean(s):
    s = re.sub(r"<br\s*/?>", " ", s)
    s = re.sub(r"<[^>]+>", "", s)
    s = html.unescape(s)
    return re.sub(r"\s+", " ", s).strip()


def posts():
    """{comment id: (author, date, text)} for page one of the posts tab."""
    h = fetch(MOD + "?tab=posts")
    out = {}
    for m in re.finditer(r'id="comment-(\d+)"(.*?)<ul class="comment-inline', h, re.S):
        cid, body = m.group(1), m.group(2)
        a = re.search(r'class="comment-name">\s*<a[^>]*>\s*([^<]+?)\s*</a>', body)
        d = re.search(r'data-date="(\d+)"[^>]*>([^<]+)</time>', body)
        t = re.search(r'id="comment-content-\d+"[^>]*>(.*?)</div>', body, re.S)
        out[cid] = (a.group(1) if a else "?", d.group(2) if d else "?", clean(t.group(1))[:160] if t else "")
    return out


def bugs():
    """{issue id: (title, status)} for the bugs tab."""
    h = fetch(MOD + "?tab=bugs")
    out = {}
    for m in re.finditer(r'id="issue_(\d+)"(.*?)</tr>', h, re.S):
        iid, body = m.group(1), m.group(2)
        t = re.search(r'class="issue-title"[^>]*>(.*?)</a>', body, re.S)
        s = re.search(r'inline-status"><span[^>]*>([^<]+)</span>', body)
        out[iid] = (clean(t.group(1)) if t else "?", s.group(1).strip() if s else "?")
    return out


def gh(path):
    r = subprocess.run(["gh", "api", path], capture_output=True, text=True, encoding="utf-8")
    if r.returncode != 0:
        return []
    try:
        return json.loads(r.stdout)
    except ValueError:
        return []


def github(since):
    lines = []
    for it in gh("repos/%s/issues?state=all&since=%s&per_page=50" % (GH, since)):
        if it.get("pull_request"):
            continue
        if it["created_at"] > since and it["user"]["login"] != "shin2344234":
            lines.append("github: new issue #%d by %s: %s" % (it["number"], it["user"]["login"], it["title"]))
    for c in gh("repos/%s/issues/comments?since=%s&per_page=50" % (GH, since)):
        if c["created_at"] > since and c["user"]["login"] != "shin2344234":
            n = c["issue_url"].rsplit("/", 1)[-1]
            lines.append("github: comment on #%s by %s: %s" % (n, c["user"]["login"], clean(c["body"])[:120]))
    return lines


def discord_token():
    tok = os.environ.get("DISCORD_BOT_TOKEN", "").strip()
    if not tok and os.path.exists(KEYFILE):
        for line in open(KEYFILE, encoding="utf-8"):
            line = line.strip()
            if line.startswith("DISCORD_BOT_TOKEN="):
                tok = line.split("=", 1)[1].strip().strip('"')
    return tok


def dapi(path, token):
    req = urllib.request.Request("https://discord.com/api/v10" + path,
                                 headers={"Authorization": "Bot " + token, "User-Agent": "MasterLooterWatch (https://github.com/shin2344234/master-looter, 1)"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read().decode("utf-8"))


def discord(st, seeded, token):
    """New threads and new replies in the forum. State is {thread id: last
    message id seen}; a thread whose last message id has not moved costs no
    call, so a quiet pass is one request."""
    lines = []
    known = st.setdefault("discord", {})
    act = dapi("/guilds/%s/threads/active" % DISCORD_GUILD, token).get("threads", [])
    for t in act:
        if t.get("parent_id") != DISCORD_FORUM:
            continue
        tid, last, name = t["id"], known.get(t["id"]), t.get("name", "?")
        newest = t.get("last_message_id") or tid
        if not seeded:
            known[tid] = newest
            continue
        if last is not None and last == newest:
            continue
        msgs = dapi("/channels/%s/messages?limit=100&after=%s" % (tid, last or "0"), token)
        for m in sorted(msgs, key=lambda m: int(m["id"])):
            a = m.get("author") or {}
            if a.get("id") in DISCORD_SELF:
                continue
            what = "new thread" if m["id"] == tid else "reply"
            body = clean(m.get("content", "")) or ("(%d attachment(s))" % len(m.get("attachments", [])))
            lines.append("discord: %s in %s by %s: %s: %s" % (what, DISCORD_FORUM_NAME, a.get("username", "?"), name, body[:140]))
        if last is None and all((m.get("author") or {}).get("id") in DISCORD_SELF for m in msgs):
            lines.append("discord: new thread in %s: %s" % (DISCORD_FORUM_NAME, name))
        known[tid] = max([newest] + [m["id"] for m in msgs], key=int)
    return lines


def load():
    try:
        return json.load(io.open(STATE, encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def save(st):
    io.open(STATE, "w", encoding="utf-8").write(json.dumps(st, indent=1))


def once(st):
    lines = []
    now = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    seeded = bool(st)
    fails = st.setdefault("fails", {})
    try:
        p = posts()
        seen = set(st.get("posts", []))
        for cid, (a, d, t) in p.items():
            # Seth's own replies are not news; the watch exists for everyone else.
            if seeded and cid not in seen and a != "shin234":
                lines.append("nexus post: %s, %s: %s" % (a, d, t))
        st["posts"] = sorted(seen | set(p))[-400:]
    except Exception as e:  # a failed fetch is not news until it keeps failing
        fails["posts"] = fails.get("posts", 0) + 1
        if fails["posts"] == 3:
            lines.append("watch: posts fetch has failed three passes running: %s" % e)
    else:
        fails["posts"] = 0
    try:
        b = bugs()
        old = st.get("bugs", {})
        for iid, (t, s) in b.items():
            if not seeded:
                continue
            if iid not in old:
                lines.append("nexus bug: new row %s [%s]: %s" % (iid, s, t))
            elif old[iid][1] != s:
                lines.append("nexus bug: %s now %s (was %s): %s" % (iid, s, old[iid][1], t))
        st["bugs"] = {k: list(v) for k, v in b.items()}
    except Exception as e:
        fails["bugs"] = fails.get("bugs", 0) + 1
        if fails["bugs"] == 3:
            lines.append("watch: bugs fetch has failed three passes running: %s" % e)
    else:
        fails["bugs"] = 0
    since = st.get("github_since")
    if since:
        lines.extend(github(since))
    st["github_since"] = now
    token = discord_token()
    if token:
        try:
            lines.extend(discord(st, "discord" in st, token))
        except Exception as e:
            fails["discord"] = fails.get("discord", 0) + 1
            if fails["discord"] == 3:
                lines.append("watch: discord fetch has failed three passes running: %s" % e)
        else:
            fails["discord"] = 0
    save(st)
    if not seeded:
        lines.append("watch: seeded with %d comments and %d bug rows; github from %s; discord %s" % (
            len(st.get("posts", [])), len(st.get("bugs", {})), now,
            "%d threads" % len(st.get("discord", {})) if token else "not watched (no DISCORD_BOT_TOKEN)"))
    elif token and "discord" in st and st.get("discord_seeded_at") is None:
        st["discord_seeded_at"] = now
        save(st)
        lines.append("watch: discord seeded with %d active threads in %s" % (len(st["discord"]), DISCORD_FORUM_NAME))
    return lines


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace", line_buffering=True)
    st = load()
    if "--once" in sys.argv:
        for l in once(st):
            print(l)
        return
    while True:
        for l in once(st):
            print(l)
        time.sleep(INTERVAL)


if __name__ == "__main__":
    main()
