"""Dump every .text function of the original into reference/fn/<name>.lst"""
import os, re, sys, io, bisect

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
TXT = "reference/text.lst"
HEX = set("0123456789ABCDEFabcdef")
OUT = "reference/fn"
os.makedirs(OUT, exist_ok=True)

lines = open(TXT, encoding="utf-8", errors="replace").read().splitlines()

# find every "proc near/far"
starts = []
for i, ln in enumerate(lines):
    if " proc near" in ln or " proc far" in ln:
        if len(ln) > 8 and all(c in HEX for c in ln[:8]):
            name = ln[9:].split()[0]
            starts.append((i, name, int(ln[:8], 16)))

ends = {}
for k, (i, name, a) in enumerate(starts):
    j = starts[k + 1][0] if k + 1 < len(starts) else len(lines)
    for t in range(i, j):
        if " endp" in lines[t]:
            j = t + 1
            break
    ends[i] = j

for k, (i, name, a) in enumerate(starts):
    body = lines[i:ends[i]]
    open(os.path.join(OUT, name + ".lst"), "w", encoding="utf-8",
         errors="replace").write("\n".join(body) + "\n")

# also a map
with open("reference/fn_index.txt", "w", encoding="utf-8") as h:
    for k, (i, name, a) in enumerate(starts):
        h.write("%08X-%08X  %-24s %5d lines\n"
                % (a, int(lines[ends[i] - 1][:8], 16), name, ends[i] - i))
print("wrote %d functions to %s" % (len(starts), OUT))