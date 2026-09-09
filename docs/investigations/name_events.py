"""Name the gimmick transition ids the mod logs, by hashing the game's strings.

The state driver hook writes lines like

    [gstate] event ED6D77EF target B0100537 comp 345819BE000 sample 1

and the id is Jenkins lookup3 over the lowercase event name (see statehash.py),
so it cannot be read back. It can be matched forward: hash every identifier in
CrimsonDesert.exe, in both ASCII and UTF-16, and see which ones land on an id
the log actually saw.

    python name_events.py [logfile]

About a third of them resolve. The rest are names the exe does not carry as a
contiguous string, and they are not in the .binarygimmick definitions either:
hashing all twelve well definitions matched none of the ids seen on a well.
An unnamed id is still usable, because DriveGimmickEvent takes the number.
"""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from statehash import hashlittle  # noqa: E402

EXE = r"D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\CrimsonDesert.exe"
LOG = r"D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\MasterLooter.log"
ASCII = re.compile(rb"[A-Za-z][A-Za-z0-9_]{2,95}")
WIDE = re.compile(rb"(?:[A-Za-z][\x00][A-Za-z0-9_][\x00]){3,96}")


def seen_ids(path):
    out = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.search(r"\[gstate\] event ([0-9A-F]{8}) target (\S+)", line)
            if m:
                out.setdefault(int(m.group(1), 16), set()).add(m.group(2))
    return out


def name_them(ids):
    found = {}
    with open(EXE, "rb") as f:
        prev = b""
        while True:
            chunk = f.read(1 << 24)
            if not chunk:
                break
            buf = prev + chunk
            for m in ASCII.finditer(buf):
                w = m.group()
                if hashlittle(w.lower()) in ids:
                    found.setdefault(hashlittle(w.lower()), set()).add(w.decode("ascii"))
            for m in WIDE.finditer(buf):
                w = m.group().replace(b"\x00", b"")
                if hashlittle(w.lower()) in ids:
                    found.setdefault(hashlittle(w.lower()), set()).add(w.decode("ascii"))
            prev = chunk[-256:]
    return found


def main():
    log = sys.argv[1] if len(sys.argv) > 1 else LOG
    ids = seen_ids(log)
    # These are recorded in loot/game.h and are the check that the hash is right.
    for word, want in (("onbreak", 0xFD8F7D2C), ("wait", 0x866C7489),
                       ("gimmickon", 0x150B14D0), ("break", 0x353C1CAD)):
        got = hashlittle(word.encode())
        if got != want:
            print("hash is wrong: %s gives %08X, should be %08X" % (word, got, want))
            return 1
    found = name_them(ids)
    print("%d ids in the log, %d named\n" % (len(ids), len(found)))
    for h in sorted(ids):
        print("  %08X  %-38s %s" % (h, "/".join(sorted(found.get(h, ["-"])))[:38],
                                    " ".join(sorted(ids[h]))[:60]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
