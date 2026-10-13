import struct
import sys

# Compare two lzdump_aux images *by position*: every array is a ring indexed by
# (position & mask), so position p lives at index p & mask in BOTH files.  The
# two dumps cover different windows, so only the overlap is meaningful.
LAYOUT = [('MatchStart', 32768, 4), ('MatchArea', 32768, 4),
          ('RunLen', 65536, 2), ('HashHead', 4096, 2), ('DistSlot', 32768, 2)]


def load(path, dpos):
    blob = open(path, 'rb').read()
    out = {}
    off = 0
    for nm, n, sz in LAYOUT:
        fmt = '<%di' % n if sz == 4 else '<%dH' % n
        out[nm] = struct.unpack(fmt, blob[off:off + n * sz])
        off += n * sz
    return out, dpos


def main(opath, opos, rpath, rpos):
    A, ap = load(opath, opos)
    B, bp = load(rpath, rpos)
    print('orig pos %d, recon pos %d' % (ap, bp))
    for nm, n, sz in LAYOUT:
        av = A[nm]
        bv = B[nm]
        if n >= 32768 and n in (32768, 65536):
            # Clamp to positions both sides have actually written: RunLen is
            # never reset, so entries for positions before the current one are
            # stale in one binary and untouched (0) in the other.
            lo = max(0, ap - n + 1, bp - n + 1)
            hi = min(ap, bp)
            diffs = []
            for p in range(lo, hi + 1):
                i = p & (n - 1)
                if av[i] != bv[i]:
                    diffs.append((p, av[i], bv[i]))
            print('  %-10s window %d..%d diffs %d' % (nm, lo, hi, len(diffs)))
            for r in diffs[:4]:
                print('      pos %8d orig %8d recon %8d' % r)
        else:
            d = [i for i in range(n) if av[i] != bv[i]]
            print('  %-10s (global) diffs %d' % (nm, len(d)))
            for i in d[:4]:
                print('      idx %6d orig %8d recon %8d' % (i, av[i], bv[i]))


if __name__ == '__main__':
    main(sys.argv[1], int(sys.argv[2]), sys.argv[3], int(sys.argv[4]))
