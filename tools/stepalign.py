import sys

# Compare two full-table images whose cursors are at different offsets: find
# the shift that aligns them and report the mismatch rate.  Logical entry k of
# the original lives at recon index k+3 (the table's 3-entry preamble).
op, rp, shift = sys.argv[1], sys.argv[2], int(sys.argv[3])
a = open(op, 'rb').read()
b = open(rp, 'rb').read()
N = 8388612
sample = 1 << 16


def mism(s, lo=0, hi=1 << 18):
    n = 0
    for k in range(lo, hi):
        i = k + s + 3
        if i < 0 or i >= N:
            continue
        if a[2 * k:2 * k + 2] != b[2 * i:2 * i + 2]:
            n += 1
    return n


best = None
for s in range(shift - 4000, shift + 4000, 1):
    m = mism(s, 0, sample)
    if best is None or m < best[1]:
        best = (s, m)
        if m == 0:
            break
print('best shift %d (mismatches %d / %d)' % (best[0], best[1], sample))
s = best[0]
tot = 0
buckets = {}
for k in range(N):
    i = k + s + 3
    if i < 0 or i >= N:
        continue
    if a[2 * k:2 * k + 2] != b[2 * i:2 * i + 2]:
        tot += 1
        bk = (k >> 18) << 18
        buckets[bk] = buckets.get(bk, 0) + 1
print('total mismatches %d / %d' % (tot, N))
for bk in sorted(buckets)[:24]:
    print('   %8d..%8d : %d' % (bk, bk + (1 << 18) - 1, buckets[bk]))
