"""Look up, and optionally submit, the release files on VirusTotal.

    py -3 vtscan.py             report what VirusTotal already knows
    py -3 vtscan.py --upload    submit anything it has never seen, then wait
    py -3 vtscan.py --prose     also print the paragraphs the README and the
                                Nexus description want, with the numbers filled in

Run package.py first: this reads the two archives and the plugin out of dist
and takes the version from version.h, so the three files it reports are the
three the release actually ships.

Needs a key in VT_API_KEY. Get one by signing in at virustotal.com and opening
the API key page from the account menu. The free key is enough: the limits are
500 requests a day at four a minute, and a release costs six. The free key may
not be used commercially, which this is not.

Nexus submits the DMM archive by itself when a file is uploaded, so that one
usually has a report before this script asks. The loose plugin never does, and
the plugin's number is the one worth quoting, because the scanners are
objecting to the PE rather than to a zip container.
"""
import argparse
import hashlib
import io
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

API = "https://www.virustotal.com/api/v3"
GUI = "https://www.virustotal.com/gui/file/"
HERE = os.path.dirname(os.path.abspath(__file__))
MOD = os.path.dirname(HERE)
DIST = os.path.join(MOD, "dist")

# The order the README and the description list them.
def release_files(version):
    return [
        ("MasterLooter-%s-DMM.zip" % version, "the DMM archive"),
        ("MasterLooter-%s.zip" % version, "the full archive"),
        ("MasterLooter.asi", "the plugin"),
    ]


def version_from_header():
    h = os.path.join(MOD, "src", "version.h")
    text = io.open(h, encoding="utf-8").read()
    m = re.search(r'#define\s+ML_VERSION\s+"([^"]+)"', text)
    if not m:
        raise SystemExit("no ML_VERSION in %s" % h)
    return m.group(1)


# Keys come from the environment first. keys.local.env beside this script is
# the fallback, so both keys can live in one gitignored file instead of two
# environment variables. Nothing here ever prints a key.
KEYFILE = os.path.join(HERE, "keys.local.env")


def read_key(name):
    got = os.environ.get(name)
    if got:
        return got.strip()
    if not os.path.exists(KEYFILE):
        return None
    for line in io.open(KEYFILE, encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, _, v = line.partition("=")
        if k.strip() == name:
            return v.strip().strip('"').strip("'")
    return None


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def call(key, path, method="GET", body=None, ctype=None, full_url=None):
    url = full_url or (API + path)
    req = urllib.request.Request(url, data=body, method=method)
    req.add_header("x-apikey", key)
    req.add_header("accept", "application/json")
    if ctype:
        req.add_header("content-type", ctype)
    try:
        with urllib.request.urlopen(req, timeout=180) as r:
            return json.loads(r.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        detail = e.read().decode("utf-8", "replace")[:400]
        raise SystemExit("VirusTotal %s %s failed: %s %s\n%s"
                         % (method, url, e.code, e.reason, detail))


def multipart(path):
    """A file part, built by hand so there is no third-party dependency."""
    boundary = "----vtscan%s" % hashlib.md5(path.encode("utf-8")).hexdigest()
    name = os.path.basename(path)
    head = (
        "--%s\r\n"
        'Content-Disposition: form-data; name="file"; filename="%s"\r\n'
        "Content-Type: application/octet-stream\r\n\r\n" % (boundary, name)
    ).encode("utf-8")
    tail = ("\r\n--%s--\r\n" % boundary).encode("utf-8")
    with open(path, "rb") as f:
        body = head + f.read() + tail
    return body, "multipart/form-data; boundary=%s" % boundary


def flagged(attrs):
    """Engine name to label, for engines that called it something."""
    out = {}
    for engine, res in (attrs.get("last_analysis_results") or {}).items():
        if res.get("category") in ("malicious", "suspicious") and res.get("result"):
            out[engine] = res["result"]
    return out


def score(attrs):
    stats = attrs.get("last_analysis_stats") or {}
    hits = stats.get("malicious", 0) + stats.get("suspicious", 0)
    total = sum(v for k, v in stats.items() if k != "timeout")
    return hits, total


def report_for(key, path, do_upload):
    digest = sha256(path)
    got = call(key, "/files/" + digest)
    if got is None:
        if not do_upload:
            return digest, None, "never scanned; run with --upload to submit it"
        size = os.path.getsize(path)
        if size > 32 * 1024 * 1024:
            return digest, None, "over the 32 MB direct upload limit; needs /files/upload_url"
        body, ctype = multipart(path)
        sub = call(key, "/files", "POST", body, ctype)
        aid = sub["data"]["id"]
        # Four requests a minute on a free key, so poll gently.
        for _ in range(60):
            time.sleep(20)
            an = call(key, "/analyses/" + aid)
            if (an["data"]["attributes"].get("status")) == "completed":
                break
        else:
            return digest, None, "analysis did not complete in twenty minutes"
        got = call(key, "/files/" + digest)
        if got is None:
            return digest, None, "analysed but no file report yet"
    return digest, got["data"]["attributes"], None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--upload", action="store_true",
                    help="submit any file VirusTotal has never seen")
    ap.add_argument("--prose", action="store_true",
                    help="print the README and description paragraphs")
    ap.add_argument("--version", help="override the version read from version.h")
    args = ap.parse_args()

    key = read_key("VT_API_KEY") or read_key("VIRUSTOTAL_API_KEY")
    if not key:
        raise SystemExit(
            "No key. Get one from the account menu at virustotal.com, then either\n"
            "  setx VT_API_KEY <key>          and open a new shell, or\n"
            "  put VT_API_KEY=<key> in %s\n"
            "which is gitignored." % KEYFILE)

    version = args.version or version_from_header()
    rows = []
    for name, label in release_files(version):
        path = os.path.join(DIST, name)
        if not os.path.exists(path):
            print("missing, skipped: %s" % path)
            continue
        digest, attrs, why = report_for(key, path, args.upload)
        if attrs is None:
            print("%-34s %s" % (name, why))
            rows.append((name, label, digest, None, None))
            continue
        hits, total = score(attrs)
        names = flagged(attrs)
        print("%-34s %d/%d  %s" % (name, hits, total,
                                   ", ".join("%s %s" % kv for kv in sorted(names.items())) or "clean"))
        print("%-34s %s%s" % ("", GUI, digest))
        rows.append((name, label, digest, (hits, total), names))

    if not args.prose:
        return

    plugin = next((r for r in rows if r[0] == "MasterLooter.asi" and r[3]), None)
    dmm = next((r for r in rows if r[0].endswith("-DMM.zip") and r[3]), None)
    if not plugin:
        print("\nNo plugin report yet, so no prose.")
        return
    hits, total = plugin[3]
    named = ", ".join("%s (%s)" % (e, r) for e, r in sorted(plugin[4].items()))
    print("\n--- for README.md and the description ---")
    print("plugin %d/%d: %s" % (hits, total, named or "nothing"))
    if dmm:
        print("DMM archive %d/%d" % dmm[3])
    print("plugin report:  %s%s" % (GUI, plugin[2]))
    if dmm:
        print("archive report: %s%s" % (GUI, dmm[2]))
    print("\nQuote the trend across releases, not this one number. The line in\n"
          "README.md and nexus-description.bbcode carries the whole sequence.")


if __name__ == "__main__":
    sys.exit(main())
