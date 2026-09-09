"""Find the function containing an RVA, using the PE exception directory (.pdata).

Usage: python funcbounds.py <rva hex>
x64 PE images carry a RUNTIME_FUNCTION table: begin/end/unwind RVAs per function.
That gives exact bounds without symbols.
"""
import bisect, os, struct, sys

EXE = r"D:\SteamLibrary\steamapps\common\Crimson Desert\bin64\CrimsonDesert.exe"
IMAGE_BASE = 0x140000000


def read_pe(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    # Data directories start at 0x70 (PE32+) / 0x60 (PE32) into the optional header.
    ddoff = opt + (0x70 if magic == 0x20B else 0x60)
    # Directory 3 is the exception directory.
    exc_rva, exc_size = struct.unpack_from("<II", data, ddoff + 3 * 8)
    secs = []
    tbl = opt + optsz
    for i in range(nsec):
        e = tbl + i * 40
        name = data[e:e + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", data, e + 8)
        secs.append((name, vaddr, vsize, raddr, rsize))
    return secs, exc_rva, exc_size


def rva_to_off(secs, rva):
    for name, vaddr, vsize, raddr, rsize in secs:
        if vaddr <= rva < vaddr + max(vsize, rsize):
            return raddr + (rva - vaddr), name
    return None, None


def main():
    target = int(sys.argv[1], 16)
    data = open(EXE, "rb").read()
    secs, exc_rva, exc_size = read_pe(data)
    if not exc_rva:
        print("no exception directory")
        return
    off, sec = rva_to_off(secs, exc_rva)
    n = exc_size // 12
    print("exception directory: RVA 0x%X, %d function records" % (exc_rva, n))

    begins = []
    recs = []
    for i in range(n):
        b, e, u = struct.unpack_from("<III", data, off + i * 12)
        if b == 0 and e == 0:
            continue
        begins.append(b)
        recs.append((b, e, u))

    i = bisect.bisect_right(begins, target) - 1
    if i < 0:
        print("RVA 0x%X is before the first function record" % target)
        return
    b, e, u = recs[i]
    if not (b <= target < e):
        print("RVA 0x%X is not inside a recorded function (nearest starts 0x%X)" % (target, b))
        return
    print()
    print("RVA 0x%X is inside the function at 0x%X .. 0x%X  (%d bytes)" % (target, b, e, e - b))
    print("  offset into function: +0x%X" % (target - b))
    print("  virtual address of entry: 0x%X" % (IMAGE_BASE + b))
    print()
    print("disassemble the whole function with:")
    print("  python disasm.py %X %d" % (b, min(400, (e - b) // 3 + 20)))


if __name__ == "__main__":
    main()
