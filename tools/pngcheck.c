/*
 *  pngcheck.c   -   dump Optimize()'s output for tools\pngcheck.py to verify
 *
 *  The unit test in tools\test_chunk.c checks Optimize()'s output
 *  *structurally* - signature, IHDR, an IDAT is present, IEND closes it.  That
 *  is not enough: a deflate stream can be perfectly shaped and still not
 *  inflate, which is exactly what happened to WriteStored() (right shape, wrong
 *  Adler-32 seed).  This driver exists so the stream can be handed to a real
 *  inflater.
 *
 *  For each case it builds an image, runs Optimize() over it, and writes two
 *  files:
 *
 *      build\png_N.png    what Optimize() produced
 *      build\png_N.ref    the source image, one 0xAARRGGBB dword per pixel
 *
 *  pngcheck.py then inflates the PNG, undoes the per row filters, and checks the
 *  samples against the .ref.  That closes the loop over everything between the
 *  pixels and the wire: the filters, the LZ coder, the Huffman codes, the block
 *  splitting and the chunk framing.
 *
 *  Not part of pngout.exe.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pngout.h"

#define MAXPIX (4 * 96 * 96)

static unsigned char pixels[MAXPIX];
static unsigned char outbuf[1 << 20];

/*
 *  The one chunk Optimize() expects the caller to have kept: IEND.
 *
 *  The compressed path never writes IEND itself - FinishBlock() emits the tail
 *  of the kept chunk list - so the record has to be there for it to close the
 *  file.  A chunk record is [length][tag][data][crc] with the length stored
 *  little endian and everything else verbatim, so IEND's is these twelve bytes.
 *  With chunkbeg == chunkmid == chunktail == 0 and chunkend == 1,
 *  WritePngHeader() emits nothing and FinishBlock() writes the IEND last.
 *
 *  /s4 is the exception: WriteStored() closes the file with its own IEND, so
 *  handing it the same record would emit it twice (and it does - see the
 *  "known defect" note in README.md).  The list is therefore left empty there.
 */
static unsigned char iend_rec[12] = {
    0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82
};
static unsigned int *chunklist[1];

/* dword_185BEC4: swallow the progress chatter. */
static void quiet(const char *fmt, ...)
{
    (void)fmt;
}

static int esc_never(int unused)
{
    (void)unused;
    return 0;
}

static int ncase;
static int only = -1;          /* argv[1]: run just this case */
static char *alsorun;          /* argv[2]: a comma separated list, or NULL */
static int ask_randomize;      /* /r: consumed by the next run_case() call */

/* is this case wanted? */
static int wanted(int n)
{
    const char *p;

    if (only == n)
        return 1;
    if (alsorun == NULL)
        return only < 0;               /* no selector at all: run them all */
    for (p = alsorun; *p; p++) {
        if (*p >= '0' && *p <= '9' && *p - '0' == n)
            return 1;
    }
    return 0;
}

/*
 *  A deterministic image with every channel distinct, so a channel swap cannot
 *  pass, plus enough distinct colours that PNGOUT has to make a real decision
 *  about the colour type rather than falling into a palette by default.
 *  `mode` picks the shape:
 *      0   a smooth ramp - long matches, filters that help
 *      1   a flat field  - maximal runs
 *      2   noise         - incompressible, so the trials have to work for it
 *      3   blocks        - flat regions of differing colour, for /b splitting
 *      4   grey with a transparent strip, so the image needs an alpha channel
 *      5   the same grey level opaque *and* see-through, which raises the
 *          "needs alpha" flag on its own
 *
 *  `shape` is 1 for a /c4 case, see the note at the bottom.
 */
static void build_image(int w, int h, int mode, unsigned int seed, int shape)
{
    int r, c;

    for (r = 0; r < h; r++) {
        for (c = 0; c < w; c++) {
            unsigned char *p = pixels + 4 * (r * w + c);
            unsigned int v;

            switch (mode) {
            case 0:
                v = 0xFF000000u |
                    ((unsigned)((r * 7 + c * 3) & 0xFF) << 16) |
                    ((unsigned)((r * 3 + c * 5) & 0xFF) << 8) |
                    ((unsigned)((r + c * 2) & 0xFF));
                break;

            case 1:
                v = 0xFF000000u | 0x203040u;
                break;

            case 2:
                v = 0xFF000000u |
                    ((unsigned)((c * 97 + r * 31 + seed) & 0xFF) << 16) |
                    ((unsigned)((c * 61 + r * 17 + seed) & 0xFF) << 8) |
                    ((unsigned)((c * 29 + r * 53 + seed) & 0xFF));
                break;

            case 4:
                /*
                 *  Grey, with the leftmost third of every row replaced by a
                 *  fully transparent black.  One transparent colour only, so
                 *  the classification does not force an alpha channel and the
                 *  result is colour type 0 with a tRNS - the ramp palette can
                 *  only name one see-through colour, since FindColor() matches
                 *  the whole DWORD including the alpha byte.
                 */
                v = 0xFF000000u |
                    ((unsigned)((c * 5 + r * 3 + seed) & 0xFF) << 16) |
                    ((unsigned)((c * 5 + r * 3 + seed) & 0xFF) << 8) |
                    ((unsigned)((c * 5 + r * 3 + seed) & 0xFF));
                if (c < w / 3)
                    v = 0;
                break;

            case 5:
                /*
                 *  The same grey level both opaque and see-through, which is
                 *  what raises the "needs alpha" flag - so this one comes out
                 *  as a palette with a tRNS rather than as greyscale.  64
                 *  levels keeps it to 128 distinct colours, well inside the
                 *  256 a palette can hold: a colour the palette cannot name is
                 *  looked up and missed, and FindColor() returns -1, which
                 *  reaches the filter as the byte 0xFF.
                 */
                v = 0xFF000000u |
                    ((unsigned)(((c * 5 + r * 3 + seed) >> 1) & 0x3F) << 16) |
                    ((unsigned)(((c * 5 + r * 3 + seed) >> 1) & 0x3F) << 8) |
                    ((unsigned)(((c * 5 + r * 3 + seed) >> 1) & 0x3F));
                if (c < w / 3 && ((r + c) & 1) == 0)
                    v &= 0x00FFFFFFu;
                break;

            default:
                v = 0xFF000000u |
                    ((unsigned)(((r / 4) * 0x40 + seed) & 0xFF) << 16) |
                    ((unsigned)(((c / 4) * 0x10) & 0xFF) << 8) |
                    ((unsigned)(((r / 4 + c / 4) * 0x20) & 0xFF));
                break;
            }

            /*
             *  Colour type 4 is the one type whose internal layout is not
             *  B, G, R, A: sub_407B00 emits bytes 0 and 1 of the sample, so
             *  the decoder has to leave the grey level in byte 0 and the
             *  alpha in byte 1 - which is what a /c4 case is handed here.
             */
            if (shape == 1) {
                unsigned int g = v & 0xFF;
                unsigned int a = v >> 24;

                v = g | (a << 8);
            }

            memcpy(p, &v, 4);
        }
    }
}

static void run_case(const char *name, int w, int h, int mode, unsigned int seed,
                     int strategy, int coltype, int filter, int nblocks,
                     double split)
{
    SETTINGS s;
    FILE *f;
    char path[64];
    int delta, i;

    if (!wanted(ncase)) {
        ncase++;
        return;
    }

    build_image(w, h, mode, seed, coltype == 4 ? 1 : 0);

    memset(&s, 0, sizeof(s));
    s.pixels    = pixels;
    s.pitch     = 4 * w;
    s.width     = w;
    s.height    = h;
    s.outbuf    = outbuf;
    s.origsize  = (int)sizeof(outbuf);
    s.msg       = quiet;
    s.checkesc  = esc_never;
    s.coltype   = coltype;
    s.filter    = filter;
    s.bitdepth  = -1;
    s.strategy  = strategy;
    s.blocksplit= split;
    s.nblocks   = nblocks;
    s.randomize = ask_randomize;
    ask_randomize = 0;

    /* the kept chunks: the IEND, which only the compressed path needs (it is */
    /* what closes the file); WriteStored() writes its own, see above        */
    if (strategy != 4) {
        chunklist[0] = (unsigned int *)iend_rec;
        s.chunkA  = 0;               /* +0x60 chunkbeg  */
        s.nIHDR   = 0;               /* +0x64 chunkmid  */
        s.nPLTE   = 0;               /* +0x68 chunktail */
        s.nchunks = 1;               /* +0x6C chunkend  */
        s.chunks  = chunklist;       /* +0x70           */
    }

    printf("-> %s ... ", name);
    fflush(stdout);

    delta = Optimize(&s);

    printf("%-26s %5d bytes  /c%d /d%d  %s\n", name, delta,
           unk_185BED0, unk_180A570,
           delta == 0x7FFFFFFF ? "FAILED" : "ok");
    fflush(stdout);

    if (delta == 0x7FFFFFFF) {
        ncase++;               /* a refusal is still a case: keep the numbering */
        return;
    }

    sprintf(path, "png_%d.png", ncase);
    f = fopen(path, "wb");
    if (f != NULL) {
        fwrite(outbuf, 1, (size_t)delta, f);
        fclose(f);
    }

    /*
     *  The reference is the *image*, not the output, so that pngcheck.py has to
     *  do the same job PNGOUT claims to: turn these pixels back out of a
     *  filtered, Huffman coded, deflated stream.
     */
    sprintf(path, "png_%d.ref", ncase);
    f = fopen(path, "wb");
    if (f != NULL) {
        fprintf(f, "%d %d\n", w, h);
        for (i = 0; i < w * h; i++) {
            unsigned int v = *(unsigned int *)(pixels + 4 * i);

            fprintf(f, "%08x\n", v);
        }
        fclose(f);
    }

    ncase++;
}

int main(int argc, char **argv)
{
    if (argc > 1)
        only = atoi(argv[1]);
    if (argc > 2)
        alsorun = argv[2];

    /* the deflate strategies, on a ramp and on noise */
    run_case("s0 ramp",       32, 24, 0, 0,  0, -1, -1, 0, 0.0);
    run_case("s1 ramp",       32, 24, 0, 0,  1, -1, -1, 0, 0.0);
    run_case("s2 ramp",       32, 24, 0, 0,  2, -1, -1, 0, 0.0);
    run_case("s3 ramp",       32, 24, 0, 0,  3, -1, -1, 0, 0.0);
    run_case("s4 ramp",       32, 24, 0, 0,  4, -1, -1, 0, 0.0);

    run_case("s0 noise",      32, 24, 2, 11, 0, -1, -1, 0, 0.0);
    run_case("s4 noise",      32, 24, 2, 11, 4, -1, -1, 0, 0.0);

    /* /r, which takes the third seed in SeedBlockTables(): random code lengths
       built from random frequencies, so the first row's code is *not* the
       built-in one.  The output is not reproducible run to run - it is seeded
       from the performance counter - but it still has to round trip. */
    ask_randomize = 1;
    run_case("r random",      32, 24, 0, 0,  0, -1, -1, 0, 0.0);

    /* flat, which is where the run coder earns its keep */
    run_case("s0 flat",       64, 40, 1, 0,  0, -1, -1, 0, 0.0);
    run_case("s2 flat",       64, 40, 1, 0,  2, -1, -1, 0, 0.0);
    run_case("s4 flat",       64, 40, 1, 0,  4, -1, -1, 0, 0.0);

    /* forced colour types, at 8 bits a sample so nothing is rescaled */
    run_case("c2 forced",     24, 16, 0, 0,  0, 2,  -1, 0, 0.0);
    run_case("c3 forced",     24, 16, 3, 0,  0, 3,  -1, 0, 0.0);
    run_case("c0 forced",     24, 16, 4, 1,  0, 0,  -1, 0, 0.0);
    /* c6 exercises FilterPixel32 end to end.  There is no /c4 case here: the
       internal layout of a grey+alpha pixel - which byte carries the alpha -
       is decided by the decoder, sub_409560, which is still a stub, and
       sub_407B00 emits bytes 0 and 1 whatever they are.  See README.md. */
    run_case("c6 forced",     24, 16, 4, 1,  0, 6,  -1, 0, 0.0);
    run_case("c4 automatic",  24, 16, 4, 1,  0, -1, -1, 0, 0.0);
    run_case("c6 automatic",  24, 16, 4, 1,  2, -1, -1, 0, 0.0);
    /* the "needs alpha" flag on its own: the output is a palette with a tRNS
       whose RGB entries repeat */
    run_case("alpha automatic", 24, 16, 5, 1,  0, -1, -1, 0, 0.0);

    /* fixed filters, so every one of the five is exercised for real */
    run_case("f0 fixed",      32, 24, 0, 0,  0, -1, 0, 0, 0.0);
    run_case("f1 fixed",      32, 24, 0, 0,  0, -1, 1, 0, 0.0);
    run_case("f2 fixed",      32, 24, 0, 0,  0, -1, 2, 0, 0.0);
    run_case("f3 fixed",      32, 24, 0, 0,  0, -1, 3, 0, 0.0);
    run_case("f4 fixed",      32, 24, 0, 0,  0, -1, 4, 0, 0.0);

    /* block splitting: an exact count and a threshold */
    run_case("n2 blocks",     48, 32, 3, 0,  0, -1, -1, 2, 0.0);
    run_case("n8 blocks",     48, 32, 3, 0,  0, -1, -1, 8, 0.0);
    run_case("b64 threshold", 48, 32, 3, 0,  0, -1, -1, 0, 64.0);
    run_case("b8 threshold",  48, 32, 3, 0,  0, -1, -1, 0, 8.0);

    /* odd widths, so a row that is not a whole number of samples still works */
    run_case("odd width",     17, 13, 0, 0,  0, -1, -1, 0, 0.0);
    run_case("odd width s2",  17, 13, 3, 3,  2, -1, -1, 0, 0.0);

    /* a one row image: the progress scale takes its `neg` branch here */
    run_case("one row",       32,  1, 0, 0,  0, -1, -1, 0, 0.0);

    printf("\n%d case(s) written\n", ncase);
    return 0;
}