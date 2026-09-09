"""Diff two [dump] snapshots out of a MasterLooter log.

Usage: python diffdump.py <log> <ts-a> <ts-b>
Each ts is the timestamp on the '[dump] ---- ... ----' header line.
"""
import re, sys, collections

log, tsa, tsb = sys.argv[1], sys.argv[2], sys.argv[3]

HDR = re.compile(r"\[(\d\d:\d\d:\d\d\.\d+)\] \[info \] \[dump\] ---- (.+?) break, component ([0-9A-F]+) ----")
REG = re.compile(r"\[dump\] (\S+) ([0-9A-F]+) \((.*?)\) (\d+) bytes")
ROW = re.compile(r"\[dump\]   (\S+) \+([0-9A-F]{3}) ((?:[0-9A-F]{2} ?)+)")
BUF = re.compile(r"\[dump\]   buff\[(\d+)\] ((?:[0-9A-F]{2} ?)+)")
BUFH = re.compile(r"\[dump\] player collect-buff array ([0-9A-F]+) x(\d+)")


def grab(ts):
    """Return {tag: {off: bytes}} for the snapshot whose header carries ts."""
    out = collections.OrderedDict()
    meta = {}
    on = False
    for line in open(log, encoding="utf-8", errors="replace"):
        h = HDR.search(line)
        if h:
            if on:
                break
            on = h.group(1) == ts
            if on:
                meta["how"], meta["comp"] = h.group(2), h.group(3)
            continue
        if not on:
            continue
        m = REG.search(line)
        if m:
            out.setdefault(m.group(1), {})["__addr"] = m.group(2)
            out[m.group(1)]["__cls"] = m.group(3)
            continue
        m = ROW.search(line)
        if m:
            out.setdefault(m.group(1), {})[int(m.group(2), 16)] = m.group(3).split()
            continue
        m = BUFH.search(line)
        if m:
            out.setdefault("buff", {})["__addr"] = m.group(1)
            out["buff"]["__cls"] = "x" + m.group(2)
            continue
        m = BUF.search(line)
        if m:
            out.setdefault("buff", {})[int(m.group(1))] = m.group(2).split()
    return meta, out


ma, A = grab(tsa)
mb, B = grab(tsb)
print("A %s  %s break  comp %s" % (tsa, ma.get("how"), ma.get("comp")))
print("B %s  %s break  comp %s" % (tsb, mb.get("how"), mb.get("comp")))
print()

for tag in sorted(set(A) | set(B)):
    ra, rb = A.get(tag, {}), B.get(tag, {})
    offs = sorted(o for o in set(ra) | set(rb) if isinstance(o, int))
    diffs = []
    for o in offs:
        va, vb = ra.get(o), rb.get(o)
        if va == vb:
            continue
        diffs.append((o, va, vb))
    same = len(offs) - len(diffs)
    print("== %-10s A %s (%s)  B %s (%s)   %d rows same, %d differ"
          % (tag, ra.get("__addr", "-"), ra.get("__cls", "-"),
             rb.get("__addr", "-"), rb.get("__cls", "-"), same, len(diffs)))
    for o, va, vb in diffs:
        # byte-level, so a whole-row pointer change does not hide a one-byte flag
        if va and vb:
            marks = "".join("^^ " if x != y else ".. " for x, y in zip(va, vb))
            print("   +%03X A %s" % (o, " ".join(va)))
            print("        B %s" % " ".join(vb))
            print("          %s" % marks)
        else:
            side = "A only" if va else "B only"
            print("   +%03X %s %s" % (o, side, " ".join(va or vb)))
    print()
