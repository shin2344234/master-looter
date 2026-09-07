"""Pull the ore-vein evidence out of every surviving log into one table."""
import io, os, re, collections

HERE = os.path.dirname(os.path.abspath(__file__))

LOGS = [
    (os.path.join(HERE, "logs", "session-A-v1.2.1-2026-09-06.log"), "A"),
    (os.path.join(HERE, "logs", "session-B-v1.3.0-0942.log"), "B"),
]

def ts(line):
    m = re.match(r"\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]", line)
    if not m:
        return None
    h, mi, s, ms = (int(x) for x in m.groups())
    return ((h * 60 + mi) * 60 + s) * 1000 + ms

def hhmmss(t):
    return "%02d:%02d:%02d.%03d" % (t // 3600000, (t // 60000) % 60, (t // 1000) % 60, t % 1000)

for path, tag in LOGS:
    if not os.path.exists(path):
        print("MISSING", path)
        continue
    lines = io.open(path, encoding="utf-8", errors="replace").read().splitlines()
    banner = lines[0][:100] if lines else ""
    breaks, picks, armfill, armgiveup, oreReview, armnames = [], [], [], [], [], []
    armCount = collections.Counter()
    for ln in lines:
        t = ts(ln)
        if "[loot] break" in ln:
            m = re.search(r"break (.+?) \(([\d.]+) m\) eid ([0-9A-F]{8}) type (\d+)", ln)
            if m: breaks.append((t,) + m.groups())
        elif "[loot] pick up" in ln:
            m = re.search(r"pick up (.+?) \(", ln)
            if m: picks.append((t, m.group(1)))
        if "filled" in ln and "[arm]" in ln:
            m = re.search(r"eid ([0-9A-F]{8}) filled (\d+) ms.*\(([a-z]+), type (\d+)\)", ln)
            if m: armfill.append((t,) + m.groups())
        if "nothing after" in ln:
            m = re.search(r"eid ([0-9A-F]{8}).*nothing after (\d+) arms", ln)
            if m: armgiveup.append((t,) + m.groups())
        if "[ore-review]" in ln or "native break-related event" in ln:
            oreReview.append((t, ln.strip()))
        if "[armname]" in ln:
            armnames.append((t, ln.strip()))
        m = re.search(r"\[arm\] arming eid ([0-9A-F]{8}).*node .*/([a-z0-9_]+)\.prefab", ln)
        if m and "mine" in m.group(2):
            armCount[(m.group(1), m.group(2))] += 1

    print("=" * 78)
    print("SESSION %s  %s" % (tag, os.path.basename(path)))
    print("  " + banner)
    print("  breaks=%d  pickups=%d  arm-fills=%d  arm-giveups=%d" % (len(breaks), len(picks), len(armfill), len(armgiveup)))
    if picks:
        print("  pickup breakdown: " + ", ".join("%s x%d" % (k, v) for k, v in collections.Counter(p[1] for p in picks).items()))
    if breaks:
        types = collections.Counter(b[4] for b in breaks)
        print("  break 'type' field: " + ", ".join("type %s x%d" % (k, v) for k, v in types.items()))
    if armfill:
        print("  nodes that FILLED after arming:")
        for t, eid, ms, kind, ty in armfill:
            print("     %s eid %s after %s ms (%s, type %s)" % (hhmmss(t), eid, ms, kind, ty))
    if armgiveup:
        print("  nodes that gave up after N arms:")
        for t, eid, n in armgiveup:
            print("     %s eid %s after %s arms" % (hhmmss(t), eid, n))
    mineArms = [(k, v) for k, v in armCount.items() if v >= 2]
    if mineArms:
        print("  mine nodes armed 2+ times (eid, prefab, count):")
        for (eid, pf), n in sorted(mineArms, key=lambda x: -x[1])[:10]:
            struck = any(b[3] == eid for b in breaks)
            print("     %s %-42s armed %d  struck=%s" % (eid, pf, n, struck))
    if oreReview:
        print("  ore-review / native captures:")
        for t, ln in oreReview:
            print("     " + ln[:150])
    if armnames:
        mine = [a for a in armnames if "mine_" in a[1]]
        print("  armname lines for mine prefabs: %d of %d total" % (len(mine), len(armnames)))
        for t, ln in mine:
            print("     " + re.sub(r"^\[[\d:.]+\] \[info \] ", "", ln)[:130])
    print()
