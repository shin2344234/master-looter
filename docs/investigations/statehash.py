"""Jenkins lookup3 hashlittle, as the game computes gimmick state ids.

The routine at RVA 0x12D5630 is hashlittle with the seed baked in:
  a = b = c = length + 0xDEBA1DCD
(the `lea ebx,[rdx-0x2145e233]` at 0x12D563F, since -0x2145E233 == 0xDEBA1DCD)
and the standard mix/final rotation schedules.

Observed ids to reproduce:
  0x866C7489  a vein's intact state
  0x150B14D0  after the first transition of a break
  0x353C1CAD  after the second
"""
import sys

M = 0xFFFFFFFF


def rot(x, k):
    return ((x << k) | (x >> (32 - k))) & M


def hashlittle(data, initval=0):
    length = len(data)
    a = b = c = (length + 0xDEBA1DCD + initval) & M

    i = 0
    while length - i > 12:
        a = (a + int.from_bytes(data[i:i + 4], "little")) & M
        b = (b + int.from_bytes(data[i + 4:i + 8], "little")) & M
        c = (c + int.from_bytes(data[i + 8:i + 12], "little")) & M
        # mix(a,b,c)
        a = (a - c) & M; a ^= rot(c, 4);  c = (c + b) & M
        b = (b - a) & M; b ^= rot(a, 6);  a = (a + c) & M
        c = (c - b) & M; c ^= rot(b, 8);  b = (b + a) & M
        a = (a - c) & M; a ^= rot(c, 16); c = (c + b) & M
        b = (b - a) & M; b ^= rot(a, 19); a = (a + c) & M
        c = (c - b) & M; c ^= rot(b, 4);  b = (b + a) & M
        i += 12

    tail = data[i:]
    n = len(tail)
    if n:
        pad = tail + b"\0" * (12 - n)
        a = (a + int.from_bytes(pad[0:4], "little")) & M
        b = (b + int.from_bytes(pad[4:8], "little")) & M
        c = (c + int.from_bytes(pad[8:12], "little")) & M
        # final(a,b,c)
        c ^= b; c = (c - rot(b, 14)) & M
        a ^= c; a = (a - rot(c, 11)) & M
        b ^= a; b = (b - rot(a, 25)) & M
        c ^= b; c = (c - rot(b, 16)) & M
        a ^= c; a = (a - rot(c, 4))  & M
        b ^= a; b = (b - rot(a, 14)) & M
        c ^= b; c = (c - rot(b, 24)) & M
    return c


TARGETS = {
    0x866C7489: "intact state (before any break)",
    0x150B14D0: "after break transition 1",
    0x353C1CAD: "after break transition 2",
}

CANDIDATES = [
    "Clear", "Deactive", "Deactivate", "Active", "Activate", "Initial", "Init",
    "Break", "Broken", "BreakStart", "BreakEnd", "Breaking",
    "SelfForceBreakImpulse", "ForceBreak", "SelfForceBreak",
    "Destroy", "Destroyed", "Dead", "Death", "Empty", "Open", "Opened",
    "Close", "Closed", "Idle", "Default", "None", "Normal", "Spawn", "Spawned",
    "Collect", "Collected", "Gather", "Gathered", "Mine", "Mined",
    "Hit", "Damaged", "Damage", "Disable", "Disabled", "Enable", "Enabled",
    "Reset", "Respawn", "End", "Start", "Finish", "Finished", "Complete",
    "Wait", "Ready", "Done", "Off", "On", "Hide", "Hidden", "Show",
]


def main():
    if len(sys.argv) > 1:
        for s in sys.argv[1:]:
            print("%-28s -> 0x%08X" % (s, hashlittle(s.encode())))
        return
    print("checking %d candidate names against %d observed ids\n" % (len(CANDIDATES), len(TARGETS)))
    hit = False
    for name in CANDIDATES:
        for enc, label in ((name.encode("ascii"), "ascii"),
                           (name.encode("utf-16-le"), "utf16"),
                           (name.lower().encode("ascii"), "lower")):
            h = hashlittle(enc)
            if h in TARGETS:
                hit = True
                print("  MATCH  %-24s %-6s -> 0x%08X   %s" % (name, label, h, TARGETS[h]))
    if not hit:
        print("  no match. Either the seed/schedule differs, the names are not in this")
        print("  list, or the id is hashed from something other than a bare state name.")
        print("\n  sample output so the schedule can be eyeballed:")
        for n in ("Clear", "Deactive", "Break", "SelfForceBreakImpulse"):
            print("    %-24s -> 0x%08X" % (n, hashlittle(n.encode())))


if __name__ == "__main__":
    main()
