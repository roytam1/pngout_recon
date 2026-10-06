"""Reassemble a NUL terminated string from the byte dump in an IDA listing.

    python tools/str.py 0040F728
"""
import re, sys, io

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")

seg = sys.argv[2] if len(sys.argv) > 2 else "rdata"
want = int(sys.argv[1], 16)
decl = re.compile(r"^(?:[A-Za-z]\w*\s+)?(db|dw|dd)\s+(.*)$")
item = re.compile(r"'((?:[^']|'')*)'|([0-9A-Fa-f]+)h|\b(\d+)\b")

vals = {}
for line in open("reference/%s.lst" % seg, encoding="utf-8", errors="replace"):
    if len(line) < 9:
        continue
    a = line[:8]
    if not all(c in "0123456789abcdefABCDEF" for c in a):
        continue
    a = int(a, 16)
    body = line[8:].rstrip()
    body = body.split(";")[0].strip()
    m = decl.match(body)
    if not m:
        continue
    kind, args = m.group(1), m.group(2)
    width = 4 if kind == "dd" else 2 if kind == "dw" else 1
    off = 0
    for t in item.finditer(args):
        if t.group(1) is not None:
            for ch in t.group(1).replace("''", "'"):
                vals[a + off] = ord(ch)
                off += 1
        else:
            v = int(t.group(2), 16) if t.group(2) else int(t.group(3))
            for i in range(width):
                vals[a + off + i] = (v >> (8 * i)) & 0xFF
            off += width

out = bytearray()
a = want
while a in vals and vals[a] != 0:
    out.append(vals[a])
    a += 1
print("0x%08X  (%d chars):" % (want, len(out)))
print(repr(bytes(out)))
print()
print(bytes(out).decode("latin1"))