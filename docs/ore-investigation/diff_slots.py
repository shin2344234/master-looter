"""Diff a gimmick's slot dump before and after the moment it was broken.

Usage: python diff_slots.py <log> <eid> <hh:mm:ss.mmm of the break>
"""
import io, os, re, sys

def ts(line):
    m = re.match(r"\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]", line)
    if not m:
        return None
    h, mi, s, ms = (int(x) for x in m.groups())
    return ((h * 60 + mi) * 60 + s) * 1000 + ms

def parse_break(s):
    h, mi, rest = s.split(":")
    sec, ms = rest.split(".")
    return ((int(h) * 60 + int(mi)) * 60 + int(sec)) * 1000 + int(ms)

def slots(line):
    """+OFF=VALUE or +OFF->desc pairs into a dict."""
    out = {}
    for m in re.finditer(r"\+([0-9A-F]+)(=|->)(\S+)", line):
        out["+" + m.group(1)] = m.group(3)
    return out

def main():
    log, eid, when = sys.argv[1], sys.argv[2].upper(), parse_break(sys.argv[3])
    dumps = []
    for ln in io.open(log, encoding="utf-8", errors="replace"):
        if "[probe] gimmick eid %s" % eid not in ln:
            continue
        t = ts(ln)
        if t is not None:
            dumps.append((t, slots(ln)))
    if not dumps:
        print("no dumps for", eid)
        return
    before = [d for d in dumps if d[0] <= when]
    after = [d for d in dumps if d[0] > when]
    print("eid %s: %d dumps, %d before the break, %d after" % (eid, len(dumps), len(before), len(after)))
    if not before or not after:
        print("need dumps on both sides")
        return
    b_t, b = before[-1]
    a_t, a = after[0]
    print("comparing last-before %s against first-after %s (%+d ms)\n"
          % (fmt(b_t), fmt(a_t), a_t - b_t))
    keys = sorted(set(b) | set(a), key=lambda k: int(k[1:], 16))
    changed = 0
    for k in keys:
        bv, av = b.get(k), a.get(k)
        if bv == av:
            continue
        changed += 1
        print("  %-8s %-24s -> %s" % (k, bv if bv is not None else "(absent)",
                                      av if av is not None else "(absent)"))
    if not changed:
        print("  nothing changed")
    print("\n  %d of %d slots differ" % (changed, len(keys)))

    # Stability check: do two dumps on the SAME side differ? If they do, some of
    # the above is noise rather than the break.
    if len(before) >= 2:
        _, b2 = before[-2]
        noise = [k for k in set(b) | set(b2) if b.get(k) != b2.get(k)]
        print("  control: %d slots differ between two dumps BEFORE the break%s"
              % (len(noise), (" -> " + ", ".join(sorted(noise)[:12])) if noise else ""))

def fmt(t):
    return "%02d:%02d:%02d.%03d" % (t // 3600000, (t // 60000) % 60, (t // 1000) % 60, t % 1000)

if __name__ == "__main__":
    main()
