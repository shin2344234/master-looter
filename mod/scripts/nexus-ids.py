"""Read back the Nexus ids a release needs, and what is already published.

    py -3 nexus-ids.py

The mod page URL carries 3402, which is not the id the v3 API wants for either
call. This resolves the real ones and lists the files on the page with their
versions, so publish-nexus.ps1 can be checked against reality if the page is
ever restructured.

Read only. It sends nothing and changes nothing.

Needs NEXUS_API_KEY, in the environment or in keys.local.env beside this file.
"""
import json
import os
import sys
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vtscan import read_key  # same environment-then-keyfile lookup

API = "https://api.nexusmods.com/v3"
GAME = "crimsondesert"
PAGE_ID = "3402"          # the number in the mod page URL


def get(key, path):
    req = urllib.request.Request(API + path)
    req.add_header("apikey", key)
    req.add_header("accept", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return json.loads(r.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        raise SystemExit("Nexus GET %s failed: %s %s\n%s"
                         % (path, e.code, e.reason,
                            e.read().decode("utf-8", "replace")[:300]))


def main():
    key = read_key("NEXUS_API_KEY")
    if not key:
        raise SystemExit(
            "No key. Get one at https://www.nexusmods.com/settings/api-keys,\n"
            "then set NEXUS_API_KEY or put it in keys.local.env beside this file.")

    mod = get(key, "/games/%s/mods/%s" % (GAME, PAGE_ID))["data"]
    mod_id = mod["id"]
    print("mod page %s is %s" % (PAGE_ID, mod.get("name")))
    print("  v3 mod id  %s   (-ModId)" % mod_id)
    print()

    files = get(key, "/mods/%s/files" % mod_id)["data"]["mod_files"]
    for f in files:
        state = "active" if f.get("is_active") else "inactive"
        print("  file id %-10s %-26s %s, %s versions"
              % (f["id"], f.get("name", "")[:26], state, f.get("versions_count")))
        if not f.get("is_active"):
            continue
        vs = get(key, "/mod-files/%s/versions" % f["id"])["data"]["versions"]
        for v in sorted(vs, key=lambda x: x.get("uploaded_at", ""), reverse=True):
            print("      %-8s %-10s %s%s"
                  % (v.get("version"), v.get("category"),
                     (v.get("uploaded_at") or "")[:19],
                     "   primary" if v.get("is_primary") else ""))
    print()
    print("publish-nexus.ps1 uses the active file id and the v3 mod id above.")
    print("A version already listed must not be published again: the changelog")
    print("endpoint appends, so a second run posts the text twice.")


if __name__ == "__main__":
    sys.exit(main())
