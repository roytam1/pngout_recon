"""Split the Hex-Rays output in pngout.exe.c into one file per function."""
import os, re, io, sys

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
SRC = "pngout.exe.c"
OUT = "reference/decomp"
os.makedirs(OUT, exist_ok=True)

src = open(SRC, encoding="utf-8", errors="replace").read().splitlines()

# Section banner:  //----- (00401180) ----------------------------
banner = re.compile(r"^//----- \(([0-9A-Fa-f]{8})\) -+$")
fndef  = re.compile(r"^([A-Za-z_][\w \*]*?)\b((?:sub_|UserMath|_\w)[A-Za-z0-9_]*)\(")

marks = []
for i, ln in enumerate(src):
    m = banner.match(ln)
    if m:
        marks.append((i, int(m.group(1), 16)))

print("sections: %d" % len(marks))

# map address -> real name, from the disassembly index
ADDR2NAME = {}
for ln in open("reference/fn_index.txt", encoding="utf-8"):
    p = ln.split()
    if len(p) >= 2:
        ADDR2NAME[int(p[0].split("-")[0], 16)] = p[1]

# header (declares + globals) up to first function body
bodies = []
for k, (i, a) in enumerate(marks):
    j = marks[k + 1][0] if k + 1 < len(marks) else len(src)
    chunk = src[i:j]
    name = ADDR2NAME.get(a)
    if not name:
        for ln in chunk:
            m = re.match(r"^([A-Za-z_][\w \*]*?)\s+((?:sub_[0-9A-Fa-f]+|UserMathErrorFunction|_\w+))\((.*)$", ln)
            if m:
                name = m.group(2)
                break
    bodies.append((a, name or ("sec_%08X" % a), chunk))

idx = []
for a, name, chunk in bodies:
    p = os.path.join(OUT, name + ".c")
    open(p, "w", encoding="utf-8", errors="replace").write("\n".join(chunk) + "\n")
    idx.append((a, name, len(chunk)))

with open("reference/decomp_index.txt", "w", encoding="utf-8") as h:
    for a, name, n in idx:
        h.write("%08X  %-24s %5d lines\n" % (a, name, n))
for a, name, n in idx:
    print("%08X  %-24s %5d" % (a, name, n))

# global declarations section (everything before the first section banner)
hdr = src[:marks[0][0]] if marks else src
open(os.path.join(OUT, "_globals.c"), "w", encoding="utf-8", errors="replace").write("\n".join(hdr) + "\n")
print("\nglobals header: %d lines" % len(hdr))