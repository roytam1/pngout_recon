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
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

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
    # type 4 with a real varying alpha, which is what pins the internal
    # layout: grey in bytes 0..2 and alpha in byte 3
    add("alpha-ct4", 24, 24, 4, 8, alpha_ramp(24, 24, 2))
    add("grey-ct4", 20, 20, 4, 8, grey_few(20, 20))
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


def write_pal8(path, w, h, indices, palette):
    """
    Write the 8bpp palettised format PNGOUT also reads, format code 4.

    Header, from sub_409260 and loc_40D150: `19 91`, the width and height as
    little endian words, byte +10 = 8 and byte +11 = 0, then a 256 entry RGB
    palette at +0x20 and width*height index bytes after that.  The palette is
    padded out to the full 256 entries because loc_40D150 always reads 256.
    """
    out = bytearray(b"\x19\x91")
    out += struct.pack("<HH", w, h)
    out += b"\x00" * (0x20 - len(out))
    out[10] = 8
    for (r, g, b) in palette:
        out += bytes([r & 0xFF, g & 0xFF, b & 0xFF])
    for i in range(len(palette), 256):
        out += b"\x00\x00\x00"
    assert len(out) == 0x20 + 768, len(out)
    out += bytes(indices[:w * h])
    with open(path, "wb") as fh:
        fh.write(bytes(out))
    return path


def tga_pack(px, depth):
    """One Targa pixel, in the file's own byte order: B, G, R, A."""
    r, g, b, a = px
    if depth == 32:
        return bytes([b, g, r, a])
    if depth == 16:
        word = ((0x8000 if a >= 128 else 0) | ((r >> 3) << 10) |
                ((g >> 3) << 5) | (b >> 3))
        return struct.pack("<H", word)
    if depth == 24:
        return bytes([b, g, r])
    return bytes([r])


def tga_rle(seq, depth):
    """RLE packets, the way sub_40D250 reads them: run first, then raw."""
    out = bytearray()
    i, n = 0, len(seq)
    while i < n:
        v = seq[i]
        j = i
        while j < n and seq[j] == v and j - i < 128:
            j += 1
        if j - i >= 2:
            out.append(0x80 | (j - i - 1))
            out += tga_pack(v, depth)
            i = j
        else:
            raw = []
            while i < n and len(raw) < 128:
                if i + 1 < n and seq[i] == seq[i + 1]:
                    break
                raw.append(seq[i])
                i += 1
            out.append(len(raw) - 1)
            for c in raw:
                out += tga_pack(c, depth)
    return bytes(out)


def write_targa(path, w, h, pixels, depth=24, imgtype=2, desc=0x20,
                palette=None):
    """
    Write a Targa, format code 8.

    There is no magic, so the header has to be self consistent for
    GetImageSize() - and therefore DecodeImage() - to accept it: colour map type
    0 or 1 at +1, an image type below 12 at +2 with one of bits 1, 2, 3, 11, 15
    of `1 << type` set, a pixel depth that is a multiple of 8 and no more than 32
    at +10, and a descriptor at +11 whose top two bits are clear.

    Bits 4 and 5 of the descriptor are the origin and both are allowed, so the
    writer has to emit rows and columns in the order the decoder will walk them:
    bit 5 set means the origin is at the bottom, bit 4 set at the right.
    """
    hdr = bytearray(18)
    hdr[1] = 1 if palette else 0
    hdr[2] = imgtype
    if palette:
        struct.pack_into("<HHB", hdr, 3, 0, len(palette), depth)
    struct.pack_into("<HHHH", hdr, 8, 0, 0, w, h)
    hdr[16] = depth
    hdr[17] = desc

    body = bytearray()
    if palette:
        for r, g, b, _a in palette:
            body += bytes([b, g, r])

    ys = range(h - 1, -1, -1) if (desc & 0x20) else range(h)
    xs = range(w - 1, -1, -1) if (desc & 0x10) else range(w)
    if imgtype >= 8:
        for y in ys:
            body += tga_rle([pixels[y][x] for x in xs], depth)
    else:
        for y in ys:
            for x in xs:
                body += tga_pack(pixels[y][x], depth)

    with open(path, "wb") as fh:
        fh.write(bytes(hdr) + bytes(body))
    return path


def tga_expected(px, depth):
    """
    What sub_40D250 makes of one pixel, as an internal 0xAARRGGBB dword read
    B, G, R, A from the bottom up.

    Sixteen bits is the interesting one.  The listing masks *in place* - bits
    10-14 for R, 5-9 for G, 0-4 for B - and the three multiplies and the `shl`
    then bring them down to bytes 2, 1, 0 as `v << 3`.  So the channels are
    truncated rather than scaled: 31 comes out as 0xF8, not 0xFF.  Bit 15, the
    alpha bit, is ignored, because the top byte comes from the borrow out of
    `- 0x40000` and is 0xFF whatever bit 15 says.
    """
    r, g, b, a = px
    if depth == 32:
        return (b, g, r, a)
    if depth == 24:
        return (b, g, r, 255)
    if depth == 16:
        word = ((0x8000 if a >= 128 else 0) | ((r >> 3) << 10) |
                ((g >> 3) << 5) | (b >> 3))
        v = ((word & 0x001F) +
             ((word & 0x03E0) + (word & 0x7C00) * 8 - 0x40000) * 8) << 3
        v &= 0xFFFFFFFF
        return (v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF)
    return (r, r, r, 255)          # the ramp 0x40D340 builds in place


def run_targa(case):
    """The same comparison as run(), for Targa."""
    name, w, h, pixels, depth, imgtype, desc, palette = case
    src = os.path.join(BUILD, "dt_%s.tga" % name)
    write_targa(src, w, h, pixels, depth, imgtype, desc, palette)
    try:
        gw, gh, got = dump(src)
    except Exception as exc:                       # noqa: BLE001
        return "%-14s %s" % (name, exc)
    if (gw, gh) != (w, h):
        return "%-14s wrong size %dx%d" % (name, gw, gh)
    bad = 0
    first = None
    for y in range(h):
        for x in range(w):
            px = tga_expected(pixels[y][x], depth)
            if got[y][x] != px:
                bad += 1
                if first is None:
                    first = "x=%d y=%d got %s want %s" % (x, y, got[y][x], px)
    if bad:
        return "%-14s %d/%d pixels differ, first %s" % (name, bad, w * h, first)
    return "%-14s  ok  (%dx%d tga d%d)" % (name, w, h, depth)


def roundtrip_targa(case):
    """The same file through pngout.exe and back out of the PNG it writes."""
    name, w, h, pixels, depth, imgtype, desc, palette = case
    src = os.path.join(BUILD, "dt_%s.tga" % name)
    dst = os.path.join(BUILD, "dt_%s.tga.out.png" % name)
    write_targa(src, w, h, pixels, depth, imgtype, desc, palette)
    if os.path.exists(dst):
        os.remove(dst)
    r = subprocess.run([PNGOUT, "/force", src, dst], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    if not os.path.exists(dst):
        out = (r.stdout + r.stderr).decode("latin1")
        return "%-14s no output: %s" % (name, out.strip()[-70:])
    try:
        got = read_pixels(dst)
    except Exception as exc:                       # noqa: BLE001
        return "%-14s output unreadable: %s" % (name, exc)
    if (got["width"], got["height"]) != (w, h):
        return "%-14s wrong size %dx%d" % (name, got["width"], got["height"])
    bad = 0
    first = None
    for y in range(h):
        for x in range(w):
            r_, g_, b_, a_ = pixels[y][x]
            if depth == 8:
                # the decoder publishes it as greyscale, so what comes back is
                # the one channel in all three, not the RGB triple I wrote
                px = (r_, r_, r_, 255)
            else:
                px = (r_, g_, b_, a_ if depth == 32 else 255)
            if tuple(got["rows"][y][x]) != px:
                bad += 1
                if first is None:
                    first = "x=%d y=%d got %s want %s" % (
                        x, y, got["rows"][y][x], px)
    if bad:
        return "%-14s %d/%d pixels differ, first %s" % (name, bad, w * h, first)
    return "%-14s  ok  (%dx%d tga d%d round trip)" % (name, w, h, depth)


def rle_pcx(seq):
    """PCX run length encoding: a byte >= 0xC0 is `count = b & 0x3F` repeats."""
    out = bytearray()
    i, n = 0, len(seq)
    while i < n:
        v = seq[i]
        j = i
        while j < n and seq[j] == v and j - i < 63:
            j += 1
        run = j - i
        if run >= 2 or v >= 0xC0:
            out.append(0xC0 | run)
            out.append(v)
        else:
            out.append(v)
        i = j
    return bytes(out)


def write_pcx(path, w, h, pixels, nplanes=3, bytesperline=None):
    """
    Write a three plane 8bpp PCX, format code 6.

    The header is recognised by its first four bytes as one dword, 0x0801050A -
    0A 05 01 08 - and the image size comes from the window bounds at +4..+0B.
    The layout sub_40DAC0 reads is the standard one: reserved byte at 0x40,
    nplanes at 0x41, bytesPerLine at 0x42, paletteInfo at 0x44, data at 0x80.

    All three planes of one row are adjacent - R then G then B - because that is
    what 0x40DD01 assumes: the plane counter goes 2, 1, 0 and only wraps to the
    next row when it falls below zero.
    """
    if bytesperline is None:
        bytesperline = w
    hdr = bytearray(0x80)
    hdr[0:4] = bytes([0x0A, 0x05, 0x01, 0x08])
    struct.pack_into("<hhhh", hdr, 4, 0, 0, w - 1, h - 1)
    struct.pack_into("<HH", hdr, 0x0C, 72, 72)
    hdr[0x41] = nplanes
    struct.pack_into("<H", hdr, 0x42, bytesperline)
    struct.pack_into("<H", hdr, 0x44, 1)
    body = bytearray()
    for y in range(h):
        for plane in (2, 1, 0):
            row = [pixels[y][x][2 - plane] for x in range(w)]
            row += [0] * (bytesperline - w)
            body += rle_pcx(row)
    with open(path, "wb") as fh:
        fh.write(bytes(hdr) + bytes(body))
    return path


def run_pcx(case):
    """
    The same comparison as run(), for a three plane PCX.

    A three plane file has no alpha, so loc_40DBF0 fills byte 3 of every pixel in
    the clipped rectangle with 0xFF and the pixel comes out (B, G, R, FF) in
    internal order - the planes go 2, 1, 0, and the plane number is the byte
    offset within the pixel.
    """
    name, w, h, pixels, bytesperline = case
    src = os.path.join(BUILD, "dt_%s.pcx" % name)
    write_pcx(src, w, h, pixels, bytesperline=bytesperline)
    try:
        gw, gh, got = dump(src)
    except Exception as exc:                       # noqa: BLE001
        return "%-14s %s" % (name, exc)
    if (gw, gh) != (w, h):
        return "%-14s wrong size %dx%d" % (name, gw, gh)
    bad = 0
    first = None
    for y in range(h):
        for x in range(w):
            r, g, b = pixels[y][x]
            px = (b, g, r, 255)
            if got[y][x] != px:
                bad += 1
                if first is None:
                    first = "x=%d y=%d got %s want %s" % (x, y, got[y][x], px)
    if bad:
        return "%-14s %d/%d pixels differ, first %s" % (name, bad, w * h, first)
    return "%-14s  ok  (%dx%d pcx3)" % (name, w, h)


def roundtrip_pcx(case):
    """The same file through pngout.exe and back out of the PNG it writes."""
    name, w, h, pixels, bytesperline = case
    src = os.path.join(BUILD, "dt_%s.pcx" % name)
    dst = os.path.join(BUILD, "dt_%s.pcx.out.png" % name)
    write_pcx(src, w, h, pixels, bytesperline=bytesperline)
    if os.path.exists(dst):
        os.remove(dst)
    r = subprocess.run([PNGOUT, "/force", src, dst], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    if not os.path.exists(dst):
        out = (r.stdout + r.stderr).decode("latin1")
        return "%-14s no output: %s" % (name, out.strip()[-70:])
    try:
        got = read_pixels(dst)
    except Exception as exc:                       # noqa: BLE001
        return "%-14s output unreadable: %s" % (name, exc)
    if (got["width"], got["height"]) != (w, h):
        return "%-14s wrong size %dx%d" % (name, got["width"], got["height"])
    bad = 0
    first = None
    for y in range(h):
        for x in range(w):
            px = pixels[y][x] + (255,)
            if tuple(got["rows"][y][x]) != px:
                bad += 1
                if first is None:
                    first = "x=%d y=%d got %s want %s" % (
                        x, y, got["rows"][y][x], px)
    if bad:
        return "%-14s %d/%d pixels differ, first %s" % (name, bad, w * h, first)
    return "%-14s  ok  (%dx%d pcx3 round trip)" % (name, w, h)


def pal8_expected(palette):
    """
    What loc_40D150 makes of one palette entry, as an internal 0xAARRGGBB dword.

    `sub edx, 40h` / `shl edx, 8` / `add edx, ebx` / `shl edx, 8` / `add edx, ebx`
    / `shl edx, 2`, all in a signed 32 bit register, so a first channel below
    64 borrows from the top byte - which is where the alpha ends up, and there
    is no separate alpha to set.  Python ints are not 32 bit, so the wrap has to
    be done here or the comparison is meaningless.
    """
    out = []
    for (r, g, b) in palette:
        v = (((((r - 0x40) << 8) + g) << 8) + b) << 2
        v &= 0xFFFFFFFF
        out.append((v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF))
    return out


def run_pal8(case):
    """
    The same comparison as run(), for the 8bpp palettised format.  Only the
    decoder is involved: the pixels are compared against what the palette
    transform says they should be, so a failure here is loc_40D150's.
    """
    name, w, h, indices, palette = case
    src = os.path.join(BUILD, "dt_%s.pal8" % name)
    write_pal8(src, w, h, indices, palette)
    try:
        gw, gh, got = dump(src)
    except Exception as exc:                       # noqa: BLE001
        return "%-14s %s" % (name, exc)
    if (gw, gh) != (w, h):
        return "%-14s wrong size %dx%d" % (name, gw, gh)
    want = pal8_expected(palette)
    bad = 0
    first = None
    for y in range(h):
        for x in range(w):
            px = want[indices[y * w + x]]
            if got[y][x] != px:
                bad += 1
                if first is None:
                    first = "x=%d y=%d got %s want %s" % (
                        x, y, got[y][x], px)
    if bad:
        return "%-14s %d/%d pixels differ, first %s" % (name, bad, w * h,
                                                        first)
    return "%-14s  ok  (%dx%d pal8)" % (name, w, h)


def roundtrip_pal8(case):
    """The same file through pngout.exe and back out of the PNG it writes."""
    name, w, h, indices, palette = case
    src = os.path.join(BUILD, "dt_%s.pal8" % name)
    dst = os.path.join(BUILD, "dt_%s.pal8.out.png" % name)
    want = pal8_expected(palette)
    if os.path.exists(dst):
        os.remove(dst)
    r = subprocess.run([PNGOUT, "/force", src, dst], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    if not os.path.exists(dst):
        out = (r.stdout + r.stderr).decode("latin1")
        return "%-14s no output: %s" % (name, out.strip()[-70:])
    try:
        got = read_pixels(dst)
    except Exception as exc:                       # noqa: BLE001
        return "%-14s output unreadable: %s" % (name, exc)
    if (got["width"], got["height"]) != (w, h):
        return "%-14s wrong size %dx%d" % (name, got["width"], got["height"])
    bad = 0
    first = None
    for y in range(h):
        for x in range(w):
            b, g, r, a = want[indices[y * w + x]]
            # the internal pixel is one dword read B, G, R, A from the bottom up,
            # so the RGB triple a PNG file carries is that order reversed
            px = (r, g, b, a)
            if tuple(got["rows"][y][x]) != px:
                bad += 1
                if first is None:
                    first = "x=%d y=%d got %s want %s" % (
                        x, y, got["rows"][y][x], px)
    if bad:
        return "%-14s %d/%d pixels differ, first %s" % (name, bad, w * h,
                                                        first)
    return "%-14s  ok  (%dx%d pal8 round trip)" % (name, w, h)


def alpha_ramp(w, h, chans):
    """Opaque rows with one see-through row: needs colour type 6 or 4."""
    rows = dense(w, h, chans)
    if chans == 4:
        for y in range(h):
            if y % 3 == 1:
                rows[y] = [px[:3] + (0x80,) for px in rows[y]]
    elif chans == 2:
        for y in range(h):
            if y % 3 == 1:
                rows[y] = [px[:1] + (0x80,) for px in rows[y]]
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


def grey_few(w, h):
    """Few distinct greys with a varying alpha: type 4, but paletteable.

    SuggestColType() sends flags 1 (alpha wanted, <= 256 colours) to a palette
    and only flags 1|4 to type 4, so this is the shape of type 4 input that
    survives a round trip. See README.md: sub_407B00 emits bytes 0 and 1, which
    are both the grey, so type 4 *output* from PNGOUT drops the alpha.
    """
    return [[(((x + y) % 4) * 60, 0xFF if (x + y) % 2 else 0x40)
             for x in range(w)] for y in range(h)]


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
    out.append(("e-grey-ct4", 20, 20, 4, 8, grey_few(20, 20)))
    return out


def dense(w, h, chans):
    """An image with a lot of repeated rows, so PNGOUT really does beat it."""
    base = [[tuple(((x // 2) * 61 + c * 29) & 0xFF for c in range(chans))
              for x in range(w)] for _y in range(4)]
    return [list(base[y % 4]) for y in range(h)]


def pal8_palette():
    """
    A palette that exercises the transform's edges.

    loc_40D150 subtracts 64 from the first channel and scales the whole entry by
    four, in a signed 32 bit register, with no clamp - so a first channel below
    64 borrows out of the top byte and that byte is the alpha.  Entries 0, 2 and
    3 here are exactly that case, and the *expected* values have to be computed
    the same way (see pal8_expected) rather than as plain RGB.
    """
    named = [(0, 0, 0), (255, 0, 0), (0, 255, 0), (0, 0, 255), (10, 20, 30),
             (200, 100, 50), (7, 7, 7), (250, 250, 250), (64, 128, 192),
             (128, 64, 32), (1, 2, 3), (191, 223, 255)]
    grey = [(i, i, i) for i in range(256 - len(named))]
    return named + grey


def pal8_cases():
    """The 8bpp palettised format: the decode, and then the round trip."""
    pal = pal8_palette()
    n = len(pal)
    out = []
    w, h = 13, 9
    # a smooth index pattern, so neighbouring pixels share entries and PNGOUT has
    # something to compress
    out.append(("p-ramp", w, h, [(x + y) % n for y in range(h) for x in range(w)],
                pal))
    # only the named entries, to keep the palette small and the image flat
    few = pal[:12]
    out.append(("p-named", 20, 20, [(x * 5 + y * 7) % 12
                                    for y in range(20) for x in range(20)],
                few))
    # one entry everywhere: the degenerate but legal case
    out.append(("p-flat", 9, 7, [3] * (9 * 7), few))
    return out


def pcx_cases():
    """Three plane PCX: the decode, and then the round trip."""
    out = []
    w, h = 13, 9
    out.append(("x-grad", w, h, [[((x * 29 + y * 7) & 0xFF,
                                   (x * 11 + y * 53) & 0xFF,
                                   (x * 3 + y * 97) & 0xFF)
                                  for x in range(w)] for y in range(h)], None))
    # long runs, so the RLE escapes are actually used
    out.append(("x-runs", w, h, [[(17, 200, 3)] * w for _y in range(h)], None))
    # bytesPerLine wider than the image: the scanline limit is bytesPerLine when
    # that is the larger of the two, which is the odd comparison at 0x40DC36
    out.append(("x-bpl24", w, h, [[((x * 41) & 0xFF, (y * 67) & 0xFF,
                                    ((x + y) * 23) & 0xFF)
                                   for x in range(w)] for y in range(h)], 24))
    # something with more colours than a 256 entry palette holds, so the
    # round trip cannot pick one
    out.append(("x-many", 24, 18, [[((x * 5) & 0xFF, (y * 7) & 0xFF,
                                     ((x * y) // 3) & 0xFF)
                                    for x in range(24)] for y in range(18)],
                None))
    return out


def bmp_palette(pixels, w, h, bpp):
    """
    The palette the writer builds for a palettised image, and the same table the
    checker compares against.

    sub_40D560 reads the entries as BGR(A) and forces the alpha opaque
    (`entry | 0xFF000000`), so the table is stored in PNG's RGB order here and
    the checker reverses it again - the same reversal every other case in this
    script needs, because the internal pixel is one dword read B, G, R, A from
    the bottom up.
    """
    n = 1 << bpp
    used = sorted(set(pixels[y][x][0] & (n - 1) for y in range(h)
                      for x in range(w)))
    out = []
    for i in range(n):
        v = used[i % len(used)]
        if bpp > 1:
            out.append(((v * 16) & 0xFF, (v * 8) & 0xFF, (v * 4) & 0xFF))
        else:
            out.append((v * 255, v * 255, v * 255))
    return out


def write_bmp(path, w, h, pixels, bpp=24, core=False, compression=0):
    """
    Write a BMP, format code 5.

    Both header shapes the original accepts: BITMAPCOREHEADER (biSize 12, a 3
    byte palette entry, width and height words) and BITMAPINFOHEADER (biSize 40,
    a 4 byte entry, everything a dword).  The palette and, for BI_BITFIELDS, the
    three channel masks sit right after the header, which is where
    `lea eax, [esi+ebp+0Eh]` looks - *after* the 14 byte file header, not at
    `fileimg + biSize`, and getting that wrong is invisible until a palettised
    image comes out with every entry shifted by three and a half.

    Rows are stored bottom up unless the height is negative, and padded to four
    bytes, so the writer emits them in the order sub_40D560 walks them.
    """
    stride = ((w * bpp + 31) >> 3) & ~3
    pal = bmp_palette(pixels, w, h, bpp) if bpp <= 8 else []

    rows = []
    for y in range(h - 1, -1, -1):
        r = bytearray()
        if bpp == 24:
            for x in range(w):
                r += bytes([pixels[y][x][2], pixels[y][x][1], pixels[y][x][0]])
        elif bpp == 32:
            for x in range(w):
                px = pixels[y][x]
                r += bytes([px[2], px[1], px[0], px[3]])
        elif bpp == 16:
            for x in range(w):
                pr, pg, pb, _pa = pixels[y][x]
                r += struct.pack("<H", ((pr >> 3) << 10) | ((pg >> 3) << 5) |
                                    (pb >> 3))
        elif bpp == 8:
            for x in range(w):
                r.append(pixels[y][x][0] & 0xFF)
        elif bpp == 4:
            for x in range(0, w, 2):
                hi = pixels[y][x][0] & 0xF
                lo = (pixels[y][x + 1][0] & 0xF) if x + 1 < w else 0
                r.append((hi << 4) | lo)
        else:
            for bytei in range((w + 7) // 8):
                b = 0
                for bit in range(8):
                    x = bytei * 8 + bit
                    if x < w and pixels[y][x][0] & 1:
                        b |= 0x80 >> bit
                r.append(b)
        r += bytes(stride - len(r))
        rows.append(bytes(r))

    palbytes = bytearray()
    for pr, pg, pb in pal:
        palbytes += bytes([pb, pg, pr]) if core else bytes([pb, pg, pr, 0])
    if compression == 3:
        masks = (0x7C00, 0x03E0, 0x001F) if bpp == 16 else \
                (0x00FF0000, 0x0000FF00, 0x000000FF)
        palbytes += b"".join(struct.pack("<I", m) for m in masks)

    hdrsize = 12 if core else 40
    off = 14 + hdrsize + len(palbytes)
    body = b"".join(rows)
    fh = b"BM" + struct.pack("<IHHI", off + len(body), 0, 0, off)
    if core:
        info = struct.pack("<IHHHH", 12, w, h, 1, bpp)
    else:
        info = struct.pack("<IiiHHIIiiII", 40, w, h, 1, bpp, compression,
                           len(palbytes), 2835, 2835, 0, 0)
    with open(path, "wb") as f:
        f.write(fh + info + bytes(palbytes) + body)
    return path


def bmp_expected(px, bpp, pal):
    """What sub_40D560 makes of one pixel, as an internal 0xAARRGGBB dword."""
    r, g, b, a = px
    if bpp == 32:
        return (b, g, r, a)
    if bpp == 24:
        return (b, g, r, 255)
    if bpp == 16:
        # RGB565 with no scaling at all: each channel is its 5 bits `v << 3`,
        # so R and B reach 0xF8 and G, five bits at position 6, tops out at
        # 0x78.  The alpha byte comes from the borrow out of the - 0x1000000.
        r5, g5, b5 = (r >> 3) & 31, (g >> 3) & 31, (b >> 3) & 31
        return ((b5 << 3) & 0xFF, (g5 << 3) & 0xFF, (r5 << 3) & 0xFF, 255)
    if bpp == 8:
        pr, pg, pb = pal[px[0] & 0xFF]
        return (pb, pg, pr, 255)
    if bpp == 4:
        pr, pg, pb = pal[px[0] & 0xF]
        return (pb, pg, pr, 255)
    pr, pg, pb = pal[px[0] & 1]
    return (pb, pg, pr, 255)


def run_bmp(case):
    """The same comparison as run(), for BMP."""
    name, w, h, pixels, bpp, core = case
    src = os.path.join(BUILD, "dt_%s.bmp" % name)
    write_bmp(src, w, h, pixels, bpp, core)
    pal = bmp_palette(pixels, w, h, bpp) if bpp <= 8 else []
    try:
        gw, gh, got = dump(src)
    except Exception as exc:                       # noqa: BLE001
        return "%-14s %s" % (name, exc)
    if (gw, gh) != (w, h):
        return "%-14s wrong size %dx%d" % (name, gw, gh)
    bad = 0
    first = None
    for y in range(h):
        for x in range(w):
            px = bmp_expected(pixels[y][x], bpp, pal)
            if got[y][x] != px:
                bad += 1
                if first is None:
                    first = "x=%d y=%d got %s want %s" % (x, y, got[y][x], px)
    if bad:
        return "%-14s %d/%d pixels differ, first %s" % (name, bad, w * h, first)
    return "%-14s  ok  (%dx%d bmp d%d)" % (name, w, h, bpp)


def roundtrip_bmp(case):
    """The same file through pngout.exe and back out of the PNG it writes."""
    name, w, h, pixels, bpp, core = case
    src = os.path.join(BUILD, "dt_%s.bmp" % name)
    dst = os.path.join(BUILD, "dt_%s.bmp.out.png" % name)
    write_bmp(src, w, h, pixels, bpp, core)
    pal = bmp_palette(pixels, w, h, bpp) if bpp <= 8 else []
    if os.path.exists(dst):
        os.remove(dst)
    r = subprocess.run([PNGOUT, "/force", src, dst], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    if not os.path.exists(dst):
        out = (r.stdout + r.stderr).decode("latin1")
        return "%-14s no output: %s" % (name, out.strip()[-70:])
    try:
        got = read_pixels(dst)
    except Exception as exc:                       # noqa: BLE001
        return "%-14s output unreadable: %s" % (name, exc)
    if (got["width"], got["height"]) != (w, h):
        return "%-14s wrong size %dx%d" % (name, got["width"], got["height"])
    bad = 0
    first = None
    for y in range(h):
        for x in range(w):
            px = bmp_expected(pixels[y][x], bpp, pal)
            want = (px[2], px[1], px[0], px[3])
            if tuple(got["rows"][y][x]) != want:
                bad += 1
                if first is None:
                    first = "x=%d y=%d got %s want %s" % (
                        x, y, got["rows"][y][x], want)
    if bad:
        return "%-14s %d/%d pixels differ, first %s" % (name, bad, w * h, first)
    return "%-14s  ok  (%dx%d bmp d%d round trip)" % (name, w, h, bpp)


def targa_cases():
    """Targa: every depth, both origins, RLE and raw, and a colour map."""
    w, h = 11, 6
    p24 = [[((x * 23 + y * 5) & 0xFF, (x * 13 + y * 29) & 0xFF,
             (x * 7 + y * 41) & 0xFF, 255) for x in range(w)] for y in range(h)]
    p32 = [[((x * 7) & 0xFF, (y * 11) & 0xFF, ((x + y) * 5) & 0xFF,
             (x * 16 + 8) & 0xFF) for x in range(w)] for y in range(h)]
    # 5 bit channels, so the `v << 3` truncation is visible
    p16 = [[((x * 24) & 0xF8, ((x * 13 + y * 7) & 0xF8), ((y * 41) & 0xF8), 255)
            for x in range(w)] for y in range(h)]
    flat = [[(9, 9, 9, 255)] * w for _y in range(h)]
    p8 = [[(x + y * 3, 0, 0, 255) for x in range(w)] for y in range(h)]
    out = [
        ("t-rgb24-bl", w, h, p24, 24, 2, 0x20, None),
        ("t-rgb24-tl", w, h, p24, 24, 2, 0x00, None),
        ("t-rgb24-br", w, h, p24, 24, 2, 0x30, None),
        ("t-rgb24-tr", w, h, p24, 24, 2, 0x10, None),
        ("t-rgba32", w, h, p32, 32, 2, 0x20, None),
        ("t-rgba32-tl", w, h, p32, 32, 2, 0x00, None),
        ("t-a1r5g5b5", w, h, p16, 16, 2, 0x20, None),
        ("t-gray8", w, h, p8, 8, 1, 0x20, None),
        ("t-gray8-rle", w, h, p8, 8, 9, 0x20, None),
        ("t-rle24", w, h, p24, 24, 10, 0x20, None),
        ("t-rle24-flat", w, h, flat, 24, 10, 0x00, None),
        ("t-rle32", w, h, p32, 32, 10, 0x20, None),
    ]
    return out


def bmp_cases():
    """BMP: both header shapes, every depth the original decodes."""
    w, h = 9, 5
    p24 = [[((x * 23 + y * 11) & 0xFF, (x * 17 + y * 41) & 0xFF,
             (x * 7 + y * 61) & 0xFF, 255) for x in range(w)] for y in range(h)]
    p32 = [[((x * 23 + y * 11) & 0xFF, (x * 17 + y * 41) & 0xFF,
             (x * 7 + y * 61) & 0xFF, (x * 40 + 16) & 0xFF)
            for x in range(w)] for y in range(h)]
    # 5 bit channels, so the `v << 3` truncation is visible
    p16 = [[((x * 32) & 0xF8, ((x * 8 + y * 8) & 0xF8), ((y * 40) & 0xF8), 255)
            for x in range(w)] for y in range(h)]
    p8 = [[((x + y) % 16, 0, 0, 255) for x in range(w)] for y in range(h)]
    return [
        ("b-rgb24", w, h, p24, 24, False),
        ("b-rgb24-core", w, h, p24, 24, True),
        ("b-rgba32", w, h, p32, 32, False),
        ("b-rgb16", w, h, p16, 16, False),
        ("b-pal8", w, h, p8, 8, False),
        ("b-pal8-core", w, h, p8, 8, True),
    ]


# ------------------------------------------------------------------ GIF
class GifBits(object):
    """LSB-first bit packer, the order GIF's codes arrive in."""

    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def put(self, code, width):
        self.acc |= (code & ((1 << width) - 1)) << self.n
        self.n += width
        while self.n >= 8:
            self.out.append(self.acc & 0xFF)
            self.acc >>= 8
            self.n -= 8

    def flush(self):
        if self.n:
            self.out.append(self.acc & 0xFF)
            self.acc = 0
            self.n = 0
        return bytes(self.out)


def gif_lzw(indices, mincode):
    """
    A real GIF LZW encoder, so the tests exercise the code table and not just a
    stream of literals.  The size bump matches sub_40CA70's: the code size goes
    up when the *next free slot* reaches `1 << bits`, checked before the slot is
    added, so codes 0-7 go out in three bits and code 8 in four.
    """
    clear = 1 << mincode
    end = clear + 1
    bits = mincode + 1
    nxt = end + 1
    table = {}
    for i in range(clear):
        table[bytes([i])] = i
    w = GifBits()
    w.put(clear, bits)
    prefix = b""
    for c in indices:
        b = bytes([c])
        if prefix + b in table:
            prefix += b
            continue
        w.put(table[prefix], bits)
        if nxt < 4096:
            table[prefix + b] = nxt
            # the decoder's table runs one entry behind, so it reaches 2**bits
            # one code later; this is the pairing that keeps the two in step
            if nxt + 1 == (1 << bits) and bits < 12:
                bits += 1
            nxt += 1
        else:
            w.put(clear, bits)
            table = {}
            for i in range(clear):
                table[bytes([i])] = i
            bits = mincode + 1
            nxt = end + 1
        prefix = b
    if prefix:
        w.put(table[prefix], bits)
    w.put(end, bits)
    return w.flush()


def gif_subblocks(data):
    out = bytearray()
    i = 0
    while i < len(data):
        n = min(255, len(data) - i)
        out.append(n)
        out += data[i:i + n]
        i += n
    out.append(0)
    return bytes(out)


def write_gif(path, w, h, indices, palette, left=0, top=0, screenw=None,
              screenh=None, gct=True, lct=False, transp=None,
              fake_interlace=False, bgindex=0, comment=False):
    """
    Write a GIF, format code 3.

    `screenw`/`screenh` are the logical screen, and when they are left out they
    default to the image size, which is the case sub_40CA70 takes the 0x40CC1D
    fast path through.

    `lct` puts a local colour table in the image descriptor *and* sets bit 6,
    because the original reads bit 6 as the interlace flag; `fake_interlace`
    sets only bit 6.  Both produce the four-pass walk rather than GIF's own
    interlace, which is the point of having them.
    """
    if screenw is None:
        screenw = w
    if screenh is None:
        screenh = h
    out = bytearray(b"GIF89a")
    out += struct.pack("<HH", screenw, screenh)
    gsize = 1
    while gsize < len(palette):
        gsize <<= 1
    if gct:
        out += bytes([0x80 | ((gsize.bit_length() - 2) & 7), bgindex, 0])
        for i in range(gsize):
            e = palette[i] if i < len(palette) else (0, 0, 0)
            out += bytes([e[0] & 0xFF, e[1] & 0xFF, e[2] & 0xFF])
    else:
        out += bytes([0, bgindex, 0])

    if comment:
        out += b"\x21\xFE" + gif_subblocks(b"decodetest")

    if transp is not None:
        out += b"\x21\xF9\x04\x01\x00\x00" + bytes([transp]) + b"\x00"

    flags = 0
    if lct or fake_interlace:
        flags |= 0x40
    if fake_interlace and not lct:
        flags |= 0x40
    out += b"\x2C" + struct.pack("<HHHH", left, top, w, h) + bytes([flags])
    # no local colour table whatever bit 6 says, so the descriptor ends here
    mincode = max(2, (gsize.bit_length() - 1))
    out += bytes([mincode])
    out += gif_subblocks(gif_lzw(indices, mincode))
    out += b"\x3B"
    with open(path, "wb") as fh:
        fh.write(bytes(out))
    return bytes(out)


def gif_walk(w, h, top, interlace, stride, yorigin=0):
    """
    The row each *pixel* lands on, in the order sub_40CA70 walks them, so the
    cases can be checked without trusting this module.  The passes are 8, 8, 4,
    2 and a stride of 1 or 2 ends the image; bit 6 is what starts the walk.

    Each visited row contributes its whole width, because the emit loop runs
    `width` times before advancing the row by the stride.
    """
    rows = list(range(h))
    if interlace:
        rows = []
        row, stride, v55 = yorigin + top, stride, 1
        while True:
            while row < yorigin + top + h:
                rows.append(row)
                row += stride
            if stride <= 2:
                break
            if stride == 4:
                row, stride = yorigin + top + 1, 2
            elif v55:
                v55 = 0
                row, stride = yorigin + top + 4, 8
            else:
                row, stride = yorigin + top + 2, 4
    out = []
    for r in rows:
        out.extend([r] * w)
    return out


def gif_palette(pixels):
    """The colour table, in index order, from (index, r, g, b, a) pixels."""
    found = {}
    top = 0
    for row in pixels:
        for px in row:
            found[px[0]] = px[1:4]
            top = max(top, px[0])
    return [found.get(i, (0, 0, 0)) for i in range(top + 1)]


def gif_build(case):
    """Write the file and work out what sub_40CA70 should put in the frame."""
    name, w, h, pixels, screen, transp, lct, fi, comment = case
    sw, sh = screen if screen else (w, h)

    palette = gif_palette(pixels)
    flat = [px[0] for row in pixels for px in row]
    src = os.path.join(BUILD, "dt_%s.gif" % name)
    write_gif(src, w, h, flat, palette, screenw=sw, screenh=sh,
              transp=transp, lct=lct, fake_interlace=fi, comment=comment)

    # bit 6 is what starts the four-pass walk, and 8 is its first stride
    interlace = lct or fi
    order = gif_walk(w, h, 0, interlace, 8)

    # the background is logical-screen colour table entry 0
    br, bg_, bb = palette[0]
    bg = (bb, bg_, br, 255)
    want = {}
    for y in range(sh):
        for x in range(sw):
            want[(y, x)] = bg
    for k, idx in enumerate(flat):
        if k >= len(order):
            break
        r, g, b = palette[idx]
        want[(order[k], k % w)] = (b, g, r, 0 if transp == idx else 255)
    return src, sw, sh, want


def run_gif(case):
    """The same comparison as run(), for GIF."""
    name = case[0]
    try:
        src, sw, sh, want = gif_build(case)
        gw, gh, got = dump(src)
    except Exception as exc:                       # noqa: BLE001
        return "%-14s %s" % (name, exc)
    if (gw, gh) != (sw, sh):
        return "%-14s wrong size %dx%d" % (name, gw, gh)
    bad = 0
    first = None
    for y in range(sh):
        for x in range(sw):
            px = want[(y, x)]
            if got[y][x] != px:
                bad += 1
                if first is None:
                    first = "x=%d y=%d got %s want %s" % (x, y, got[y][x], px)
    if bad:
        return "%-14s %d/%d pixels differ, first %s" % (name, bad, sw * sh, first)
    return "%-14s  ok  (%dx%d gif%s)" % (
        name, sw, sh, " bit6-interlaced" if (case[6] or case[7]) else "")


def roundtrip_gif(case):
    """The same file through pngout.exe and back out of the PNG it writes."""
    name = case[0]
    src, sw, sh, want = gif_build(case)
    dst = os.path.join(BUILD, "dt_%s.gif.out.png" % name)
    if os.path.exists(dst):
        os.remove(dst)
    r = subprocess.run([PNGOUT, "/force", src, dst], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    if not os.path.exists(dst):
        out = (r.stdout + r.stderr).decode("latin1")
        return "%-14s no output: %s" % (name, out.strip()[-70:])
    try:
        got = read_pixels(dst)
    except Exception as exc:                       # noqa: BLE001
        return "%-14s output unreadable: %s" % (name, exc)
    if (got["width"], got["height"]) != (sw, sh):
        return "%-14s wrong size %dx%d" % (name, got["width"], got["height"])
    bad = 0
    first = None
    for y in range(sh):
        for x in range(sw):
            b, g, rr, a = want[(y, x)]
            wantpx = (rr, g, b, a)
            if tuple(got["rows"][y][x]) != wantpx:
                bad += 1
                if first is None:
                    first = "x=%d y=%d got %s want %s" % (
                        x, y, got["rows"][y][x], wantpx)
    if bad:
        return "%-14s %d/%d pixels differ, first %s" % (name, bad, sw * sh, first)
    return "%-14s  ok  (%dx%d gif round trip)" % (name, sw, sh)


def gif_cases():
    """
    GIF: the fast path, the LZW code table, the extension blocks,
    transparency, the background fill the fast path skips, and bit 6 read as the
    interlace flag.
    """
    w, h = 9, 5
    pal = [(i * 7 & 0xFF, i * 13 & 0xFF, i * 29 & 0xFF) for i in range(40)]
    ramp = [[((x * 3 + y * 5,) + pal[(x * 3 + y * 5) % 40] + (255,))
             for x in range(w)] for y in range(h)]
    # one long run per row, so the LZW table has something to compress
    runs = [[(0 if y % 2 else 1,) + pal[0 if y % 2 else 1] + (255,)
             for x in range(w)] for y in range(h)]
    # a repeat of the last symbol, which is what produces a code that is not in
    # the table yet - the KwKwK case
    kwkwk = [[((1 if x < w - 1 else 2,) + pal[1 if x < w - 1 else 2] + (255,))
              for x in range(w)] for y in range(h)]
    # every index used, so the table is a full 256 and the code size grows
    many = [[((x + y * w,) + pal[(x + y * w) % 40] + (255,))
             for x in range(w)] for y in range(h)]
    return [
        ("g-ramp", w, h, ramp, None, None, False, False, False),
        ("g-runs", w, h, runs, None, None, False, False, False),
        ("g-kwkwk", w, h, kwkwk, None, None, False, False, False),
        ("g-many", w, h, many, None, None, False, False, False),
        ("g-comment", w, h, ramp, None, None, False, False, True),
        ("g-transp", w, h, ramp, None, 0, False, False, False),
        ("g-transp2", w, h, ramp, None, 3, False, False, False),
        # the screen is bigger than the image, so the fill at 0x40CCAE runs
        ("g-fill", w, h, ramp, (16, 12), None, False, False, False),
        ("g-fill-tall", w, h, ramp, (16, 20), None, False, False, False),
        # bit 6 starts the four-pass walk, which is not GIF's interlace
        ("g-interlace", w, h, ramp, None, None, False, True, False),
        ("g-lct", w, h, ramp, None, None, True, False, False),
    ]


# ------------------------------------------------------------------ JPEG
def jpeg_sources(w, h):
    """
    Pictures for the JPEG cases.

    They are all smooth, and deliberately so.  A source with a sharp edge or a
    modulo wrap in it produces a large *legitimate* error - JPEG's own ringing
    at a step edge reaches 255 on a near-Nyquist pattern - so a bound measured
    against such a source says nothing about anything.  A smooth gradient comes
    back within about 13 levels at these table settings and a flat picture
    within one, and both of those are worth asserting.

    `checker` is the exception: it is kept as a smoke case only.  It must
    round-trip without raising and at the right size, and nothing more.
    """
    out = {}
    out["flat"] = [[(90, 140, 200)] * w for _ in range(h)]
    # a gentle colour gradient: the chroma ramp has to be shallow as well as
    # the luma one, because the annex K chroma table is 99 throughout, so a
    # steep one loses most of itself to quantisation before subsampling even
    # starts.  `checker` below is where the contrast goes instead.
    out["ramp"] = [[((x * 70) // (w - 1) + 90,
                     (y * 60) // (h - 1) + 100,
                     (x * 40) // (w - 1) + (y * 30) // (h - 1) + 130)
                    for x in range(w)] for y in range(h)]
    out["grey"] = [[((min(x * 255 // (w - 1) + y * 255 // (h - 1) // 2
                          + y * 3, 255),) * 3) for x in range(w)]
                   for y in range(h)]
    out["checker"] = [[((255 if (x // 3 + y // 3) % 2 else 20),
                        (255 if (x // 5 + y // 2) % 2 else 90),
                        (200 if (x // 2 + y // 5) % 2 else 40))
                       for x in range(w)] for y in range(h)]
    return out


# What each source may be expected to lose, measured with the sources above and
# given headroom.  The worst observed are 1 (flat), 33 (ramp, 4:2:2) and 19
# (grey); the extra room is for the arithmetic differing between this build of
# Python and another.
JPEG_BOUND = {"flat": 4, "ramp": 48, "grey": 32}


def jpeg_cases():
    """
    JPEG: every component layout the front end accepts, plus restart
    intervals and 16 bit quantisation tables.
    """
    w, h = 17, 9
    out = []
    for name in ("flat", "ramp", "grey", "checker"):
        for comps in ("grey", "444", "422", "420"):
            for restart in (0, 2):
                out.append((name, comps, restart, 8))
    out.append(("ramp", "444", 0, 16))
    return out


def run_jpeg(case):
    """
    sub_40B180 is not transcribed, so this cannot check pixels.  What it does
    check is the two halves that are in place: that tools/jpeglib.py writes a
    baseline JPEG which reads back to within a known error (so the corpus is
    legal and the expectations are sane), and that both binaries refuse the
    file cleanly rather than crashing or half-decoding it.
    """
    import jpeglib

    name, comps, restart, precision = case
    w, h = 17, 9
    rgb = jpeg_sources(w, h)[name]
    if comps == "grey" and name != "grey":
        rgb = [[(sum(px) // 3,) * 3 for px in row] for row in rgb]
    src = os.path.join(BUILD, "dt_j_%s_%s_%d_%d.jpg"
                       % (name, comps, restart, precision))
    try:
        jpeglib.write_jpeg(src, w, h, rgb, comps=comps, restart=restart,
                           precision=precision)
    except Exception as exc:                       # noqa: BLE001
        return "%-22s write failed: %s" % (name, exc)

    # 1. the file is a baseline JPEG that decodes back to a known picture
    try:
        gw, gh, got = jpeglib.read_jpeg(src)
    except Exception as exc:                       # noqa: BLE001
        return "%-22s not decodable: %s" % (name, exc)
    if (gw, gh) != (w, h):
        return "%-22s wrong size %dx%d" % (name, gw, gh)
    worst = 0
    for y in range(h):
        for x in range(w):
            for k in range(3):
                worst = max(worst, abs(rgb[y][x][k] - got[y][x][k]))
    # `checker` has no bound: see jpeg_sources().  The smooth ones do.
    bound = JPEG_BOUND.get(name)
    if bound is not None and worst > bound:
        return "%-22s re-read differs by %d (bound %d)" % (name, worst, bound)

    # 2. GetImageSize() must size it, and both binaries must refuse it
    #    cleanly while sub_40B180 is still missing
    r = subprocess.run([DUMPDEC, src], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    err = r.stderr.decode("latin1")
    if r.returncode == 0:
        return "%-22s dumpdec decoded it; expected a refusal" % name
    if "not reconstructed yet" not in err:
        return "%-22s dumpdec said %r" % (name, err.strip()[-60:])

    dst = os.path.join(BUILD, "dt_j_%s_%s_%d_%d.out.png"
                       % (name, comps, restart, precision))
    if os.path.exists(dst):
        os.remove(dst)
    r = subprocess.run([PNGOUT, "/force", src, dst], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    if os.path.exists(dst):
        return "%-22s pngout.exe wrote a PNG; expected a refusal" % name
    if r.returncode == 0 or "not been reconstructed" not in (
            (r.stdout + r.stderr).decode("latin1")):
        return "%-22s pngout.exe exit %d" % (name, r.returncode)

    return ("%-22s  ok  (%dx%d %s restart=%d q%d, re-read within %d%s)"
            % (name, w, h, comps, restart, precision, worst,
               "" if bound is None else " of %d" % bound))


def _im_convert():
    """
    ImageMagick's convert, or None.  It is an optional dependency of the
    suite, so a machine without it skips these rather than failing.
    """
    for p in (r"C:\ImageMagick\convert.exe",
              r"C:\ImageMagick\convert-6.9.exe"):
        if os.path.exists(p):
            return p
    return None


def im_pixels(path, conv):
    """
    The 8-bit RGB at each pixel, from ImageMagick's `txt:` output.  This is
    the suite's only third-party JPEG decoder, which is what makes --jpeg-im
    worth having: jpeglib.read_jpeg() shares the writer's assumptions, and
    ImageMagick does not.

    The leading parenthesised value is parsed, not the name that follows it:
    ImageMagick writes `black` and `white` (and other names) for exact
    primaries instead of `srgb(...)`/`gray(...)`, so matching on those misses
    every pixel that lands exactly.  The values are Q16 (this build scales
    255 to 65535), so eight bits are the top byte.
    """
    import re as _re

    r = subprocess.run([conv, path, "txt:-"], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    if r.returncode != 0:
        raise RuntimeError((r.stdout + r.stderr).decode("latin1")[-200:])
    out = {}
    for line in r.stdout.decode("latin1").split("\n"):
        # one, three or four components: grey, rgb and rgba.  The alpha is
        # dropped - the DDS cases compare RGB, and the JPEG ones never have it.
        m = _re.match(r"^(\d+),(\d+):\s+\((\d+)(?:,(\d+),(\d+)(?:,(\d+))?)?\)",
                      line)
        if not m:
            continue
        x, y = int(m.group(1)), int(m.group(2))
        if m.group(4) is not None:
            out[(x, y)] = tuple(int(m.group(i)) >> 8 for i in (3, 4, 5))
        else:
            v = int(m.group(3)) >> 8
            out[(x, y)] = (v, v, v)
    return out


def run_jpeg_im(case):
    """
    jpeglib's output read back through ImageMagick rather than through
    jpeglib's own reader, so the check does not share the writer's
    assumptions.  This is what found the always-RST0 marker and the SOF
    precision bug.
    """
    import jpeglib

    name, comps, restart, precision = case
    conv = _im_convert()
    if conv is None:
        return "%-22s skipped (no ImageMagick)" % name

    w, h = 17, 9
    rgb = jpeg_sources(w, h)[name]
    if comps == "grey" and name != "grey":
        rgb = [[(sum(px) // 3,) * 3 for px in row] for row in rgb]
    src = os.path.join(BUILD, "dt_j_%s_%s_%d_%d.jpg"
                       % (name, comps, restart, precision))
    jpeglib.write_jpeg(src, w, h, rgb, comps=comps, restart=restart,
                       precision=precision)
    try:
        px = im_pixels(src, conv)
    except Exception as exc:                       # noqa: BLE001
        return "%-22s ImageMagick cannot read it: %s" % (name, exc)
    if len(px) != w * h:
        return "%-22s ImageMagick read %d/%d pixels" % (name, len(px), w * h)
    worst = 0
    for y in range(h):
        for x in range(w):
            for k in range(3):
                worst = max(worst, abs(rgb[y][x][k] - px[(x, y)][k]))
    bound = JPEG_BOUND.get(name)
    # ImageMagick's IDCT rounds differently, so it gets its own, looser
    # bound on top of the module's: the point is that it decodes at all and
    # to nearly the right picture, not that it agrees to the level.
    imbound = None if bound is None else bound + 48
    if imbound is not None and worst > imbound:
        return "%-22s ImageMagick differs by %d (bound %d)" % (name, worst,
                                                               imbound)
    return "%-22s  ok  (ImageMagick within %d%s)" % (
        name, worst, "" if imbound is None else " of %d" % imbound)


def run_jpeg_progressive():
    """
    A progressive (SOF2) JPEG, which jpeglib cannot write: generated by
    ImageMagick from a PPM, then sized and refused cleanly.  This is the
    corpus side of the `0xC2 shares the SOF0 arm` finding - when sub_40B180
    lands, this is the file that checks what it actually does.
    """
    conv = _im_convert()
    if conv is None:
        return "j-progressive        skipped (no ImageMagick)"

    w, h = 17, 9
    rgb = jpeg_sources(w, h)["ramp"]
    ppm = os.path.join(BUILD, "dt_j_prog.ppm")
    with open(ppm, "wb") as fh:
        fh.write(b"P6\n%d %d\n255\n" % (w, h))
        for row in rgb:
            for px in row:
                fh.write(bytes(px))
    src = os.path.join(BUILD, "dt_j_prog.jpg")
    r = subprocess.run([conv, ppm, "-interlace", "JPEG", "-sampling-factor",
                        "2x2", "-quality", "75", src], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    if r.returncode != 0 or not os.path.exists(src):
        return "j-progressive        ImageMagick cannot write it"
    data = open(src, "rb").read()
    if b"\xff\xc2" not in data:
        return "j-progressive        not progressive (no SOF2 marker)"
    if data.count(b"\xff\xda") < 2:
        return "j-progressive        one scan only"

    # GetImageSize() must still size it - it reads the SOF2 the same way -
    # and both binaries must refuse it while sub_40B180 is missing
    r = subprocess.run([DUMPDEC, src], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    if r.returncode == 0:
        return "j-progressive        dumpdec decoded it; expected a refusal"
    if "not reconstructed yet" not in r.stderr.decode("latin1"):
        return "j-progressive        dumpdec said %r" % (
            r.stderr.decode("latin1").strip()[-60:])

    dst = os.path.join(BUILD, "dt_j_prog.out.png")
    if os.path.exists(dst):
        os.remove(dst)
    r = subprocess.run([PNGOUT, "/force", src, dst], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    if os.path.exists(dst):
        return "j-progressive        pngout.exe wrote a PNG; expected a refusal"
    return "j-progressive        ok  (SOF2, %d scans, refused cleanly)" % (
        data.count(b"\xff\xda"))


# ------------------------------------------------------------------ DDS
def dds_sources(w, h):
    """
    Pictures for the DDS cases, written as PPM so ImageMagick can take them.
    Smooth, for the same reason as the JPEG sources: DXT1 fits four colours
    to each 4x4 block, so a wrap or an edge in the source measures DXT's own
    error rather than anything about the file.  RGBA - the PPM writer drops
    the alpha and the DXT3/DXT5 builder uses it.
    """
    return [[((x * 180) // (w - 1),
             (y * 170) // (h - 1),
             ((x + y) * 90) // (w + h - 2),
             (x * 200) // (w - 1) + (y * 55) // (h - 1))
             for x in range(w)] for y in range(h)]


def dds_alpha_block(tag, vals):
    """
    Sixteen alpha bytes to an 8-byte DXT alpha block, row major.

    DXT3 is 4 bits a pixel, low nibble first.  DXT5 is two endpoints and 48
    bits of 3-bit indices, LSB first, with the endpoints ordered max then min
    so the eight-value interpolation is used rather than the six-value one
    with explicit 0 and 255.
    """
    if tag == "dxt3":
        out = bytearray()
        for i in range(0, 16, 2):
            out.append(((vals[i] >> 4) & 15) | (((vals[i + 1] >> 4) & 15) << 4))
        return bytes(out)
    a0, a1 = max(vals), min(vals)
    if a0 == a1:
        idx = [0] * 16
    else:
        ramp = [a0, a1] + [((6 - i) * a0 + (1 + i) * a1 + 3) // 7
                          for i in range(6)]
        idx = []
        for v in vals:
            best, bi = None, 0
            for i, rr in enumerate(ramp):
                dd = abs(v - rr)
                if best is None or dd < best:
                    best, bi = dd, i
            idx.append(bi)
    bits = 0
    for i in range(15, -1, -1):
        bits = (bits << 3) | idx[i]
    return bytes([a0, a1]) + bytes((bits >> (8 * i)) & 255 for i in range(6))


def im_rgba(path, conv):
    """
    The 8-bit RGBA at each pixel, from ImageMagick's `txt:` output - the alpha
    defaults to 255 when the file has none.  `im_pixels()` drops the alpha;
    the DXT3/DXT5 cases need it.
    """
    import re as _re

    r = subprocess.run([conv, path, "txt:-"], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    if r.returncode != 0:
        raise RuntimeError((r.stdout + r.stderr).decode("latin1")[-200:])
    out = {}
    for line in r.stdout.decode("latin1").split("\n"):
        m = _re.match(r"^(\d+),(\d+):\s+\((\d+)(?:,(\d+),(\d+)(?:,(\d+))?)?\)",
                      line)
        if not m:
            continue
        x, y = int(m.group(1)), int(m.group(2))
        if m.group(4) is not None:
            rgb = tuple(int(m.group(i)) >> 8 for i in (3, 4, 5))
            a = int(m.group(6)) >> 8 if m.group(6) is not None else 255
            out[(x, y)] = rgb + (a,)
        else:
            v = int(m.group(3)) >> 8
            out[(x, y)] = (v, v, v, 255)
    return out


def dds_decode_expected(path):
    """
    What sub_40DD80 puts in the frame, as (b, g, r, a) dwords - a second,
    independent implementation of the block decoder, written from the
    formulas in the listing rather than from the C.  It catches
    transcription slips (wrong shifts, sign errors, off-by-ones), not
    misunderstandings of DXT itself.
    """
    import struct as _st

    data = open(path, "rb").read()
    flags = _st.unpack("<I", data[80:84])[0]
    h, w = _st.unpack("<II", data[12:20])
    out = {}

    if flags & 0x40:
        for y in range(h):
            for x in range(w):
                o = 128 + (y * w + x) * 4
                out[(y, x)] = tuple(data[o:o + 4])
        return w, h, out

    code = data[87] - 0x30
    odd = code & 1
    premult = [0x10000] + [0xFF0000 // a for a in range(1, 256)]
    bs = 8 if code == 1 else 16
    stride = 2 * w if code == 1 else 4 * w
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            o = 128 + (by // 4) * stride + (bx // 4) * bs
            blk = data[o:o + bs]
            cb = blk if code == 1 else blk[8:]
            c0 = cb[0] | (cb[1] << 8)
            c1 = cb[2] | (cb[3] << 8)
            idx = cb[4] | (cb[5] << 8) | (cb[6] << 16) | (cb[7] << 24)
            r0, g0, b0 = (c0 >> 8) & 0xF8, (c0 >> 3) & 0xFC, (c0 & 31) << 3
            r1, g1, b1 = (c1 >> 8) & 0xF8, (c1 >> 3) & 0xFC, (c1 & 31) << 3
            if c0 <= c1 and code == 1:
                cols = [(r0, g0, b0, 255), (r1, g1, b1, 255),
                        ((r0 + r1) >> 1, (g0 + g1) >> 1, (b0 + b1) >> 1, 255),
                        (0, 0, 0, 0)]
            else:
                cols = [(r0, g0, b0, 255), (r1, g1, b1, 255),
                        ((21845 * (r1 + 2 * r0 + 1)) >> 16,
                         (21845 * (g1 + 2 * g0 + 1)) >> 16,
                         (21845 * (b1 + 2 * b0 + 1)) >> 16, 255),
                        ((21845 * (r0 + 2 * r1 + 1)) >> 16,
                         (21845 * (g0 + 2 * g1 + 1)) >> 16,
                         (21845 * (b0 + 2 * b1 + 1)) >> 16, 255)]
            for py in range(4):
                for px in range(4):
                    x, y = bx + px, by + py
                    if x >= w or y >= h:
                        continue
                    pi = py * 4 + px
                    ci = (idx >> (2 * pi)) & 3
                    rr, gg, bb, aa = cols[ci]
                    if code == 1:
                        aa = cols[ci][3]
                    elif code in (2, 3):
                        ab = blk[pi >> 1]
                        nib = ((ab >> 4) & 15) if (pi & 1) else (ab & 15)
                        aa = nib * 17
                    else:
                        a0, a1 = blk[0], blk[1]
                        abits = 0
                        for k in range(5, -1, -1):
                            abits = (abits << 8) | blk[2 + k]
                        ai = (abits >> (3 * pi)) & 7
                        if a1 >= a0:
                            ramp = [a0, a1]
                            acc = 13107 * (a1 + 4 * a0 + 2)
                            step = 13107 * (a1 - a0)
                            for _ in range(4):
                                ramp.append(acc >> 16)
                                acc += step
                            ramp += [0, 255]
                            aa = ramp[ai]
                        else:
                            ramp = [a0, a1]
                            acc = 9362 * (a1 + 6 * a0 + 3)
                            step = 9362 * (a1 - a0)
                            for _ in range(6):
                                ramp.append(acc >> 16)
                                acc += step
                            aa = ramp[ai]
                    if not odd:
                        sc = premult[aa]
                        rr = min((rr * sc) >> 16, 255)
                        gg = min((gg * sc) >> 16, 255)
                        bb = min((bb * sc) >> 16, 255)
                    out[(y, x)] = (bb & 255, gg & 255, rr & 255, aa & 255)
    return w, h, out


def dds_cases():
    """
    DDS: DXT1, DXT3, DXT5 and uncompressed A8R8G8B32 - everything sub_40DD80
    takes.  DXT1 and the uncompressed file are generated by ImageMagick;
    DXT3 and DXT5 are ImageMagick's DXT1 with an alpha block prepended to
    each colour block, and validated through ImageMagick's own reader before
    they count as corpus.
    """
    # 16x8 and 24x16 are multiples of 4, so the source row stride is
    # exact (2*w / 4*w) and every block is visited.  An odd width drifts -
    # see sub_40DD80 - and is not asserted pixel-exact here.
    return [
        ("d-dxt1", 16, 8, "dxt1"),
        ("d-dxt1-big", 24, 16, "dxt1"),
        ("d-dxt3", 16, 8, "dxt3"),
        ("d-dxt5", 16, 8, "dxt5"),
        ("d-rgb32", 16, 8, "rgb32"),
        ("d-rgb32-big", 24, 16, "rgb32"),
    ]


def run_dds(case):
    """
    sub_40DD80 is not transcribed, so this cannot check pixels.  It checks
    that ImageMagick writes the file, that ImageMagick reads it back to the
    picture it was given (so the corpus is valid), and that both binaries
    refuse it cleanly while it is missing.
    """
    name, w, h, kind = case
    conv = _im_convert()
    if conv is None:
        return "%-12s skipped (no ImageMagick)" % name

    ppm = os.path.join(BUILD, "dt_%s.ppm" % name)
    rgba = dds_sources(w, h)
    with open(ppm, "wb") as fh:
        fh.write(b"P6\n%d %d\n255\n" % (w, h))
        for row in rgba:
            for px in row:
                fh.write(bytes(px[:3]))

    src = os.path.join(BUILD, "dt_%s.dds" % name)
    if kind in ("dxt1", "dxt3", "dxt5"):
        # DXT3 and DXT5 start life as ImageMagick's DXT1; the alpha block is
        # prepended to each colour block below
        args = []
    else:
        # 32 bits a pixel with the four masks sub_40DD80 requires at
        # 0x40DDAA: 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000
        args = ["-define", "dds:compression=none", "-type", "TrueColorAlpha"]
    r = subprocess.run([conv, ppm] + args + [src], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    if r.returncode != 0 or not os.path.exists(src):
        return "%-12s ImageMagick cannot write it" % name

    if kind in ("dxt3", "dxt5"):
        import struct as _st

        raw = open(src, "rb").read()
        hdr, body = raw[:128], raw[128:]
        nbx = (w + 3) // 4
        out = bytearray(hdr)
        out[84:88] = kind.upper().encode("ascii")
        for by in range((h + 3) // 4):
            for bx in range(nbx):
                cb = body[(by * nbx + bx) * 8:(by * nbx + bx + 1) * 8]
                c0, c1 = _st.unpack("<HH", cb[:4])
                # an opaque source must not produce punch-through blocks,
                # or the colour half of the DXT3/5 file would be one
                if c0 <= c1:
                    return "%-12s DXT1 has punch-through, cannot extend" % name
                vals = []
                for py in range(4):
                    for px in range(4):
                        x, y = min(bx * 4 + px, w - 1), min(by * 4 + py, h - 1)
                        vals.append(rgba[y][x][3] & 255)
                out += dds_alpha_block(kind, vals)
                out += cb
        with open(src, "wb") as fh:
            fh.write(bytes(out))

    # ImageMagick must read the file back to the picture
    try:
        px = im_rgba(src, conv)
    except Exception as exc:                       # noqa: BLE001
        return "%-12s ImageMagick cannot read it: %s" % (name, exc)
    if len(px) != w * h:
        return "%-12s ImageMagick read %d/%d pixels" % (name, len(px), w * h)
    worst = [0, 0, 0, 0]
    has_alpha = kind in ("dxt3", "dxt5")
    for y in range(h):
        for x in range(w):
            for k in range(3):
                worst[k] = max(worst[k], abs(rgba[y][x][k] - px[(x, y)][k]))
            # DXT1 and the uncompressed file carry no alpha, so there is none
            # to compare - the read-back defaults to 255 and the source is not
            # 255.  Only DXT3 and DXT5 store one.
            want_a = rgba[y][x][3] if has_alpha else 255
            worst[3] = max(worst[3], abs(want_a - px[(x, y)][3]))
    # DXT1 fits four colours to each 4x4 block, so a gradient loses a real
    # amount to it (26 observed on the 17x9) - the bound is loose, but it
    # still catches a file that decodes to garbage.  DXT3's alpha is 4 bits
    # (14 observed) and DXT5's interpolated (4 observed).  The uncompressed
    # file must come back exactly.
    bound = {"dxt1": (48, 0), "dxt3": (48, 32), "dxt5": (48, 16),
             "rgb32": (0, 0)}[kind]
    if worst[0] > bound[0] or worst[1] > bound[0] or worst[2] > bound[0] \
            or worst[3] > bound[1]:
        return "%-12s re-read differs by %s (bound %s)" % (name, worst, bound)

    # sub_40DD80 must produce exactly what the independent block decoder
    # says, pixel for pixel
    try:
        gw, gh, got = dump(src)
    except Exception as exc:                       # noqa: BLE001
        return "%-12s %s" % (name, exc)
    if (gw, gh) != (w, h):
        return "%-12s wrong size %dx%d" % (name, gw, gh)
    try:
        ew, eh, want = dds_decode_expected(src)
    except Exception as exc:                       # noqa: BLE001
        return "%-12s expected-decode failed: %s" % (name, exc)
    bad = 0
    first = None
    for y in range(h):
        for x in range(w):
            if got[y][x] != want[(y, x)]:
                bad += 1
                if first is None:
                    first = "x=%d y=%d got %s want %s" % (x, y, got[y][x],
                                                          want[(y, x)])
    if bad:
        return "%-12s %d/%d pixels differ, first %s" % (name, bad, w * h,
                                                        first)
    return "%-12s  ok  (%dx%d %s)" % (name, w, h, kind)


def roundtrip_dds(case):
    """The same file through pngout.exe and back out of the PNG it writes."""
    name, w, h, kind = case
    src = os.path.join(BUILD, "dt_%s.dds" % name)
    # regenerate it exactly as run_dds does, so the round trip stands alone
    conv = _im_convert()
    if conv is None:
        return "%-12s skipped (no ImageMagick)" % name
    ppm = os.path.join(BUILD, "dt_%s.ppm" % name)
    rgba = dds_sources(w, h)
    with open(ppm, "wb") as fh:
        fh.write(b"P6\n%d %d\n255\n" % (w, h))
        for row in rgba:
            for px in row:
                fh.write(bytes(px[:3]))
    if kind in ("dxt1", "dxt3", "dxt5"):
        args = []
    else:
        args = ["-define", "dds:compression=none", "-type", "TrueColorAlpha"]
    r = subprocess.run([conv, ppm] + args + [src], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    if r.returncode != 0 or not os.path.exists(src):
        return "%-12s ImageMagick cannot write it" % name
    if kind in ("dxt3", "dxt5"):
        import struct as _st

        raw = open(src, "rb").read()
        hdr, body = raw[:128], raw[128:]
        nbx = (w + 3) // 4
        out = bytearray(hdr)
        out[84:88] = kind.upper().encode("ascii")
        for by in range((h + 3) // 4):
            for bx in range(nbx):
                cb = body[(by * nbx + bx) * 8:(by * nbx + bx + 1) * 8]
                c0, c1 = _st.unpack("<HH", cb[:4])
                if c0 <= c1:
                    return "%-12s DXT1 has punch-through" % name
                vals = []
                for py in range(4):
                    for px in range(4):
                        x = min(bx * 4 + px, w - 1)
                        y = min(by * 4 + py, h - 1)
                        vals.append(rgba[y][x][3] & 255)
                out += dds_alpha_block(kind, vals)
                out += cb
        with open(src, "wb") as fh:
            fh.write(bytes(out))

    dst = os.path.join(BUILD, "dt_%s.dds.out.png" % name)
    if os.path.exists(dst):
        os.remove(dst)
    r = subprocess.run([PNGOUT, "/force", src, dst], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    if not os.path.exists(dst):
        out = (r.stdout + r.stderr).decode("latin1")
        return "%-12s no output: %s" % (name, out.strip()[-70:])
    try:
        got = read_pixels(dst)
    except Exception as exc:                       # noqa: BLE001
        return "%-12s output unreadable: %s" % (name, exc)
    if (got["width"], got["height"]) != (w, h):
        return "%-12s wrong size %dx%d" % (name, got["width"], got["height"])
    try:
        ew, eh, want = dds_decode_expected(src)
    except Exception as exc:                       # noqa: BLE001
        return "%-12s expected-decode failed: %s" % (name, exc)
    bad = 0
    first = None
    for y in range(h):
        for x in range(w):
            b, g, rr, a = want[(y, x)]
            wantpx = (rr, g, b, a)
            if tuple(got["rows"][y][x]) != wantpx:
                bad += 1
                if first is None:
                    first = "x=%d y=%d got %s want %s" % (
                        x, y, got["rows"][y][x], wantpx)
    if bad:
        return "%-12s %d/%d pixels differ, first %s" % (name, bad, w * h,
                                                        first)
    return "%-12s  ok  (%dx%d %s round trip)" % (name, w, h, kind)


def main():
    if "--dds" in sys.argv:
        cases = dds_cases()
        mode = run_dds
        argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    elif "--dds-roundtrip" in sys.argv:
        cases = dds_cases()
        mode = roundtrip_dds
        argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    elif "--jpeg-im" in sys.argv:
        cases = jpeg_cases()
        results = [run_jpeg_im(c) for c in cases]
        results.append(run_jpeg_progressive())
        fails = sum(1 for line in results if "  ok  " not in line
                    and "skipped" not in line)
        for line in results:
            if "  ok  " not in line:
                print(line)
        print("%d/%d cases pass" % (len(results) - fails, len(results)))
        return 1 if fails else 0
    elif "--jpeg" in sys.argv:
        cases = jpeg_cases()
        mode = run_jpeg
        argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    elif "--gif" in sys.argv:
        cases = gif_cases()
        mode = run_gif
        argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    elif "--gif-roundtrip" in sys.argv:
        cases = gif_cases()
        mode = roundtrip_gif
        argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    elif "--bmp" in sys.argv:
        cases = bmp_cases()
        mode = run_bmp
        argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    elif "--bmp-roundtrip" in sys.argv:
        cases = bmp_cases()
        mode = roundtrip_bmp
        argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    elif "--targa" in sys.argv:
        cases = targa_cases()
        mode = run_targa
        argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    elif "--targa-roundtrip" in sys.argv:
        cases = targa_cases()
        mode = roundtrip_targa
        argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    elif "--pal8" in sys.argv:
        cases = pal8_cases()
        mode = run_pal8
        argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    elif "--pal8-roundtrip" in sys.argv:
        cases = pal8_cases()
        mode = roundtrip_pal8
        argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    elif "--pcx" in sys.argv:
        cases = pcx_cases()
        mode = run_pcx
        argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    elif "--pcx-roundtrip" in sys.argv:
        cases = pcx_cases()
        mode = roundtrip_pcx
        argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    elif "--roundtrip" in sys.argv:
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