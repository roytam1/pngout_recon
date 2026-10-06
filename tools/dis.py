"""Dump the original disassembly of one address range from reference/text.lst.

    python tools/dis.py 401000 40101E
    python tools/dis.py _main
    python tools/dis.py _main +80
"""
import sys, bisect, os, io

TXT = "reference/text.lst"

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")


def load_lines():
    return open(TXT, encoding="utf-8", errors="replace").read().splitlines()


HEX = set("0123456789ABCDEFabcdef")


def is_addr(s):
    return len(s) == 8 and all(c in HEX for c in s)


def build(lines):
    seen = {}
    for i, ln in enumerate(lines):
        if len(ln) > 8 and is_addr(ln[:8]):
            seen[int(ln[:8], 16)] = i        # last occurrence wins
    return sorted(seen), seen


def find_name(lines, name):
    n = name.lstrip("_")
    for i, ln in enumerate(lines):
        if " proc near" in ln or " proc far" in ln:
            head = ln[8:].split()[0] if len(ln) > 9 else ""
            if head == name or head.lstrip("_") == n:
                return i
    return None


def main():
    lines = load_lines()
    addrs, seen = build(lines)
    arg = sys.argv[1]

    def hexnum(s):
        s = s.lstrip("+")
        return len(s) > 1 and all(c in "0123456789abcdefABCDEF" for c in s)

    if hexnum(arg):
        lo = int(arg.lstrip("+"), 16)
        hi = lo
        if len(sys.argv) > 2:
            if sys.argv[2].startswith("+"):
                hi = lo + int(sys.argv[2][1:], 16)
            else:
                hi = int(sys.argv[2], 16)
    else:
        i = find_name(lines, arg)
        if i is None:
            sys.exit("not found: " + arg)
        lo = int(lines[i][:8], 16)
        hi = lo
        if len(sys.argv) > 2 and sys.argv[2].startswith("+"):
            hi = lo + int(sys.argv[2][1:], 16)
        else:
            for j in range(i + 1, len(lines)):
                if " endp" in lines[j]:
                    hi = int(lines[j][:8], 16)
                    break

    if lo not in seen:
        lo = max((a for a in addrs if a <= lo), default=lo)

    k = bisect.bisect_left(addrs, lo)
    while k < len(addrs) and addrs[k] <= hi:
        print(lines[seen[addrs[k]]])
        k += 1


main()