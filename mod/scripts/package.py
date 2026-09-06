"""Zip the staged dist folder as MasterLooter-<version>.zip, ready to upload.

    py -3 package.py

Run build.bat first. The archive is flat: the four runtime files plus the
README, the licence and the third-party notices, so it can be dropped into
bin64 by hand or fed to a mod manager.
"""
import os
import re
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
MOD = os.path.dirname(HERE)
DIST = os.path.join(MOD, "dist")
FILES = ["MasterLooter.asi", "MasterLooter.items.tsv", "MasterLooter.creatures.tsv",
         "README.md", "THIRD_PARTY_NOTICES.md", "LICENSE"]


def main():
    header = open(os.path.join(MOD, "src", "version.h"), encoding="utf-8").read()
    version = re.search(r'ML_VERSION\s+"([^"]+)"', header).group(1)
    out = os.path.join(DIST, "MasterLooter-%s.zip" % version)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for name in FILES:
            path = os.path.join(DIST, name)
            if not os.path.exists(path):
                sys.exit("missing %s; run build.bat first" % path)
            z.write(path, name)
    print("wrote", out, "(%d bytes)" % os.path.getsize(out))


if __name__ == "__main__":
    main()
