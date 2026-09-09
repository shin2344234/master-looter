"""Look for a 32-bit constant in CrimsonDesert.exe, and find callers of a function.

  python find_const.py const 150B14D0     where does this dword appear?
  python find_const.py xref  2047580      which call sites target this RVA?
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


def off_to_rva(secs, off):
    for name, vaddr, vsize, raddr, rsize in secs:
        if raddr <= off < raddr + rsize:
            return vaddr + (off - raddr), name
    return None, None


def const_mode(data, secs, val):
    pat = struct.pack("<I", val)
    found = []
    start = 0
    while True:
        i = data.find(pat, start)
        if i < 0:
            break
        start = i + 1
        rva, sec = off_to_rva(secs, i)
        if rva is not None:
            found.append((rva, sec))
    print("0x%08X appears %d time(s) in the image" % (val, len(found)))
    for rva, sec in found[:30]:
        print("  %-10s RVA 0x%08X" % (sec, rva))
    if len(found) > 30:
        print("  ... and %d more" % (len(found) - 30))
    if not found:
        print("  Not present as a literal dword. It is computed at runtime, so it")
        print("  cannot be hardcoded in the mod and must be learned live.")


def xref_mode(data, secs, target_rva):
    # E8 rel32 : call. site_rva + 5 + rel32 == target_rva
    hits = []
    for name, vaddr, vsize, raddr, rsize in secs:
        if not rsize:
            continue
        blob = data[raddr:raddr + rsize]
        for i in range(len(blob) - 5):
            if blob[i] != 0xE8:
                continue
            rel = struct.unpack_from("<i", blob, i + 1)[0]
            site = vaddr + i
            if site + 5 + rel == target_rva:
                hits.append((site, name))
    print("%d direct call site(s) target RVA 0x%X:" % (len(hits), target_rva))
    for site, sec in hits[:40]:
        print("  %-10s call from RVA 0x%08X" % (sec, site))
    if not hits:
        print("  None. It is reached indirectly (a vtable slot or a function pointer),")
        print("  which fits a virtual state setter.")


def main():
    mode, arg = sys.argv[1], int(sys.argv[2], 16)
    data = open(EXE, "rb").read()
    secs = sections(data)
    if mode == "const":
        const_mode(data, secs, arg)
    else:
        xref_mode(data, secs, arg)


if __name__ == "__main__":
    main()
