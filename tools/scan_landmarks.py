"""Locate structural landmarks in an IDA .lst listing without loading it all."""
import sys, io, re

PATH = "pngout.exe.lst"

seg_re = re.compile(rb"^(\.[a-zA-Z0-9_]+):([0-9A-Fa-f]{8}) (\s*)(.*)$")

def ascii_safe(s):
    return s.encode("ascii", "replace").decode("ascii")

def main():
    with open(PATH, "rb") as f:
        # Stream, report every segment change and every "proc near"/"endp"
        cur = None
        lineno = 0
        marks = []
        for line in f:
            lineno += 1
            m = seg_re.match(line)
            if not m:
                continue
            seg, addr, _, rest = m.group(1), m.group(2), m.group(3), m.group(4)
            key = seg
            if key != cur:
                marks.append((lineno, seg.decode(), addr.decode(), "SEGSTART"))
                cur = key
            if b"proc near" in rest or b"proc far" in rest or b"proc segment" in rest:
                marks.append((lineno, seg.decode(), addr.decode(), ascii_safe(rest.decode('latin1')).strip()))
        for m in marks:
            print("%9d  %-10s %-9s %s" % m)

main()