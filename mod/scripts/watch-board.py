"""Watch the Nexus posts tab, the Nexus bugs tab and the GitHub issues for
anything new, and print one line per new thing. Silence means nothing changed.

    py -3 watch-board.py --once      one pass, then exit
    py -3 watch-board.py             poll every ten minutes for ever

State lives in private/watch-state.json so a restart does not replay what
was already seen. The first run seeds the state and prints only a summary.

What it sees: every comment on page one of the posts tab (a new comment
anywhere bumps its thread to page one), every row on the bugs tab with its
status, and every issue and issue comment on GitHub since the last pass.
What it cannot see: replies inside a bugs-tab row, which load through a
script call the page does not expose to a plain fetch.
"""
import io, json, os, re, subprocess, sys, time, html

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
STATE = os.path.join(REPO, "private", "watch-state.json")
MOD = "https://www.nexusmods.com/crimsondesert/mods/3402"
GH = "shin2344234/master-looter"
UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/128 Safari/537.36"
INTERVAL = 600


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
    save(st)
    if not seeded:
        lines.append("watch: seeded with %d comments and %d bug rows; github from %s" % (len(st.get("posts", [])), len(st.get("bugs", {})), now))
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
