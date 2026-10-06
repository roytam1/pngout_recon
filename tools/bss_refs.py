"""
Collect every reference the .text makes into the uninitialised .data region
(0x411200 .. 0x1B0A838) so the static arrays can be re-declared.

IDA renders operands as symbol names (dword_180BA80, unk_16A09E6, ...) so we
decode the hex suffix of every symbol mentioned.
"""
import re, sys, io, collections

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")

TEXT = "reference/text.lst"
BSS_LO = 0x411200
BSS_HI = 0x411000 + 0x14F9838

HEXSET = set("0123456789abcdef")
sym = re.compile(r"\b(?:dword|byte|word|off|unk)_([0-9A-Fa-f]{6,8})\b")

refs = collections.defaultdict(list)
seg = None
for line in open(TEXT, encoding="utf-8", errors="replace"):
    if len(line) < 9:
        continue
    a, body = line[:8], line[9:]
    if not all(c in HEXSET for c in a):
        continue
    ca = int(a, 16)
    if " proc near" in body or " proc far" in body:
        seg = body.split()[0]
    if body.strip().startswith("endp"):
        seg = None
    for m in sym.finditer(body):
        b = int(m.group(1), 16)
        if BSS_LO <= b < BSS_HI:
            refs[b].append((ca, seg, body.strip()))

addrs = sorted(refs)
print("distinct BSS symbols referenced: %d   range 0x%X..0x%X"
      % (len(addrs), addrs[0], addrs[-1]))

clusters = []
for a in addrs:
    if clusters and a - clusters[-1][1] <= 0x100:
        clusters[-1][1] = a
    else:
        clusters.append([a, a])

print()
print("%-12s %-12s %6s %-10s %s" % ("start", "end", "refs", "span", "first referring funcs"))
for lo, hi in clusters:
    funcs = []
    for a in addrs:
        if lo <= a <= hi:
            for ca, seg, txt in refs[a]:
                if seg and seg not in funcs:
                    funcs.append(seg)
    print("0x%08X   0x%08X   %6d 0x%-8X %s"
          % (lo, hi, sum(len(refs[a]) for a in addrs if lo <= a <= hi),
             hi - lo, ",".join(funcs[:4])))

with open("reference/bss_refs.txt", "w", encoding="utf-8") as h:
    for a in addrs:
        h.write("0x%08X  (%d refs)\n" % (a, len(refs[a])))
        for ca, seg, txt in refs[a]:
            h.write("      %08X  %-18s %s\n" % (ca, seg or "", txt))
print("\n-> reference/bss_refs.txt")