/*
 *  test_chunk.c   -   checks the reconstructed chunk/format layer
 *
 *  Built by tools\selftest.cmd; not part of pngout.exe.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pngout.h"

static int failures;

static void check(const char *what, int got, int want)
{
    if (got != want) {
        printf("FAIL %-34s got %d want %d\n", what, got, want);
        failures++;
    } else {
        printf("ok   %-34s %d\n", what, got);
    }
}

/* DDS stores its header little endian. */
static void put(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* PNG stores its dimensions big endian. */
static void putbe(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static unsigned short rd16(const unsigned char *p)
{
    return (unsigned short)(((unsigned int)p[0] << 8) | p[1]);
}

int main(void)
{
    unsigned char buf[4096];
    int w, h, fmt, i;

    /* ---------------- Swap16 ---------------- */
    check("Swap16(0x1234)", Swap16(0x1234), 0x3412);
    check("Swap16(0x00FF)", Swap16(0x00FF), 0xFF00);

    /* ---------------- PNG ---------------- */
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x89; buf[1] = 'P'; buf[2] = 'N'; buf[3] = 'G';
    buf[4] = 0x0D; buf[5] = 0x0A; buf[6] = 0x1A; buf[7] = 0x0A;
    /* IHDR: length 13, then the tag, then width 640 height 480, both 4 byte */
    buf[8] = 0; buf[9] = 0; buf[10] = 0; buf[11] = 13;
    memcpy(buf + 12, "IHDR", 4);
    putbe(buf + 16, 640);
    putbe(buf + 20, 480);
    buf[24] = 8;                        /* bit depth  */
    buf[25] = 2;                        /* colour type */
    memset(buf + 33, 0, sizeof(buf) - 33);

    fmt = GetImageSize(buf, 100, &w, &h);
    check("PNG format code", fmt, FMT_PNG);
    check("PNG width",  w, 640);
    check("PNG height", h, 480);

/* IHDR is not always the first chunk: put a gAMA in front of it. */
    {
        unsigned char b2[4096];
        memcpy(b2, buf, sizeof(b2));
        /* gAMA record at 8: len 4, tag, data, crc -> next chunk at 24 */
        putbe(b2 + 8, 4);
        memcpy(b2 + 12, "gAMA", 4);
        /* move the IHDR record down to 24 */
        memcpy(b2 + 24, buf + 8, 12 + 13 + 4);
        fmt = GetImageSize(b2, 100, &w, &h);
        check("PNG with gAMA before IHDR: 640x480", w * 10000 + h, 640 * 10000 + 480);
    }

    /* truncated PNG */
    check("PNG truncated to 20 bytes", GetImageSize(buf, 20, &w, &h), FMT_NONE);

    /* ---------------- GIF ---------------- */
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "GIF89a", 6);
    buf[6] = 0x40; buf[7] = 0x00;          /* width  64 */
    buf[8] = 0xF0; buf[9] = 0x00;          /* height 240 */
    fmt = GetImageSize(buf, 64, &w, &h);
    check("GIF format code", fmt, FMT_GIF);
    check("GIF width",  w, 64);
    check("GIF height", h, 240);

    /* ---------------- BMP, BITMAPCOREHEADER ---------------- */
    memset(buf, 0, sizeof(buf));
    buf[0] = 'B'; buf[1] = 'M';
    buf[14] = 12;                          /* biSize */
    buf[18] = 100; buf[19] = 0;            /* width */
    buf[20] = 50;  buf[21] = 0;            /* height */
    buf[22] = 1;   buf[23] = 0;            /* planes */
    fmt = GetImageSize(buf, 64, &w, &h);
    check("BMP core format code", fmt, FMT_BMP);
    check("BMP core width",  w, 100);
    check("BMP core height", h, 50);

    /* ---------------- PCX ---------------- */
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x0A; buf[1] = 0x05; buf[2] = 0x01; buf[3] = 0x08;
    buf[4] = 10;  buf[5] = 0;              /* xmin */
    buf[6] = 20;  buf[7] = 0;              /* ymin */
    buf[8] = 109; buf[9] = 0;              /* xmax -> width 100 */
    buf[10] = 59; buf[11] = 0;             /* ymax -> height 40 */
    fmt = GetImageSize(buf, 64, &w, &h);
    check("PCX format code", fmt, FMT_PCX);
    check("PCX width from bounds",  w, 100);
    check("PCX height from bounds", h, 40);

    /* ---------------- DDS ---------------- */
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "DDS ", 4);
    put(buf + 4, 124);
    put(buf + 12, 200);                    /* height */
    put(buf + 16, 400);                    /* width  */
    fmt = GetImageSize(buf, 128, &w, &h);
    check("DDS format code", fmt, FMT_DDS);
    check("DDS width",  w, 400);
    check("DDS height", h, 200);

    /* ---------------- 8bpp palettised ---------------- */
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x19; buf[1] = 0x91;
    buf[2] = 16; buf[3] = 0;               /* width  16 */
    buf[4] = 9;  buf[5] = 0;               /* height 9  */
    buf[10] = 8;
    fmt = GetImageSize(buf, 64, &w, &h);
    check("PAL8 format code", fmt, FMT_PAL8);
    check("PAL8 width",  w, 16);
    check("PAL8 height", h, 9);

    /* ---------------- JPEG: walk to SOF0 ---------------- */
    memset(buf, 0, sizeof(buf));
    buf[0] = 0xFF; buf[1] = 0xD8;
    buf[2] = 0xFF; buf[3] = 0xE0;          /* APP0, length 0 */
    buf[6] = 0xFF; buf[7] = 0xDB;          /* DQT,  length 67, so it ends at 75 */
    buf[8] = 0; buf[9] = 67;
    buf[75] = 0xFF; buf[76] = 0xC0;        /* SOF0 */
    buf[77] = 0; buf[78] = 17; buf[79] = 8;
    buf[80] = 0x02; buf[81] = 0x00;        /* height 512 */
    buf[82] = 0x03; buf[83] = 0x20;        /* width  800 */
    fmt = GetImageSize(buf, 128, &w, &h);
    check("JPEG format code", fmt, FMT_JPEG);
    check("JPEG width",  w, 800);
    check("JPEG height", h, 512);

/* ---------------- ScanChunks ---------------- */
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x89; buf[1] = 'P'; buf[2] = 'N'; buf[3] = 'G';
    buf[4] = 0x0D; buf[5] = 0x0A; buf[6] = 0x1A; buf[7] = 0x0A;
    /*
     *  Record layout is len:4 tag:4 data:len crc:4, so the records are
     *  IHDR at 8 (13 bytes of data), gAMA at 33 (4 bytes), IEND at 49.
     */
    putbe(buf + 8,  13);
    memcpy(buf + 12, "IHDR", 4);
    putbe(buf + 16, 640);
    putbe(buf + 20, 480);
    putbe(buf + 33, 4);
    memcpy(buf + 37, "gAMA", 4);
    memcpy(buf + 53, "IEND", 4);

    Stream = NULL;                     /* keep the walker quiet */
    ChunkCount = ChunkCountIHDR = ChunkCountPLTE = -7;

    {
        char empty[] = "";
        ScanChunks(buf, 61, empty, -1, 0);
        check("ScanChunks no /k list records nothing", ChunkCount, 0);
    }
    {
        char one[] = "1";
        ScanChunks(buf, 61, one, -1, 0);
        check("ScanChunks /k1 keeps gAMA",  ChunkCount, 1);
        check("ScanChunks /k1 window = IHDR", ChunkCountIHDR, 1);
        check("ScanChunks /k1 PLTE count",   ChunkCountPLTE, 0);
    }
    {
        char gam[] = "gAMA";
        ScanChunks(buf, 61, gam, -1, 0);
        check("ScanChunks /kgAMA keeps gAMA", ChunkCount, 1);
    }
    {
        char gam2[] = "sRGB,tEXt,gAMA,iCCP";
        ScanChunks(buf, 61, gam2, -1, 0);
        check("ScanChunks /k list with 4 entries", ChunkCount, 1);
        check("ScanChunks /k list leaves buffer intact", gam2[4], ',');
    }
    {
        char nope[] = "sRGB,tEXt";
        ScanChunks(buf, 61, nope, -1, 0);
        check("ScanChunks /k with no match", ChunkCount, 0);
    }
    {
        char pal[] = "PLTE";
        ScanChunks(buf, 61, pal, -1, 0);
        check("ScanChunks /kPLTE skips critical chunks", ChunkCount, 0);
    }

/* ---------------- FilterPredict (sub_40AF40) ----------------
 *
 *  Returns whichever of the three candidates is closest to b, measured as
 *      |a3-a2|   vs   |a1-a2|   vs   |a1+a3-2*a2|
 *  picking a1 when the first wins, then a2 if |a1-a2| exceeds the third.
 */
    check("Predict(0,0,0)",     FilterPredict(0, 0, 0), 0);
    check("Predict(10,0,0)",    FilterPredict(10, 0, 0), 10);   /* a3 == a2 */
    check("Predict(1,2,3)",     FilterPredict(1, 2, 3), 2);      /* a1+a3-2a2 == 0 */
    check("Predict(100,7,0)",   FilterPredict(100, 7, 0), 100);  /* a1 wins on tie */
    check("Predict(-5,5,5)",    FilterPredict(-5, 5, 5), -5);     /* a1 wins on tie */
    check("Predict(255,0,255)", FilterPredict(255, 0, 255), 255); /* all equal */
    check("Predict(3,10,4)",    FilterPredict(3, 10, 4), 3);     /* a1 nearest */

    /* ---------------- CpuFeatures (sub_40B060) ---------------- */
    {
        unsigned f = CpuFeatures();
        unsigned r[4];

        /* the raw CPUID(1).EDX, so the mask applied by CpuFeatures can be
           checked against the real hardware */
        unsigned a, b, c, d;
        __asm {
            push ebx
            mov  eax, 1
            cpuid
            mov  a, eax
            mov  b, ebx
            mov  c, ecx
            mov  d, edx
            pop  ebx
        }
        r[3] = d;
        printf("info CPUID(1).EDX                  0x%08x\n", r[3]);
        printf("info   ... & 0x3FBFFFFF           0x%08x\n", r[3] & 0x3FBFFFFFu);
        printf("info CpuFeatures()                  0x%08x\n", f);
        check("CpuFeatures keeps bit 26 (SSE2)",
              ((f ^ (r[3] & 0x3FBFFFFFu)) & 0x04000000u) == 0, 1);
        check("CpuFeatures clears bit 22 unless forced",
              ((f ^ (r[3] & 0x3FBFFFFFu)) & 0x00400000u) == 0x00400000u, 1);
        check("CpuFeatures reports FPU (bit 0)", (f & 1u) != 0, 1);
        check("CpuFeatures reports MMX (bit 23)", (f & 0x00800000u) != 0, 1);
    }

    /* ---------------- optimiser primitives (src/optimize.c) ---------------- */

    InitCrcTable();
    /* the standard reflected CRC-32 table, polynomial 0xEDB88320 */
    check("CrcTable[0]",   (int)CrcTable[0],   0x00000000);
    check("CrcTable[1]",   (int)CrcTable[1],   0x77073096);
    check("CrcTable[2]",   (int)CrcTable[2],   0xEE0E612C);
    check("CrcTable[3]",   (int)CrcTable[3],   0x990951BA);
    check("CrcTable[255]", (int)CrcTable[255], 0x2D02EF8D);

    /* ---- Huffman heap ---- */
    InitHuff();
    Freq[10] = 5; Freq[20] = 1; Freq[30] = 9; Freq[40] = 3;
    HuffPush(10); HuffPush(20); HuffPush(30); HuffPush(40);
    /* the four pops must come out in increasing frequency order */
    check("Huff pop 1 (freq 1)",  HuffPop(), 20);
    check("Huff pop 2 (freq 3)",  HuffPop(), 40);
    check("Huff pop 3 (freq 5)",  HuffPop(), 10);
    check("Huff pop 4 (freq 9)",  HuffPop(), 30);

    /* ---- colour hash ---- */
    {
        static int pal[MAX_COLORS];
        int i, found;

        InitCrcTable();
        InitColorHash(pal);
        check("color hash starts empty", FindColor(0x123456), -1);
        for (i = 0; i < 64; i++) {
            pal[i] = 0x010101 * i + 7;
            AddColor(pal[i], i);
        }
        found = 1;
        for (i = 0; i < 64; i++)
            if (FindColor(pal[i]) != i)
                found = 0;
        check("all 64 colours round trip through the hash", found, 1);
        check("a colour never inserted is not found",
              FindColor(0xDEADBEEF) >= 0, 0);
    }

    /* ---- PutBits: 5 bits of 9, then 6 bits of 34 ----
       9 = 0b01001, 34 = 0b100010.  Stream MSB first: 01001 100010
       -> bytes 0x4C, 0x82 (plus padding bits in the low 2 of the last) */
    {
        static unsigned char out[64];
        static IMAGEDESC img;

        /*
         *  PutBits is a deflate style LSB-first packer: each value goes in
         *  above the bits already pending and whole bytes fall out of the
         *  bottom.  Writing 5 bits of 9 then 6 bits of 34 therefore emits
         *  9 at stream bits 0..4 and 34 at bits 5..10, giving
         *  byte0 = bits 0..7 = 0x49 with three bits still pending.
         */
        memset(&img, 0, sizeof(img));
        img.outbuf = out;
        img.outsize = sizeof(out);
        Image = &img;
        OutCount = 0;
        BitCount = 0;
        BitBuf = 0;
        BitCrc = 0;

        PutBits(9, 5);
        check("PutBits: nothing emitted yet", OutCount, 0);
        PutBits(34, 6);
        printf("info PutBits 5+6 bits -> %02x %02x (count %d, %d bits left)\n",
               out[0], out[1], OutCount, BitCount);
        check("PutBits first byte", out[0], 0x49);
        check("PutBits emitted one byte", OutCount, 1);
        check("PutBits left 3 bits pending", BitCount, 3);

        /* five more zero bits flush the remainder into 0x04 */
        PutBits(0, 5);
        check("PutBits flushed the second byte", out[1], 0x04);
        check("PutBits emitted both bytes", OutCount, 2);
        check("PutBits left nothing pending", BitCount, 0);
    }

    /* EmitBytes must drop anything past the end of the buffer */
    {
        static unsigned char out[8];
        static IMAGEDESC img;
        static unsigned char src[12];
        int i;

        for (i = 0; i < 12; i++)
            src[i] = (unsigned char)(0xA0 + i);
        memset(&img, 0, sizeof(img));
        img.outbuf = out;
        img.outsize = sizeof(out);
        Image = &img;
        OutCount = 0;
        check("EmitBytes returns the count", EmitBytes(src, 12), 12);
        check("EmitBytes still counts past the end", OutCount, 12);
        check("EmitBytes wrote the whole buffer", out[7], 0xA7);
    }

    /* ---- BuildCodeLengths (sub_404DA0) ---- */
    {
        static IMAGEDESC img;
        static int counts[16];
        static int lens[16];
        static int expect[5][16] = {
            { 1,1,2,2,2,3,3,3,3,4,4,4,4,4,4,4 },
            { 1,2,2,3,3,4,4,4,4,4,4,4,4,4,4,4 },
            { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 },   /* degenerate */
            { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 },   /* degenerate */
            { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 }
        };
        int trial, n;

        memset(&img, 0, sizeof(img));
        img.ndistinct = 16;
        Image = &img;

        /* a geometric distribution: symbol 0 is commonest so gets 1 bit,
           symbol 15 rarest so gets the longest code */
        for (n = 0; n < 16; n++)
            counts[n] = 1 << (15 - n);
        BuildCodeLengths(counts, 16, lens, 16);
        printf("info geometric counts -> lens");
        for (n = 0; n < 16; n++)
            printf(" %d", lens[n]);
        printf("\n");

        /* a canonical code must satisfy sum(2^-len) == 1 exactly */
        {
            /* scaled by 2^15 to stay in integers */
            int kraft = 0, ok = 1;
            for (n = 0; n < 16; n++) {
                int len = lens[n];
                if (len == 0)
                    continue;
                if (len > 15)
                    ok = 0;
                else
                    kraft += 1 << (15 - len);
            }
            printf("info Kraft sum x32768 = %d (want 32768), lengths valid=%d\n",
                   kraft, ok);
            check("code lengths satisfy Kraft equality", kraft, 32768);
        }
        check("commonest symbol gets a 1 bit code", lens[0], 1);
        check("rarest symbol gets the longest code", lens[15], 15);

        /* a uniform distribution gives a perfectly balanced tree */
        for (n = 0; n < 16; n++)
            counts[n] = 100;
        BuildCodeLengths(counts, 16, lens, 16);
        printf("info uniform counts -> lens");
        for (n = 0; n < 16; n++)
            printf(" %d", lens[n]);
        printf("\n");
        {
            int allfour = 1;
            for (n = 0; n < 16; n++)
                if (lens[n] != 4)
                    allfour = 0;
            check("16 equal frequencies all give 4 bits", allfour, 1);
        }

        /* unused symbols must come back as length 0 */
        for (n = 0; n < 16; n++)
            counts[n] = (n % 2) ? (1 << (8 - n / 2)) : 0;
        BuildCodeLengths(counts, 16, lens, 16);
        printf("info half the symbols unused -> lens");
        for (n = 0; n < 16; n++)
            printf(" %d", lens[n]);
        printf("\n");
        check("unused symbols get length 0", lens[0], 0);
        check("used symbols get a non-zero length", lens[1] > 0, 1);

        /* a very skewed distribution must be flattened to fit maxlen */
        for (n = 0; n < 16; n++)
            counts[n] = (n == 0) ? 1000000 : (1 << n);
        BuildCodeLengths(counts, 16, lens, 7);
        printf("info skewed, maxlen 7 -> lens");
        for (n = 0; n < 16; n++)
            printf(" %d", lens[n]);
        printf("\n");
        trial = 0;
        for (n = 0; n < 16; n++)
            if (lens[n] > 7)
                trial = 1;
        check("no code exceeds maxlen 7", trial, 0);

        /* degenerate cases */
        memset(counts, 0, sizeof(counts));
        counts[5] = 3;
        BuildCodeLengths(counts, 16, lens, 16);
        check("single used symbol gets a 1 bit code", lens[5], 1);

        memset(counts, 0, sizeof(counts));
        counts[0] = 3;
        counts[1] = 4;
        BuildCodeLengths(counts, 16, lens, 16);
        check("two used symbols both get 1 bit", lens[0] + lens[1], 2);
        (void)expect;
    }

    /* ---- deflate length / distance tables (sub_4058A0) ---- */
    {
        static IMAGEDESC img;
        int i, ok;

        memset(&img, 0, sizeof(img));
        img.ndistinct = 256;
        img.strategy = 0;                 /* so the LZ area gets reset */
        Image = &img;
        RowCount = 4;

        BuildDeflateTables();

        printf("info &EmitBlock=%p LenPairs[0..5]=%d,%d,%d,%d,%d,%d\n",
               (void *)EmitBlock, LenPairs[0], LenPairs[1], LenPairs[2],
               LenPairs[3], LenPairs[4], LenPairs[5]);
        printf("info LenBase/Extra:");
        for (i = 0; i < 12; i++)
            printf(" (%d,%d)", LenPairs[2*i], LenPairs[2*i+1]);
        printf("\n");
        printf("info DistBase/Extra:");
        for (i = 0; i < 12; i++)
            printf(" (%d,%d)", DistPairs[2*i], DistPairs[2*i+1]);
        printf("\n");

        /* bases must increase and extra bits must never decrease */
        ok = 1;
        for (i = 1; i < 30; i++) {
            if (LenPairs[2*i] < LenPairs[2*(i-1)]) ok = 0;
            if (LenPairs[2*i+1] < LenPairs[2*(i-1)+1]) ok = 0;
        }
        check("length table is monotone", ok, 1);

        ok = 1;
        for (i = 1; i < 28; i++) {
            if (DistPairs[2*i] < DistPairs[2*(i-1)]) ok = 0;
            if (DistPairs[2*i+1] < DistPairs[2*(i-1)+1]) ok = 0;
        }
        check("distance table is monotone", ok, 1);

        /*
         *  In the length table the *second* field is the cumulative base
         *  (1,2,3,4,5,7,9,13,17,25,33,49,...) and the first is the number of
         *  extra bits, stepping up every second code.  So the span of code i
         *  is base2[i+1] - base2[i] and must equal 2^extra1[i].
         */
        ok = 1;
        for (i = 0; i < 29; i++) {
            if (LenPairs[2*(i+1)+1] - LenPairs[2*i+1] != (1 << LenPairs[2*i]))
                ok = 0;
        }
        check("length spans are 2^extra", ok, 1);

        ok = 1;
        for (i = 0; i < 27; i++) {
            if (DistPairs[2*(i+1)] - DistPairs[2*i] != (1 << DistPairs[2*i+1])) ok = 0;
        }
        check("distance bases step by 2^extra", ok, 1);

        /* the one-shot guard: calling again must not rebuild the tables */
        {
            int before = LenPairs[14];
            DistTablesDone = 1;
            LenPairs[14] = 0x5A5A;
            BuildDeflateTables();
            check("tables are built only once", LenPairs[14], 0x5A5A);
            LenPairs[14] = before;
        }
    }

    printf("\n%s  (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}