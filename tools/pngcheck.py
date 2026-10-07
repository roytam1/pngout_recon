#!/usr/bin/env python3
"""
pngcheck.py   -   verify Optimize()'s output against the image it came from.

Run from build\\ after pngcheck.exe has written png_N.png and png_N.ref:

    python ..\\tools\\pngcheck.py

Each .png is parsed as a PNG, its IDAT payloads are handed to zlib (which checks
the zlib header, the Adler-32 and every Huffman code), the per row PNG filters
are undone, and the resulting samples are compared against the source pixels in
the matching .ref.

This is the check the unit test cannot make on its own: the reconstruction's
filters, LZ coder, Huffman codes, block splitting and chunk framing all have to
be right *at once* for a stream to inflate, and then the samples have to be
right *too* for the image to come back.  A wrong channel order, a wrong stride,
a filter applied to the wrong byte or a bit depth mismatch all show up here as a
mismatch rather than as a well formed but wrong file.
"""

import os
import struct
import sys
import zlib


def die(msg):
    print("FAIL " + msg)
    sys.exit(1)


class CaseError(Exception):
    """One case is wrong; the others are still worth reporting."""


# ---------------------------------------------------------------- parsing


def parse_png(path):
    """Return (width, height, depth, coltype, palette, tRNS, raw, idat_count)."""
    with open(path, "rb") as f:
        data = f.read()

    if data[:8] != b"\x89PNG\x0D\x0A\x1A\x0A":
        die("%s: not a PNG (bad signature)" % path)

    pos = 8
    ihdr = None
    palette = None
    trns = None
    idat = bytearray()
    n_idat = 0
    seen_iend = False

    while pos + 8 <= len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        tag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        (crc,) = struct.unpack(">I", data[pos + 8 + length:pos + 12 + length])

        # every chunk's CRC is checked here; a wrong one is a real defect even
        # if the payload happens to inflate
        if zlib.crc32(tag + body) & 0xFFFFFFFF != crc:
            die("%s: chunk %s has a bad CRC" % (path, tag.decode("latin1")))

        if tag == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif tag == b"PLTE":
            palette = body
        elif tag == b"tRNS":
            trns = body
        elif tag == b"IDAT":
            idat += body
            n_idat += 1
        elif tag == b"IEND":
            seen_iend = True

        pos += 12 + length
        if tag == b"IEND":
            break

    if ihdr is None:
        die("%s: no IHDR" % path)
    if n_idat == 0:
        die("%s: no IDAT" % path)
    if not seen_iend:
        die("%s: no IEND" % path)

    w, h, depth, coltype, comp, filt, inter = ihdr
    if comp != 0:
        die("%s: compression method %d" % (path, comp))
    if filt != 0:
        die("%s: filter method %d" % (path, filt))
    if inter != 0:
        die("%s: interlaced, not checked here" % path)

    return w, h, depth, coltype, palette, trns, bytes(idat), n_idat


# ---------------------------------------------------------------- unfiltering


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def unfilter(raw, height, stride, bpp):
    """
    Undo the per row PNG filters.  `bpp` is bytes per pixel, at least 1 - the
    reconstruction writes sub-byte depths as one packed byte, so 4 bit grey is
    handled with bpp 1.
    """
    out = bytearray()
    prev = bytearray(stride)
    pos = 0

    for _ in range(height):
        if pos >= len(raw):
            die("ran out of data at row %d" % len(out) // stride)
        ft = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        if len(line) != stride:
            die("short row: wanted %d bytes, got %d" % (stride, len(line)))
        pos += stride

        if ft == 0:
            pass
        elif ft == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                ul = prev[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + paeth(left, prev[i], ul)) & 0xFF
        else:
            die("unknown filter type %d" % ft)

        out += line
        prev = line

    if pos != len(raw):
        die("trailing data after the last row: %d bytes" % (len(raw) - pos))
    return out


def samples_of(raw, w, h, depth, coltype):
    """
    Turn the unfiltered scanlines into a flat list of sample values, scaled the
    way a PNG reader scales them: a depth d sample n means n * 255 / (2^d - 1).
    """
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[coltype]
    per_byte = 8 // depth
    out = []

    if coltype in (0, 3) and depth < 8:
        total = (w + per_byte - 1) // per_byte
        stride = total
        maxv = (1 << depth) - 1
        for r in range(h):
            row = raw[r * stride:(r + 1) * stride]
            bits = []
            for byte in row:
                for k in range(per_byte):
                    shift = 8 - depth - k * depth
                    bits.append((byte >> shift) & maxv)
            bits = bits[:w]
            if coltype == 3:
                # a palette sample is an *index*, not a shade: it is never
                # rescaled to 0..255 the way a grayscale sample is
                out.extend(bits)
            else:
                out.extend((n * 255 + maxv // 2) // maxv for n in bits)
    else:
        stride = w * channels
        for r in range(h):
            row = raw[r * stride:(r + 1) * stride]
            out.extend(row)

    return out


# ---------------------------------------------------------------- comparison


CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def check(path_png, path_ref):
    w, h, depth, coltype, palette, trns, idat, n_idat = parse_png(path_png)

    # zlib does the real work: it validates the zlib wrapper, every Huffman
    # code and the Adler-32
    raw = zlib.decompress(idat)

    ch = CHANNELS[coltype]
    if coltype in (0, 3) and depth < 8:
        stride = (w + (8 // depth) - 1) // (8 // depth)
        bpp = 1
    else:
        stride = w * ch
        bpp = ch

    flat = unfilter(raw, h, stride, bpp)
    got = samples_of(flat, w, h, depth, coltype)

    with open(path_ref) as f:
        rw, rh = (int(x) for x in f.readline().split())
        ref = [int(line, 16) for line in f if line.strip()]

    if (rw, rh) != (w, h):
        die("%s: IHDR says %dx%d, the reference is %dx%d" % (path_png, w, h, rw, rh))

    # The samples a scanline is built from are the bytes of the internal pixel
    # in the order sub_407D80 / sub_407B00 / sub_4077C0 / sub_4071D0 emit them,
    # which is *not* ascending for anything but the 1 and 2 byte cases:
    #
    #     colour type 0   0
    #              2      2, 1, 0
    #              4      0, 1
    #              6      2, 1, 0, 3
    #
    # An internal pixel is one DWORD, B, G, R, A from the bottom up, except for
    # colour type 4 where the decoder leaves the grey level in byte 0 and the
    # alpha in byte 1 so that the two byte walk picks them up in that order.
    order = {0: (0,), 2: (2, 1, 0), 4: (0, 1), 6: (2, 1, 0, 3)}

    if coltype == 3:
        if palette is None:
            die("%s: colour type 3 with no PLTE" % path_png)
        # a palette sample is an index into PLTE, so the expectation is the
        # index whose entry matches the pixel.  The match has to include the
        # alpha: PNGOUT's palette is built from whole pixels, so two entries
        # can share the same RGB and differ only in the tRNS alpha - which is
        # exactly what happens to a picture whose greys are both opaque and
        # see-through.
        entries = [tuple(palette[3 * i:3 * i + 3]) for i in range(len(palette) // 3)]
        # A palette's tRNS is a prefix: one alpha per entry from 0 up to the
        # last entry that is not opaque.  Everything past it is left out, which
        # PNG reads as opaque.
        alphas = [255] * len(entries)
        if trns is not None:
            if len(trns) > len(entries):
                die("%s: tRNS holds %d bytes for %d palette entries"
                    % (path_png, len(trns), len(entries)))
            for i, a in enumerate(trns):
                alphas[i] = a
            table = [e + (alphas[i],) for i, e in enumerate(entries)]
        else:
            table = entries
        want = []
        for v in ref:
            rgb = ((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)
            key = rgb + ((v >> 24) & 0xFF,) if trns is not None else rgb
            if key not in table:
                die("%s: a reference colour %s is not in the palette"
                    % (path_png, key))
            want.append(table.index(key))
    elif coltype in order:
        want = []
        for v in ref:
            for b in order[coltype]:
                want.append((v >> (8 * b)) & 0xFF)
    else:
        die("%s: unsupported colour type %d" % (path_png, coltype))

    if len(got) != len(want):
        raise CaseError("%s: %d samples came back, expected %d"
                        % (path_png, len(got), len(want)))

    bad = 0
    first = None
    for i in range(len(want)):
        if got[i] != want[i]:
            bad += 1
            if first is None:
                first = (i, got[i], want[i])

    if bad:
        raise CaseError("%s: %d of %d samples differ, first at %d: got %d want %d"
                        % (path_png, bad, len(want), first[0], first[1], first[2]))

    return "c%d/d%d %dx%d, %d IDAT, %d samples, all match" % (
        coltype, depth, w, h, n_idat, len(got))


def main():
    here = os.getcwd()
    refs = sorted(x for x in os.listdir(here)
                  if x.startswith("png_") and x.endswith(".ref"))
    if not refs:
        die("no png_*.ref files here - run pngcheck.exe first")

    print("%-26s %s" % ("case", "result"))
    fails = 0
    for r in refs:
        png = r[:-4] + ".png"
        try:
            res = check(png, r)
        except zlib.error as e:
            print("FAIL %-24s inflate: %s" % (r[:-4], e))
            fails += 1
            continue
        except CaseError as e:
            print("FAIL %-24s %s" % (r[:-4], e))
            fails += 1
            continue
        print("ok   %-24s %s" % (r[:-4], res))

    print("\n%s  (%d failure%s)" % ("FAILED" if fails else "PASSED",
                                    fails, "" if fails == 1 else "s"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())