#!/usr/bin/env python3
"""
decodetest.py - check what the reconstructed decoder makes of a PNG.

pngcheck.py checks the encoder: it feeds Optimize() a known image and compares
what comes back out.  This checks the decoder, which is the other half and the
one the reconstruction spent longest on.  PNGs are written here whose pixels are
known by construction, build\\dumpdec.exe decodes them through the real
DecodeImage(), and the raw 0xAARRGGBB buffer it dumps is compared against the
pixels that went in.

Between them the cases cover the whole decoder: the chunk walk, the bit reader
and its pump, the three Huffman helpers, the inflate body over stored, fixed and
dynamic blocks, the unfilter's five branches, and the row expansion for every
colour type and bit depth the reconstruction accepts.  Nothing here involves the
optimiser, so a failure is always the decoder's.

Every row's filter byte is under this script's control rather than zlib's, which
is what makes the five unfilter branches reachable on purpose instead of by
luck; the deflate side is varied through the level, the strategy and the number
of IDAT chunks.

    tools\\dumpdec.cmd             build the probe
    python tools\\decodetest.py    run every case
    python tools\\decodetest.py 3 7   run two of them
"""

import os
import struct
import subprocess
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
DUMPDEC = os.path.join(BUILD, "dumpdec.exe")
PNGOUT = os.path.join(BUILD, "pngout.exe")

CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


# ------------------------------------------------------------------ writing


def chunk(tag, body):
    return (struct.pack(">I", len(body)) + tag + body
            + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF))


def pack_row(samples, depth):
    """Pack a row of samples MSB first, the way PNG stores sub-byte depths."""
    out = bytearray()
    acc = 0
    nbits = 0
    for s in samples:
        acc = (acc << depth) | (s & ((1 << depth) - 1))
        nbits += depth
        if nbits == 8:
            out.append(acc)
            acc = 0
            nbits = 0
    if nbits:
        out.append(acc << (8 - nbits))
    return bytes(out)


def row_bytes(row, depth):
    flat = [s for px in row for s in px]
    return pack_row(flat, depth) if depth < 8 else bytes(flat)


def filter_row(line, prev, bpp, ft):
    """Apply one PNG filter to a row and return (filter byte, filtered row)."""
    cur = bytearray(line)
    up = bytearray(prev)
    out = bytearray(len(cur))
    for i in range(len(cur)):
        a = cur[i - bpp] if i >= bpp else 0
        b = up[i]
        c = up[i - bpp] if i >= bpp else 0
        if ft == 0:
            pred = 0
        elif ft == 1:
            pred = a
        elif ft == 2:
            pred = b
        elif ft == 3:
            pred = (a + b) >> 1
        elif ft == 4 or ft == 5:
            #
            #  Filter byte 4 is *not* PNG's Paeth predictor.  PNGOUT redirects
            #  it to its own (0x4110CC makes 4 decode as 5) and both branches
            #  of that predictor measure the distances its own way:
            #
            #      d1 = |a + b - 2c|   d2 = |b - c|   d3 = |a - c|
            #      a if d2 <= d3 and d2 <= d1, else b if d3 <= d1, else c
            #
            #  so a foreign PNG row that uses real Paeth is reconstructed
            #  wrongly by PNGOUT, and the reconstruction reproduces that.  The
            #  filter here has to be the PNGOUT one, or the test would be
            #  measuring the difference instead of the decoder.
            d1 = abs(a + b - 2 * c)
            d2 = abs(b - c)
            d3 = abs(a - c)
            if d2 <= d3 and d2 <= d1:
                pred = a
            elif d3 <= d1:
                pred = b
            else:
                pred = c
        else:
            raise ValueError("filter %d" % ft)
        out[i] = (cur[i] - pred) & 0xFF
    return bytes([ft]) + bytes(out)


def write_png(path, width, height, coltype, depth, rows, palette=None,
              trns=None, filters=None, level=6, strategy=zlib.Z_DEFAULT_STRATEGY,
              split=1, extra=()):
    """rows: one list per row, each a list of pixels, each pixel a tuple."""
    ihdr = struct.pack(">IIBBBBB", width, height, depth, coltype, 0, 0, 0)
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
    if palette is not None:
        out += chunk(b"PLTE", b"".join(bytes(p[:3]) for p in palette))
    if trns is not None:
        out += chunk(b"tRNS", bytes(trns))
    for tag, body in extra:
        out += chunk(tag, body)

    bpp = max(1, CHANNELS[coltype] * depth // 8)
    raw = b""
    prev = bytes((width * CHANNELS[coltype] * depth + 7) // 8)
    for y, row in enumerate(rows):
        line = row_bytes(row, depth)
        if filters is None:
            raw += b"\0" + line          # zlib picks the real filters later
        else:
            ft = filters[y] if isinstance(filters, (list, tuple)) else filters
            raw += filter_row(line, prev, bpp, ft)
        prev = line

    if filters is None:
        data = zlib.compress(raw, level)
    else:
        co = zlib.compressobj(level, zlib.DEFLATED, 15, 9, strategy)
        data = co.compress(raw) + co.flush()

    # split the deflate stream across n IDAT chunks, the way encoders do
    step = max(1, (len(data) + split - 1) // split)
    for i in range(0, max(len(data), 1), step):
        out += chunk(b"IDAT", data[i:i + step])
    out += chunk(b"IEND", b"")
    with open(path, "wb") as fh:
        fh.write(out)


# ------------------------------------------------------------------ expecting


def expect(coltype, depth, rows, palette=None, trns=None):
    """
    The 0xAARRGGBB buffer the decoder is supposed to produce.

    Returned in *internal* byte order - least significant byte first, so B, G,
    R, A for a colour pixel - because that is the layout loc_40ADB0 and
    loc_40AD20 build and the layout FilterSample() reads back out.  An internal
    pixel is one DWORD, so "0xFFBBGGRR" and "bytes R, G, B, FF" describe the
    same thing and the order is what has to be right.
    """
    px = []
    maxval = (1 << depth) - 1
    for row in rows:
        out = []
        for p in row:
            if coltype == 0:
                v = p[0] * 255 // maxval
                out.append((v, v, v, 255))
            elif coltype == 2:
                out.append((p[2], p[1], p[0], 255))
            elif coltype == 3:
                r, g, b = palette[p[0]][:3]
                a = trns[p[0]] if trns and p[0] < len(trns) else 255
                out.append((b, g, r, a))
            elif coltype == 4:
                v = p[0] * 255 // maxval
                out.append((v, v, v, p[1]))
            else:
                out.append((p[2], p[1], p[0], p[3]))
        px.append(tuple(out))
    return px


# ------------------------------------------------------------------ the corpus


def grad(w, h, chans):
    return [[tuple((x * 17 + y * 23 + c * 61) & 0xFF for c in range(chans))
             for x in range(w)] for y in range(h)]


def blocks(w, h, chans):
    return [[tuple(((x // 3 * 61 + y // 2 * 83 + c * 29) & 0xFF)
                   for c in range(chans)) for x in range(w)]
            for y in range(h)]


def noise(w, h, chans):
    out = []
    s = 12345
    for _y in range(h):
        row = []
        for _x in range(w):
            px = []
            for _c in range(chans):
                s = (s * 1103515245 + 12345) & 0x7FFFFFFF
                px.append((s >> 16) & 0xFF)
            row.append(tuple(px))
        out.append(row)
    return out


def pal8_early():
    """The 256 entry palette, defined before the case list needs it."""
    return [(i * 11 & 0xFF, (i * 17) & 0xFF, (i * 3) & 0xFF)
            for i in range(256)]


def expect_rgba(ct, depth, rows, palette=None, trns=None):
    """expect(), but in PNG channel order - what a PNG *file* carries.

    The internal buffer is 0xFFBBGGRR, so this is the same pixels with the red
    and blue bytes swapped back.  Only the round trip through pngout.exe needs
    this; read_pixels() reads a file, not a pixel buffer.
    """
    return [[(p[2], p[1], p[0], p[3]) for p in row]
            for row in expect(ct, depth, rows, palette, trns)]


def roundtrip(case):
    """
    The whole path: DecodeImage(), then Optimize(), then compare the file.

    Only meaningful on images PNGOUT can actually make smaller - the output
    buffer is the size of the *input* file (that is dword_411540's job in the
    original: writes past it are dropped), so a picture PNGOUT cannot beat is
    refused with "Unable to compress further" and keeps its own bytes.  These
    cases are therefore built from compressible content.
    """
    name, w, h, ct, depth, rows = case[:6]
    palette = case[6] if len(case) > 6 else None
    trns = case[7] if len(case) > 7 else None
    opts = dict(case[8]) if len(case) > 8 else {}
    src = os.path.join(BUILD, "rt_%s.png" % name)
    dst = os.path.join(BUILD, "rt_%s.out.png" % name)
    want = expect_rgba(ct, depth, rows, palette, trns)
    write_png(src, w, h, ct, depth, rows, palette, trns, **opts)
    if os.path.exists(dst):
        os.remove(dst)
    r = subprocess.run([PNGOUT, "/force", src, dst], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    out = (r.stdout + r.stderr).decode("latin1")
    if not os.path.exists(dst):
        return "%-14s no output: %s" % (name, out.strip()[-70:])
    try:
        got = read_pixels(dst)
    except Exception as exc:                       # noqa: BLE001
        return "%-14s output unreadable: %s" % (name, exc)
    if (got["width"], got["height"]) != (w, h):
        return "%-14s wrong size %dx%d" % (name, got["width"], got["height"])
    bad = first = None
    bad = 0
    first = None
    for y in range(h):
        for x in range(w):
            if tuple(got["rows"][y][x]) != want[y][x]:
                bad += 1
                if first is None:
                    first = "x=%d y=%d got %s want %s" % (
                        x, y, got["rows"][y][x], want[y][x])
    if bad:
        return "%-14s %d/%d pixels differ, first %s" % (name, bad, w * h,
                                                        first)
    return "%-14s  ok  (%dx%d ct%d d%d)" % (name, w, h, ct, depth)


def read_pixels(path):
    """Decode a PNG into per-row lists of RGBA tuples (a reader of our own)."""
    with open(path, "rb") as fh:
        data = fh.read()
    pos = 8
    ihdr = palette = trns = None
    idat = bytearray()
    while pos + 8 <= len(data):
        (ln,) = struct.unpack(">I", data[pos:pos + 4])
        tag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        (crc,) = struct.unpack(">I", data[pos + 8 + ln:pos + 12 + ln])
        if zlib.crc32(tag + body) & 0xFFFFFFFF != crc:
            raise ValueError("chunk %s bad CRC" % tag.decode("latin1"))
        if tag == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif tag == b"PLTE":
            palette = body
        elif tag == b"tRNS":
            trns = body
        elif tag == b"IDAT":
            idat += body
        pos += 12 + ln
    w, h, depth, ct = ihdr[0], ihdr[1], ihdr[2], ihdr[3]
    raw = zlib.decompress(bytes(idat))
    chans = CHANNELS[ct]
    stride = (w * chans * depth + 7) // 8
    bpp = max(1, chans * depth // 8)
    flat = bytearray()
    prev = bytearray(stride)
    pos = 0
    for _ in range(h):
        ft = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        if ft == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                c = prev[i - bpp] if i >= bpp else 0
                b = prev[i]
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        elif ft != 0:
            raise ValueError("PNGOUT wrote filter %d" % ft)
        flat += line
        prev = line
    maxv = (1 << depth) - 1
    rows = []
    for y in range(h):
        base = y * stride
        row = []
        for x in range(w):
            if ct in (0, 3) and depth < 8:
                per = 8 // depth
                n = (flat[base + (x * depth) // 8]
                     >> (8 - depth - (x % per) * depth)) & maxv
            else:
                n = flat[base + x * chans]
            if ct == 0:
                v = n * 255 // maxv
                row.append((v, v, v, 255))
            elif ct == 2:
                row.append((flat[base + x * 3], flat[base + x * 3 + 1],
                            flat[base + x * 3 + 2], 255))
            elif ct == 3:
                i = n * 3
                a = trns[n] if trns is not None and n < len(trns) else 255
                row.append((palette[i], palette[i + 1], palette[i + 2], a))
            elif ct == 4:
                v = n * 255 // maxv
                row.append((v, v, v, flat[base + x * 2 + 1]))
            else:
                row.append((flat[base + x * 4], flat[base + x * 4 + 1],
                            flat[base + x * 4 + 2], flat[base + x * 4 + 3]))
        rows.append(tuple(row))
    return {"width": w, "height": h, "rows": rows}


def make_cases():
    cases = []
    w, h = 13, 9

    def add(name, ww, hh, ct, depth, rows, pal=None, trns=None, **opts):
        cases.append((name, ww, hh, ct, depth, rows, pal, trns, opts))

    # every colour type at depth 8, every filter on every row
    for ct, chans in ((0, 1), (2, 3), (4, 2), (6, 4)):
        for maker in (grad, blocks, noise):
            rows = maker(w, h, chans)
            add("%s-ct%d" % (maker.__name__, ct), w, h, ct, 8, rows)
    for ft in range(6):
        add("f%d-ct2" % ft, w, h, 2, 8, grad(w, h, 3), filters=ft)
        add("f%d-ct0" % ft, w, h, 0, 8, grad(w, h, 1), filters=ft)
        add("f%d-ct4" % ft, w, h, 4, 8, grad(w, h, 2), filters=ft)
        add("f%d-ct6" % ft, w, h, 6, 8, grad(w, h, 4), filters=ft)
    # filters rotating row by row, which is what the "mixed" filter produces
    for ct, chans in ((0, 1), (2, 3), (4, 2), (6, 4)):
        add("rot-ct%d" % ct, w, h, ct, 8, grad(w, h, chans),
            filters=[y % 5 for y in range(h)])
    add("rot-ct3", w, h, 3, 8, grad(w, h, 1), pal=pal8_early(),
        filters=[y % 5 for y in range(h)])

    # sub-byte greyscale
    for depth in (1, 2, 4):
        maxv = (1 << depth) - 1
        rows = [[(((x + y) % (maxv + 1)),) for x in range(w)]
                for y in range(h)]
        for ft in range(6):
            add("d%d-f%d" % (depth, ft), w, h, 0, depth, rows, filters=ft)

    # palettes
    pal4 = [(i * 7 & 0xFF, (i * 13) & 0xFF, (i * 29) & 0xFF)
            for i in range(1 << 4)]
    trns = [0xFF] * 16
    trns[3] = 0x40
    rows4 = [[(((x * 5 + y * 3) % 16),) for x in range(w)] for y in range(h)]
    add("ct3-d4", w, h, 3, 4, rows4, pal4, trns)
    for ft in range(6):
        add("ct3-d4-f%d" % ft, w, h, 3, 4, rows4, pal4, trns, filters=ft)

    pal8 = pal8_early()
    rows8 = [[(((x * 31 + y * 7) & 0xFF),) for x in range(w)]
             for y in range(h)]
    add("ct3-d8", w, h, 3, 8, rows8, pal8)
    for ft in range(6):
        add("ct3-d8-f%d" % ft, w, h, 3, 8, rows8, pal8, filters=ft)

    # an RGB tRNS: one see-through colour, matched per pixel
    trnsrgb = b"\x00\x00\x2a\x00\x00\x2a"          # (42, 42, 0)
    rows = [[((x * 17 + y * 23) & 0x7F,
              (x * 17 + y * 23) & 0x7F,
              0 if (x + y) % 3 == 0 else 200)
             for x in range(w)] for y in range(h)]
    add("ct2-trns", w, h, 2, 8, rows, trns=trnsrgb)
    add("ct2-trns-f3", w, h, 2, 8, rows, trns=trnsrgb, filters=3)

    # deflate shape: stored, fixed, dynamic, split IDATs, every strategy
    for level, tagname in ((0, "stored"), (1, "fast"), (9, "best")):
        add("%s-ct2" % tagname, w, h, 2, 8, noise(w, h, 3), level=level)
        add("%s-ct6" % tagname, w, h, 6, 8, grad(w, h, 4), level=level)
    for strat, tagname in ((zlib.Z_FILTERED, "filtered"), (zlib.Z_RLE, "rle"),
                           (zlib.Z_FIXED, "fixed"), (zlib.Z_HUFFMAN_ONLY,
                                                    "huffonly")):
        add("%s-ct2" % tagname, w, h, 2, 8, grad(w, h, 3), strategy=strat)
    for n in (2, 3, 5):
        add("split%d-ct2" % n, w, h, 2, 8, grad(w, h, 3), split=n)
        add("split%d-ct6" % n, w, h, 6, 8, noise(w, h, 4), split=n)

    # shapes
    add("odd-wide", 1, 1, 2, 8, [[(1, 2, 3)]])
    add("one-col", 1, 5, 2, 8, [[(x, x * 3 & 0xFF, 9)] for x in range(5)])
    add("one-row", 17, 1, 6, 8, [[(x, x + 1, x + 2, 255) for x in range(17)]])
    add("tall", 3, 40, 2, 8,
        [[((x * 9 + y * 5) & 0xFF, (x * 3) & 0xFF, (y * 7) & 0xFF)
          for x in range(3)] for y in range(40)])
    add("big", 64, 48, 2, 8, blocks(64, 48, 3))
    add("big-ct6", 40, 33, 6, 8, noise(40, 33, 4))
    # a row that has to straddle the 16 KB window boundary
    add("window-edge", 300, 120, 2, 8, blocks(300, 120, 3))
    add("window-edge-ct6", 200, 90, 6, 8, noise(200, 90, 4))
    # and one where a match runs over the boundary, which is what makes the
    # hand-over length matter
    for w, hh in ((100, 60), (300, 60), (64, 120)):
        for lvl in (1, 6, 9):
            add("match-%dx%d-l%d" % (w, hh, lvl), w, hh, 2, 8,
                blocks(w, hh, 3), level=lvl)
            add("match-%dx%d-l%d-ct0" % (w, hh, lvl), w, hh, 0, 8,
                blocks(w, hh, 1), level=lvl)

    # ancillary chunks the decoder has to carry past without tripping over
    add("chunks-ct2", w, h, 2, 8, grad(w, h, 3), extra=[
        (b"gAMA", struct.pack(">I", 45455)),
        (b"pHYs", struct.pack(">IIB", 2835, 2835, 1)),
        (b"tEXt", b"Comment\0hello"),
        (b"tIME", struct.pack(">HBBBBB", 2011, 7, 2, 3, 4, 5)),
    ])
    add("chunks-ct3", w, h, 3, 8, grad(w, h, 1), pal=pal8_early(),
        trns=[0xFF] * 256, extra=[
            (b"gAMA", struct.pack(">I", 100000)),
        ])
    # bKGD, which the decoder turns into three channel globals
    add("bkgd-ct2", w, h, 2, 8, grad(w, h, 3), extra=[
        (b"bKGD", b"\x10\x20\x30\x40\x50\x60"),
    ])

    return cases


# ------------------------------------------------------------------ running


def dump(src):
    """Run the decoder probe; return (width, height, rows of RGBA tuples)."""
    r = subprocess.run([DUMPDEC, src], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    if r.returncode != 0 or len(r.stdout) < 8:
        raise RuntimeError(r.stderr.decode("latin1").strip()[:70] or "no output")
    w, h = struct.unpack("<II", r.stdout[:8])
    rows = []
    for y in range(h):
        row = []
        for x in range(w):
            p = struct.unpack_from("<I", r.stdout, 8 + 4 * (y * w + x))[0]
            row.append((p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF,
                        (p >> 24) & 0xFF))
        rows.append(tuple(row))
    return w, h, rows


def run(case):
    name, w, h, ct, depth, rows, palette, trns, opts = case
    src = os.path.join(BUILD, "dt_%s.png" % name)
    write_png(src, w, h, ct, depth, rows, palette, trns, **opts)
    try:
        gw, gh, got = dump(src)
    except Exception as exc:                       # noqa: BLE001
        return "%-14s %s" % (name, exc)
    if (gw, gh) != (w, h):
        return "%-14s wrong size %dx%d" % (name, gw, gh)
    want = expect(ct, depth, rows, palette, trns)
    bad = 0
    first = None
    for y in range(h):
        for x in range(w):
            if got[y][x] != want[y][x]:
                bad += 1
                if first is None:
                    first = "x=%d y=%d got %s want %s" % (
                        x, y, got[y][x], want[y][x])
    if bad:
        return "%-14s %d/%d pixels differ, first %s" % (name, bad, w * h,
                                                        first)
    return "%-14s  ok  (%dx%d ct%d d%d)" % (name, w, h, ct, depth)


def alpha_ramp(w, h, chans):
    """Opaque rows with one see-through row: needs colour type 6 or 4."""
    rows = dense(w, h, chans)
    if chans == 4:
        for y in range(h):
            if y % 3 == 1:
                rows[y] = [px[:3] + (0x80,) for px in rows[y]]
    return rows


def clear_bg(w, h, chans):
    """A fully transparent background with opaque content on top."""
    rows = dense(w, h, chans)
    for y in range(h):
        row = []
        for x in range(w):
            if x < w // 3:
                row.append((0, 0, 0, 0) if chans == 4 else (0, 0, 0))
            else:
                row.append(rows[y][x])
        rows[y] = row
    return rows


def roundtrip_cases():
    """The same corpus again, but through pngout.exe rather than the probe."""
    w, h = 13, 9
    out = []
    for ct, chans in ((0, 1), (2, 3), (4, 2), (6, 4)):
        out.append(("e-dense-ct%d" % ct, w, h, ct, 8, dense(w, h, chans)))
    for ft in range(6):
        out.append(("e-f%d-ct2" % ft, w, h, 2, 8, dense(w, h, 3),
                    None, None, {"filters": ft}))
    for ct, chans in ((0, 1), (2, 3), (6, 4)):
        out.append(("e-blocks-ct%d" % ct, 40, 40, ct, 8, blocks(40, 40, chans)))
        out.append(("e-noise-ct%d" % ct, 24, 24, ct, 8, noise(24, 24, chans)))
    # alpha: the decision to keep it is a separate path from the decode
    for name, maker in (("alpha", alpha_ramp), ("clear", clear_bg)):
        out.append(("e-%s-ct6" % name, 24, 24, 6, 8, maker(24, 24, 4)))
        out.append(("e-%s-ct2" % name, 24, 24, 2, 8, maker(24, 24, 3)))
    return out


def dense(w, h, chans):
    """An image with a lot of repeated rows, so PNGOUT really does beat it."""
    base = [[tuple(((x // 2) * 61 + c * 29) & 0xFF for c in range(chans))
              for x in range(w)] for _y in range(4)]
    return [list(base[y % 4]) for y in range(h)]


def main():
    if "--roundtrip" in sys.argv:
        cases = roundtrip_cases()
        mode = roundtrip
        argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    else:
        cases = make_cases()
        mode = run
        argv = sys.argv[1:]
    if len(argv) > 1:
        want = set()
        for a in argv[1:]:
            want.update(int(x) for x in a.split(","))
        cases = [cases[i - 1] for i in sorted(want)]
    fails = 0
    for c in cases:
        line = mode(c)
        if "  ok  " not in line:
            fails += 1
            print(line)
    print("%d/%d cases pass" % (len(cases) - fails, len(cases)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())