#!/usr/bin/env python
"""Dump the deflate symbol stream of a PNG's IDAT, one symbol at a time.

zlib will only tell you that a stream is truncated; it will not tell you which
symbols came out.  This walks the block itself so that a row-by-row mismatch can
be pinned to a specific match instead of guessed at.
"""

import struct
import sys

LENS = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51,
        59, 67, 83, 99, 115, 131, 163, 195, 227, 258]
LEXT = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4,
        5, 5, 5, 5, 0]
DIST = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
        513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577]
DEXT = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10,
        10, 11, 11, 12, 12, 13, 13]
CLORDER = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15]


class Bits:
    def __init__(self, data):
        self.d = data
        self.p = 0

    def get(self, n):
        v = 0
        for i in range(n):
            byte = self.d[self.p >> 3]
            v |= ((byte >> (self.p & 7)) & 1) << i
            self.p += 1
        return v

    def align(self):
        self.p = (self.p + 7) & ~7


def table(lengths):
    """Canonical Huffman: {(bits, len): symbol}."""
    maxlen = max(lengths) if lengths else 0
    if not maxlen:
        return {}
    bl_count = [0] * (maxlen + 1)
    for l in lengths:
        if l:
            bl_count[l] += 1
    code = 0
    next_code = [0] * (maxlen + 2)
    for bits in range(1, maxlen + 1):
        code = (code + bl_count[bits - 1]) << 1
        next_code[bits] = code
    out = {}
    for sym, l in enumerate(lengths):
        if l:
            out[(next_code[l], l)] = sym
            next_code[l] += 1
    return out


def decode(bits, tbl):
    code = 0
    for l in range(1, 16):
        code = (code << 1) | bits.get(1)
        if (code, l) in tbl:
            return tbl[(code, l)]
    raise ValueError("bad Huffman code at bit %d" % bits.p)


def idat(path):
    d = open(path, 'rb').read()
    pos, out = 8, b''
    while pos + 8 <= len(d):
        ln = struct.unpack('>I', d[pos:pos + 4])[0]
        tag = d[pos + 4:pos + 8]
        if tag == b'IDAT':
            out += d[pos + 8:pos + 8 + ln]
        pos += 12 + ln
    return out


def main(path):
    payload = idat(path)
    cmf, flg = payload[0], payload[1]
    print("%s: IDAT %d bytes, zlib %02x %02x (CM=%d CINFO=%d %%31=%d)"
          % (path, len(payload), cmf, flg, cmf & 15, cmf >> 4,
             (cmf * 256 + flg) % 31))

    bits = Bits(payload[2:])
    produced = 0
    nsym = 0
    while True:
        final = bits.get(1)
        btype = bits.get(2)
        print("block: BFINAL=%d BTYPE=%d at bit %d" % (final, btype, bits.p))
        if btype == 2:
            hlit = bits.get(5) + 257
            hdist = bits.get(5) + 1
            hclen = bits.get(4) + 4
            cl = [0] * 19
            for i in range(hclen):
                cl[CLORDER[i]] = bits.get(3)
            clt = table(cl)
            lens = []
            while len(lens) < hlit + hdist:
                s = decode(bits, clt)
                if s < 16:
                    lens.append(s)
                elif s == 16:
                    lens += [lens[-1]] * (3 + bits.get(2))
                elif s == 17:
                    lens += [0] * (3 + bits.get(3))
                else:
                    lens += [0] * (11 + bits.get(7))
            lit = table(lens[:hlit])
            dst = table(lens[hlit:hlit + hdist])
        elif btype == 1:
            l = [8] * 144 + [9] * 112 + [7] * 24 + [8] * 8
            lit, dst = table(l), table([5] * 30)
        else:
            bits.align()
            n = bits.get(16)
            bits.get(16)
            produced += n
            print("  stored %d bytes -> total %d" % (n, produced))
            continue

        while True:
            s = decode(bits, lit)
            if s == 256:
                print("  end-of-block at output %d after %d symbols"
                      % (produced, nsym))
                break
            if s < 256:
                produced += 1
                nsym += 1
                continue
            i = s - 257
            length = LENS[i] + bits.get(LEXT[i])
            ds = decode(bits, dst)
            distance = DIST[ds] + bits.get(DEXT[ds])
            print("  match len=%3d dist=%3d at output %4d..%4d  (back to %d)"
                  % (length, distance, produced, produced + length,
                     produced - distance))
            produced += length
            nsym += 1
        if final:
            break

    tail = (len(payload) - 2) * 8 - bits.p
    print("total output %d bytes from %d symbols; %d bits left over"
          % (produced, nsym, tail))


if __name__ == '__main__':
    main(sys.argv[1])