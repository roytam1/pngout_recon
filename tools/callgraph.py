"""Call graph + call-site listing from the reference disassembly."""
import re, sys, io, collections

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
HEX = set("0123456789ABCDEFabcdef")

FNS = {}
for ln in open("reference/fn_index.txt", encoding="utf-8"):
    p = ln.split()
    FNS[int(p[0].split("-")[0], 16)] = p[1]

cur = None
calls = collections.defaultdict(list)     # caller -> [(addr, callee)]
callees = collections.defaultdict(set)
callre = re.compile(r"\bcall\s+(?:dword ptr\s+)?(?:\[)?([A-Za-z_][\w]*)")

for ln in open("reference/text.lst", encoding="utf-8", errors="replace"):
    if len(ln) < 9 or not all(c in HEX for c in ln[:8]):
        continue
    a, body = int(ln[:8], 16), ln[9:]
    if " proc near" in body or " proc far" in body:
        cur = body.split()[0]
    if body.strip().startswith("endp"):
        cur = None
    m = callre.search(body)
    if m and cur:
        t = m.group(1)
        calls[cur].append((a, t))
        callees[cur].add(t)

print("%-24s %6s  %s" % ("function", "calls", "callees"))
for f in sorted(calls, key=lambda x: FNS.get(x, 0)):
    if not calls[f]:
        continue
    c = sorted(callees[f])
    print("%-24s %6d  %s" % (f, len(calls[f]), " ".join(c)))

print("\n\n--- reverse: who calls the big functions ---")
for target in ("sub_402AD0", "sub_409560", "sub_40B180", "sub_408D80",
               "sub_4071D0", "sub_4077C0", "sub_407B00", "sub_407D80"):
    who = sorted({f for f in calls if any(t == target for _, t in calls[f])})
    print("%-14s <- %s" % (target, ", ".join(who) or "(indirect only)"))