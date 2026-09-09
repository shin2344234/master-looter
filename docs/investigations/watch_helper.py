"""Helpers for catching the vein state writer with x64dbg.

  python watch_helper.py addr
      Tail MasterLooter.log and print the x64dbg command for each vein the mod
      probes within 4 m. Leave it running, walk up to a vein, copy the line.

  python watch_helper.py rip 7FF6ABCD1234
      Turn an address x64dbg stopped at into an RVA and disassemble around it.
      A data breakpoint is a TRAP: it fires after the store, so the writing
      instruction is the one BEFORE the address shown. This prints both.
"""
import io, os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
LOG = r"D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\MasterLooter.log"
IMAGE_BASE = 0x140000000
STATE_OFF = 0x350

PROBE = re.compile(r"\[probe\] gimmick eid ([0-9A-F]{8}) ([0-9.]+) m at ([0-9A-F]+) slots:")
# Only mine prefabs are interesting; the probe fires on any unfilled node.
MINE = re.compile(r"\[why\] ([0-9A-F]{8}).*?(mine|collect_her_rock)")


def addr_mode():
    if not os.path.exists(LOG):
        print("no log at %s -- start the game first" % LOG)
        return
    print("watching %s" % LOG)
    print("walk within 4 m of an untouched vein; each probe prints a ready command\n")
    seen = set()
    with io.open(LOG, encoding="utf-8", errors="replace") as fh:
        fh.seek(0, os.SEEK_END)
        while True:
            line = fh.readline()
            if not line:
                time.sleep(0.25)
                continue
            m = PROBE.search(line)
            if not m:
                continue
            eid, dist, base = m.group(1), m.group(2), int(m.group(3), 16)
            if eid in seen:
                continue
            seen.add(eid)
            print("eid %s at %s m" % (eid, dist))
            print("   component   %X" % base)
            print("   state byte  %X   (+0x350, reads 3 intact / 5 broken)" % (base + STATE_OFF))
            print("   x64dbg:     bphws %X, w, 1" % (base + STATE_OFF))
            print("   then mine it by hand.\n")


def rip_mode(text):
    rip = int(text.replace("0x", "").replace("`", ""), 16)
    rva = rip - IMAGE_BASE
    if rva < 0 or rva > 0x20000000:
        print("%X does not look like it is inside CrimsonDesert.exe (ImageBase %X)."
              % (rip, IMAGE_BASE))
        print("If x64dbg shows a module other than CrimsonDesert, the writer is in that")
        print("module and this will not help; say which module it was.")
        return
    print("x64dbg stopped at %X  ->  RVA 0x%X" % (rip, rva))
    print("A data breakpoint is a trap, so the STORE is the instruction before this.\n")
    back = max(0, rva - 0x40)
    print("--- disassembly from RVA 0x%X (look for the store into [reg+0x350]) ---" % back)
    subprocess.call([sys.executable, os.path.join(HERE, "disasm.py"), "%X" % back, "40"])


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "rip" and len(sys.argv) > 2:
        rip_mode(sys.argv[2])
    else:
        addr_mode()
