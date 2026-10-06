"""
Split the giant IDA .lst into a small, workable reference corpus.

Outputs (into reference/):
  text.lst      - full .text disassembly
  idata.lst     - imports
  rdata.lst     - read-only data (includes string literals)
  data.lst      - initialised .data only (up to raw size)
  strings.tsv   addr <TAB> section <TAB> string   for every printable run >= 3 chars
"""
import os, re

SRC = "pngout.exe.lst"
OUT = "reference"
DATA_RAW = 0x200          # .data section size in file
DATA_VA = 0x411000

seg_re = re.compile(rb"^(\.[a-zA-Z0-9_]+):([0-9A-Fa-f]{8})(\s*)(.*)$")
db_str_re = re.compile(r"(\w+)\s+db\s+'((?:[^']|'')*)'")

def as_str(b):
    return b.decode("latin1").replace("\ufffd", "~")

os.makedirs(OUT, exist_ok=True)
handles = {}
strings = []
lineno = 0
data_kept = 0

with open(SRC, "rb", buffering=1 << 22) as f:
    for line in f:
        lineno += 1
        m = seg_re.match(line)
        if not m:
            continue
        seg = m.group(1).decode()
        addr = int(m.group(2), 16)
        body = as_str(m.group(4))

        if seg == ".data" and (addr - DATA_VA) >= DATA_RAW:
            continue                      # 22MB zero fill: skip
        if seg == ".data":
            data_kept += 1

        h = handles.get(seg)
        if h is None:
            h = handles[seg] = open(os.path.join(OUT, seg.strip(".") + ".lst"), "w",
                                    encoding="utf-8", errors="replace")
        h.write("%08X  %s\n" % (addr, body))

        for mm in db_str_re.finditer(body):
            s = mm.group(2).replace("''", "'")
            if len(s) >= 3:
                strings.append((addr, seg, s))

for h in handles.values():
    h.close()

with open(os.path.join(OUT, "strings.tsv"), "w", encoding="utf-8", errors="replace") as h:
    for a, s, t in strings:
        h.write("%08X\t%s\t%s\n" % (a, s, t))

print("lines scanned :", lineno)
print("data kept     :", data_kept)
print("strings       :", len(strings))
for n in sorted(os.listdir(OUT)):
    print("  %-14s %10d bytes" % (n, os.path.getsize(os.path.join(OUT, n))))