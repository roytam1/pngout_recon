/*
 *  trials.c   -   sub_402AD0, `Optimize()`
 *
 *  The trial-and-error search that is the whole point of PNGOUT.  Given the
 *  decoded 32bpp image and the user's options, it
 *
 *    - works out what colour type, bit depth and palette can represent the
 *      image at all (and says so, in the original's exact wording, when they
 *      cannot),
 *    - sizes an output buffer from a closed-form estimate of the PNG,
 *    - takes the /s4 shortcut when the image is to be stored uncompressed,
 *    - otherwise cuts the image into deflate blocks (/n, /b) and runs the whole
 *      image through the filter/compress machinery once per *trial*, keeping the
 *      smallest PNG any trial produced.
 *
 *  The trial loop is not a search over a parameter it passes in.  It re-runs the
 *      same image with the same options, and it is the *row records* - the per
 *      block Huffman machinery in src/optimize.c - that carry state from one
 *      trial to the next: trial 0 seeds them, every later trial leaves them
 *      primed by the previous one, so the code lengths get better and the output
 *      shrinks until it stops.  The loop notices that it has stopped by
 *  fingerprinting each trial's filter choices (dword_1809568) and its size
 *  (dword_180A578): once a trial reproduces an earlier trial's pair, no further
 *  progress is possible and the best result is restored and returned.
 *
 *  Three globals from the original are load-bearing here and are worth naming:
 *
 *      dword_6D5664   TrialFilter  - the filter the current row is using
 *      dword_6D6668   ByteSink     - where the filtered bytes go: the histogram
 *                                    while a filter is being *scored*, and the
 *                                    LZ coder while it is being *committed*
 *      dword_42E960   Image        - the settings block, which doubles as the
 *                                    image descriptor
 */

/* Windows' rand/srand are the ones the original calls; declaring them here keeps
 * <stdlib.h> from mapping rand() to a different one under VC6's rand macros. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "pngout.h"

/* ------------------------------------------------------------------ */
/*  the messages, verbatim from the string table                       */
/* ------------------------------------------------------------------ */

/*  v227[], the five "try this instead" hints indexed by the suggested type */
static const char *const ColTypeName[7] = {
    "/c0 (Gray)", "/c1", "/c2 (RGB)", "/c3 (Pal)",
    "/c4 (Gray+Alpha)", "/c5", "/c6 (RGB+Alpha)"
};

/*
 *  The failure exit, loc_404C89.  Both work buffers the trials may have
 *  allocated are released and the caller is told "no result" with the sentinel
 *  0x7FFFFFFF, which main() turns into exit code 3.
 */
static int Fail(void)
{
    if (EntropyLut != NULL) {
        free(EntropyLut);
        EntropyLut = NULL;
    }
    if (SplitBuf != NULL) {
        free(SplitBuf);
        SplitBuf = NULL;
    }
    return 0x7FFFFFFF;
}

/* ------------------------------------------------------------------ */
/*  phase A: what can represent this image?                             */
/* ------------------------------------------------------------------ */

/*
 *  Walk every pixel once and set three facts:
 *
 *      MinSample     the smallest pixel as a 32 bit value, -1 if the image is
 *                    empty.  Kept separately from the palette scan because a
 *                    fully transparent black is *below* every opaque colour and
 *                    its presence is what forces colour type 4 or 6.
 *      flags bit 1   some pixel is not fully opaque
 *      flags bit 2   some pixel is not grey (r != g != b)
 *      flags bit 4   the image needs more than 256 distinct colours
 *      palcount      how many distinct colours were found, and unk_180B578[]
 *                    holds them in first-seen order
 *
 *  The distinct colour count is found with the same colour hash the compressor
 *  uses (sub_407FC0 / sub_407F80), so a colour that hashes to an existing chain
 *  entry is shared.  Once the palette is full the scan stops adding and just
 *  sets bit 4 - there is no point walking the rest of a 24 bit photograph
 *  looking for a 257th colour.
 */
static void ScanImage(unsigned int *flags, int *ncolors)
{
    unsigned char *base = (unsigned char *)Image->pixels;
    int row, col, rows = Image->height, width = Image->width;
    int palcount = 0;
    unsigned int f = 0;
    int minsample = -1;

    InitColorHash(unk_180B578);

    for (row = 0; row < rows; row++) {
        unsigned int *p = (unsigned int *)(base + (size_t)row * Image->stride);

        if (CheckEscHook(1))
            return;

        for (col = 0; col < width; col++) {
            unsigned int v = p[col];

            /*
             *  The minimum is taken *unsigned*, against a seed of 0xFFFFFFFF.
             *  That is not an accident of the ordering: every real pixel is
             *  0xAARRGGBB, and a transparent black is 0x00000000, which is the
             *  whole point - it has to win.  Compared as signed values 0 is
             *  greater than -1 and the transparent background would never be
             *  noticed, which is exactly the pixel whose presence decides
             *  whether the image needs an alpha channel at all.
             */
            if (v < (unsigned int)minsample)
                minsample = (int)v;

            /*
             *  Greyscale test.  The original builds it as two shifted halves
             *  rather than three masks:
             *
             *      eax = v & 0xFFFF      ; B | G<<8
             *      eax <<= 8             ; B<<8 | G<<16
             *      ecx = v & 0xFFFF00    ; G<<8 | R<<16
             *      xor                    ; (B^G)<<8 | (G^R)<<16
             *
             *  which is zero exactly when R == G == B.  The tempting single
             *  mask, `(v & 0xFFFF00FF) == ((v & 0xFF00) << 8)`, is *not* the
             *  same test: it leaves bit 24..31 (the alpha) out of the left side
             *  but shifts the green channel into it on the right, so it is zero
             *  only for a pixel whose blue byte is zero - it would call every
             *  grey with a non-zero blue a colour.
             */
            if ((f & 2) == 0 &&
                (unsigned)(((((v & 0xFFFFu) << 8) ^ (v & 0xFFFF00u)) &
                            0xFFFF00u)) != 0)
                f |= 2;

            if ((f & 4) == 0) {
                if (FindColor((int)v) < 0) {
                    if (palcount < MAX_COLORS) {
                        AddColor((int)v, palcount);
                        unk_180B578[palcount++] = (int)v;
                    } else {
                        f |= 4;
                    }
                }
            }
        }
    }

    /*
     *  The bKGD chunk, if one survived, has to be a colour the palette can
     *  actually name.  A bKGD outside the palette would make the file invalid,
     *  so it counts as a reason to give up on this colour type.
     *
     *  This runs before the count is published, because it is the count the
     *  colour type decision turns on and the bKGD can change it.
     */
    if (Image->bkgd != 0 && (f & 4) == 0) {
        int i;

        if ((f & 2) == 0 &&
            (unsigned)(((((Image->bkgd & 0xFFFFu) << 8) ^
                          (Image->bkgd & 0xFFFF00u)) & 0xFFFF00u)) != 0)
            f |= 2;

        if ((f & 4) == 0 && palcount == MAX_COLORS) {
            for (i = MAX_COLORS - 1; i >= 0; i--) {
                if (((unsigned int)(Image->bkgd ^ unk_180B578[i]) &
                     0xFFFFFFu) == 0)
                    break;
            }
            if (i < 0)
                f |= 4;
        }
    }

    /*
     *  MinSample only counts as "the image needs alpha" if some pixel actually
     *  carries that value *and* it is either exactly that RGB triple with a
     *  non-zero alpha, or a fully transparent black.  A smaller RGB with zero
     *  alpha is the normal way a transparent background shows up and does not
     *  force alpha on its own.
     */
    if ((unsigned int)minsample < 0xFF000000u) {
        /*
         *  Both comparisons are *unsigned*, as they are at 0x402B94 and inside
         *  the search loop below.  A pixel is 0xAARRGGBB, so any pixel that is
         *  not opaque has a top byte below 0xFF and is negative as a signed
         *  int: read signed, `minsample >= 0x01000000` is false for every one
         *  of them, the image is decided not to need alpha, and a picture full
         *  of half-transparent pixels is written as RGB with the alpha
         *  silently dropped.
         */
        if ((unsigned int)minsample >= 0x01000000u) {
            f |= 1;
        } else {
            int r2, c2;
            unsigned char *p2 = base;

            /*
             *  0x402D42.  The darkest pixel's own RGB with a non-zero alpha is
             *  one way to want alpha; the other, at 0x402D5C, is *any* pixel
             *  that is not opaque and is not that pixel.  Both set bit 1 and
             *  both leave the loop, so an image whose darkest pixel is the
             *  fully transparent background still gets alpha when some other
             *  colour is see-through.  Leaving the loop without the flag is
             *  what silently drops the transparency.
             */
            for (r2 = 0; r2 < Image->height; r2++) {
                if (CheckEscHook(1))
                    return;
                for (c2 = 0; c2 < Image->width; c2++) {
                    unsigned int v = *(unsigned int *)(p2 + (size_t)c2 * 4);

                    if ((v & 0xFFFFFFu) == (unsigned int)minsample &&
                        (v & 0xFF000000u) != 0) {
                        f |= 1;
                        r2 = Image->height;
                        break;
                    }
                    if ((unsigned int)v < 0xFF000000u &&
                        (int)v != minsample) {
                        f |= 1;
                        r2 = Image->height;
                        break;
                    }
                }
                p2 += Image->stride;
            }
        }
    } else {
        minsample = -1;
    }

    *flags = f;
    *ncolors = palcount;
    MinSample = minsample;
    unk_180B978 = palcount;
}

/*
 *  loc_402D8C: turn the three flag bits into the colour type that *can* hold the
 *  image.  Bit 4 (more than 256 colours) is what makes it choose 3, and it is
 *  why an image that is both colourful and huge ends up a palette: PNGOUT always
 *  prefers the smallest legal encoding, and a 256 entry palette costs 768 bytes
 *  of PLTE against three bytes a pixel.
 *
 *  With no flags at all - every pixel opaque, grey, and at most 256 distinct
 *  values - the choice is between greyscale and a palette, and it turns on the
 *  *number of distinct values*, not on whether they are grey.  A handful of
 *  shades is cheaper as a two entry PLTE than as a grayscale image; only an
 *  image using 128 or more of them (0x80) is written as colour type 0.
 */
static int SuggestColType(unsigned int flags, int ncolors)
{
    switch (flags) {
    case 0:                 return ncolors >= 128 ? 0 : 3;
    case 1: case 2: case 3: return 3;
    case 5:                 return 4;
    case 6:                 return 2;
    case 7:                 return 6;
    default:                return unk_185BED0;
    }
}

/*
 *  loc_402DE9 through loc_402F51: the requested colour type has to be able to
 *  hold what the scan found, and if it cannot the user is told why in the
 *  original's exact wording before the whole thing is abandoned.
 */
static int CheckColType(unsigned int flags, int ncolors)
{
    int suggestion = SuggestColType(flags, ncolors);

    if (unk_185BED0 < 0) {
        unk_185BED0 = suggestion;
    }

    switch (unk_185BED0) {
    case 0:
        if (flags == 0)
            return 1;
        ProgressMsg("\rImage ");
        if (flags & 1)
            ProgressMsg("requires full alpha; ");
        if (flags & 2)
            ProgressMsg("is not grayscale; ");
        if (flags & 4)
            ProgressMsg("doesn't fit into 256 color entries; ");
        ProgressMsg("try %s instead", ColTypeName[suggestion]);
        break;

    case 2:
        if ((flags & 1) == 0)
            return 1;
        ProgressMsg("\rImage requires full alpha; try %s instead",
                    ColTypeName[suggestion]);
        break;

    case 3:
        if ((flags & 4) == 0)
            return 1;
        ProgressMsg("\rImage doesn't fit into 256 color entries; try %s instead",
                    ColTypeName[suggestion]);
        break;

    case 4:
        if ((flags & 2) == 0)
            return 1;
        ProgressMsg("\rImage is not grayscale; try %s instead",
                    ColTypeName[suggestion]);
        break;

    default:
        return 1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  the colour type 0 ramp                                              */
/* ------------------------------------------------------------------ */

/*
 *  loc_402FA2: how many bits does a grayscale image need?
 *
 *  Three flag bits, accumulated over grey levels 128 down to 1 (the table walk
 *  stops at 128 because a value below that is a mirror of one above it once the
 *  low and high bytes are compared):
 *
 *      bit 1   some level is present
 *      bit 2   some level other than the exempt one (85) is present
 *      bit 4   some level's low and high nibbles disagree
 *
 *  A grey level survives the nibble test when `(v & 0xF0) == (v & 0x0F) << 4`,
 *  i.e. when it is a multiple of 17 - which is exactly the set of values an
 *  evenly spaced 4 bit ramp can name.  So the returned depth is
 *
 *      flags 1 -> 2   only the exempt level survives
 *      flags 3 -> 4   every level present is a multiple of 17
 *      flags 7 -> 8   something is not, so 8 bits it is
 *      flags 0 -> 1   nothing present at all
 *
 *  and flags 2, 4, 5, 6 name depths PNG does not have, which is how the caller
 *  ends up reporting "doesn't fit in selected bitdepth".
 */
static int GrayLevelsUsed(void)
{
    int v, flags = 0;

    for (v = 128; v >= 1; v--) {
        if ((GrayLevelFlags[128 - v] | PaletteUse[v]) != 0) {
            int low;

            flags |= 1;
            if (v == 85)               /* unk_412008, the exempted table entry */
                continue;
            flags |= 2;
            low = (v & 0x0F) << 4;
            if (low != (v & 0xF0))
                flags |= 4;
        }
    }

    return flags;
}

/*
 *  loc_403019: build the ramp the grayscale image is written against.  Entry k
 *  is 0xFF000000 + k * step, where step is 0xFFFFFF, 0x555555, 0x111111 or
 *  0x010101 for depths 1, 2, 4 and 8 - the evenly spaced ramp PNG expects.  The
 *  one exception is MinSample: if the image has a colour *darker* than the
 *  bottom of the ramp (a transparent black, normally), it is spliced in at its
 *  own index, which is what lets PNGOUT write colour type 0 with a tRNS chunk.
 */
static void BuildGrayPalette(int depth)
{
    static const int step[9] = {
        0, 0xFFFFFF, 0x555555, 0, 0x111111, 0, 0, 0, 0x010101
    };
    int n = 1 << depth;
    int inc = step[depth];
    int i;
    unsigned int c = 0xFF000000u;

    unk_180B978 = n;

    for (i = 0; i < n; i++) {
        unk_180B578[i] = (int)c;
        c += (unsigned int)inc;
    }

    if (MinSample != -1)
        unk_180B578[MinSample / inc] = MinSample;
}

/* ------------------------------------------------------------------ */
/*  the colour type 3 palette                                          */
/* ------------------------------------------------------------------ */

/*
 *  loc_40316B: order the palette by how often each entry is used, most used
 *  first.  It is a shell sort with a halving gap and, at each gap, a full
 *  insertion pass - which is why the inner walk steps back by `gap` and keeps
 *  going rather than stopping after one exchange.
 *
 *  The counts and the colours move together: PaletteUse[] and unk_180B578[] are
 *  the two halves of one swap, and getting that wrong would leave the counts
 *  describing a different palette than the one written out.
 */
static void SortPaletteByUse(void)
{
    int n = unk_180B978;
    int gap = n >> 1;

    while (gap != 0) {
        int i, span = n - gap;

        for (i = 0; i < span; i++) {
            int a = i, b = i + gap;

            while (a >= 0) {
                int ca = PaletteUse[a], cb = PaletteUse[b];
                int ta, tb;

                if (ca >= cb)
                    break;
                PaletteUse[a] = cb;
                PaletteUse[b] = ca;
                ta = unk_180B578[a];
                tb = unk_180B578[b];
                unk_180B578[a] = tb;
                unk_180B578[b] = ta;
                a -= gap;
                b -= gap;
            }
        }

        gap >>= 1;
    }
}

/*
 *  loc_40321A: with /kp the palette the file arrived with is kept, and all that
 *  is left is to drop the entries nothing points at.  The loop walks *down* from
 *  the last entry, which is why it is a backwards walk and not a forward one:
 *  adding entry i can only make entry i-1 unreachable, never i+1.
 */
static void CompactKeptPalette(void)
{
    int i, keep = 0;

    for (i = unk_180B978 - 1; i >= 0; i--) {
        if (PaletteUse[i] == 0)
            continue;
        if (keep != i) {
            unk_180B578[keep] = unk_180B578[i];
            PaletteUse[keep] = PaletteUse[i];
        }
        keep++;
    }

    unk_180B978 = keep;
}

/*
 *  loc_403340: the bit depth a palette needs is the smallest power of two that
 *  can index it - one more entry than the palette has if the bKGD chunk names a
 *  colour that is not in the palette, because then the palette has to grow by
 *  one to hold it.
 */
static int PaletteBitDepth(void)
{
    int need = unk_180B978;
    int n;

    if (Image->bkgd != 0) {
        int i;

        for (i = unk_180B978 - 1; i >= 0; i--) {
            if (((unsigned int)(Image->bkgd ^ unk_180B578[i]) & 0xFFFFFFu) == 0)
                break;
        }
        if (i < 0)
            need = unk_180B978 + 1;
    }

    for (n = 1; n < 8; n *= 2) {
        if ((1 << n) >= need)
            break;
    }

    if (unk_180A570 == 1 || unk_180A570 == 2 ||
        unk_180A570 == 4 || unk_180A570 == 8) {
        if (n > unk_180A570)
            return 0;                   /* "doesn't fit in selected bitdepth" */
    } else {
        unk_180A570 = n;
    }

    return 1;
}

/*
 *  loc_403250: with /kp off, the fully opaque entries are pushed to the front of
 *  the palette.  A tRNS chunk is a run of entries from the end, so an entry that
 *  is opaque but sits past a transparent one makes the tRNS run say "this colour
 *  is transparent" when it is not.  The swap loop is the usual bubble: each
 *  `v62` walks forwards pushing any transparent entry behind it.
 */
static void OpaqueToFront(void)
{
    int front = 1, back = 0;

    if (unk_180B978 <= 1)
        return;

    do {
        int i;

        for (i = 0; i < front; i++) {
            if ((unsigned int)unk_180B578[i] >= 0xFF000000u &&
                (unsigned int)unk_180B578[back] < 0xFF000000u) {
                int t;

                t = PaletteUse[i];  PaletteUse[i]  = PaletteUse[back];
                PaletteUse[back] = t;
                t = unk_180B578[i]; unk_180B578[i]  = unk_180B578[back];
                unk_180B578[back] = t;
            }
        }

        front++;
        back++;
    } while (front < unk_180B978);
}

/*
 *  loc_40327E: count how often each colour is used and put the common ones
 *  first.  With /kp off and no palette to start from, the palette *and* the
 *  colour hash were already built by the first pass over the image - this one
 *  only has to ask FindColor() how many pixels each entry accounts for.  That is
 *  why it does not call InitColorHash() or AddColor(): re-seeding the hash here
 *  would make every pixel look like a new colour and append a second copy of the
 *  palette on top of the first.
 *
 *  The counts and the colours are built in first-seen order and only sorted
 *  afterwards, so a colour's position here has nothing to do with the order the
 *  pixels appeared in.
 */
static void CountPalette(void)
{
    unsigned char *base = (unsigned char *)Image->pixels;
    int row, col;

    memset(PaletteUse, 0, sizeof(PaletteUse));

    for (row = 0; row < Image->height; row++) {
        unsigned char *p = base + (size_t)row * Image->stride;

        if (CheckEscHook(1))
            return;

        for (col = 0; col < Image->width; col++)
            PaletteUse[FindColor(*(int *)(p + (size_t)col * 4))]++;
    }

    SortPaletteByUse();
}

/*
 *  loc_403271: install `count` entries into the colour hash, last first.  The
 *  order matters: the hash chains on equality, and chaining from the top down
 *  means FindColor reports the *lowest* index of a colour, which is the one the
 *  LZ coder wants - it lets the two runs of equal colours share a match.
 */
static void HashPalette(int count)
{
    int i;

    InitColorHash(unk_180B578);
    for (i = count - 1; i >= 0; i--)
        AddColor(unk_180B578[i], i);
}

/* ------------------------------------------------------------------ */
/*  how big is the PNG going to be?                                     */
/* ------------------------------------------------------------------ */

/*
 *  loc_4034C8: a closed form upper bound on the output, used for two things -
 *  sizing the work buffer, and as the `/s4` fallback size.  2111 bytes is the
 *  fixed part: the signature, a maximal IHDR, a 256 entry PLTE with its tRNS,
 *  the largest sBIT and bKGD, a handful of ancillary chunks and IEND.  The rest
 *  is the image data at the *worst* case for that colour type:
 *
 *      0   one byte per pixel plus the filter byte, and 14 more if a tRNS is
 *          needed (which is what a dark MinSample implies)
 *      2   three bytes per pixel, plus 18 for the tRNS
 *      3   the PLTE, then as colour type 0
 *      4   two bytes per pixel plus three per row
 *      6   four bytes per pixel plus six per row
 *
 *  Anything already in the file that we are keeping is added on top, length +
 *  12 for the chunk framing.
 */
static int EstimateSize(void)
{
    int size = 2111;
    int i;

for (i = 0; i < Image->chunkend; i++) {
        unsigned char *c = (unsigned char *)Image->chunks[i];

        size += Swap32(*(unsigned int *)c) + 12;
    }

    switch (unk_185BED0) {
    case 0:
        size += Image->height *
                (((unk_180A570 * Image->width + 7) >> 3) + 6);
        if (unk_185BED0 == 0 && MinSample == -1)
            size += 14;
        break;

    case 2:
        size += Image->height * (Image->width + 2) * 3;
        if (MinSample == -1)
            break;
        size += 18;
        break;

    case 3:
        size += 4 * unk_180B978 + 24 +
                Image->height * (((unk_180A570 * Image->width + 7) >> 3) + 6);
        break;

    case 4:
        size += 2 * Image->height * (Image->width + 3);
        break;

    case 6:
        size += Image->height * (4 * Image->width + 6);
        break;

default:
        break;
    }

    return size;
}

/* ------------------------------------------------------------------ */
/*  phase C: the trials                                                 */
/* ------------------------------------------------------------------ */

/*
 *  loc_40382F: seed the row records for a block.
 *
 *  Every block starts its first row with a *hypothetical* set of code lengths
 *  rather than with frequencies, because there are no frequencies yet - this is
 *  the state a block is in before a single row has been fed to it.  Three
 *  presets:
 *
 *      Image->strategy >= 3  a flat 8 bit code everywhere.  The strategies from 3
 *                            up are the ones that only look at the raw stream, so
 *                            they do not care about a good starting tree.
 *      Image->randomize      random lengths from 0x4000 up, seeded from the
 *                            performance counter, and code lengths built from
 *                            them.  This is /r: it gives a run of identical
 *                            trials different starting points, which is the only
 *                            way more than one trial can help when the options
 *                            are fixed.
 *      otherwise             the built-in default: 8 bits for the literals, 9 for
 *                            the lengths and distances, and the 4 bit codes the
 *                            length/distance tables start at.
 *
 *  Where the seeds land matters, and it is worth spelling out what the original
 *  writes.  All three presets write one contiguous run of 320 dwords starting at
 *  field +0x510 - the row's *code length* array, whose entries the LZ coder reads
 *  back as `lens[b]`, and which AdvanceRow() rebuilds from the samples.  It is
 *  indexed here as `code[i]` below:
 *
 *      i 0 .. 143      a literal's length
 *      i 144 .. 255    a literal's length
 *      i 256 .. 285    a length code's length, 7 bits to start with
 *      i 286 .. 287    unused, zero
 *      i 288 .. 317    a distance code's length
 *      i 318 .. 319    unused, zero
 *
 *  The original reaches these through three different addresses - `[ebx+500h]`
 *  for the first 144, `unk_42EDE0 + 0x2C8` for the next 112 and `0x42F268` for
 *  the rest - and they happen to be one array.  Reproducing them as separate
 *  named fields is what went wrong before: `samples + ROW_SYMBOLS` is field
 *  +0x520, sixteen bytes *past* where the literal lengths belong, so every
 *  literal's length stayed zero and no match could ever be cheaper than a
 *  literal, which is why the compressed path coded whole images as literals.
 */
static void SeedBlockTables(int block)
{
    int *samples = ROWOF(block, ROW_SAMPLES);
    int *code    = ROWOF(block, ROW_SYMBOLS);
    int i;

    if (Image->strategy >= 3) {
        /* 8 bits everywhere, then 9 at index 128, then 8 on, then all zeros */
        memset32(code + 0, 8, 0x80);
        code[0x80] = 9;
        memset32(code + 0x81, 8, 0x7F);
        code[0x100] = 9;
        memset32(code + 0x101, 0, 0x3F);
    } else if (Image->randomize) {
        LARGE_INTEGER tick;

        /* /r: a different starting tree every run, from the clock */
        QueryPerformanceCounter(&tick);
        srand((unsigned int)tick.LowPart);

        for (i = 0; i < ROW_SAMPLE_SYMS; i++)
            samples[i] = (rand() & 0x7FFF) + 0x4000;
        samples[286] = 0;                   /* 0x42EDE0, 0x42EDE4: the two
                                               samples the 286 entry build
                                               skips over */
        samples[287] = 0;
        for (i = 0; i < ROW_DISTSLOTS; i++)
            ROWOF(block, ROW_DISTFREQ)[i] = (rand() & 0x7FFF) + 0x4000;
        samples[318] = 0;
        samples[319] = 0;

        BuildCodeLengths(samples, ROW_SAMPLE_SYMS, code, 16);
        BuildCodeLengths(ROWOF(block, ROW_DISTFREQ), ROW_DISTSLOTS,
                         ROWOF(block, ROW_DISTBITS), 16);
    } else {
        /* the default seed */
        memset32(code + 0, 8, 0x90);         /* literals 0 .. 143    */
        memset32(code + 144, 9, 0x70);       /* literals 144 .. 255  */
        memset32(code + 256, 7, 0x1A);       /* length codes 0 .. 25 */

        code[0x11A] = 8;                     /* field +0x978: the four that */
        code[0x11B] = 8;                     /* the reverse slot builders  */
        code[0x11C] = 8;                     /* start from, then go over  */
        code[0x11D] = 8;                     /* field +0x98C with zeros  */
        code[0x11E] = 0;
        code[0x11F] = 0;                     /* field +0x990: the distance */
        code[0x120] = 4;                     /* codes start at 4 bits     */
        code[0x121] = 4;

        memset32(code + 0x122, 5, 0x1C);     /* field +0x998 .. +0xA04    */
        code[0x13E] = 0;                     /* field +0xA08: unused     */
        code[0x13F] = 0;
    }
}

/*
 *  loc_403AF4: the progress line and the ESC poll, both of which happen once per
 *  row before anything is filtered.  The percentage is a fixed point multiply of
 *  the row counter by the scale sub_4086B0 worked out, kept in Image->percent so
 *  it only prints when it moves.
 *
 *  The multiply is 32x32 -> 64 and the result is shifted back down by 32, which
 *  in C has to be spelled out: a plain `a * b >> 32` overflows the 32 bit
 *  product first, and every run prints a percentage in the hundreds of millions.
 */
static int RowProgress(int rowacc)
{
    unsigned int pct = (unsigned int)
        (((unsigned __int64)(unsigned int)(rowacc + PercentBias)
          * PercentScale) >> 32);

    if ((int)pct > Image->percent) {
        Image->percent = (int)pct;
        ProgressMsg("%2d%% \r", (int)pct);
    }

    return CheckEscHook(1);
}

/*
 *  loc_403B65: pick the filter for one row.
 *
 *  `/f5` - "mixed" - is the interesting one.  The row is filtered once per
 *  candidate filter with the histogram sink in place, and the candidate whose
 *  histogram costs the least entropy wins.  The row is then filtered *again*
 *  with the winner and the real sink, so it is filtered six times in total, but
 *  only the last pass reaches the compressor - the first five exist purely to
 *  measure, which is why they are cheap: counting a byte is a great deal less
 *  work than matching and coding it.
 *
 *  `/f6` instead takes the filter from the caller's per-row table (Block), which
 *  is how "reuse the filters that worked last time" is expressed.
 *
 *  Returns -1 if the entropy table could not be allocated, which is the one way
 *  the scoring can fail.
 */
static int ChooseRowFilter(int filtermode, int row, void (*feedrow)(unsigned char *))
{
    unsigned char *src = (unsigned char *)Image->pixels + (size_t)row * Image->stride;

    if (filtermode == 5) {
        int best = 5, bestcost = 0x7FFFFFFF, f;
        ByteSinkFn saved = ByteSink;

        for (f = 0; f < 5; f++) {
            unsigned int cost;

            TrialFilter = f;
            ByteSink = CountByte;
            memset(FilterHistogram, 0, sizeof(FilterHistogram));
            FilterByteCount = 0;

            feedrow(src);

            cost = EstimateEntropy(FilterHistogram, 256);
            if (cost == 0x80000000u)
                return -1;
            if ((int)cost < bestcost) {
                bestcost = (int)cost;
                best = f;
            }
        }

        TrialFilter = best;
        ByteSink = saved;
    } else if (filtermode == 6 && FilterChoice != NULL) {
        TrialFilter = FilterChoice[row];
    }

    return 0;
}

/* ---- one row of colour type 2 / 4 / 6: one pixel is one sample group ---- */

static void FeedRgbRow(unsigned char *src)
{
    int col;

    for (col = 0; col < Image->width; col++)
        FilterPixel24(*(int *)(src + (size_t)col * 4) & 0xFFFFFF);
}

static void FeedGrayAlphaRow(unsigned char *src)
{
    int col;

    for (col = 0; col < Image->width; col++)
        FilterPixel16(*(int *)(src + (size_t)col * 4));
}

static void FeedRgbaRow(unsigned char *src)
{
    int col;

    for (col = 0; col < Image->width; col++)
        FilterPixel32(*(int *)(src + (size_t)col * 4));
}

/*
 *  loc_404092: colour types 0 and 3 do need a lookup - a palette image is coded
 *  by index and a greyscale image by its place on the ramp - so each pixel is
 *  turned into an index by FindColor() and the indices are then packed at the
 *  bit depth before going to the filter.
 *
 *  The packing counts down from the top of the byte, so the first sample of a
 *  group lands in the high bits, which is what PNG wants.  A group is flushed
 *  when it fills *or* when the row runs out, so a width that is not a multiple
 *  of the group size still gets its last partial byte out.
 */
static void FeedIndexedRow(unsigned char *src)
{
    int perbyte = 8 / unk_180A570;
    int col, shift = perbyte - 1, packed = 0;

    if (unk_180A570 == 8) {
        for (col = 0; col < Image->width; col++)
            FilterByte(FindColor(*(int *)(src + (size_t)col * 4)));
        return;
    }

    for (col = 0; col < Image->width; col++) {
        int idx = FindColor(*(int *)(src + (size_t)col * 4));

        packed |= idx << (shift * unk_180A570);
        if (shift == 0 || col == Image->width - 1) {
            FilterByte(packed);
            packed = 0;
            shift = perbyte - 1;
        } else {
            shift--;
        }
    }
}

/*
 *  loc_403AF4: one pass over the image, calling `feedrow` once per row with the
 *  filter for that row already chosen.  This is the body every colour type's
 *  row loop shares; the progress line and the ESC poll happen here because they
 *  are the same for all of them.
 *
 *  Returns -1 if ESC was pressed or a filter could not be scored.
 */
static int RunRowLoop(int filtermode, void (*feedrow)(unsigned char *))
{
    int rowacc = 0, row;

    for (row = 0; row < Image->height; row++) {
        unsigned char *src =
            (unsigned char *)Image->pixels + (size_t)row * Image->stride;

        if (RowProgress(rowacc))
            return -1;

        if (ChooseRowFilter(filtermode, row, feedrow))
            return -1;

        feedrow(src);                /* the winning filter, for real this time */
        rowacc += 100;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  the trial bookkeeping                                              */
/* ------------------------------------------------------------------ */

/*
 *  loc_403A5E: take a copy of the state the trial just decided on, 320 bytes to
 *  a block, into the contiguous array the trial records are built from.
 *
 *  The bytes come from dword_42EE68, which is the row record's symbol stream at
 *  +0x510 - *not* the record's first field.  The copy loop reads one byte per
 *  dword and steps the record by its 0xA18 stride, so it takes dwords 0 .. 79 of
 *  that field.
 */
static void SnapshotRowFilters(void)
{
    int b, n = RowCount;

    for (b = 0; b < n; b++)
        memcpy(RowFilters + b * 320, ROWOF(b, ROW_SYMBOLS), 320);
}

/* and put a saved copy back, which is how a losing trial is undone */
static void RestoreRowFilters(void)
{
    int b, n = RowCount;

    for (b = 0; b < n; b++)
        memcpy(ROWOF(b, ROW_SYMBOLS), BestRowFilters + b * 320, 320);
}

/*
 *  loc_404A0E: fingerprint a trial, so the loop can tell whether it has stopped
 *  making progress.
 *
 *  The fingerprint is a CRC over the 320 filter choices (taken two at a time, as
 *  16 bit values) and, for a palette, over the palette bytes as well - because
 *  two trials that pick the same filters but different palettes are different
 *  trials and either may still improve on the last.
 *
 *  The result is the pair (fingerprint, size).  When a trial reproduces the pair
 *  of some earlier trial, the search has come full circle: the row records are
 *  back where they started, so no number of further trials will do better, and the
 *  loop stops.
 */
static unsigned int TrialFingerprint(int trial, int nbytes)
{
    unsigned int crc = 0xFFFFFFFFu;
    int i;

    for (i = nbytes - 2; i >= 0; i -= 2) {
        unsigned int pair = RowFilters[i] + 16u * RowFilters[i + 1];

        crc = CrcTable[(crc ^ pair) & 0xFF] ^ (crc >> 8);
    }

    if (unk_185BED0 == 3) {
        unsigned char *pal = (unsigned char *)unk_180B578;

        for (i = 1023; i >= 0; i--)
            crc = CrcTable[(crc ^ pal[i]) & 0xFF] ^ (crc >> 8);
    }

    TrialSign[trial] = (int)crc;
    return crc;
}

/* ------------------------------------------------------------------ */
/*  Optimize()                                                         */
/* ------------------------------------------------------------------ */

/*
 *  sub_402AD0.  See the file header for what this is for; the phases below run
 *  in the order the original runs them and the comments name the addresses.
 */
int Optimize(SETTINGS *s)
{
    unsigned int flags = 0;
    int ncolors = 0;
    int estimate, filtermode, i;

    /* ---- loc_402AD3: publish the settings ---- */

    Image = (IMAGEDESC *)s;

    ProgressMsg = s->msg ? s->msg : (void (*)(const char *, ...))NothingChanged;
    CheckEscHook = s->checkesc ? s->checkesc
                               : (int (*)(int))NothingChanged;

    unk_180B978 = s->palflag;
    unk_185BED0 = s->coltype;
    TrialFilter = s->filter;
    unk_180A570 = s->bitdepth;

    /*
     *  loc_402B66.  The two fields cleared here are the progress counter and
     *  the trial counter - *not* the output buffer's size.  That one arrives as
     *  the size of the file being optimised and is the capacity the writers
     *  bounds-check against, so zeroing it here would silently drop every byte
     *  of the output.
     */
    Image->percent = 0;
    Image->trial = 0;
    MinSample = -1;

    /* ---- loc_402ED4: /f with no number means "pick per row" ---- */

    if (TrialFilter < 0)
        TrialFilter = (unk_185BED0 == 3) ? 0 : 5;

    /* ---- loc_402B94: work out what can represent this image ---- */

    if (unk_185BED0 != 6) {
        ScanImage(&flags, &ncolors);

        if (CheckEscHook(1))
            return Fail();

        if (!CheckColType(flags, ncolors))
            return Fail();
    }

    /* ---- loc_402ED4: fill the palette for the chosen colour type ---- */

    if (unk_185BED0 < 0)
        unk_185BED0 = SuggestColType(flags, ncolors);

    if (unk_185BED0 == 0) {
        int levels, depth;

        /*
         *  loc_402F51: mark every grey level the image actually uses.  Only the
         *  low byte matters - a greyscale sample is R == G == B, so byte 0 names
         *  it - and the scan is done again here rather than reused from
         *  ScanImage(), because that pass was building a palette for the
         *  colour-type decision and may never have run at all (/c0 given
         *  explicitly skips it).
         */
        memset32(PaletteUse, 0, 0x100);

        /*
         *  loc_402F3E: a bKGD chunk in a greyscale image names a grey *level*,
         *  so it has to be marked in use before the scan or the level analysis
         *  would not know to keep it.  A bKGD that is not itself grey is fatal -
         *  there is no way to write it in colour type 0.
         */
        if (Image->bkgd != 0) {
            if ((unsigned)(((((Image->bkgd & 0xFFFFu) << 8) ^
                             (Image->bkgd & 0xFFFF00u)) & 0xFFFF00u)) != 0) {
                ProgressMsg("\rbKGD must be grayscale in color type 0.");
                return Fail();
            }
            PaletteUse[Image->bkgd & 0xFF] = 1;
        }

        for (i = 0; i < Image->height; i++) {
            unsigned char *p = (unsigned char *)Image->pixels +
                               (size_t)i * Image->stride;
            int c;

            if (CheckEscHook(1))
                return Fail();
            for (c = 0; c < Image->width; c++)
                PaletteUse[p[4 * c]] = 1;
        }

        levels = GrayLevelsUsed();
        depth = levels + 1;

        if (unk_180A570 != 1 && unk_180A570 != 2 &&
            unk_180A570 != 4 && unk_180A570 != 8) {
            unk_180A570 = depth;
        } else if (depth > unk_180A570) {
            ProgressMsg("\rImage doesn't fit in selected bitdepth");
            return Fail();
        }

        BuildGrayPalette(unk_180A570);
    } else if (unk_185BED0 == 3) {
        if (s->palflag != 0) {
            /* /ks or a PNG that carried a PLTE: keep the palette we were given */
            HashPalette(unk_180B978);
            if (!s->keeppal) {
                int row, col;

                memset(PaletteUse, 0, sizeof(PaletteUse));
                for (row = 0; row < Image->height; row++) {
                    unsigned char *p = (unsigned char *)Image->pixels +
                                       (size_t)row * Image->stride;

                    if (CheckEscHook(1))
                        return Fail();
                    for (col = 0; col < Image->width; col++)
                        PaletteUse[FindColor(
                            *(int *)(p + (size_t)col * 4))]++;
                }
                CompactKeptPalette();
            }
        } else {
            if (s->keeppal)
                ProgressMsg("\nIgnoring /kp (palette not found)");

            CountPalette();
            if (CheckEscHook(1))
                return Fail();
        }

        if (!PaletteBitDepth()) {
            ProgressMsg("\rImage doesn't fit in selected bitdepth");
            return Fail();
        }

        if (!s->keeppal)
            OpaqueToFront();
    } else if (unk_180A570 != 8) {
        /* colour types 2, 4 and 6 are always 8 bits a sample */
        unk_180A570 = 8;
    }

    /* ---- loc_403178: say what was decided ---- */

    ProgressMsg(" /c%d /f%d", unk_185BED0, TrialFilter);
    if (unk_185BED0 == 3)
        ProgressMsg(" /d%d, %d colors", unk_180A570, unk_180B978);
    else if (unk_185BED0 == 0)
        ProgressMsg(" /d%d", unk_180A570);

    /* the six fields the bit writer reads back out of Image */
    Image->palpalette  = unk_180B578;   /* +0x38: the palette it will write */
    Image->palcount    = unk_180B978;   /* +0x3C */
    Image->filtermode  = TrialFilter;   /* +0x40 */
    Image->outcoltype  = unk_185BED0;   /* +0x44 */
    Image->outbitdepth = unk_180A570;   /* +0x48 */

    if (CheckEscHook(2))
        return Fail();

    /* ---- loc_4034C8: size the output buffer ---- */

    estimate = EstimateSize();
    TrialEstimate = estimate;

    if (estimate < Image->outsize)
        Image->outsize = estimate;
    if (Image->outbuf == NULL) {
        if (Image->outsize == 0)
            Image->outsize = estimate;
        Image->outbuf = (unsigned char *)malloc((size_t)Image->outsize);
        if (Image->outbuf == NULL) {
            ProgressMsg("\rmalloc failed (gpo->kbuf)");
            return Fail();
        }
    }

    if (Image->verbose) {
        QueryPerformanceFrequency(&ClockFrequency);
        QueryPerformanceCounter(&StartCount);
    }

    InitCrcTable();

    /* ---- loc_403560: the /s4 fast path ---- */

    if (Image->strategy == 4) {
        if (estimate > Image->outsize) {
            Image->outsize = estimate;
            if (Image->outbuf != NULL)
                free(Image->outbuf);
            Image->outbuf = (unsigned char *)malloc((size_t)Image->outsize);
        }
        if (Image->outbuf != NULL) {
            if (WriteStored())
                goto finished;
        } else {
            ProgressMsg("\r!malloc failed (gpo->kbuf)");
        }
        return Fail();
    }

    /* ---- loc_403609: set up the block splitting pass ---- */

    if (Image->maxblocks > 0 || Image->splitthreshold > 0.0) {
        /*
         *  /n or /b.  A first pass runs the whole image through the filters with
         *  PutByte() as the sink, which collects the *filtered* bytes into a flat
         *  buffer; PlanBlocks() then cuts that buffer into blocks.  The trial
         *  counter starts at -1 to mark that this collecting pass is running, and
         *  the real trials begin at 0.
         */
        Image->trial = -1;
        ByteSink = PutByte;

        SplitBuf = (unsigned char *)malloc((size_t)Image->height *
                                           (4 * (size_t)Image->width + 1));
        if (SplitBuf == NULL) {
            ProgressMsg("\rmalloc failed");
            return Fail();
        }
        SplitEnd = 0;
        NumBlockList = 1;
    } else {
        Image->trial = 0;
    }

    HaveBestTrial = 0;
    BestTrialSize = 0x7FFFFFFF;
    filtermode = TrialFilter;

    InitProgressScale(Image->height);

    /* ---- loc_4036D0: the trial loop ---- */

    for (;;) {
        int trial = Image->trial;
        int collecting = (trial < 0);

        if (trial == 0) {
            if (Image->maxblocks > 0 || Image->splitthreshold > 0.0) {
                if (PlanBlocks(0, SplitEnd) < 0) {
                    if (SplitBuf != NULL) {
                        free(SplitBuf);
                        SplitBuf = NULL;
                    }
                    return Fail();
                }
                if (SplitBuf != NULL) {
                    free(SplitBuf);
                    SplitBuf = NULL;
                }
                if (Image->verbose) {
                    int b;

                    ProgressMsg("Out:\nBlock boundaries: ");
                    if (NumBlockList <= 1) {
                        ProgressMsg("none");
                    } else {
                        for (b = 0; b < NumBlockList - 1; b++) {
                            if (b != 0)
                                ProgressMsg(",");
                            ProgressMsg("%x", ROW_LIMIT(b));
                        }
                    }
                    ProgressMsg(" (%d block", NumBlockList);
                    if (NumBlockList > 1)
                        ProgressMsg("s");
                    ProgressMsg(")\n\n");
                }
            } else {
                NumBlockList = 1;
                if (Image->verbose)
                    ProgressMsg("\n");
            }

            for (i = 0; i < NumBlockList; i++)
                SeedBlockTables(i);

            ByteSink = CompressByte;
            collecting = (Image->trial < 0);
        }

        if (!collecting)
            SnapshotRowFilters();

        ProgressMsg("\r");
        Image->percent = -1;

        /* one trial: build the PNG and run every row through it */

        switch (unk_185BED0) {
        case 0:
        case 3:
            HashPalette(unk_180B978);
            if (!EmitBlock((unsigned)Image->width, (unsigned)Image->height,
                           unk_185BED0, unk_180A570, unk_180B578, unk_180B978))
                return Fail();
            if (RunRowLoop(filtermode, FeedIndexedRow))
                return Fail();
            break;

        case 2:
            if (!EmitBlock((unsigned)Image->width, (unsigned)Image->height, 2, 8,
                           NULL, 0))
                return Fail();
            if (RunRowLoop(filtermode, FeedRgbRow))
                return Fail();
            break;

        case 4:
            if (!EmitBlock((unsigned)Image->width, (unsigned)Image->height, 4, 8,
                           NULL, 0))
                return Fail();
            if (RunRowLoop(filtermode, FeedGrayAlphaRow))
                return Fail();
            break;

        case 6:
            if (!EmitBlock((unsigned)Image->width, (unsigned)Image->height, 6, 8,
                           NULL, 0))
                return Fail();
            if (RunRowLoop(filtermode, FeedRgbaRow))
                return Fail();
            break;

        default:
            ProgressMsg("\rPNG color type %d not supported\n", unk_185BED0);
            return Fail();
        }

        /* ---- loc_40492D: record this trial and decide whether to go on ---- */

        Image->trial++;
        trial = Image->trial;
        if (trial <= 0)
            continue;                   /* the collecting pass just finished */

        if (HaveBestTrial)
            break;

        if (RowCost > 0) {
            int found = -1;

            if (trial >= (int)(sizeof(TrialSizes) / sizeof(TrialSizes[0])))
                break;

            TrialSizes[trial] = OutCount;

            if (TrialSizes[trial] < BestTrialSize) {
                BestTrialSize = TrialSizes[trial];
                memcpy(BestRowFilters, RowFilters,
                       (size_t)320 * NumBlockList);
                memcpy(BestPalette, unk_180B578, 0x400);
            }

            TrialFingerprint(trial, 320 * NumBlockList);

            /*
             *  Has this trial reproduced an earlier one?  Scanning backwards is
             *  what makes the *most recent* repeat the one that ends the search:
             *  a repeat of something far back means the search has been cycling.
             */
            for (i = trial - 1; i >= 0; i--) {
                if (TrialSign[trial] == TrialSign[i] &&
                    TrialSizes[trial] == TrialSizes[i]) {
                    found = i;
                    break;
                }
            }

            if (found >= 0) {
                if (TrialSizes[trial] <= BestTrialSize)
                    break;              /* converged, and not an improvement */
                if (NumBlockList > 0)
                    RestoreRowFilters();
                memcpy(unk_180B578, BestPalette, 0x400);
                HaveBestTrial = 1;
            }
        } else {
            /*
             *  RowCost of zero means the row records never produced a cost, so
             *  there is nothing to compare and the first trial is taken as it
             *  stands.
             */
            if (OutCount > BestTrialSize) {
                if (NumBlockList > 0)
                    RestoreRowFilters();
                memcpy(unk_180B578, BestPalette, 0x400);
                HaveBestTrial = 1;
            }
            break;
        }

        if (CheckEscHook(0)) {
            ProgressMsg("\nEarly ESC! ");
            break;
        }
    }

    /*
     *  Loc_404B60: the best trial's bytes are in Image->outbuf and OutCount is
     *  its size.  If that beats the closed-form estimate, keep it; otherwise the
     *  stored (/s4) form is smaller still and is written instead.
     */
    if (TrialEstimate != 0 && OutCount > TrialEstimate) {
        if (OutCount > Image->outsize) {
            Image->outsize = OutCount;
            if (Image->outbuf != NULL)
                free(Image->outbuf);
            Image->outbuf = (unsigned char *)malloc((size_t)Image->outsize);
        }
        if (Image->outbuf != NULL) {
            if (WriteStored())
                goto finished;
        } else {
            ProgressMsg("\r!malloc failed (gpo->kbuf)");
        }
        return Fail();
    }

finished:
    if (Image->verbose) {
        LARGE_INTEGER now;

        QueryPerformanceCounter(&now);
        ProgressMsg("Took %.3f sec.\n",
                    (double)(now.QuadPart - StartCount.QuadPart) /
                    (double)ClockFrequency.QuadPart);
    }

    if (EntropyLut != NULL) {
        free(EntropyLut);
        EntropyLut = NULL;
    }

    return OutCount;
}
