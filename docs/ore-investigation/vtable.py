"""Find a class's vtable via MSVC RTTI, and read a slot from it.

  python vtable.py find ClientGimmickActorComponent
  python vtable.py slot <vtable rva hex> 878

MSVC layout: a vtable's slot -1 holds a pointer to the Complete Object Locator,
whose +0x0C is an RVA to the type descriptor, whose +0x10 is the mangled name.
So: find the type descriptor by name, find COLs pointing at it, then find the
qword in .rdata-ish data whose value is that COL, and the vtable starts 8 after.
"""
import os, struct, sys

EXE = r"D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\CrimsonDesert.exe"
IMAGE_BASE = 0x140000000


def sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    tbl = pe + 24 + optsz
    out = []
    for i in range(nsec):
        e = tbl + i * 40
        name = data[e:e + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", data, e + 8)
        out.append((name, vaddr, vsize, raddr, rsize))
    return out


def rva_to_off(secs, rva):
    for name, vaddr, vsize, raddr, rsize in secs:
        if vaddr <= rva < vaddr + max(vsize, rsize):
            if rva - vaddr < rsize:
                return raddr + (rva - vaddr)
    return None


def off_to_rva(secs, off):
    for name, vaddr, vsize, raddr, rsize in secs:
        if raddr <= off < raddr + rsize:
            return vaddr + (off - raddr)
    return None


def find_mode(data, secs, cls):
    needle = (".?AV" + cls + "@").encode("ascii")
    tds = []
    start = 0
    while True:
        i = data.find(needle, start)
        if i < 0:
            break
        start = i + 1
        # The mangled name sits at typeDescriptor+0x10.
        td_off = i - 0x10
        td_rva = off_to_rva(secs, td_off)
        if td_rva is not None:
            name_end = data.find(b"\0", i)
            tds.append((td_rva, data[i:name_end].decode("ascii", "replace")))
    print("type descriptor(s) matching %s:" % cls)
    for rva, nm in tds:
        print("  RVA 0x%08X  %s" % (rva, nm))
    if not tds:
        print("  none")
        return

    # Complete Object Locators: +0x0C is the type descriptor RVA.
    for td_rva, nm in tds:
        pat = struct.pack("<I", td_rva)
        cols = []
        start = 0
        while True:
            i = data.find(pat, start)
            if i < 0:
                break
            start = i + 1
            col_off = i - 0x0C
            col_rva = off_to_rva(secs, col_off)
            if col_rva is None:
                continue
            sig = struct.unpack_from("<I", data, col_off)[0] if col_off >= 0 else 0xFFFF
            if sig in (0, 1):
                cols.append(col_rva)
        print("\n  %s -> %d complete object locator(s)" % (nm, len(cols)))
        for col_rva in cols[:6]:
            print("    COL RVA 0x%08X" % col_rva)
            va = IMAGE_BASE + col_rva
            vpat = struct.pack("<Q", va)
            s2 = 0
            while True:
                j = data.find(vpat, s2)
                if j < 0:
                    break
                s2 = j + 1
                vt_rva = off_to_rva(secs, j + 8)
                if vt_rva is not None:
                    print("      vtable RVA 0x%08X  (slot0 at this address)" % vt_rva)


def slot_mode(data, secs, vt_rva, slot_off):
    off = rva_to_off(secs, vt_rva + slot_off)
    if off is None:
        print("vtable RVA 0x%X + 0x%X is not in the file" % (vt_rva, slot_off))
        return
    va = struct.unpack_from("<Q", data, off)[0]
    print("vtable 0x%X slot +0x%X = VA 0x%X" % (vt_rva, slot_off, va))
    if va:
        print("  RVA 0x%X" % (va - IMAGE_BASE))
        print("  disassemble: python disasm.py %X 40" % (va - IMAGE_BASE))


def main():
    data = open(EXE, "rb").read()
    secs = sections(data)
    if sys.argv[1] == "find":
        find_mode(data, secs, sys.argv[2])
    else:
        slot_mode(data, secs, int(sys.argv[2], 16), int(sys.argv[3], 16))


if __name__ == "__main__":
    main()
