"""Dump a .binarygimmick as the key/value list it actually is.

    py -3 gimmick_dump.py <file.binarygimmick> [--filter substring]
    py -3 gimmick_dump.py <dir> --name mine_iron

The format is a flat stream of length-prefixed strings: a 4-byte little-endian
length, then that many bytes of ASCII, repeated. They read as key then value,
so the whole definition comes out as pairs in declaration order.

    14 00 00 00  "AmbushSummonOffset01"      key, length 20
    05 00 00 00  "0 0 0"                     value, length 5

gimmick_states.py, which came first, regex-scanned for printable runs and kept
only the state names. That threw away every value, which is where the answers
are: what attack key triggers the node, what level it needs, which drop set it
carries. This keeps all of it.

Anything that is not a valid length-prefixed string is skipped a byte at a time,
so leading binary and trailing padding do not derail the walk.
"""
import io
import os
import re
import struct
import sys

PRINTABLE = re.compile(rb"^[ -~]*$")


def strings(data):
    """Every length-prefixed ASCII string, with the offset it started at."""
    out = []
    i = 0
    n = len(data)
    while i + 4 <= n:
        ln = struct.unpack_from("<I", data, i)[0]
        # A key or value in these files is short. Anything else is not a string
        # header, so step one byte and try again.
        if 1 <= ln <= 512 and i + 4 + ln <= n:
            raw = data[i + 4:i + 4 + ln]
            if PRINTABLE.match(raw):
                out.append((i, raw.decode("ascii")))
                i += 4 + ln
                continue
        i += 1
    return out


def pairs(items):
    """Key/value, where a key is followed by its value.

    A key whose value is numeric rather than a string has no string after it,
    so the next entry reads as another key. Treat a run of two consecutive
    entries that both look like identifiers as key with an empty value.
    """
    ident = re.compile(r"^[A-Za-z][A-Za-z0-9_]{2,}$")
    out = []
    i = 0
    while i < len(items):
        off, key = items[i]
        if not ident.match(key):
            i += 1
            continue
        val = ""
        if i + 1 < len(items):
            nxt = items[i + 1][1]
            # The next entry is this key's value unless it is plainly another
            # key: an identifier that the file uses as a key elsewhere.
            if not ident.match(nxt) or nxt in ("Wait", "GimmickOn", "Lock",
                                               "Deactive", "Clear", "None"):
                val = nxt
                i += 1
        out.append((off, key, val))
        i += 1
    return out


def dump(path, needle=None):
    data = io.open(path, "rb").read()
    kv = pairs(strings(data))
    print("=== %s  (%d bytes, %d entries) ===" % (os.path.basename(path), len(data), len(kv)))
    for off, k, v in kv:
        if needle and needle.lower() not in k.lower() and needle.lower() not in v.lower():
            continue
        print("  %6d  %-38s %s" % (off, k, v))
    print()


def main():
    args = [a for a in sys.argv[1:]]
    if not args:
        raise SystemExit(__doc__)
    target = args[0]
    needle = None
    name = None
    if "--filter" in args:
        needle = args[args.index("--filter") + 1]
    if "--name" in args:
        name = args[args.index("--name") + 1]

    if os.path.isdir(target):
        for f in sorted(os.listdir(target)):
            if not f.endswith(".binarygimmick"):
                continue
            if name and name.lower() not in f.lower():
                continue
            dump(os.path.join(target, f), needle)
    else:
        dump(target, needle)


if __name__ == "__main__":
    main()
