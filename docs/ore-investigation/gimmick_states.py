"""Pull the declared state machine out of extracted .binarygimmick files.

The format stores null-terminated ASCII strings in order, as key then value:
    InitialBranchState / Wait / GimmickOnEnterState / GimmickOn / ...
So a key ending in EnterState or ExitState, or named InitialBranchState, is
followed by the state name it refers to.

Usage: python gimmick_states.py <dir with .binarygimmick files> [name filter]
"""
import collections, io, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from statehash import hashlittle  # noqa: E402

KEY = re.compile(r"^(InitialBranchState|[A-Za-z0-9_]+(?:Enter|Exit)State)$")
STR = re.compile(rb"[ -~]{2,64}")


def states_of(path):
    data = open(path, "rb").read()
    strings = [m.group().decode("ascii") for m in STR.finditer(data)]
    out = []
    for i, s in enumerate(strings):
        if KEY.match(s) and i + 1 < len(strings):
            val = strings[i + 1]
            if re.match(r"^[A-Za-z][A-Za-z0-9_]{1,30}$", val):
                out.append((s, val))
    return out


def main():
    root = sys.argv[1]
    want = sys.argv[2].lower() if len(sys.argv) > 2 else None

    files = []
    for dirpath, _, names in os.walk(root):
        for n in names:
            if n.endswith(".binarygimmick") and (not want or want in n.lower()):
                files.append(os.path.join(dirpath, n))
    files.sort()
    print("parsed %d definition(s)%s\n" % (len(files), " matching %r" % want if want else ""))

    machines = collections.Counter()   # frozenset of state names -> count
    example = {}
    state_use = collections.Counter()
    for f in files:
        pairs = states_of(f)
        if not pairs:
            continue
        names = frozenset(v for _, v in pairs)
        machines[names] += 1
        example.setdefault(names, os.path.basename(f))
        for v in names:
            state_use[v] += 1

    print("distinct state machines: %d\n" % len(machines))
    for names, n in machines.most_common(12):
        print("  %4d file(s)  e.g. %s" % (n, example[names]))
        print("      states: %s" % ", ".join(sorted(names)))
    print("\nstate names by how many definitions declare them:")
    for name, n in state_use.most_common(20):
        print("  %-28s %5d   hash 0x%08X" % (name, n, hashlittle(name.lower().encode())))


if __name__ == "__main__":
    main()
