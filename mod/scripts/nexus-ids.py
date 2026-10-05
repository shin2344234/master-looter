"""Read back the Nexus ids a release needs, and what is already published.

    py -3 nexus-ids.py

Synced from release-kit. Do not edit it here; the next sync overwrites it.

The mod page URL carries a page number, which is not the id the v3 API wants
for either call. This resolves the real ones from nexus.page in release.json
and lists the files on the page with their versions, so the ids in
release.json can be checked against reality. Paste what it reads back into
nexus.modId, nexus.dmmFileId and nexus.manualFileId.

Read only. It sends nothing and changes nothing.

Needs NEXUS_API_KEY, in the environment or in keys.local.env beside this file.
"""
import json
import os
import sys
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import release_config as rc

CFG = rc.load()
read_key = rc.read_key

API = "https://api.nexusmods.com/v3"
GAME = CFG.nexus.get("game", "crimsondesert")
# The number in the mod page URL, from release.json. Never borrow another
# mod's, which is how a Glint Spotter announcement reached the Flight Freedom
# page on 14 September 2026.
PAGE_ID = os.environ.get("NEXUS_PAGE_ID", "").strip() or str(CFG.nexus.get("page", "")).strip()


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
    if not PAGE_ID:
        raise SystemExit("nexus.page is empty in release.json. Create the page first, then set it.")
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
    # The DMM entry's newest version must be the page's primary download, or a
    # mod manager can fetch the manual archive in its place. The manual one's
    # Mod manager download button must also be off, which the API does not
    # report, so check that on the Files tab.
    dmm = str(CFG.nexus.get("dmmFileId", ""))
    if dmm:
        vs = get(key, "/mod-files/%s/versions" % dmm)["data"]["versions"]
        live = [v for v in vs if v.get("category") not in ("archived", "removed", "old_version")]
        if live and not any(v.get("is_primary") for v in live):
            print("WARNING: the DMM entry %s is not the primary download. Tick 'Set as primary file"
                  " for download' on its newest version in the Edit dialog, or publish again." % dmm)
    print("release.json has: modId %s, dmmFileId %s, manualFileId %s"
          % (CFG.nexus.get("modId"), CFG.nexus.get("dmmFileId"), CFG.nexus.get("manualFileId")))
    print("publish-nexus.ps1 uses those. If they differ from the ids above, fix release.json.")
    print("A version already listed must not be published again: the changelog")
    print("endpoint appends, so a second run posts the text twice.")


if __name__ == "__main__":
    sys.exit(main())
