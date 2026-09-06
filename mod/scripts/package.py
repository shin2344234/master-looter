"""Zip the staged dist folder into the two release archives.

    py -3 package.py

Run build.bat first. Two archives come out of dist:

  MasterLooter-<version>.zip      the full package: plugin, README, licence
                                  and third-party notices, for installing by
                                  hand.
  MasterLooter-<version>-DMM.zip  the plugin alone, for Definitive Mod
                                  Manager: it registers the .asi as an add-on
                                  and deploys it itself. The data tables are
                                  built into the plugin, so nothing else is
                                  needed.

It prints the SHA-256 of both archives and of the plugin when it is done, in
the order the README and the Nexus description list them, so a release can be
published with the checksums it claims.
"""
import hashlib
import os
import re
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
MOD = os.path.dirname(HERE)
DIST = os.path.join(MOD, "dist")
FULL = ["MasterLooter.asi", "README.md", "THIRD_PARTY_NOTICES.md", "LICENSE"]
DMM = ["MasterLooter.asi"]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def write_zip(name, files):
    out = os.path.join(DIST, name)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for f in files:
            path = os.path.join(DIST, f)
            if not os.path.exists(path):
                sys.exit("missing %s; run build.bat first" % path)
            z.write(path, f)
    print("wrote", out, "(%d bytes)" % os.path.getsize(out))
    return out


def main():
    header = open(os.path.join(MOD, "src", "version.h"), encoding="utf-8").read()
    version = re.search(r'ML_VERSION\s+"([^"]+)"', header).group(1)
    full = write_zip("MasterLooter-%s.zip" % version, FULL)
    dmm = write_zip("MasterLooter-%s-DMM.zip" % version, DMM)
    print("\nSHA-256 for %s:" % version)
    for path in (dmm, full, os.path.join(DIST, "MasterLooter.asi")):
        print("%s  %s" % (sha256(path), os.path.basename(path)))


if __name__ == "__main__":
    main()
