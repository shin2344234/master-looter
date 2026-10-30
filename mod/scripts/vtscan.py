"""Look up, and optionally submit, the release files on VirusTotal.

    py -3 vtscan.py             report what VirusTotal already knows
    py -3 vtscan.py --upload    submit anything it has never seen or never
                                analysed, then wait
    py -3 vtscan.py --prose     also print the paragraphs the README and the
                                Nexus description want, with the numbers filled in

Synced from release-kit. Do not edit it here; the next sync overwrites it.

Run package.ps1 first: this reads the release files out of dist
and takes the version from the header named in release.json, so the files
it reports are the ones the release actually ships.

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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import release_config as rc

API = "https://www.virustotal.com/api/v3"
GUI = "https://www.virustotal.com/gui/file/"
CFG = rc.load()
DIST = CFG.dist
KEYFILE = rc.KEYFILE
read_key = rc.read_key


# The order the README and the description list them.
def release_files(version):
    return [(a["name"], "the %s file" % a["role"]) for a in CFG.assets(version)] + [
        (CFG.archive_name("dmm", version), "the DMM archive"),
        (CFG.archive_name("manual", version), "the full archive"),
        ("%s.asi" % CFG.file_base, "the plugin"),
    ]


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
    """Detections over engines that returned a verdict.

    This has to match the number on the web page, because that is the number
    the README and the description quote and someone will check it. The site
    counts only the four verdict categories. Engines that could not process
    the file (type-unsupported, four of them on the plugin) or errored
    (failure) are left out of the denominator, so summing every category
    reads 3/75 where the page says 3/70.
    """
    stats = attrs.get("last_analysis_stats") or {}
    hits = stats.get("malicious", 0) + stats.get("suspicious", 0)
    total = sum(stats.get(k, 0) for k in
                ("malicious", "suspicious", "undetected", "harmless"))
    return hits, total


def wait_for(key, aid):
    """Whether analysis aid completed within twenty minutes."""
    # Four requests a minute on a free key, so poll gently. An analysis can
    # read 404: Broomy 1.0.5's requested one did 20 minutes after the request.
    for _ in range(60):
        time.sleep(20)
        an = call(key, "/analyses/" + aid)
        if an and an["data"]["attributes"].get("status") == "completed":
            return True
    return False


def report_for(key, path, do_upload):
    digest = sha256(path)
    got = call(key, "/files/" + digest)
    # VirusTotal can hold a file it never analyses. Broomy 1.0.5's plugin was
    # accepted by an upload on 22 October 2026 and still had no analysis an
    # hour later, through two /files/<sha256>/analyse requests, the site's
    # Reanalyze. A second upload of the same file was analysed in two minutes,
    # so a file in that state is uploaded again.
    never = got is not None and not got["data"]["attributes"].get("last_analysis_date")
    if got is None or (never and do_upload):
        if not do_upload:
            return digest, None, "never scanned; run with --upload to submit it"
        size = os.path.getsize(path)
        if size > 32 * 1024 * 1024:
            return digest, None, "over the 32 MB direct upload limit; needs /files/upload_url"
        body, ctype = multipart(path)
        sub = call(key, "/files", "POST", body, ctype)
        if not wait_for(key, sub["data"]["id"]):
            return digest, None, "analysis did not complete in twenty minutes"
        got = call(key, "/files/" + digest)
        if got is None:
            return digest, None, "analysed but no file report yet"
    return digest, got["data"]["attributes"], None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--upload", action="store_true",
                    help="submit any file VirusTotal has never seen or never analysed")
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

    version = args.version or CFG.version
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
        if total == 0:
            # A fresh upload reads 0/0 until the engines report, which took
            # eight minutes on PSM 1.1.3. That is no result, not a clean one.
            if attrs.get("last_analysis_date"):
                print("%-34s 0/0  no engine has reported yet; run again in a few minutes" % name)
            else:
                print("%-34s 0/0  VirusTotal holds it but never analysed it; run with --upload to send it again" % name)
            print("%-34s %s%s" % ("", GUI, digest))
            rows.append((name, label, digest, None, None))
            continue
        print("%-34s %d/%d  %s" % (name, hits, total,
                                   ", ".join("%s %s" % kv for kv in sorted(names.items())) or "clean"))
        print("%-34s %s%s" % ("", GUI, digest))
        rows.append((name, label, digest, (hits, total), names))

    if not args.prose:
        return

    plugin = next((r for r in rows if r[0] == "%s.asi" % CFG.file_base and r[3]), None)
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
