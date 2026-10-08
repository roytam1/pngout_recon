"""
jpeglib.py - a baseline JPEG writer and a from-spec baseline reader.

The writer exists so tools/decodetest.py can produce JPEG input for
sub_40B180.  Nothing in the tree encodes JPEG, so this does.

A useful thing about it: sub_40B180 reads whatever Huffman and quantisation
tables the file carries, so the corpus does not have to use the exact
tables from the JPEG spec's annex K - any legal tables work, and this
module builds its own from a code-length list.  That means a slip in a
table here cannot make a test pass that should fail, because the decoder
under test never consults these tables at all; they only have to be
self-consistent.  The counts are still checked against the values, since a
mismatch there produces a file that is not legal.

The reader is a separate, deliberately naive implementation written from
the spec, used only to check that the writer produces something that
decodes back to the picture it was given.  It is not a substitute for a
third party decoder: if both sides share a misreading of the standard they
will agree with each other and both be wrong.
"""

import math
import struct

# ---------------------------------------------------------------- tables

ZIGZAG = [
    0, 1, 8, 16, 9, 2, 3, 10,
    17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
]

QUANT_LUMA = [
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77,
    24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103, 99,
]

QUANT_CHROMA = [
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
]

# (counts for code lengths 1..16, values).  Annex K's tables, so a real
# decoder can read the files; the counts are checked against the values
# below, which is the only thing that has to be right.
DC_LUMA_COUNTS = [0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0]
DC_LUMA_VALUES = list(range(12))

DC_CHROMA_COUNTS = [0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0]
DC_CHROMA_VALUES = list(range(12))

AC_LUMA_COUNTS = [0, 2, 1, 3, 3, 2, 4, 3,
                  5, 5, 4, 4, 0, 0, 1, 0x7D]
AC_LUMA_VALUES = [
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12,
    0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08,
    0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16,
    0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
    0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
    0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
    0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6,
    0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5,
    0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4,
    0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
    0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA,
    0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
    0xF9, 0xFA,
]

AC_CHROMA_COUNTS = [0, 2, 1, 2, 4, 4, 3, 4,
                    7, 5, 4, 4, 0, 1, 2, 0x77]
AC_CHROMA_VALUES = [
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21,
    0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
    0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0,
    0x15, 0x62, 0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34,
    0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26,
    0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96,
    0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5,
    0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4,
    0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3,
    0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2,
    0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA,
    0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9,
    0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
    0xF9, 0xFA,
]


def huff_codes(counts, values):
    """
    The canonical code for each symbol: walk the code lengths in order,
    doubling the running code on every step, which is what annex C
    describes.
    """
    codes = {}
    code = 0
    k = 0
    for length in range(1, 17):
        for _ in range(counts[length - 1]):
            codes[values[k]] = (code, length)
            k += 1
            code += 1
        code <<= 1
    return codes


def check_tables():
    """The counts have to add up to the values, or the file is not legal."""
    for counts, values, what in (
        (DC_LUMA_COUNTS, DC_LUMA_VALUES, "dc luma"),
        (DC_CHROMA_COUNTS, DC_CHROMA_VALUES, "dc chroma"),
        (AC_LUMA_COUNTS, AC_LUMA_VALUES, "ac luma"),
        (AC_CHROMA_COUNTS, AC_CHROMA_VALUES, "ac chroma"),
    ):
        if sum(counts) != len(values):
            raise ValueError("%s: counts %d != %d values"
                             % (what, sum(counts), len(values)))
    for counts, values in ((AC_LUMA_COUNTS, AC_LUMA_VALUES),
                           (AC_CHROMA_COUNTS, AC_CHROMA_VALUES)):
        if len(values) != 162:
            raise ValueError("an AC table needs 162 symbols, got %d"
                             % len(values))


# ---------------------------------------------------------------- the DCT

# The 2D transform carries a 1/4 and each axis a C(u) = 1/sqrt(2) at u = 0,
# so the C(u) lives in _SCALE only - putting it in _COS as well counts it
# twice and halves every coefficient.
_COS = [[math.cos((2 * x + 1) * u * math.pi / 16.0) for u in range(8)]
        for x in range(8)]
_SCALE = [0.5 / math.sqrt(2.0) if u == 0 else 0.5 for u in range(8)]


def fdct8x8(samples):
    """A float forward DCT-II with the usual 1/4 normalisation."""
    tmp = [[0.0] * 8 for _ in range(8)]
    for y in range(8):
        row = samples[y]
        for u in range(8):
            cu = _SCALE[u]
            s = 0.0
            for x in range(8):
                s += row[x] * _COS[x][u]
            tmp[y][u] = cu * s
    out = [[0.0] * 8 for _ in range(8)]
    for u in range(8):
        col = [tmp[y][u] for y in range(8)]
        for v in range(8):
            cv = _SCALE[v]
            s = 0.0
            for y in range(8):
                s += col[y] * _COS[y][v]
            out[v][u] = cv * s
    return out


# ---------------------------------------------------------------- the writer

class Bits(object):
    """The entropy-coded segment's bit stream: MSB first, with byte stuffing."""

    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def put(self, code, length):
        for i in range(length - 1, -1, -1):
            self.acc = (self.acc << 1) | ((code >> i) & 1)
            self.n += 1
            if self.n == 8:
                byte = self.acc & 0xFF
                self.out.append(byte)
                if byte == 0xFF:
                    self.out.append(0x00)       # the byte stuffing
                self.acc = 0
                self.n = 0

    def flush(self):
        while self.n:
            self.put(1, 1)                       # pad with 1 bits
        return bytes(self.out)


def _category(value):
    """The JPEG SSSS: how many bits the magnitude needs."""
    a = abs(value)
    n = 0
    while a:
        a >>= 1
        n += 1
    return n


def _segment(marker, body):
    return bytes([0xFF, marker]) + struct.pack(">H", len(body) + 2) + body


def write_jpeg(path, w, h, rgb, comps="444", restart=0, precision=8):
    """
    Write a baseline JPEG.

    `rgb` is a list of h rows of w (r, g, b) tuples in 0..255.  `comps` is
    one of "grey", "444", "422" or "420" - greyscale has one component, the
    rest are YCbCr with the sampling factors that name implies.  `restart`
    puts a restart marker every that many MCUs, and `precision` is 8 or 16
    for the quantisation table's entry size.
    """
    check_tables()

    if comps == "grey":
        samp = [(1, 1, 1, 0, 0, 0)]             # id, h, v, dq, dc, ac
        quant = [QUANT_LUMA]
    elif comps == "444":
        samp = [(1, 1, 1, 0, 0, 0), (2, 1, 1, 1, 1, 1), (3, 1, 1, 1, 1, 1)]
        quant = [QUANT_LUMA, QUANT_CHROMA]
    elif comps == "422":
        samp = [(1, 2, 1, 0, 0, 0), (2, 1, 1, 1, 1, 1), (3, 1, 1, 1, 1, 1)]
        quant = [QUANT_LUMA, QUANT_CHROMA]
    elif comps == "420":
        samp = [(1, 2, 2, 0, 0, 0), (2, 1, 1, 1, 1, 1), (3, 1, 1, 1, 1, 1)]
        quant = [QUANT_LUMA, QUANT_CHROMA]
    else:
        raise ValueError("comps must be grey, 444, 422 or 420")

    # ---- the component planes ----
    if comps == "grey":
        planes = [[[(19595 * px[0] + 38470 * px[1] + 7471 * px[2] + 32768)
                    >> 16 for px in row] for row in rgb]]
    else:
        ys, cbs, crs = [], [], []
        for row in rgb:
            yy, bb, rr = [], [], []
            for (r, g, b) in row:
                yy.append((19595 * r + 38470 * g + 7471 * b + 32768) >> 16)
                bb.append((-11056 * r - 21712 * g + 32768 * b + 8388608) >> 16)
                rr.append((32768 * r - 27440 * g - 5328 * b + 8388608) >> 16)
            ys.append(yy)
            cbs.append(bb)
            crs.append(rr)
        planes = [ys, cbs, crs]

    hmax = max(c[1] for c in samp)
    vmax = max(c[2] for c in samp)
    mcux = (w + 8 * hmax - 1) // (8 * hmax)
    mcuy = (h + 8 * vmax - 1) // (8 * vmax)

# each component, padded up to a whole number of MCUs
    padded = []
    for i, (cid, hs, vs, dq, dc, ac) in enumerate(samp):
        pw = mcux * hs * 8
        ph = mcuy * vs * 8
        plane = [[0] * pw for _ in range(ph)]
        # a subsampled plane can be narrower than the picture: the columns
        # past the last whole sample are not part of this component at all,
        # so they are dropped rather than clipped
        for y in range(min(h, ph)):
            src = planes[i][y]
            dst = plane[y]
            for x in range(min(w, pw)):
                dst[x] = src[x]
        # The edge of the picture is repeated into the padding rather than
        # filled with black: a partial edge block then mixes real samples with
        # copies of their neighbour, which is what a real encoder does and
        # what keeps the decoded edge close to the original.  Zero padding
        # would drag the last row and column towards black.
        #
        # The row copy has to run over the rows already filled, and the column
        # copy over the columns, so both go from what is there outwards.
        for y in range(min(h, ph)):
            row = plane[y]
            if w < pw:
                for x in range(w, pw):
                    row[x] = row[w - 1]
        for y in range(h, ph):
            plane[y] = list(plane[h - 1])
        padded.append(plane)

    # ---- quantise every block once ----
    blocks = []
    for i, (cid, hs, vs, dq, dc, ac) in enumerate(samp):
        pw = mcux * hs * 8
        per_row = pw // 8
        # this component's own row count, not the last one's - which is the
        # same number for 4:4:4, 4:2:2 and grey and wrong only for 4:2:0,
        # where the luma plane is twice as tall as the chroma planes
        rows = mcuy * vs
        plane = padded[i]
        grid = []
        for by in range(rows):
            row = []
            for bx in range(per_row):
                blk = []
                for y in range(8):
                    srow = plane[by * 8 + y]
                    blk.append([srow[bx * 8 + x] - 128 for x in range(8)])
                coef = fdct8x8(blk)
                # `coef` is natural (row major) order and so is the table, but
                # the block is emitted in zigzag order, so both get indexed by
                # ZIGZAG[k]: once for the coefficient, once for its divisor
                row.append([int(round(
                    coef[ZIGZAG[k] // 8][ZIGZAG[k] % 8] / quant[dq][ZIGZAG[k]]))
                    for k in range(64)])
            grid.append(row)
        blocks.append(grid)

    codes = {}
    slots = []
    if comps == "grey":
        slots = [(0, 0, DC_LUMA_COUNTS, DC_LUMA_VALUES),
                 (1, 0, AC_LUMA_COUNTS, AC_LUMA_VALUES)]
    else:
        slots = [(0, 0, DC_LUMA_COUNTS, DC_LUMA_VALUES),
                 (1, 0, AC_LUMA_COUNTS, AC_LUMA_VALUES),
                 (0, 1, DC_CHROMA_COUNTS, DC_CHROMA_VALUES),
                 (1, 1, AC_CHROMA_COUNTS, AC_CHROMA_VALUES)]
    for tc, th, counts, values in slots:
        codes[(tc, th)] = huff_codes(counts, values)

    # ---- the entropy-coded body ----
    bits = Bits()
    pred = [0] * len(samp)
    since_restart = 0
    rstseq = 0                        # which RSTn is next: D0, D1, ... D7, D0
    for my in range(mcuy):
        for mx in range(mcux):
            if restart and since_restart == restart:
                bits.flush()
                # The marker has to *count*: RST0, RST1, ... RST7 and round
                # again, one per interval.  Emitting RST0 every time looks
                # plausible and is wrong - libjpeg says "found marker 0xd0
                # instead of RST1" and resynchronises at the wrong place.
                bits.out.extend(bytes([0xFF, 0xD0 + (rstseq & 7)]))
                rstseq += 1
                bits.acc = 0
                bits.n = 0
                for k in range(len(pred)):
                    pred[k] = 0
                since_restart = 0
            since_restart += 1
            for ci, (cid, hs, vs, dq, dc, ac) in enumerate(samp):
                dcc = codes[(0, dc)]
                acc = codes[(1, ac)]
                for by in range(vs):
                    for bx in range(hs):
                        blk = blocks[ci][my * vs + by][mx * hs + bx]
                        diff = blk[0] - pred[ci]
                        pred[ci] = blk[0]
                        cat = _category(diff)
                        code, length = dcc[cat]
                        bits.put(code, length)
                        if cat:
                            val = diff if diff > 0 else diff + (1 << cat) - 1
                            bits.put(val, cat)
                        run = 0
                        for k in range(1, 64):
                            if blk[k] == 0:
                                run += 1
                                continue
                            while run > 15:
                                code, length = acc[0xF0]
                                bits.put(code, length)
                                run -= 16
                            cat = _category(blk[k])
                            code, length = acc[(run << 4) | cat]
                            bits.put(code, length)
                            val = blk[k]
                            if val < 0:
                                val += (1 << cat) - 1
                            bits.put(val, cat)
                            run = 0
                        if run:
                            code, length = acc[0x00]
                            bits.put(code, length)

    # ---- the file ----
    out = bytearray(b"\xFF\xD8")
    out += _segment(0xE0, b"JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00")
    for i, q in enumerate(quant):
        if precision == 8:
            body = bytes([i]) + bytes(q[k] for k in range(64))
        else:
            body = bytes([(1 << 4) | i])
            body += b"".join(struct.pack(">H", q[k]) for k in range(64))
        out += _segment(0xDB, body)
    # the frame header's sample precision is always 8 - baseline JPEG has no
    # other, and libjpeg rejects "Unsupported JPEG data precision 16".  It is
    # NOT the same field as the quantisation table's precision byte, which is
    # what `precision` selects here.
    sof = bytes([8]) + struct.pack(">HH", h, w) + bytes([len(samp)])
    for cid, hs, vs, dq, dc, ac in samp:
        sof += bytes([cid, (hs << 4) | vs, dq])
    out += _segment(0xC0, sof)
    if restart:
        out += _segment(0xDD, struct.pack(">H", restart))
    for tc, th, counts, values in slots:
        out += _segment(0xC4, bytes([(tc << 4) | th]) + bytes(counts)
                        + bytes(values))
    sos = bytes([len(samp)])
    for cid, hs, vs, dq, dc, ac in samp:
        sos += bytes([cid, (dc << 4) | ac])
    sos += bytes([0, 63, 0])
    out += _segment(0xDA, sos)
    out += bits.flush()
    out += b"\xFF\xD9"

    with open(path, "wb") as fh:
        fh.write(bytes(out))
    return bytes(out)


# ---------------------------------------------------------------- the reader

class _Huff(object):
    """A canonical Huffman table, decoded one bit at a time."""

    def __init__(self, counts, values):
        self.lookup = {}
        code = 0
        k = 0
        for length in range(1, 17):
            for _ in range(counts[length - 1]):
                self.lookup[(code, length)] = values[k]
                k += 1
                code += 1
            code <<= 1

    def decode(self, br):
        code = 0
        for length in range(1, 17):
            code = (code << 1) | br.bit()
            if (code, length) in self.lookup:
                return self.lookup[(code, length)]
        raise ValueError("no huffman code")


class _Reader(object):
    def __init__(self, data, pos):
        self.d = data
        self.p = pos
        self.acc = 0
        self.n = 0

    def bit(self):
        if self.n == 0:
            if self.p >= len(self.d):
                raise ValueError("past the end")
            b = self.d[self.p]
            self.p += 1
            if b == 0xFF:
                nxt = self.d[self.p] if self.p < len(self.d) else 0
                if nxt != 0x00:
                    raise ValueError("marker %02X in the scan" % nxt)
                self.p += 1
            self.acc = b
            self.n = 8
        self.n -= 1
        return (self.acc >> self.n) & 1

    def bits(self, count):
        v = 0
        for _ in range(count):
            v = (v << 1) | self.bit()
        return v


def _extend(v, cat):
    return v if v >= (1 << (cat - 1)) else v - (1 << cat) + 1


def read_jpeg(path):
    """
    Decode a baseline JPEG written by write_jpeg() and return (w, h, rows)
    with rows of (r, g, b).  Naive and spec-first; it exists to check the
    writer, not to be a reference decoder.
    """
    with open(path, "rb") as fh:
        d = fh.read()
    if d[:2] != b"\xFF\xD8":
        raise ValueError("not a JPEG")
    p = 2
    quant = {}
    huff = {}
    comps = []
    w = h = 0
    restart = 0
    while True:
        if d[p] != 0xFF:
            raise ValueError("expected a marker at %d" % p)
        m = d[p + 1]
        p += 2
        if m == 0xD9:
            break
        (ln,) = struct.unpack(">H", d[p:p + 2])
        body = d[p + 2:p + ln]
        p += ln
        if m == 0xDB:
            i = 0
            while i < len(body):
                pq = body[i] >> 4
                tq = body[i] & 15
                i += 1
                tbl = []
                for _ in range(64):
                    if pq:
                        tbl.append(struct.unpack(">H", body[i:i + 2])[0])
                        i += 2
                    else:
                        tbl.append(body[i])
                        i += 1
                q = [0] * 64
                for k in range(64):
                    q[k] = tbl[ZIGZAG[k]]
                quant[tq] = q
        elif m == 0xC4:
            i = 0
            while i < len(body):
                tc = body[i] >> 4
                th = body[i] & 15
                i += 1
                counts = list(body[i:i + 16])
                i += 16
                total = sum(counts)
                values = list(body[i:i + total])
                i += total
                huff[(tc, th)] = _Huff(counts, values)
        elif m == 0xDD:
            (restart,) = struct.unpack(">H", body[:2])
        elif m == 0xC0:
            h, w = struct.unpack(">HH", body[1:5])
            nc = body[5]
            comps = []
            for c in range(nc):
                cid = body[6 + c * 3]
                samp = body[7 + c * 3]
                comps.append((cid, samp >> 4, samp & 15, body[8 + c * 3]))
        elif m == 0xDA:
            ns = body[0]
            sel = []
            for c in range(ns):
                sel.append((body[1 + c * 2], body[2 + c * 2] >> 4,
                            body[2 + c * 2] & 15))
            return _decode_scan(d, p, w, h, comps, sel, quant, huff,
                                restart, body[ns * 2 + 1], body[ns * 2 + 2],
                                body[ns * 2 + 3])
        else:
            pass
    raise ValueError("no scan")


def _idct(blk):
    tmp = [[0.0] * 8 for _ in range(8)]
    for y in range(8):
        for x in range(8):
            s = 0.0
            for u in range(8):
                cu = _SCALE[u]
                if blk[y * 8 + u]:
                    s += cu * blk[y * 8 + u] * _COS[x][u]
            tmp[y][x] = s
    out = [[0.0] * 8 for _ in range(8)]
    for x in range(8):
        for y in range(8):
            s = 0.0
            for v in range(8):
                cv = _SCALE[v]
                if tmp[v][x]:
                    s += cv * tmp[v][x] * _COS[y][v]
            out[y][x] = s
    return out


def _decode_scan(d, p, w, h, comps, sel, quant, huff, restart, ss, se, ah_al):
    if ss != 0 or se != 63 or ah_al != 0:
        raise ValueError("only a full baseline scan is handled")
    hmax = max(c[1] for c in comps)
    vmax = max(c[2] for c in comps)
    mcux = (w + 8 * hmax - 1) // (8 * hmax)
    mcuy = (h + 8 * vmax - 1) // (8 * vmax)
    br = _Reader(d, p)

    planes = {}
    for cid, hs, vs, dq in comps:
        pw = mcux * hs * 8
        ph = mcuy * vs * 8
        planes[cid] = [[0] * pw for _ in range(ph)]

    pred = dict((cid, 0) for cid, hs, vs, dq in comps)
    since = 0
    for my in range(mcuy):
        for mx in range(mcux):
            if restart and since == restart:
                # skip the marker and the padding to the byte boundary
                br.n = 0
                while br.p < len(d) - 1 and not (d[br.p] == 0xFF
                                                 and 0xD0 <= d[br.p + 1] <= 0xD7):
                    br.p += 1
                br.p += 2
                for k in pred:
                    pred[k] = 0
                since = 0
            since += 1
            for cid, hs, vs, dq in comps:
                if not any(s[0] == cid for s in sel):
                    continue
                dc = next(s[1] for s in sel if s[0] == cid)
                ac = next(s[2] for s in sel if s[0] == cid)
                dtab = huff[(0, dc)]
                atab = huff[(1, ac)]
                q = quant[dq]
                plane = planes[cid]
                for by in range(vs):
                    for bx in range(hs):
                        blk = [0] * 64
                        cat = dtab.decode(br)
                        diff = _extend(br.bits(cat), cat) if cat else 0
                        pred[cid] += diff
                        blk[0] = pred[cid] * q[0]
                        k = 1
                        while k < 64:
                            rs = atab.decode(br)
                            run, size = rs >> 4, rs & 15
                            if size == 0:
                                if run == 15:
                                    k += 16
                                    continue
                                break
                            k += run
                            if k > 63:
                                break
                            zz = ZIGZAG[k]
                            blk[zz] = _extend(br.bits(size), size) * q[zz]
                            k += 1
                        px = _idct(blk)
                        oy = (my * vs + by) * 8
                        ox = (mx * hs + bx) * 8
                        for yy in range(8):
                            row = plane[oy + yy]
                            for xx in range(8):
                                v = int(round(px[yy][xx] + 128))
                                row[ox + xx] = 0 if v < 0 else (255 if v > 255 else v)

    out = []
    grey = len(comps) == 1
    for y in range(h):
        row = []
        for x in range(w):
            if grey:
                v = planes[comps[0][0]][y][x]
                row.append((v, v, v))
                continue
            vals = []
            for cid, hs, vs, dq in comps:
                yy = y * vs // vmax
                xx = x * hs // hmax
                vals.append(planes[cid][yy][xx])
            yyv, cb, cr = vals[0], vals[1] - 128, vals[2] - 128
            r = yyv + 1.402 * cr
            g = yyv - 0.344136 * cb - 0.714136 * cr
            b = yyv + 1.772 * cb
            row.append((max(0, min(255, int(round(r)))),
                        max(0, min(255, int(round(g)))),
                        max(0, min(255, int(round(b))))))
        out.append(row)
    return w, h, out