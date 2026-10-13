import sys

# full-table diff: the original's binary dump is logical entries 0..n-1 (entry k
# lives at StepTable[k+3] in its own array), the recon's file is the raw array,
# so logical entry k is recon index k+3.
op, rp, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
a = open(op, 'rb').read()
b = open(rp, 'rb').read()
print('orig bytes %d recon bytes %d' % (len(a), len(b)))
av = memoryview(a)
bv = memoryview(b)
first = -1
diffs = 0
buckets = {}
lastdiff = -1
i = 0
while i < n:
    if a[2 * i:2 * i + 2] != b[2 * (i + 3):2 * (i + 3) + 2]:
        if first < 0:
            first = i
        diffs += 1
        lastdiff = i
        k = (i >> 18) << 18
        buckets[k] = buckets.get(k, 0) + 1
    i += 1
print('diffs %d / %d  first=%d last=%d' % (diffs, n, first, lastdiff))
if first >= 0:
    for k in sorted(buckets):
        print('  bucket %8d..%8d : %d' % (k, k + (1 << 18) - 1, buckets[k]))
    lo = max(0, first - 6)
    hi = min(n, first + 14)
    print('orig  %d..%d:' % (lo, hi), list(a[2 * lo:2 * hi:2]))
    print('recon %d..%d:' % (lo, hi), list(b[2 * (lo + 3):2 * (hi + 3):2]))
