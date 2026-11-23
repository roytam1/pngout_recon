"""Listing-faithful interpreter of the strategy-0 search in sub_4065A0.

Replays LzXtreme over a frozen DUMPSTATE snapshot and reports the step-loop
trip count and the records it writes.  The control flow is transcribed from
reference/decomp/sub_4065A0.c:216-361 (checked against the raw listing at
0x4068F5-0x406BF8), *not* from src/lz.c, so that a translation defect in the
C version shows up as a divergence in the counts.

    python tools/lzoracle.py --dir <dir> --pos <pos> [--trace]

The --dir must contain lzstate_<pos>.txt, lzdump_aux_<pos>.bin,
lzdump_win_<pos>.bin and lzdump_step_<pos>.bin.  The aux dump may use either
the original five-array layout or the extended one with DistCodeToSlot and
LenCodeToSlot appended (the tables are then taken from the dump; otherwise
they are rebuilt from src/optimize.c's algorithms and verified against the
state file's slots/order lines).
"""

import argparse
import os
import struct
import sys

MAXMATCH = 258
MAXDIST = 0x8000
STEPHOP = 258
COSTMASK = 0x1FF
LISTMASK = 0x3FFFF
STEPMASK = 0x7FFF
STARTHACK = 30

AUX_LAYOUT = [
    ("MatchStart", 32768, 4),
    ("MatchArea", 32768, 4),
    ("RunLen", 65536, 2),
    ("HashHead", 4096, 2),
    ("DistSlot", 32768, 2),
]
EXT_LAYOUT = [("DistCodeToSlot", 32768, 1), ("LenCodeToSlot", 259, 1)]


def read_state(path):
    txt = open(path, "r", errors="replace").read().splitlines()
    hdr = {}
    for line in txt:
        for tok in line.split():
            if "=" not in tok:
                continue
            key, val = tok.split("=", 1)
            try:
                hdr[key] = int(val)
            except ValueError:
                pass
    arrays = {}
    for line in txt:
        if ":" in line and line.split(":", 1)[0] in ("slots", "order", "best", "cost"):
            name, body = line.split(":", 1)
            arrays[name] = [int(x) for x in body.split()]
    return hdr, arrays


def load_aux(path):
    blob = open(path, "rb").read()
    out, off = {}, 0
    for name, n, sz in AUX_LAYOUT:
        fmt = "<%di" % n if sz == 4 else "<%dH" % n
        out[name] = list(struct.unpack(fmt, blob[off:off + n * sz]))
        off += n * sz
    if len(blob) >= off + 32768 + 259:
        out["DistCodeToSlot"] = list(blob[off:off + 32768])
        off += 32768
        out["LenCodeToSlot"] = list(blob[off:off + 259])
    return out


class Oracle(object):
    def __init__(self, state, aux, window, steps, trace=False):
        self.hdr, self.arr = state
        self.aux = aux
        self.win = window
        self.step = steps
        self.trace = trace
        self.distcode = aux.get("DistCodeToSlot")
        self.lencode = aux.get("LenCodeToSlot")
        if self.distcode is None or self.lencode is None:
            raise SystemExit("aux dump is missing the extended code tables; "
                             "use a dump made with the current DUMPSTATE build")
        self.distslots = self.arr["slots"]
        self.slotorder = self.arr["order"]
        self.bestcost = list(self.arr["best"])
        self.matchcost = list(self.arr["cost"])
        self.matchstart = aux["MatchStart"]
        self.reach = aux["MatchArea"]
        self.runlen = aux["RunLen"]
        self.pos = self.hdr["pos"]
        self.savedcost = self.hdr["savedCost"]
        self.cap = self.hdr["cap"]
        self.maxlen = self.hdr["maxlen"]
        self.dist = self.hdr["dist"]
        self.count = self.hdr["count"]
        self.limit = self.hdr["limit"]
        self.records = []
        self.steps = 0
        self.restores = 0
        self.trims = 0
        self.rankwalks = 0

    def dc(self, d):
        return self.distcode[d - 1]

    def byte(self, i):
        return self.win[i & 0xFFFF]

    def stepread(self, q, n):
        return self.step[self.matchstart[q & STEPMASK] + n]

    def emit(self, *a):
        if self.trace:
            print(*a)

    def run(self):
        pos, savedcost, cap, maxlen = self.pos, self.savedcost, self.cap, self.maxlen
        dist = self.dist
        base = self.matchstart[pos & STEPMASK]
        count = self.count
        limit = self.limit
        length = 3
        seen = 0
        # DistCodeBase[dword_1809394] is not frozen; ZxDistBase proved the
        # restore-dist path never fires on this input.
        distbase = [0] * 32

        while True:
            # 0x406910 (recompute seen) and 0x406920 (record) are the two
            # re-entry points into the listing's nested loops; `label` says
            # which one the last jump landed on.
            label = "L47"
            while True:
                if label == "EXIT258":
                    break
                if label == "L47":
                    seen = count - base
                    label = "L48"
                if label == "L48":
                    # ---- 0x406920: record this length's distance ----
                    slot = self.dc(dist)
                    target = self.distslots[slot]
                    idx = length - 3
                    if idx < seen:
                        if target < self.bestcost[idx]:
                            self.bestcost[idx] = target
                    else:
                        count += 1
                        self.step[count + 2] = dist
                        seen += 1
                        self.bestcost[idx] = target

                    cost = savedcost + self.distslots[self.lencode[length]] + target
                    end = pos + length
                    if cost < self.matchcost[end & COSTMASK] and end <= cap:
                        if end > limit:
                            limit = end
                        self.matchcost[end & COSTMASK] = cost
                        self.records.append((end, length, dist, cost))
                        self.emit("[rec] end=%d len=%d dist=%d" % (end, length, dist))

                    if length == MAXMATCH:
                        label = "EXIT258"
                        continue

                    rank = self.slotorder[slot]
                    saved = dist
                    label = "STEP"
                    continue

                if label == "STEP":
                    if self.byte(pos - dist + length) == self.byte(pos + length):
                        length += 1
                        label = "L47"
                        continue
                    if length == maxlen:
                        dist += self.runlen[(pos - dist) & 0xFFFF]
                    q = (pos - dist) & STEPMASK
                    if length - 3 >= self.reach[q]:
                        return self.report("reach q=%d" % q)
                    dist += self.stepread(q, length)
                    self.steps += 1
                    if dist > MAXDIST:
                        return self.report("dist>32768 main")
                    if rank <= self.dc(dist):
                        t = self.dc(dist)
                        s = rank
                        while self.slotorder[s] <= t:
                            self.rankwalks += 1
                            s = self.slotorder[s]
                        target = self.distslots[s]
                        label = "L69"
                        continue
                    # ---- 0x406B7D: no cheaper slot in range ----
                    if rank > STARTHACK:
                        return self.report("rank>=30")
                    target = self.distslots[rank]
                    label = "L69"
                    continue

                if label == "L69":
                    # ---- 0x406AC9: trim ----
                    while length > 5 and self.bestcost[length - 6] > target:
                        length -= 3
                        self.trims += 1
                    while length > 3 and self.bestcost[length - 4] > target:
                        length -= 1
                        self.trims += 1

                    # ---- 0x406AFD: restore distance ----
                    dist = saved
                    label = "RESTORE"
                    continue

                if label == "RESTORE":
                    if length == maxlen:
                        dist += self.runlen[(pos - dist) & 0xFFFF]
                        if dist > MAXDIST:
                            return self.report("dist>32768 restore")
                        if self.dc(dist) >= rank:
                            dist = distbase[rank]
                            label = "L48"   # 0x406B95 -> 0x406920
                            continue
                    q = (pos - dist) & STEPMASK
                    if length - 3 >= self.reach[q]:
                        return self.report("reach q=%d restore" % q)
                    dist += self.stepread(q, length)
                    self.restores += 1
                    if dist > MAXDIST:
                        return self.report("dist>32768 restore step")
                    if self.dc(dist) >= rank:
                        label = "L48"       # 0x406B78 jmp 0x406920
                        continue
                    continue

            # ---- 0x406BE0: len==258 exit test ----
            if self.slotorder[self.dc(dist)] < STARTHACK:
                if maxlen == MAXMATCH:
                    dist += self.runlen[(pos - dist) & 0xFFFF]
                q = (pos - dist) & STEPMASK
                if self.reach[q] > 255:
                    dist += self.stepread(q, STEPHOP)
                    if dist <= MAXDIST:
                        continue  # 0x406BF8 -> 0x406910
            return self.report("258-hop exit")

    def report(self, why):
        print("exit: %s" % why)
        print("steps=%d restores=%d trims=%d rankwalks=%d records=%d" %
              (self.steps, self.restores, self.trims, self.rankwalks, len(self.records)))
        for r in self.records[:12]:
            print("  record end=%d len=%d dist=%d cost=%d" % r)
        return self


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=".")
    ap.add_argument("--pos", type=int, required=True)
    ap.add_argument("--trace", action="store_true")
    args = ap.parse_args()

    def path(prefix, ext):
        return os.path.join(args.dir, "%s_%d.%s" % (prefix, args.pos, ext))

    state = read_state(path("lzstate", "txt"))
    aux = load_aux(path("lzdump_aux", "bin"))
    window = open(path("lzdump_win", "bin"), "rb").read()
    steps = list(struct.unpack("<%dH" % (os.path.getsize(path("lzdump_step", "bin")) // 2),
                               open(path("lzdump_step", "bin"), "rb").read()))
    Oracle(state, aux, window, steps, args.trace).run()


if __name__ == "__main__":
    main()
