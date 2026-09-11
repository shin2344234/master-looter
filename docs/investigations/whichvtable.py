"""Given a function's RVA, say which vtable holds it, at which slot, for which class.

  python whichvtable.py D8BA40
  python whichvtable.py D8BA40 D21040

The opposite direction from vtable.py, which starts at a class name and reads a
slot out of it. This starts at a function and works back to the class, which is
the question that comes up when another tool reports an address and nothing else.

How it works: a vtable slot is an aligned qword holding the function's virtual
address, so search the whole image for that value. From a hit, walk back while
the qwords still look like code to find the first entry, then read the pointer
just before it, which MSVC fills with the Complete Object Locator. Its +0x0C is
an RVA to the type descriptor and the mangled class name sits at that +0x10.

Written 11 September 2026 for Crimson Route's minimap, where Route reported a
verified update function and zero vtable slots. The function turned out to be
slot 35 of a 185-slot vtable for UIGamePlayControlRootMiniMap, the exact mirror
of the world map's, which said the game had not moved anything and the fault was
in how the vtable was being found. Note that `vtable.py slot` misreads its own
index, so prefer this for anything that has to be right.
"""
import struct
import sys

EXE = r"D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\CrimsonDesert.exe"

# This exe carries unusual section names; the code lives in these three.
CODE_SECTIONS = {".sbss", ".text1", ".idata"}


def load():
    data = open(EXE, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt = pe + 24
    base = struct.unpack_from("<Q", data, opt + 24)[0]
    first = opt + struct.unpack_from("<H", data, pe + 20)[0]
    secs = []
    for i in range(nsec):
        name, vsize, va, rsize, raw = struct.unpack_from("<8sIIII", data, first + i * 40)
        secs.append((name.decode(errors="replace").strip("\0"), va, vsize, raw, rsize))
    return data, secs, base


def rva_to_off(secs, rva):
    for _, va, vsize, raw, rsize in secs:
        if va <= rva < va + vsize:
            return raw + (rva - va)
    return None


def off_to_rva(secs, off):
    for name, va, vsize, raw, rsize in secs:
        if raw <= off < raw + rsize:
            return va + (off - raw), name
    return None, None


def section_of(secs, rva):
    for name, va, vsize, _, _ in secs:
        if va <= rva < va + vsize:
            return name
    return None


def qword(data, secs, rva):
    off = rva_to_off(secs, rva)
    if off is None or off + 8 > len(data):
        return None
    return struct.unpack_from("<Q", data, off)[0]


def is_code(data, secs, base, value):
    if not value or value < base or value > base + 0x20000000:
        return False
    return section_of(secs, value - base) in CODE_SECTIONS


def report(data, secs, base, fn_rva):
    target = struct.pack("<Q", base + fn_rva)
    hits, start = [], 0
    while True:
        i = data.find(target, start)
        if i < 0:
            break
        start = i + 1
        if i % 8 == 0:
            rva, sec = off_to_rva(secs, i)
            if rva is not None:
                hits.append((rva, sec))

    print("\n+0x%08X: %d aligned qword reference(s)" % (fn_rva, len(hits)))
    if not hits:
        print("    not in any vtable on this build; it is called directly")
        return

    for slot_rva, sec in hits:
        vt = slot_rva
        while is_code(data, secs, base, qword(data, secs, vt - 8)):
            vt -= 8
        n = 0
        while is_code(data, secs, base, qword(data, secs, vt + n * 8)):
            n += 1
        print("    slot at +0x%08X in %s: vtable +0x%08X, %d slots, index %d"
              % (slot_rva, sec, vt, n, (slot_rva - vt) // 8))

        col_va = qword(data, secs, vt - 8)
        col = (col_va - base) if col_va else 0
        off = rva_to_off(secs, col) if col else None
        if off is None:
            print("        no readable RTTI locator above the vtable")
            continue
        _, offset, _, ptd = struct.unpack_from("<IIII", data, off)
        toff = rva_to_off(secs, ptd)
        if toff is None:
            print("        type descriptor +0x%08X is outside any section" % ptd)
            continue
        name = data[toff + 0x10:toff + 0x110].split(b"\0")[0].decode("ascii", "replace")
        print("        class %s (locator +0x%08X, base offset %d)" % (name, col, offset))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    data, secs, base = load()
    print("image base 0x%X, %d sections" % (base, len(secs)))
    for arg in sys.argv[1:]:
        report(data, secs, base, int(arg, 16))


if __name__ == "__main__":
    main()
