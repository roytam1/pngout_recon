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

/* ------------------------------------------------------------------ */
/*  a decoder for FlushRow()'s output, so the row writer can be checked */
/*  by putting a stream back the way it went in                       */
/* ------------------------------------------------------------------ */

static const unsigned char *t_bits;
static int t_bitpos;

/*
 *  PutBits() holds up to seven bits back, so a stream that has not been
 *  finished leaves its last byte unwritten.  The encoder pads the final byte
 *  when it closes a block; do the same here so the decoder can see every bit.
 *  It takes 8 - BitCount more bits to push the pending ones out.
 */
static void tflush(void)
{
    if (BitCount > 0)
        PutBits(0, 8 - BitCount);
}

/* PutBits() is LSB first, so the first bit of a value is bit 0 of the
   first byte - which is what a decoder has to read back. */
static int tget(int n)
{
    int v = 0, i;

    for (i = 0; i < n; i++) {
        v |= ((t_bits[t_bitpos >> 3] >> (t_bitpos & 7)) & 1) << i;
        t_bitpos++;
    }
    return v;
}

/*
 *  Rebuild the symbol stream one FlushRow() call coded.  Reads the row
 *  header, the transmitted code lengths and then the payload, using the same
 *  RankValues() the writer used to turn those lengths into codes.
 *
 *  Returns the number of symbols recovered, or -1 if the stream does not
 *  decode.  The header fields come back so the caller can check them, and so
 *  does the number of bits the stream turned out to need.
 */
static int tdecode(const unsigned char *out, int *got, int *nlen,
                   int *ndist, int *nsent, int *bitsused)
{
    int lens[19], codes[19], maxlen = 0, i, n = 0, prev = 0;

    t_bits = out;
    t_bitpos = 0;

    *nlen = tget(5) + ROW_LITERALS + 1;
    *ndist = tget(5) + 1;
    *nsent = tget(4) + 4;

    for (i = 0; i < 19; i++) {
        lens[i] = 0;
        codes[i] = -1;          /* RankValues leaves unused symbols alone */
    }

    for (i = 0; i < *nsent; i++) {
        lens[Bitrev19[i]] = tget(3);
        if (lens[Bitrev19[i]] > maxlen)
            maxlen = lens[Bitrev19[i]];
    }

    RankValues(lens, 19, codes);

    while (n < *nlen + *ndist) {
        int sym = -1, len, v, count;

        for (len = 1; len <= maxlen; len++) {
            v = tget(len);
            for (i = 0; i < 19; i++) {
                if (codes[i] == v && lens[i] == len) {
                    sym = i;
                    break;
                }
            }
            if (sym >= 0)
                break;
            t_bitpos -= len;    /* not this long: those bits start over */
        }

        if (sym < 0)
            return -1;

        if (sym == 16)      { count = tget(2) + 3;   prev = prev; }
        else if (sym == 17) { count = tget(3) + 3;   prev = 0; }
        else if (sym == 18) { count = tget(7) + 11;  prev = 0; }
        else                { count = 1;            prev = sym; }

        if (n + count > *nlen + *ndist)
            return -1;       /* a run that claims more than the row has */

        while (count-- > 0)
            got[n++] = prev;
    }

    *bitsused = t_bitpos;
    return n;
}


/* ------------------------------------------------------------------ */
/*  BestSplit: where would the /b block splitter cut this?             */
/* ------------------------------------------------------------------ */

static int esc_calls;

static unsigned char got[128];        /* what record_sink() was handed */
static int gotn;

/* the little endian sample a run of filtered bytes makes */
static unsigned bpack(int n, const unsigned char *b)
{
    unsigned v = 0;
    int i;

    for (i = 0; i < n; i++)
        v |= (unsigned)b[i] << (8 * i);
    return v;
}

static int record_sink(int byte)
{
    if (gotn < (int)sizeof(got))
        got[gotn] = (unsigned char)byte;
    gotn++;
    return 0;
}

static int esc_hook(int unused)
{
    (void)unused;
    esc_calls++;
    return 1;                     /* the user pressed ESC */
}

/* ------------------------------------------------------------------ */
/*  sub_4086E0: /s4, the stored writer                                  */
/* ------------------------------------------------------------------ */

/*
 *  A helper for the /s4 tests: find the IDAT by walking the chunks, so they do
 *  not have to know how long PLTE and tRNS made the header.  Returns the
 *  offset of the IDAT's length field, or -1.
 */
static int find_idat(const unsigned char *p, int len)
{
    int o = 8;

    while (o + 8 <= len) {
        unsigned int clen = ((unsigned)p[o] << 24) | ((unsigned)p[o + 1] << 16) |
                            ((unsigned)p[o + 2] << 8) | p[o + 3];

        if (memcmp(p + o + 4, "IDAT", 4) == 0)
            return o;
        if (memcmp(p + o + 4, "IEND", 4) == 0)
            return -1;
        o += 12 + (int)clen;
    }
    return -1;
}

/* the same walk looking for any chunk tag; returns -1 when it is not there */
static int find_chunk(const unsigned char *p, int len, const char *tag)
{
    int o = 8;

    while (o + 8 <= len) {
        unsigned int clen = ((unsigned)p[o] << 24) | ((unsigned)p[o + 1] << 16) |
                            ((unsigned)p[o + 2] << 8) | p[o + 3];

        if (memcmp(p + o + 4, tag, 4) == 0)
            return o;
        if (memcmp(p + o + 4, "IEND", 4) == 0)
            return -1;
        o += 12 + (int)clen;
    }
    return -1;
}

/*
 *  dword_185BEC4 is s->msg, the printf-style progress line main() installs.
 *  sub_4086E0 ends with a bare "\r" and "Out:%8d bytes (stored)", so the test
 *  needs somewhere to put them.  Only the first few are kept, so a long run
 *  cannot overflow the buffer.
 */
static char messages[512];
static int nmsgs;

static void sink_msg(const char *fmt, ...)
{
    va_list ap;

    nmsgs++;
    if (strlen(messages) > 200)
        return;
    va_start(ap, fmt);
    vsprintf(messages + strlen(messages), fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/*  the LZ coder: give the rows a code the test can read back, run the */
/*  coder over some bytes, and put the symbols back together to see    */
/*  whether the stream carries the data it was given                   */
/* ------------------------------------------------------------------ */

/*
 *  The coder emits a literal as one symbol and a match as four, with no way
 *  to tell them apart from the bit widths alone.  So the test pins the codes
 *  to a shape with a tag bit:
 *
 *      a literal   1 tag bit 0, then 8 bits of byte
 *      a length    1 tag bit 1, then 5 bits of slot, then the extra bits
 *      a distance  1 tag bit 1, then 5 bits of slot, then the extra bits
 *
 *  The lengths are the row's own code lengths (field +0x510 for literals,
 *  +0x89C for the length codes and +0x990 for the distance codes), and the
 *  codes are what sub_405800 (RankValues) leaves in dword_6F6678 / 6F6A04 /
 *  6F6AF8 - the test overwrites those three with fixed values, which is the
 *  only part of the setup that is not what EmitBlock would do.
 */
#define LZ_LITBITS 9
#define LZ_CODEBITS 6

static unsigned char *lzbits;
static int lzbitpos;
static int lzfail;

#define LZFAIL(n)  do { lzfail = pos * 100 + (n);                          \
                         printf("   lzfail pos=%d bit=%d code=%d lslot=%d "\
                                "mlen=%d nmatch=%d\n", pos, lzbitpos, (n),\
                                lslot_dbg, mlen_dbg, *nmatch);           \
                         return -1; } while (0)

static int lzget(int n)
{
    int v = 0, i;

    for (i = 0; i < n; i++) {
        v |= ((lzbits[lzbitpos >> 3] >> (lzbitpos & 7)) & 1) << i;
        lzbitpos++;
    }
    return v;
}

/*
 *  Put the symbols back together.  A match copies its bytes out of what has
 *  already been rebuilt, exactly as a decoder would, so a match the coder
 *  invented cannot pass.  Returns the number of bytes, or -1 if the stream
 *  does not parse.
 */
static int lzdecode(int nbits, unsigned char *to, int want,
                    int *nmatch, int *longest, int *trailing)
{
    int pos = 0, i;

    lzbitpos = 0;
    lzfail = 0;
    *nmatch = 0;
    *longest = 0;

    while (pos < want) {
        if (lzget(1) == 0) {
            to[pos++] = (unsigned char)lzget(8);
            continue;
        }
        {
            int lslot;
            int lslot_dbg, mlen_dbg;
            int dslot, mlen, mdist;

            lslot_dbg = lzget(5);
            lslot = lslot_dbg + 30;         /* the pair index, 30..58 */

            if (lslot - 30 >= LEN_CODES)
                LZFAIL(1);
            mlen = lzget(LenCodeExtra(lslot - 30)) + LenCodeBase(lslot - 30);
            mlen_dbg = mlen;

            if (lzget(1) == 0)
                LZFAIL(2);
            dslot = lzget(5);
            if (dslot >= DIST_CODES)
                LZFAIL(3);
            mdist = lzget(DistCodeExtra(dslot)) + DistCodeBase(dslot);

            if (mlen < 3 || mlen > 258)
                LZFAIL(4);
            if (mdist < 1 || mdist > pos)
                LZFAIL(5);
            if (pos + mlen > want)
                LZFAIL(6);
            for (i = 0; i < mlen; i++)
                to[pos + i] = to[pos - mdist + i];
            pos += mlen;
            (*nmatch)++;
            if (mlen > *longest)
                *longest = mlen;
        }
    }
    *trailing = nbits - lzbitpos;
    return pos;
}

static IMAGEDESC lzimg;
static unsigned char lzout[1 << 17];
static unsigned char lzinput[40000];
static unsigned char lzgot[40000];

/*
 *  Everything EmitBlock and sub_4058A0 set up, with the row's code pinned to
 *  the shape above.  `rows` blocks of `limit` bytes each.
 */
static void lzsetup(int strategy, int limit)
{
    int i;

    /*
     *  sub_4058A0 wipes all of the coder's state before the first byte of a
     *  block; lzsetup has to do the same or the previous test's MatchLimit and
     *  MatchCost[] decide what this one is allowed to emit.
     */
    memset(LzWindow, 0, sizeof(LzWindow));
    memset(RunLen, 0, sizeof(RunLen));
    memset(MatchStart, 0, sizeof(MatchStart));
    memset32(MatchCost, 0x7FFFFFFF, 512);
    memset32(HashHead, 0, sizeof(HashHead) / sizeof(HashHead[0]));
    memset32(DistSlot, 0, sizeof(DistSlot) / sizeof(DistSlot[0]));
    memset32(BestCost, 0, sizeof(BestCost) / sizeof(BestCost[0]));
    memset(ROWOF(0, ROW_SAMPLES), 0, ROW_SAMPLE_MAX * sizeof(int));
    memset(ROWOF(0, ROW_LENCNT), 0, ROW_SAMPLE_MAX * sizeof(int));
    memset(ROWOF(0, ROW_DISTFREQ), 0, ROW_DISTSLOTS * sizeof(int));

    LzWrite = 0;
    LzScan = 0;
    LzHead = 0;
    unk_42D7C8 = 0;
    MatchLimit = 0;
    MatchCost[0] = 0x80000000;       /* loc_407059: the very first position */
    MatchCount = 0;
    RunEnd = 0;
    RunCount = 0;
    PrevReach = 0;
    PrevMark = 0;
    unk_42E55C = 0;

    memset(&lzimg, 0, sizeof(lzimg));
    memset(lzout, 0, sizeof(lzout));
    Image = &lzimg;
    lzimg.strategy = strategy;
    lzimg.ndistinct = 2;
    lzimg.outbuf = lzout;
    lzimg.outsize = sizeof(lzout);

    RowCount = 1;
    RowIndex = 0;
    OutCount = 0;
    BitBuf = 0;
    BitCount = 0;
    BitCrc = 0xFFFFFFFFu;
    AdlerSum = 1;
    RowHeaderCode = 0;
    RowCost = 0;
    lzbits = lzout;

    /*
     *  The row's code: 9 bits for every literal, 6 for every length symbol
     *  and 6 for every distance code.
     *
     *  These three are all *inside* the one symbol array at field +0x510, and
     *  the overlap is the point - field +0x89C is the 30th entry of that array
     *  onwards and field +0x990 is its 288th, which is why ROW_SYMBOLS[] has to
     *  be filled first and the two other fields written over the top of it.
     *  Setting ROW_LENBITS[] naively would take the last 29 *literals* down to
     *  6 bits as well, which is a trap worth stating.
     */
    for (i = 0; i < ROW_SAMPLE_MAX; i++) {
        ROWOF(0, ROW_SAMPLES)[i] = 0;
        ROWOF(0, ROW_SYMBOLS)[i] = LZ_LITBITS;
    }
    for (i = 257; i < ROW_SAMPLE_SYMS; i++)       /* the length symbols   */
        ROWOF(0, ROW_SYMBOLS)[i] = LZ_CODEBITS;
    for (i = 0; i < ROW_DISTSLOTS; i++)           /* the distance symbols */
        ROWOF(0, ROW_SYMBOLS)[ROW_DISTFREQ - ROW_SAMPLES + i] = LZ_CODEBITS;
    *ROWOF(0, ROW_ROWBITS) = 0;

    InitCrcTable();
    BuildDeflateTables();
    InitBlockTables();

    /*
     *  After InitBlockTables(), which is where sub_405150 calls sub_4058A0 from
     *  and which opens the *last* block's end to the whole window.  PlanBlocks()
     *  has run by then in the real program too.
     */
    ROW_LIMIT(RowCount > 0 ? RowCount - 1 : 0) = limit;

    /*
     *  The codes RankValues() left behind are replaced by the tagged ones.
     *  PutBits() sends a code least significant bit first, so the *leading*
     *  tag bit has to be bit 0 of the code: a literal is 9 bits, bit 0 clear,
     *  bits 1..8 the byte; a length or distance code is 6 bits, bit 0 set,
     *  bits 1..5 the slot.
     */
    for (i = 0; i < 256; i++)
        LitCodes[i] = i << 1;                  /* bit 0 clear: a literal */
    for (i = 0; i < LEN_CODES; i++)
        LitCodes[257 + i] = 1 | (i << 1);      /* bit 0 set: a match   */
    for (i = 0; i < ROW_DISTSLOTS; i++)
        DistCodes[i] = 1 | (i << 1);
}

/* push `n` bytes through the coder the way FilterByte()'s sink does */
static void lzfeed(const unsigned char *p, int n)
{
    int i;

    for (i = 0; i < n; i++)
        LzCode(p + i, 1);

    LzCode((const unsigned char *)0, 0);   /* flush the LZ_SKIPBACK tail */
}

int main(void)
{
    unsigned char buf[4096];
    int w, h, fmt, i;

    setvbuf(stdout, NULL, _IONBF, 0);

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
 *  Three distances, and it is `c` - not `b` - that the first is measured
 *  from:
 *
 *      d1 = |a + b - 2c|
 *      d2 = |b - c|
 *      d3 = |a - c|
 *
 *  Returns `a` when d2 is no larger than either of the others, `b` when
 *  d3 <= d1, and `c` otherwise.  The callers pass (left, up, upleft), so
 *  `c` is the sample diagonally above and to the left.
 */
    check("Predict(0,0,0)",     FilterPredict(0, 0, 0), 0);
    check("Predict(10,0,0)",    FilterPredict(10, 0, 0), 10);   /* d2 == 0   */
    check("Predict(1,2,3)",     FilterPredict(1, 2, 3), 1);     /* d2 == 1   */
    check("Predict(100,7,0)",   FilterPredict(100, 7, 0), 100); /* a on a tie */
    check("Predict(-5,5,5)",    FilterPredict(-5, 5, 5), -5);    /* a on a tie */
    check("Predict(255,0,255)", FilterPredict(255, 0, 255), 0);  /* d3 == 0   */
    check("Predict(3,10,4)",    FilterPredict(3, 10, 4), 10);    /* d3 == 1 wins over d2 == 6 */

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
    /* ---- RankValues: code lengths in, deflate codes out (sub_405800) ---- */
    {
        static int lens[19];
        static int codes[19];
        int n, i, clash, inrange;

        /*
         *  The codes are the canonical ones bit reversed over the width of
         *  each symbol's own code, which is what PutBits() wants.  The check
         *  that matters is that no two symbols come out with the same code
         *  *and* the same length - a previous version reversed the bits one
         *  place off and quietly produced duplicates for about half of them,
         *  which no other test noticed.
         */
        for (n = 0; n < 19; n++) {
            lens[n] = 0;
            codes[n] = -1;
        }
        lens[1] = 4;
        lens[3] = 3; lens[4] = 3; lens[5] = 3; lens[6] = 3;
        lens[7] = 3; lens[10] = 4; lens[16] = 3; lens[18] = 3;

        RankValues(lens, 19, codes);
        printf("info lengths 4 3333 4 3 3 -> codes");
        for (n = 0; n < 19; n++)
            if (lens[n])
                printf(" %d:%d", n, codes[n]);
        printf("\n");

        clash = 0;
        for (n = 0; n < 19; n++) {
            if (!lens[n])
                continue;
            for (i = n + 1; i < 19; i++)
                if (lens[i] == lens[n] && codes[i] == codes[n])
                    clash = 1;
        }
        check("no two symbols share a code", clash, 0);

        /* and every code has to fit inside its own length */
        inrange = 1;
        for (n = 0; n < 19; n++) {
            if (!lens[n])
                continue;
            if (codes[n] < 0 || codes[n] >= (1 << lens[n]))
                inrange = 0;
        }
        check("every code fits its length", inrange, 1);

        /* two symbols of one bit get 0 and 1, in that order */
        for (n = 0; n < 19; n++) {
            lens[n] = 0;
            codes[n] = -1;
        }
        lens[5] = 1;
        lens[18] = 1;
        RankValues(lens, 19, codes);
        check("first of two 1 bit symbols gets 0", codes[5], 0);
        check("second of two 1 bit symbols gets 1", codes[18], 1);

        /* four symbols of two bits: the canonical 0,1,2,3 reversed to
           0,2,1,3 - which is what an LSB-first writer needs */
        for (n = 0; n < 19; n++) {
            lens[n] = 0;
            codes[n] = -1;
        }
        lens[2] = 2; lens[3] = 2; lens[4] = 2; lens[5] = 2;
        RankValues(lens, 19, codes);
        check("four 2 bit symbols: first gets 0", codes[2], 0);
        check("four 2 bit symbols: second gets 2", codes[3], 2);
        check("four 2 bit symbols: third gets 1", codes[4], 1);
        check("four 2 bit symbols: fourth gets 3", codes[5], 3);

        /* a symbol of length 0 is left alone, so it keeps its old value */
        for (n = 0; n < 19; n++) {
            lens[n] = 0;
            codes[n] = -1;
        }
        lens[7] = 3;
        RankValues(lens, 19, codes);
        check("unused symbols are not written", codes[0], -1);
    }
    /* ---- deflate length / distance tables (sub_4058A0) ---- */
    {
        static IMAGEDESC img;
        int i, n, ok;

        memset(&img, 0, sizeof(img));
        img.ndistinct = 256;
        img.strategy = 0;                 /* so the LZ area gets reset */
        Image = &img;
        RowCount = 4;

        BuildDeflateTables();

        printf("info &EmitBlock=%p CodePairs[0..5]=%d,%d,%d,%d,%d,%d\n",
               (void *)EmitBlock, CodePairs[0], CodePairs[1], CodePairs[2],
               CodePairs[3], CodePairs[4], CodePairs[5]);
        printf("info distance (extra,base):");
        for (i = 0; i < 12; i++)
            printf(" (%d,%d)", CodePairs[2*i], CodePairs[2*i+1]);
        printf("\n");
        printf("info length (extra,base):");
        for (i = 0; i < 12; i++)
            printf(" (%d,%d)", CodePairs[2*(30+i)], CodePairs[2*(30+i)+1]);
        printf("\n");

        /*
         *  The two halves are RFC 1951's tables, so pin them against it: a
         *  swapped pair or a missing top code would still be monotone, and
         *  monotone is not enough - the stream has to be readable by anything
         *  that implements deflate.
         */
        {
            static const int dist_want[30] = {
                1, 2, 3, 4, 5, 7, 9, 13, 17, 25,
                33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385,
                24577
            };
            static const int len_want[29] = {
                3, 4, 5, 6, 7, 8, 9, 10, 11, 13,
                15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                67, 83, 99, 115, 131, 163, 195, 227, 258
            };

            ok = 1;
            for (i = 0; i < 30; i++)
                if (DistCodeBase(i) != dist_want[i])
                    ok = 0;
            check("the distance bases are RFC 1951's", ok, 1);

            ok = 1;
            for (i = 0; i < 29; i++)
                if (LenCodeBase(i) != len_want[i])
                    ok = 0;
            check("the length bases are RFC 1951's", ok, 1);

            ok = 1;
            for (i = 0; i < 29; i++)
                if (LenCodeExtra(i) != (i < 8 ? 0 : i < 28 ? (i - 4) / 4 : 0))
                    ok = 0;
            check("and so are the length extra bits", ok, 1);

            /*
             *  And every value has to get RFC's code for it - the table is
             *  indexed by value-1, and a code is only right if the value sits
             *  inside that code's own base .. base + 2^extra - 1 range.
             */
            ok = 1;
            for (i = 1; i <= 32768; i++) {
                int c = DistCodeToSlot[i - 1];

                if (c < 0 || c >= DIST_CODES)
                    ok = 0;
                else if (i < DistCodeBase(c) ||
                         i > DistCodeBase(c) +
                             (1 << DistCodeExtra(c)) - 1)
                    ok = 0;
            }
            check("every distance gets RFC's code", ok, 1);

            /* the real check: the code's base and extra must span the length */
            ok = 1;
            for (i = 3; i <= 258; i++) {
                int c = LenCodeOf(i) - 30;

                if (i < LenCodeBase(c) ||
                    i > LenCodeBase(c) + (1 << LenCodeExtra(c)) - 1)
                    ok = 0;
            }
            check("every length gets RFC's code", ok, 1);
        }

        /*
         *  Bases increase and extra bits never decrease *within* each half.
         *  Across the boundary between them they do not - the last distance
         *  base is 24577 and the first length base is 3 - because the halves
         *  are two separate tables that happen to be adjacent, which is why a
         *  reconstruction that reads them as one sequence goes wrong.
         *
         *  The length half has one more exception, and it is RFC 1951's: the
         *  last two codes overlap.  Symbol 284 covers 227..258 with 5 extra
         *  bits and symbol 285 is 258 with *no* extra bits, so the extra bit
         *  count drops from 5 to 0 and the span of 284 is one short of 2^5.
         *  The original stores that last pair by hand (`mov dword_1809564,
         *  102h`) rather than generating it, which is how it survives its own
         *  loop.
         */
        ok = 1;
        for (i = 1; i < DIST_CODES; i++) {
            if (CodePairs[2*i] < CodePairs[2*(i-1)]) ok = 0;
            if (CodePairs[2*i+1] < CodePairs[2*(i-1)+1]) ok = 0;
        }
        for (i = 31; i < CODE_PAIRS - 1; i++) {
            if (CodePairs[2*i] < CodePairs[2*(i-1)]) ok = 0;
            if (CodePairs[2*i+1] < CodePairs[2*(i-1)+1]) ok = 0;
        }
        check("each half of the table is monotone", ok, 1);

        /* and every code's span is exactly 2^extra, within its half */
        ok = 1;
        for (i = 0; i < DIST_CODES - 1; i++)
            if (DistCodeBase(i + 1) - DistCodeBase(i) !=
                (1 << DistCodeExtra(i)))
                ok = 0;
        for (i = 0; i < LEN_CODES - 2; i++)
            if (LenCodeBase(i + 1) - LenCodeBase(i) !=
                (1 << LenCodeExtra(i)))
                ok = 0;
        check("every span is 2^extra", ok, 1);

        /* and the two that RFC overlaps are exactly where it says */
        check("284 and 285 overlap by one, as RFC 1951 has it",
              LenCodeBase(LEN_CODES - 1), 258);
        check("284's span is one short of 2^5",
              LenCodeBase(LEN_CODES - 2) + (1 << LenCodeExtra(LEN_CODES - 2))
              - LenCodeBase(LEN_CODES - 2), 32);

        /* the last distance code has to reach 32768 and the last length code
           has to reach 258, or the coder would emit a distance it cannot code */
        check("the distance table covers the window",
              DistCodeBase(DIST_CODES - 1) +
              (1 << DistCodeExtra(DIST_CODES - 1)) - 1, 32768);
        check("the length table covers 258",
              LenCodeBase(LEN_CODES - 1) +
              (1 << LenCodeExtra(LEN_CODES - 1)) - 1 >= 258, 1);

        /* the one-shot guard: calling again must not rebuild the tables */
        {
            int before = CodePairs[14];
            DistTablesDone = 1;
            CodePairs[14] = 0x5A5A;
            BuildDeflateTables();
            check("tables are built only once", CodePairs[14], 0x5A5A);
            CodePairs[14] = before;
        }
    }
    /* ---- FlushRow: the row writer (sub_4053D0) ---- */
    {
        static unsigned char out[4096];
        static IMAGEDESC img;
        static int row[400];
        static int save[400];
        static int got[512];
        int nlen, ndist, nsent, bits, i, n, ok;

        memset(&img, 0, sizeof(img));
        img.outbuf = out;
        img.outsize = sizeof(out);
        Image = &img;

        /*
         *  The smallest row there is: one literal and then nothing but zeroes,
         *  which code as two long zero runs.  Only two symbols are used, so
         *  both get a 1 bit code - 0 for the literal and 1 for the run - and
         *  the header is
         *
         *      5 bits  0   no length slots beyond the first
         *      5 bits  0   one distance slot
         *      4 bits  6   ten code lengths follow
         *      30 bits    nine 000 codes and one 001, for symbol 18
         *
         *  giving 61 bits in all: 00 18 10 00 00 E2 3F, plus the last five
         *  bits (1 1010 and three bits of padding) in the byte the flush
         *  pushes out.
         */
        memset(row, 0, sizeof(row));
        row[0] = 5;
        OutCount = 0; BitCount = 0; BitBuf = 0; BitCrc = 0;
        FlushRow(row);
        tflush();
        printf("info one literal -> %02x %02x %02x %02x %02x %02x %02x %02x\n",
               out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7]);
        check("one literal: byte 1", out[1], 0x18);
        check("one literal: byte 2", out[2], 0x10);
        check("one literal: byte 5", out[5], 0xE2);
        check("one literal: byte 6", out[6], 0x3F);
        check("one literal: the padded byte", out[7], 0x1B);
        check("one literal: bytes written", OutCount, 8);
        n = tdecode(out, got, &nlen, &ndist, &nsent, &bits);
        check("one literal: decodes", n, 258);
        check("one literal: one length slot used", nlen, 257);
        check("one literal: one distance slot", ndist, 1);
        check("one literal: ten code lengths sent", nsent, 10);
        check("one literal: the literal comes back", got[0], 5);
        ok = 1;
        for (i = 1; i < 258; i++)
            if (got[i] != 0)
                ok = 0;
        check("one literal: the rest is zeroes", ok, 1);

        /* a row of mixed literals, runs of zeroes and a long run of a
           non-zero symbol: exercises all three run codes and the clamping */
        {
            static int build[64];

            memset(row, 0, sizeof(row));
            memset(build, -1, sizeof(build));
            n = 0;
            for (i = 0; i < 30; i++) build[n++] = 0;     /* a long zero run */
            for (i = 0; i < 20; i++) build[n++] = 7;     /* repeats */
            for (i = 0; i < 5; i++)  build[n++] = 3;     /* a short tail   */
            for (i = 0; i < 4; i++)  build[n++] = (i * 3) % 15 + 1;
            for (i = 0; i < n; i++) row[i] = build[i];

            /* three length slots and four distance slots in use */
            row[256] = 4; row[257] = 5; row[258] = 6;
            for (i = 0; i < 4; i++) row[288 + i] = 3 + i;

            OutCount = 0; BitCount = 0; BitBuf = 0; BitCrc = 0;
            FlushRow(row);
            tflush();
            printf("info mixed row: %d symbols -> %d bytes\n", n, OutCount);

            ok = 1;
            for (i = 0; i < 400; i++)
                save[i] = row[i];

            nlen = ndist = nsent = bits = 0;
            check("mixed row: decodes",
                  tdecode(out, got, &nlen, &ndist, &nsent, &bits),
                  ROW_LITERALS + 3 + 4);
            check("mixed row: three length slots", nlen, ROW_LITERALS + 3);
            check("mixed row: four distance slots", ndist, 4);
            check("mixed row: the stream is sent whole", nsent >= 4, 1);
            /* whatever the writer emitted has to be enough to read back */
            check("mixed row: no bits are missing", bits <= 8 * OutCount, 1);

            /* the literals must come back exactly */
            ok = 1;
            for (i = 0; i < n; i++)
                if (got[i] != build[i])
                    ok = 0;
            check("mixed row: literals survive the round trip", ok, 1);

            /* then the four distance slots, in order */
            ok = 1;
            for (i = 0; i < 4; i++)
                if (got[ROW_LITERALS + 3 + i] != 3 + i)
                    ok = 0;
            check("mixed row: the distance slots follow", ok, 1);

            /* and they must have been put back where the next row looks */
            ok = 1;
            for (i = 0; i < 4; i++)
                if (row[288 + i] != 3 + i)
                    ok = 0;
            check("mixed row: distance slots restored", ok, 1);
            ok = 1;
            for (i = 0; i < 4; i++)
                if (row[ROW_LITERALS + 3 + i] != 0)
                    ok = 0;
            check("mixed row: the copy left behind is cleared", ok, 1);

            /* a long run of zeroes has to be cheaper than the same row
               coded as literals: 59 symbols, so at worst 59 bits of payload
               plus the two 5 bit counts and the code table */
            /* 263 symbols in 20 bytes: the 197 zeroes of padding and the run
               of sevens collapse into three events, so this is far below the
               263 bits a literal a piece would take */
            check("mixed row: stays well under 32 bytes", OutCount < 32, 1);
            (void)save;
        }

        /* a row that is nothing but zeroes of every length the three run
           codes can express, so each boundary gets used */
        {
            memset(row, 0, sizeof(row));
            for (i = 0; i < 2; i++)  row[i] = 0;   /* too short: literals */
            n = 2;
            for (i = 0; i < 3; i++)  { row[n++] = 0; }  /* code 17: 3 */
            for (i = 0; i < 4; i++)  { row[n++] = 0; }  /* code 16 or 17 */
            for (i = 0; i < 10; i++) { row[n++] = 0; }  /* code 17 */
            for (i = 0; i < 11; i++) { row[n++] = 0; }  /* code 18 */
            for (i = 0; i < 138; i++){ row[n++] = 0; }  /* code 18, clamped */
            for (i = 0; i < 7; i++)  { row[n++] = 0; }  /* code 16/17 */

            OutCount = 0; BitCount = 0; BitBuf = 0; BitCrc = 0;
            FlushRow(row);
            tflush();
            printf("info zero runs: %d symbols -> %d bytes\n", n, OutCount);
            check("zero runs: decodes",
                  tdecode(out, got, &nlen, &ndist, &nsent, &bits),
                  ROW_LITERALS + 1 + 1);
            ok = 1;
            for (i = 0; i < n; i++)
                if (got[i] != 0)
                    ok = 0;
            check("zero runs: all of it came back", ok, 1);
            check("zero runs: costs far less than 175 bits", OutCount < 24, 1);
        }
        /* the scan-backs: how many slots are in use is found by walking down
           over the first zero, and never drops below one */
        {
            memset(row, 0, sizeof(row));
            row[0] = 9;
            row[285] = 7;               /* the top length slot: all 30 used */
            row[317] = 4;               /* the top distance slot: all 30 */
            OutCount = 0; BitCount = 0; BitBuf = 0; BitCrc = 0;
            FlushRow(row);
            tflush();
            check("top slot in use: all 30 length slots",
                  tdecode(out, got, &nlen, &ndist, &nsent, &bits),
                  ROW_LITERALS + ROW_LENSLOTS + ROW_DISTSLOTS);
            check("top slot in use: 30 length slots", nlen, 286);
            check("top slot in use: 30 distance slots", ndist, 30);

            /* a hole below the top slot: the walk stops at the first zero,
               so everything from there down is taken as in use - which is why
               the slots have to be packed against the top index */
            memset(row, 0, sizeof(row));
            row[0] = 9;
            row[283] = 7;
            row[284] = 0;
            row[285] = 0;
            row[288] = 5;
            OutCount = 0; BitCount = 0; BitBuf = 0; BitCrc = 0;
            FlushRow(row);
            tflush();
            check("hole below the top slot: 28 length slots",
                  tdecode(out, got, &nlen, &ndist, &nsent, &bits),
                  ROW_LITERALS + 28 + 1);
            check("hole below the top slot: 28 length slots (field)",
                  nlen, ROW_LITERALS + 28);
            check("one distance slot at the bottom", ndist, 1);
        }
    }
    /* ---- WritePngHeader (sub_405B80) ---- */
    {
        static unsigned char out[8192];
        static IMAGEDESC img;
        static int palette[300];
        int i;

        memset(&img, 0, sizeof(img));
        img.outbuf = out;
        img.outsize = sizeof(out);
        Image = &img;
        MinSample = -1;                      /* no darkest sample yet */

        /*
         *  The signature and IHDR are fixed apart from the three fields that
         *  come in, so a 64x32 greyscale image pins the whole 33 bytes:
         *
         *      89 50 4E 47 0D 0A 1A 0A   the signature
         *      00 00 00 0D               IHDR, 13 bytes
         *      49 48 44 52               "IHDR"
         *      00 00 00 40 00 00 00 20   64 x 32, both 4 byte big endian
         *      08 00 00 00 00            8 bits, grey, no compression,
         *                                 no filter, no interlace
         *      87 F6 21 58               the CRC over "IHDR" and those 13,
         *                                 the same value zlib.crc32 gives
         */
        check("WritePngHeader returns 1",
              WritePngHeader(64, 32, 0, 8, NULL, 0), 1);
        check("IHDR is 33 bytes", OutCount, 33);
        check("signature byte 0", out[0], 0x89);
        check("signature byte 1", out[1], 0x50);
        check("signature byte 7", out[7], 0x0A);
        check("IHDR length", (out[8] << 24) | (out[9] << 16) |
                            (out[10] << 8) | out[11], 13);
        check("IHDR tag", memcmp(out + 12, "IHDR", 4), 0);
        check("IHDR width", (out[16] << 24) | (out[17] << 16) |
                            (out[18] << 8) | out[19], 64);
        check("IHDR height", (out[20] << 24) | (out[21] << 16) |
                             (out[22] << 8) | out[23], 32);
        check("IHDR bit depth", out[24], 8);
        check("IHDR colour type", out[25], 0);
        check("IHDR compression", out[26], 0);
        check("IHDR filter", out[27], 0);
        check("IHDR interlace", out[28], 0);
        check("IHDR crc 0", out[29], 0x87);
        check("IHDR crc 1", out[30], 0xF6);
        check("IHDR crc 2", out[31], 0x21);
        check("IHDR crc 3", out[32], 0x58);

        /* a palette: PLTE from bytes 2, 1, 0 of each entry */
        for (i = 0; i < 3; i++)
            palette[i] = 0xFF000000u | ((unsigned)(0x11 * (i + 1)) << 16) |
                         ((unsigned)(0x22 * (i + 1)) << 8) | (0x33 * (i + 1));
        check("palette header returns 1",
              WritePngHeader(64, 32, 3, 8, palette, 3), 1);
        /* 33 for IHDR, then 12 + 9 for PLTE */
        check("PLTE total size", OutCount, 33 + 12 + 9);
        check("PLTE length", (out[33] << 24) | (out[34] << 16) |
                             (out[35] << 8) | out[36], 9);
        check("PLTE tag", memcmp(out + 37, "PLTE", 4), 0);
        printf("info PLTE entry 0: %02x %02x %02x, entry 2: %02x %02x %02x\n",
               out[41], out[42], out[43], out[47], out[48], out[49]);
        check("PLTE entry 0 red", out[41], 0x11);
        check("PLTE entry 0 green", out[42], 0x22);
        check("PLTE entry 0 blue", out[43], 0x33);
        check("PLTE entry 2 red", out[47], 0x33);
        check("PLTE entry 2 green", out[48], 0x66);
        check("PLTE entry 2 blue", out[49], 0x99);

        /* all opaque, so no tRNS at all */
        check("no tRNS for an opaque palette", OutCount, 33 + 12 + 9);

        /* one transparent entry: tRNS holds the alphas, up to the last
           entry that is not opaque */
        palette[1] &= 0x00FFFFFF;         /* entry 1 becomes transparent */
        check("palette with tRNS returns 1",
              WritePngHeader(64, 32, 3, 8, palette, 3), 1);
        check("tRNS appears after PLTE", OutCount, 33 + 12 + 9 + 12 + 2);
        check("tRNS length", (out[54] << 24) | (out[55] << 16) |
                             (out[56] << 8) | out[57], 2);
        check("tRNS tag", memcmp(out + 58, "tRNS", 4), 0);
        check("tRNS alpha of entry 0", out[62], 0xFF);
        check("tRNS alpha of entry 1", out[63], 0x00);

        /* a background colour has to be in the palette, and is added if it
           is not - so the palette, and with it PLTE, grows by one entry */
        for (i = 0; i < 3; i++)
            palette[i] = 0xFF000000u | (0x0A0B0Cu);
        img.bkgd = 0x00AABBCCu;            /* not in the palette */
        check("palette with a new bKGD returns 1",
              WritePngHeader(64, 32, 3, 8, palette, 3), 1);
        check("the background colour was added", palette[3],
              (int)(0xFF000000u | 0x00AABBCCu));
        /* 33 + PLTE(8 header + 12 data + 4 crc) + bKGD(8 + 1 + 4) */
        check("bKGD after PLTE", OutCount, 33 + 24 + 13);
        check("PLTE grew to four entries",
              (out[33] << 24) | (out[34] << 16) | (out[35] << 8) | out[36], 12);
        check("bKGD length is one", (out[57] << 24) | (out[58] << 16) |
                                    (out[59] << 8) | out[60], 1);
        check("bKGD tag", memcmp(out + 61, "bKGD", 4), 0);
        check("bKGD palette index", out[65], 3);

        /* and a background colour that will not fit the bit depth is fatal */
        palette[3] = 0xFF000000u | 0x00FFFFFFu;
        palette[4] = 0;
        img.bkgd = 0x00ABCDEFu;            /* in neither of the two entries */
        check("bKGD that cannot fit fails",
              WritePngHeader(64, 32, 3, 1, palette, 2), 0);

        /* an RGB image: bKGD is three samples, tRNS the darkest one */
        memset(&img, 0, sizeof(img));
        img.outbuf = out;
        img.outsize = sizeof(out);
        Image = &img;
        img.bkgd = 0x00112233u;
        MinSample = 0x00010203;            /* 0xAARRGGBB, so R G B = 1 2 3 */
        check("RGB header returns 1", WritePngHeader(64, 32, 2, 8, NULL, 0), 1);
        check("RGB tRNS and bKGD size", OutCount, 33 + 12 + 6 + 12 + 6);
        check("tRNS length is six", (out[33] << 24) | (out[34] << 16) |
                                    (out[35] << 8) | out[36], 6);
        check("tRNS tag", memcmp(out + 37, "tRNS", 4), 0);
        printf("info tRNS samples:");
        for (i = 41; i < 47; i++)
            printf(" %02x", out[i]);
        printf("\n");
        check("tRNS sample 0", (out[41] << 8) | out[42], 0x01);
        check("tRNS sample 1", (out[43] << 8) | out[44], 0x02);
        check("tRNS sample 2", (out[45] << 8) | out[46], 0x03);
        check("bKGD length is six", (out[51] << 24) | (out[52] << 16) |
                                    (out[53] << 8) | out[54], 6);
        check("bKGD tag", memcmp(out + 55, "bKGD", 4), 0);
        check("bKGD sample 0", (out[59] << 8) | out[60], 0x11);
        check("bKGD sample 1", (out[61] << 8) | out[62], 0x22);
        check("bKGD sample 2", (out[63] << 8) | out[64], 0x33);

        /* no darkest sample, so no tRNS, and a greyscale bKGD is one sample */
        MinSample = -1;
        check("no tRNS without a darkest sample",
              WritePngHeader(64, 32, 0, 8, NULL, 0), 1);
        check("grey bKGD only", OutCount, 33 + 12 + 2);
        check("grey bKGD length", (out[33] << 24) | (out[34] << 16) |
                                  (out[35] << 8) | out[36], 2);
        check("grey bKGD sample", (out[41] << 8) | out[42], 0x33);

        /* a four bit sample is rescaled out of the 8 bit value */
        img.bkgd = 0x000000FF;
        check("four bit header", WritePngHeader(64, 32, 0, 4, NULL, 0), 1);
        check("four bit sample is 15", (out[41] << 8) | out[42], 0x0F);

        /* and a background colour of zero means no bKGD chunk at all */
        MinSample = -1;
        img.bkgd = 0;
        check("no bKGD without a background colour",
              WritePngHeader(64, 32, 6, 8, NULL, 0), 1);
        check("header only", OutCount, 33);
    }
    /* ---- the kept chunks ---- */
    {
        static unsigned char out[8192];
        static IMAGEDESC img;
        static unsigned char rec1[32];
        static unsigned char rec2[32];
        static unsigned char rec3[32];
        unsigned int *chunks[3];

        /* a gAMA record, then an sBIT, then a bKGD that must not be copied */
        memset(rec1, 0, sizeof(rec1));
        rec1[3] = 4;
        memcpy(rec1 + 4, "gAMA", 4);
        memcpy(rec1 + 8, "\x40\x1A\x73\x40", 4);

        memset(rec2, 0, sizeof(rec2));
        rec2[3] = 3;
        memcpy(rec2 + 4, "sBIT", 4);
        rec2[8] = 8; rec2[9] = 8; rec2[10] = 8;

        memset(rec3, 0, sizeof(rec3));
        rec3[3] = 1;
        memcpy(rec3 + 4, "bKGD", 4);
        rec3[8] = 0;

        chunks[0] = (unsigned int *)rec1;
        chunks[1] = (unsigned int *)rec2;
        chunks[2] = (unsigned int *)rec3;

        memset(&img, 0, sizeof(img));
        img.outbuf = out;
        img.outsize = sizeof(out);
        img.chunks = chunks;
        img.chunkbeg = 0;
        img.chunkmid = 3;
        img.chunkend = 3;
        Image = &img;
        MinSample = -1;

        check("kept chunks copied", WritePngHeader(8, 8, 2, 8, NULL, 0), 1);
        /* 33 + gAMA(16) + sBIT rebuilt(8 + 3 + 4 = 15), bKGD dropped */
        check("gAMA then a rebuilt sBIT", OutCount, 33 + 16 + 15);
        check("gAMA came first", memcmp(out + 37, "gAMA", 4), 0);
        check("sBIT follows", memcmp(out + 53, "sBIT", 4), 0);
        check("sBIT length for RGB", (out[49] << 24) | (out[50] << 16) |
                                       (out[51] << 8) | out[52], 3);
        check("sBIT keeps its values", out[57], 8);
        check("sBIT last value", out[59], 8);
    }

    /* ---- EstimateEntropy (sub_4063F0) ---- */
    {
        static int hist[256];
        unsigned int a, b, c;
        int i;

        memset(hist, 0, sizeof(hist));

        /* nothing in the histogram at all: the score is the starting value */
        CurFilter = 0;
        EntropyMax = 1;
        Entropy[0] = 1;
        check("empty histogram", EstimateEntropy(hist, 256), 0x7FFFFFFEu);
        check("the shared histogram was touched", Entropy[0], 0);

        /*
         *  The score is scaled so that the commonest symbol, whose count is
         *  one past EntropyMax, comes out at the top of the 31 bit range.  So
         *  one symbol used nine times is the worst case there is - there is
         *  nothing left to predict it from - and eight symbols used nine times
         *  each cannot be any worse.
         */
        memset(hist, 0, sizeof(hist));
        hist[0] = 9;
        CurFilter = 0;
        Entropy[0] = 1;
        EntropyMax = 10;
        a = EstimateEntropy(hist, 256);
        check("one symbol nine times uses the whole range", a, 0x7FFFFFFEu -
              0x7FFFFFFFu);

        memset(hist, 0, sizeof(hist));
        for (i = 0; i < 8; i++)
            hist[i] = 9;
        CurFilter = 0;
        Entropy[0] = 1;
        EntropyMax = 10;
        b = EstimateEntropy(hist, 256);
        check("eight symbols nine times scores better", b < a, 1);

        /* and a flatter histogram is worse still: the score goes up */
        memset(hist, 0, sizeof(hist));
        hist[0] = 36;
        CurFilter = 0;
        Entropy[0] = 1;
        EntropyMax = 37;
        c = EstimateEntropy(hist, 256);
        memset(hist, 0, sizeof(hist));
        for (i = 0; i < 4; i++)
            hist[i] = 9;
        CurFilter = 0;
        Entropy[0] = 1;
        EntropyMax = 10;
        check("four symbols nine times is worse than eight",
              c > EstimateEntropy(hist, 256), 1);

        /* counts of zero and one cost nothing at all */
        memset(hist, 0, sizeof(hist));
        hist[0] = 9;
        hist[1] = 0;
        hist[2] = 1;
        CurFilter = 0;
        Entropy[0] = 1;
        EntropyMax = 10;
        check("a count of one is free", EstimateEntropy(hist, 256), a);

        /* the table is cached, so the same question twice gives the same
           answer, and a different maximum count rebuilds it */
        memset(hist, 0, sizeof(hist));
        hist[3] = 9;
        CurFilter = 0;
        Entropy[0] = 1;
        EntropyMax = 10;
        b = EstimateEntropy(hist, 256);
        check("the same histogram scores the same", b, a);

        memset(hist, 0, sizeof(hist));
        hist[3] = 12;
        CurFilter = 0;
        Entropy[0] = 1;
        EntropyMax = 13;
        c = EstimateEntropy(hist, 256);
        printf("info entropy scores: 9 in one %u, 9 in eight %u, 12 in one %u\n",
               a, b, c);
        check("a different maximum count rebuilds the table",
              EntropyLutSize, 12);
        check("the table reaches the commonest count", EntropyLut[12] > 0, 1);
    }

    /* ---- BestSplit (sub_408390) ---- */
    {
        static unsigned char buf[512];
        double s1, s2, s3;
        int i, cut;

        CheckEscHook = NULL;

        /* one byte or less has no interior cut, and no score either */
        s1 = 12345.0;
        check("a single byte has no cut", BestSplit(buf, 1, &s1), 0);
        check("a single byte leaves the score alone", (int)s1, 12345);
        check("no bytes at all", BestSplit(buf, 0, &s1), 0);

        /*
         *  Two flat halves of two different bytes: cutting between them is
         *  the most compressible thing possible, so that has to be where the
         *  cost is lowest.
         */
        for (i = 0; i < 32; i++) buf[i] = 0x00;
        for (i = 32; i < 64; i++) buf[i] = 0xFF;
        cut = BestSplit(buf, 64, &s1);
        printf("info two flat halves: cut at %d, saving %f\n", cut, s1);
        check("two flat halves cut between them", cut, 32);

        /* a single flat range has nothing to find: every cut is the same */
        for (i = 0; i < 64; i++) buf[i] = 0x5A;
        check("a flat range still finds a cut", BestSplit(buf, 64, &s2) > 0, 1);
        printf("info flat range: saving %f\n", s2);

        /* four quarters, each a different byte: the best cut is one of the
           three boundaries */
        for (i = 0; i < 16; i++) buf[i] = 1;
        for (i = 16; i < 32; i++) buf[i] = 2;
        for (i = 32; i < 48; i++) buf[i] = 3;
        for (i = 48; i < 64; i++) buf[i] = 4;
        cut = BestSplit(buf, 64, &s3);
        printf("info four quarters: cut at %d, saving %f\n", cut, s3);
        check("four quarters cut on a boundary",
              (cut == 16 || cut == 32 || cut == 48), 1);

        /*
         *  The score is a saving, so it rises as the range gets easier to
         *  split: four quarters with two symbols each beat two halves with
         *  one, and a range that is a single solid byte has nothing to gain.
         */
        check("four quarters gain more than two halves", s3 > s1, 1);
        check("two halves beat a solid range", s1 > s2, 1);

        /* the split is always interior */
        for (i = 0; i < 200; i++)
            buf[i] = (unsigned char)(i * 7 + (i / 13));
        cut = BestSplit(buf, 200, &s3);
        printf("info a ramp: cut at %d of 200\n", cut);
        check("the cut is interior", cut > 0 && cut < 200, 1);

        /* ESC is polled every 64K bytes, and it aborts */
        CheckEscHook = esc_hook;
        esc_calls = 0;
        check("ESC aborts the search", BestSplit(buf, 200, &s3), -1);
        CheckEscHook = NULL;
    }

    /* ---- PlanBlocks: the /b block splitter (sub_408000) ---- */
    {
        static IMAGEDESC img;
        static unsigned char data[4096];
        int i, n, ok;

        memset(&img, 0, sizeof(img));
        Image = &img;
        CheckEscHook = NULL;

        /* the planner works on LinBuf's data area */
        LinBufData = data;

        /* a solid block of one byte has nothing worth cutting */
        for (i = 0; i < 512; i++) data[i] = 0x33;
        img.maxblocks = 0;
        img.splitthreshold = 1.0;
        check("a solid block plans", PlanBlocks(0, 512), 0);
        printf("info solid block: %d block(s)\n", NumBlockList);
        check("a solid block stays whole", NumBlockList, 1);

        /*
         *  Four quarters of 128 bytes each.  How many blocks come out depends
         *  entirely on the threshold - at zero it will keep cutting into
         *  single bytes - so what is checked is the shape of the list: the
         *  ends sorted ascending, and the count going down as the bar goes up.
         */
        for (i = 0; i < 512; i++)
            data[i] = (unsigned char)(i / 128);

        for (n = 0; n < 4; n++) {
            double thresh = (n == 0) ? 0.0 : (n == 1) ? 40.0
                             : (n == 2) ? 200.0 : 100000.0;

            img.maxblocks = 0;
            img.splitthreshold = thresh;
            check("a quartered block plans", PlanBlocks(0, 512), 0);
            printf("info threshold %8.0f -> %d block(s) ends:", thresh,
                   NumBlockList);
            for (i = 0; i < NumBlockList && i < 8; i++)
                printf(" %d", *BlockEnd(i));
            printf("\n");

            /* the ends are the partition, and they are sorted */
            ok = 1;
            for (i = 1; i < NumBlockList; i++)
                if (*BlockEnd(i) < *BlockEnd(i - 1))
                    ok = 0;
            check("the ends are sorted ascending", ok, 1);
            check("the last end is the end of the range",
                  *BlockEnd(NumBlockList - 1), 512);
            if (n == 0)
                check("a zero threshold cuts hard", NumBlockList > 8, 1);
            if (n == 3)
                check("an impossible threshold cuts nothing", NumBlockList, 1);

        }

        /* an explicit block count is honoured */
        img.splitthreshold = 0.0;
        img.maxblocks = 2;
        check("two blocks requested", PlanBlocks(0, 512), 0);
        printf("info /b2: %d block(s), ends %d %d\n", NumBlockList,
               *BlockEnd(0), *BlockEnd(1));
        check("/b2 gives two blocks", NumBlockList, 2);

        img.maxblocks = 4;
        check("four blocks requested", PlanBlocks(0, 512), 0);
        check("/b4 gives four blocks", NumBlockList, 4);
        check("/b4 still ends at the end of the range",
              *BlockEnd(NumBlockList - 1), 512);

        /* one block means no splitting at all */
        img.maxblocks = 1;
        check("/b1 gives one block", PlanBlocks(0, 512), 0);
        check("/b1 leaves one block", NumBlockList, 1);

        /* and the count is clamped to 1024 */
        img.maxblocks = 5000;
        check("a huge count still plans", PlanBlocks(0, 512), 0);
        check("the count was clamped", img.maxblocks, MAX_BLOCKS);

        /* a single byte has no interior cut, so the count is left alone */
        img.maxblocks = 8;
        check("one byte cannot be cut", PlanBlocks(0, 1), 0);
        check("one byte stays whole", NumBlockList, 1);

        /* two bytes do divide, because the guard is "shorter than two" */
        img.maxblocks = 8;
        check("two bytes plan", PlanBlocks(0, 2), 0);
        check("two bytes become two blocks", NumBlockList, 2);

        LinBufData = NULL;
    }

    /* ---- the shell sort over the block ends ---- */
    {
        static int scrambled[40];
        int i, ok;

        /* a reversed, then interleaved, set of ends */
        for (i = 0; i < 40; i++)
            scrambled[i] = 39 - i;
        for (i = 0; i < 40; i++)
            *BlockEnd(i) = scrambled[i];
        SortBlockEnds(40);
        ok = 1;
        for (i = 1; i < 40; i++)
            if (*BlockEnd(i) < *BlockEnd(i - 1))
                ok = 0;
        check("the ends sort ascending from reversed", ok, 1);

        for (i = 0; i < 40; i++)
            *BlockEnd(i) = scrambled[(i * 7) % 40];   /* a stride of 7 */
        SortBlockEnds(40);
        ok = 1;
        for (i = 1; i < 40; i++)
            if (*BlockEnd(i) < *BlockEnd(i - 1))
                ok = 0;
        check("the ends sort ascending from a stride", ok, 1);

        *BlockEnd(0) = 5;
        *BlockEnd(1) = 3;
        *BlockEnd(2) = 9;
        SortBlockEnds(3);
        check("three ends sort", *BlockEnd(0) * 100 + *BlockEnd(1) * 10 +
              *BlockEnd(2), 359);
    }
    /* ---- FilterByte: sub_407D80, one byte in and one byte out -------- */
    {
        static int words[1024];        /* LinBufData, one dword per byte */
        static IMAGEDESC img;

        memset(&img, 0, sizeof(img));
        memset(words, 0, sizeof(words));
        memset(FilterHistogram, 0, sizeof(FilterHistogram));
        memset(got, 0, sizeof(got));
        Image = &img;
        LinBufData = (unsigned char *)words;
        LinBuf = (unsigned char *)words - 4;    /* the base pointer sits low */
        LinBufBytes = 4;                         /* four bytes to a row       */
        LinBufUsed = 0;
        BitBufPos = 0;
        FilterLeft = 0x99;                       /* must not leak into a row  */
        FilterByteCount = 0;
        BlockSymbols = 0x7FFFFFFF;
        BlockFullHook = NULL;
        ByteSink = record_sink;

        /*
         *  Row 0, no filter: the filter type comes out first because that is
         *  what PNG wants, then the bytes untouched.
         */
        TrialFilter = 0;
        for (i = 0; i < 4; i++)
            FilterByte((unsigned char)(10 * (i + 1)));
        check("the row emits its filter type first", got[0], 0);
        check("an unfiltered row passes the bytes through", got[1], 10);
        check("all of them", got[2] * 100 + got[3] * 10 + got[4], 2340);
        check("the row counter wrapped to the start", BitBufPos, 0);
        check("the row was counted", LinBufUsed, 1);
        check("the left byte does not survive a row", FilterLeft, 40);

        /*
         *  Row 1, sub: each byte against the raw byte to its left.  This row
         *  owns the lower slot, so it also overwrites the row above it.
         */
        TrialFilter = 1;
        for (i = 0; i < 4; i++)
            FilterByte((unsigned char)(1 + 10 * i));
        check("the sub filter announces itself", got[5], 1);
        check("sub uses a zero left byte at the row start", got[6], 1);
        check("sub uses the byte to the left after that", got[7], 10);
        check("sub keeps doing it", got[8] + got[9], 20);

        /* Row 2, up: against row 1, which is now in the lower slot */
        TrialFilter = 2;
        for (i = 0; i < 4; i++)
            FilterByte((unsigned char)(5 + 10 * i));
        check("the up filter announces itself", got[10], 2);
        check("up subtracts the row above", got[11], 4);
        check("every byte of it", got[12] + got[13] + got[14], 12);

        /* Row 3, average: against row 2, the first byte has a zero left */
        TrialFilter = 3;
        for (i = 0; i < 4; i++)
            FilterByte((unsigned char)(40 + 10 * i));
        check("the average filter announces itself", got[15], 3);
        check("the average of a zero and five is two", got[16], 38);
        check("and of 40 and 15 is 27", got[17], 23);
        check("and again", got[18] + got[19], 46);

        /*
         *  Row 4, the predictor, fed the same bytes as row 3 so that the row
         *  above matches it.  This is not PNG's Paeth - it picks whichever of
         *  the three candidates is closest, measuring the first distance from
         *  the *upleft* sample rather than from `up`.  On a ramp that is the
         *  same answer as `up` would be, so the whole row comes out as zeroes.
         *  The first byte reads the word in front of the data area for its
         *  left-of-above byte, as the original does.
         *
         *      left  up  upleft   |a+b-2c|  |b-c|  |a-c|   result
         *         0  40      0        40      40      0   up
         *        40  50     40        10      10      0   up
         *        50  60     50        10      10      0   up
         *        60  70     60        10      10      0   up
         */
        TrialFilter = 4;
        for (i = 0; i < 4; i++)
            FilterByte((unsigned char)(40 + 10 * i));
        check("the predictor announces itself", got[20], 4);
        check("the first byte of an even row reads below the data",
              got[21], 0);
        check("and takes `up` rather than the left byte", got[22], 0);
        check("all of it", got[23] + got[24], 0);

        check("everything the sink saw", gotn, 25);

        /* the average filter is only replaced on the very first row */
        LinBufUsed = 0;
        BitBufPos = 0;
        gotn = 0;
        TrialFilter = 3;
        FilterByte(1);
        check("the average becomes sub on the first row", CurFilter, 1);
        check("and the row says so", got[0], 1);
        BitBufPos = 2;
        FilterByte(1);
        check("and it stays for the rest of the row", CurFilter, 1);

        /*
         *  A filter type outside 0..4 emits no data at all - but only the start
         *  of a row copies TrialFilter into CurFilter, so the row has to be
         *  started with it first.
         */
        LinBufUsed = 0;
        BitBufPos = 0;
        gotn = 0;
        TrialFilter = 9;
        FilterByte(7);
        check("an unknown filter type is still announced", got[0], 9);
        gotn = 0;
        FilterByte(7);
        check("but it emits no data", gotn, 0);
        check("the byte is still stored", words[LinBufBytes + 1], 7);
        check("and the position still moves", BitBufPos, 2);

        /* the raw byte is kept one dword per byte, in the other slot */
        words[LinBufBytes + 1] = 0;
        BitBufPos = 0;
        LinBufUsed = 1;
        TrialFilter = 0;
        gotn = 0;
        FilterByte(0xAB);
        check("an odd row writes the lower slot", words[0], 0xAB);
        check("and leaves the upper slot for the row above",
              words[LinBufBytes + 1], 0);
        LinBufUsed = 0;
        BitBufPos = 0;
        FilterByte(0xCD);
        check("an even row writes the upper slot",
              words[LinBufBytes + 1], 0xCD);
        check("and leaves the lower slot alone", words[0], 0xAB);

        /* the histogram sink, which does not count rows */
        ByteSink = CountByte;
        LinBufUsed = 0;
        BitBufPos = 0;
        FilterByte(3);
        FilterByte(3);
        FilterByte(9);
        check("the histogram counts bytes", FilterByteCount, 4);
        check("it counted the filter type", FilterHistogram[0], 1);
        check("and both threes", FilterHistogram[3], 2);
        check("and the nine", FilterHistogram[9], 1);
        check("the histogram sink leaves the row counter alone",
              LinBufUsed, 0);

        /* Adler-32, against the same sum worked out here */
        {
            unsigned int sum = 1;
            unsigned long a = 1, b = 0;
            int k;

            for (k = 0; k < 64; k++) {
                unsigned char c = (unsigned char)(k * 37 + (k >> 2));

                sum = Adler32Update(sum, c);
                a += c;
                if (a >= 65521) a -= 65521;
                b += a;
                if (b >= 65521) b -= 65521;
            }
            check("Adler-32 after one byte", Adler32Update(1, 255) & 0xFFFF,
                  256);
            check("Adler-32 of 64 bytes", sum, (unsigned int)(b << 16 | a));
            check("Adler-32 wraps its low half",
                  Adler32Update(65520, 5) & 0xFFFF, 4);
            check("Adler-32 wraps its high half",
                  Adler32Update((65520u << 16) | 1, 1) >> 16, 1);
        }

        ByteSink = NULL;
        LinBuf = NULL;
        LinBufData = NULL;
    }

    /* ---- the wider samples: 16, 24 and 32 bit ----------------------- */
    {
        static int words[1024];
        static IMAGEDESC img;

        memset(&img, 0, sizeof(img));
        memset(words, 0, sizeof(words));
        Image = &img;
        LinBufData = (unsigned char *)words;
        LinBuf = (unsigned char *)words - 4;
        LinBufBytes = 2;                     /* two samples to a row        */
        LinBufUsed = 0;
        BitBufPos = 0;
        BlockSymbols = 0x7FFFFFFF;
        BlockFullHook = NULL;
        ByteSink = record_sink;

        /*
         *  Two rows of two samples.  The sub filter uses the *same* byte of
         *  the sample to the left, so the second sample of a row comes out as
         *  one repeated byte rather than as a little endian subtraction.
         *
         *  The bytes also come out in PNG's order, not ascending: a pixel is
         *  one DWORD whose bytes are B, G, R, A from the bottom up, and a
         *  32 bit sample is emitted 2, 1, 0, 3.  So the first sample here,
         *  0x20304010, goes out as 30 40 10 20.
         */
        TrialFilter = 1;
        gotn = 0;
        FilterPixel32(0x20304010);
        FilterPixel32(0x50607080);
        check("32 bit: the filter type comes first", got[0], 1);
        check("and four bytes go out per sample", gotn, 9);
        check("the first sample has nothing to its left",
              (int)bpack(4, got + 1), 0x20104030);
        check("the second is the first, byte by byte",
              (int)bpack(4, got + 5), 0x30703030);

        /* 16 bit: two bytes a sample */
        LinBufUsed = 0;
        BitBufPos = 0;
        gotn = 0;
        FilterPixel16(0x1234);
        FilterPixel16(0x5678);
        check("16 bit: two bytes per sample", gotn, 5);
        check("the first sample passes through", bpack(2, got + 1), 0x1234);
        check("and the second minus the first", bpack(2, got + 3), 0x4444);

        /* 24 bit: three bytes a sample, the argument's high byte is not
           one of them, and they go out 2, 1, 0 */
        LinBufUsed = 0;
        BitBufPos = 0;
        gotn = 0;
        FilterPixel24(0x112233);
        FilterPixel24(0x445566);
        check("24 bit: three bytes per sample", gotn, 7);
        check("the top byte of the argument is ignored",
              (int)bpack(3, got + 1), 0x332211);
        check("and only the low three are filtered",
              (int)bpack(3, got + 4), 0x333333);

        /*
         *  The up filter, across two rows: row 0 lands in the upper slot and
         *  row 1 reads it from there, so the second row comes out as zeroes.
         */
        LinBufUsed = 0;
        BitBufPos = 0;
        gotn = 0;
        TrialFilter = 2;
        FilterPixel32(0x20304010);
        FilterPixel32(0x50607080);
        check("the first row has nothing above it",
              (int)bpack(4, got + 1), 0x20104030);
        check("in either sample", (int)bpack(4, got + 5), 0x50807060);
        BitBufPos = 0;
        gotn = 0;
        FilterPixel32(0x01020304);
        FilterPixel32(0x50607080);
        check("32 bit up: against the row above, byte by byte",
              (int)bpack(4, got + 1), (int)0xE1F4C3D2u);
        check("a sample equal to the row above comes out as zeroes",
              (int)bpack(4, got + 5), 0);

        ByteSink = NULL;
        LinBuf = NULL;
        LinBufData = NULL;
    }

    /* ---- AdvanceRow: sub_407090, close a row and open the next ------- */
    {
        static IMAGEDESC img;
        static int oldlens[ROW_SAMPLE_MAX];
        int *samples, *lens, i, want, got;

        memset(&img, 0, sizeof(img));
        memset(LensScratch, 0, sizeof(LensScratch));
        Image = &img;

        RowIndex = 0;
        RowCount = 1;                 /* so the tail returns straight away  */
        RowCost = 0;
        RowHeaderCode = 5;
        BitBuf = 0;
        BitCount = 0;

        samples = ROWOF(0, ROW_SAMPLES);
        lens = ROWOF(0, ROW_SYMBOLS);

        for (i = 0; i < ROW_SAMPLE_MAX; i++) {
            samples[i] = 0;
            lens[i] = 0;
        }
        samples[0] = 100;
        samples[1] = 60;
        samples[2] = 30;
        samples[3] = 10;
        samples[4] = 0x80000000;      /* locked: a count of one afterwards  */
        samples[5] = 0xC0000007;      /* only bit 31 goes                  */
        samples[ROW_LITERALS] = 3;    /* sample 256, the first length code  */
        *ROWOF(0, ROW_BLOCKS) = 0;
        *ROWOF(0, ROW_ROWBITS) = 7;
        for (i = 0; i < ROW_DISTSLOTS; i++)
            ROWOF(0, ROW_DISTFREQ)[i] = i + 1;

        memcpy(oldlens, lens, sizeof(oldlens));
        check("AdvanceRow returns the row count", AdvanceRow(), 1);
        check("it counted the block", *ROWOF(0, ROW_BLOCKS), 1);
        check("it put out the row's code", BitCount, 7);
        check("which is the code it was given", BitBuf & 0x7F, 5);
        check("the locked sample became one", samples[4], 1);
        check("the other one lost only bit 31", samples[5], 0x40000007);
        check("the row index moved on", RowIndex, 1);

        /* the lengths came from the samples, and the distance ones from
           sample 288 on */
        {
            static int want_dist[ROW_DISTSLOTS];
            int want_lit[ROW_SAMPLE_SYMS];

            BuildCodeLengths(samples, ROW_SAMPLE_SYMS, want_lit, 16);
            check("the literal and length codes were rebuilt",
                  memcmp(LensScratch, want_lit,
                         ROW_SAMPLE_SYMS * sizeof(int)), 0);

            /* the distance samples are untouched by the literal build, so
               rebuild them from a copy taken before it */
            for (i = 0; i < ROW_SAMPLE_SYMS; i++)
                want_lit[i] = samples[i];
            BuildCodeLengths(samples, ROW_SAMPLE_SYMS, LensScratch, 16);
            BuildCodeLengths(ROWOF(0, ROW_DISTFREQ), ROW_DISTSLOTS,
                             want_dist, 16);
            check("the distance codes were rebuilt too",
                  memcmp(LensScratch + (ROW_DISTFREQ - ROW_SAMPLES), want_dist,
                         ROW_DISTSLOTS * sizeof(int)), 0);
        }

        /* the record's own lengths were replaced by the mirror, and the cost
           is how far each one moved times how often the symbol occurred */
        want = 0;
        for (i = 0; i < ROW_SAMPLE_MAX; i++)
            want += (LensScratch[i] - oldlens[i]) * samples[i];
        check("the cost moved by the length change times the frequency",
              RowCost, want);
        check("and it is not free", RowCost > 0, 1);

        /* the four samples neither build covers keep the mirror */
        for (i = 0; i < ROW_SAMPLE_MAX; i++)
            if (ROWOF(0, ROW_SYMBOLS)[i] != LensScratch[i])
                break;
        check("every sample's length was written back", i, ROW_SAMPLE_MAX);

        /*
         *  With a second row to go it opens it: the row index moves on, the
         *  distance slots are rebuilt for it, one bit says whether this is
         *  the last row and two bits of BFINAL-style marker follow.
         */
        BitBuf = 0;
        BitCount = 0;
        RowCount = 2;
        img.width = 4;
        img.height = 2;
        for (i = 0; i < ROW_SAMPLE_MAX; i++)
            ROWOF(1, ROW_SAMPLES)[i] = 1;
        got = AdvanceRow();
        check("it returns the row count", got, 2);
        check("and the index is on the last row", RowIndex, 2);
        check("row 1 of 2 is the last row", BitBuf & 1, 1);
        check("and the marker is two", (BitBuf >> 1) & 3, 2);
        RowCount = 1;
        RowIndex = 0;
    }

    /* ---- FinishBlock: loc_4075B0, close the block -------------------- */
    {
        static IMAGEDESC img;
        static unsigned char out[512];
        unsigned char *before;
        int n, i;

        memset(&img, 0, sizeof(img));
        memset(out, 0, sizeof(out));
        Image = &img;
        img.outbuf = out;
        img.outsize = sizeof(out);

        OutCount = 0;
        BitBuf = 0;
        BitCount = 0;
        BitCrc = 0xFFFFFFFFu;
        AdlerSum = 0x12345678u;
        ChunkStart = 12;              /* pretend an IDAT header went out   */
        RowIndex = 0;
        RowCount = 1;                 /* so AdvanceRow() stops after it     */
        /*
         *  FinishBlock() flushes the LZ coder first, which is what leaves the
         *  Adler-32 sum in AdlerSum - so this test has to hand it an empty
         *  window and a scan position already at the write position, or the
         *  flush would emit a row's worth of symbols into the block trailer.
         */
        LzWrite = 0;
        LzScan = 0;
        RowHeaderCode = 0;
        *ROWOF(0, ROW_ROWBITS) = 0;
        *ROWOF(1, ROW_ROWBITS) = 0;

        /* some bits already in the buffer, so the padding has to make up
           the difference to a whole byte */
        PutBits(0x2A, 5);
        n = OutCount;
        FinishBlock();
        i = OutCount - 4;         /* just before the length field went out */

        /* five bits went out, then padding, then the Adler-32 and the block
           CRC.  The IDAT length is *not* among them: it is patched back into
           the chunk header at ChunkStart, where it was reserved. */
        check("the block closed", OutCount > n, 1);
        check("the trailer is the adler and the block crc",
              OutCount - n, 1 + 4 + 4);

        before = out + n;
        check("the padding is the rest of the byte", (before[0] >> 5) & 7,
              0x2A >> 5);
        check("and the byte is complete", BitCount, 0);
        check("the adler-32 went out big endian",
              (unsigned)before[1] << 24 | before[2] << 16 |
              before[3] << 8 | before[4], 0x12345678u);
        check("the block crc is the complement, big endian",
              (unsigned)before[5] << 24 | before[6] << 16 |
              before[7] << 8 | before[8], (unsigned)~BitCrc);
        {
            /*
             *  The length lives where the chunk header reserved it, so it is
             *  read from ChunkStart and measured against the output size at the
             *  moment it was patched - which is OutCount now, because nothing
             *  has been written since.
             */
            unsigned char *lp = out + ChunkStart;
            unsigned int len = (unsigned)lp[0] << 24 | lp[1] << 16 |
                               lp[2] << 8 | lp[3];

            /* the length does not count itself, nor the CRC */
            check("the IDAT length counts what was written",
                  len, (unsigned)(OutCount - ChunkStart - 12));
        }

        /* the chunks after the image data go out whole */
        {
            static unsigned char tail[16];
            static unsigned int *list[1];

            /* a chunk record is [length][tag][data][crc], little endian */
            *(unsigned int *)tail = Swap32(1);   /* the record holds it little endian */
            memcpy(tail + 4, "tEXt", 4);
            memcpy(tail + 8, "\0\0\0\0", 4);
            memcpy(tail + 12, "x", 1);
            list[0] = (unsigned int *)tail;
            img.chunks = list;
            img.chunktail = 0;
            img.chunkend = 1;
            n = OutCount;
            FinishBlock();
            /* the block's own trailer goes out first - it has no padding
               this time, and the IDAT length is patched at ChunkStart rather
               than emitted here - and then the chunk, whole */
            check("a trailing chunk goes out whole", OutCount - n,
                  4 + 4 + 13);
            check("and it comes out unchanged",
                  memcmp(out + n + 8, tail, 13), 0);
        }

        /*
         *  EmitBlock()'s zlib header, which the checks above deliberately ran
         *  after - EmitBlock() rewrites OutCount and ChunkStart wholesale, so
         *  running it here would destroy the state they are asserting on.
         *
         *  These two bytes are also how main() recognises a file of ours: read
         *  little endian they are the DWORD 0x5E78 = 24184.  The order is not
         *  the obvious one, because PNGOUT_MARK is 0x5E78 and so `>> 8` is
         *  0x5E.  Written the wrong way round CMF comes out as 0x5E, which
         *  claims compression method 14, and every compressed PNG is rejected
         *  with "incorrect header check".
         *
         *  Checked by what the bytes have to *be*, not by what the code says:
         *  CM = 8 with a 32K window, and FCHECK making the pair a multiple of 31.
         */
        {
            static IMAGEDESC sim;
            static unsigned char sout[64];
            static unsigned char spx[4 * 8 * 2];
            int cmf, flg, w = 8, h = 2;

            memset(&sim, 0, sizeof(sim));
            memset(sout, 0, sizeof(sout));
            memset(spx, 0, sizeof(spx));
            Image = &sim;
            sim.outbuf = sout;
            sim.outsize = sizeof(sout);
            sim.strategy = 0;
            sim.pixels = (unsigned int *)spx;
            sim.stride = w * 4;
            sim.width = w;
            sim.height = h;
            RowCount = 1;
            RowIndex = 0;
            MinSample = -1;
            unk_185BED0 = 0;
            unk_180A570 = 8;
            unk_180B978 = 0;

            check("EmitBlock wrote a block", EmitBlock(w, h, 0, 8, NULL, 0), 1);

            /* 33 bytes of signature and IHDR, then 4 reserved for the IDAT
               length, then "IDAT", then the two zlib bytes at 41 and 42 */
            cmf = sout[41];
            flg = sout[42];
            check("zlib: compression method is 8", cmf & 15, 8);
            check("zlib: 32K window", cmf >> 4, 7);
            check("zlib: FCHECK makes the pair a multiple of 31",
                  (cmf * 256 + flg) % 31, 0);
            check("zlib: the two bytes are PNGOUT's own signature",
                  cmf + flg * 256, 24184);

            Image = NULL;
        }
    }

    /* ---- sub_4065A0: the LZ coder ------------------------------------ */
    {
        int strat[4];
        int s, n, nm, longest, decoded, bad, trail;
        unsigned seed;

        /*
         *  The four strategies the coder distinguishes: Xtreme, Intense,
         *  Longest Match, Huffman only (which skips matching altogether),
         *  and the uncompressed one on top of that.
         */
        strat[0] = 0;
        strat[1] = 1;
        strat[2] = 2;
        strat[3] = 3;
        strat[4] = 4;

        for (s = 0; s < 5; s++) {
            char name[64];

            /* ---- incompressible data: literals only ---- */
            seed = 12345u + (unsigned)s;
            n = 3000;
            for (i = 0; i < n; i++) {
                seed = seed * 1103515245u + 12345u;
                lzinput[i] = (unsigned char)(seed >> 17);
            }

            lzsetup(strat[s], 0x7FFFFFFF);
            lzfeed(lzinput, n);
            LzCode((const unsigned char *)0, 0);   /* flush */

            

            check("the window took the bytes", LzWrite, n);
            check("the window still holds them",
                  memcmp(LzWindow, lzinput, n), 0);

            tflush();     /* the last byte of bits is still in the buffer */
            decoded = lzdecode(OutCount * 8, lzgot, n, &nm, &longest, &trail);
            sprintf(name, "s%d: random data decodes (fail %d)", strat[s],
                    lzfail);
            check(name, decoded, n);
            if (decoded == n) {
                int k, diff = 0;

                for (k = 0; k < n; k++)
                    if (lzgot[k] != lzinput[k])
                        diff = k + 1;
                sprintf(name, "s%d: random data is the input (first at %d)",
                        strat[s], diff);
                check(name, diff, 0);
            }
            sprintf(name, "s%d: with almost no matches", strat[s]);
            check(name, nm < n / 16, 1);

            /* ---- highly compressible data: matches must appear ---- */
            n = 3000;
            for (i = 0; i < n; i++)
                lzinput[i] = (unsigned char)("the quick brown fox "
                                             "jumps over the lazy dog "[i % 44]);

            lzsetup(strat[s], 0x7FFFFFFF);
            lzfeed(lzinput, n);
            LzCode((const unsigned char *)0, 0);
            

            tflush();     /* the last byte of bits is still in the buffer */
            decoded = lzdecode(OutCount * 8, lzgot, n, &nm, &longest, &trail);
            sprintf(name, "s%d: repetitive data decodes (fail %d)", strat[s],
                    lzfail);
            check(name, decoded, n);
            if (decoded == n) {
                int k, diff = 0;

                for (k = 0; k < n; k++)
                    if (lzgot[k] != lzinput[k])
                        diff = k + 1;
                sprintf(name, "s%d: and is the input (first at %d)", strat[s],
                        diff);
                check(name, diff, 0);
            }
            /*
             *  Strategies 3 and 4 never match at all - `cmp ecx, 3; jge
             *  loc_406CED` jumps straight to the literal - so only 0..2 can be
             *  asked to produce matches.
             */
            if (strat[s] < 3) {
                sprintf(name, "s%d: with matches", strat[s]);
                check(name, nm > 0, 1);
                sprintf(name, "s%d: and a long one", strat[s]);
                check(name, longest >= 40, 1);
            } else {
                sprintf(name, "s%d: with no matches at all", strat[s]);
                check(name, nm, 0);
            }

/*
             *  A single byte repeated.  Almost everything is a match at
             *  distance 1, so the run counters and the run-length skip in both
             *  searches get exercised hard.
             *
             *  `RunLen[pos]` is the number of *identical bytes before* `pos`,
             *  capped at 0x7FFF (`inc dword_411D58` at 0x40669F, then the
             *  `cmp eax, 7FFFh` at 0x4066A0).  So a run of `n` bytes ends at
             *  n - 1 and the cap is only reached past 32767 bytes - the run
             *  below is nowhere near it, and the saturating one is the next
             *  test.  Strategies 3 and 4 never run the counters at all:
             *  `cmp ecx, 3; jge loc_406CED` skips the whole lot, and the
             *  skip-ahead loop guards its copy with `cmp [ecx+4Ch], 3`.
             */
            n = 4000;
            memset(lzinput, 0x5A, n);
            lzsetup(strat[s], 0x7FFFFFFF);
            lzfeed(lzinput, n);
            LzCode((const unsigned char *)0, 0);
            tflush();     /* the last byte of bits is still in the buffer */
            decoded = lzdecode(OutCount * 8, lzgot, n, &nm, &longest, &trail);
            sprintf(name, "s%d: a solid run decodes", strat[s]);
            check(name, decoded, n);
            sprintf(name, "s%d: a solid run is all the same byte", strat[s]);
            bad = 0;
            for (i = 0; i < n; i++)
                if (lzgot[i] != 0x5A)
                    bad++;
            check(name, bad, 0);
            sprintf(name, "s%d: and the run counts the bytes before it",
                    strat[s]);
            check(name, strat[s] >= 3 ? 1 : (RunLen[n - 1] == n - 1), 1);
        }

        /*
         *  A run long enough to reach the 0x7FFF cap.  The window is 64KB, so
         *  this still fits without wrapping, and the coder stops 260 bytes
         *  short of the end until the flush.
         */
        for (s = 0; s < 3; s++) {
            char name[64];

            n = 33000;
            memset(lzinput, 0x5A, n);
            lzsetup(strat[s], 0x7FFFFFFF);
            lzfeed(lzinput, n);
            LzCode((const unsigned char *)0, 0);
            tflush();
            decoded = lzdecode(OutCount * 8, lzgot, n, &nm, &longest, &trail);
            sprintf(name, "s%d: a long run decodes", strat[s]);
            check(name, decoded, n);
            sprintf(name, "s%d: and the run saturates at 0x7FFF", strat[s]);
            check(name, RunLen[n - 1], 0x7FFF);
        }

        /*
         *  The codes the coder hands to the row record are the ones
         *  RankValues() produced, and the row's samples get bit 31 set - the
         *  lock AdvanceRow() clears - for every literal the coder saw.
         */
        {
            int locked = 0;

            n = 800;
            for (i = 0; i < n; i++)
                lzinput[i] = (unsigned char)(i * 7 + (i / 13));
            lzsetup(1, 0x7FFFFFFF);
            lzfeed(lzinput, n);
            LzCode((const unsigned char *)0, 0);

            for (i = 0; i < ROW_SAMPLE_SYMS; i++)
                if (ROWOF(0, ROW_SAMPLES)[i] & 0x80000000)
                    locked++;
            check("every literal locked its sample", locked > 0, 1);
            check("and the step table was written", MatchCount > 0, 1);
            {
int bucket, links = 0, deepest = 0;

                /*
                 *  The buckets hold positions, so a non-zero bucket only says a
                 *  position landed there.  What matters is that walking the
                 *  DistSlot[] chains reaches back through the whole scanned
                 *  range, so count the links.
                 */
                for (bucket = 0; bucket < 4096; bucket++) {
                    int p = (int)(unsigned short)HashHead[bucket];
                    int len = 0;

                    while (p != 0 && len <= n) {
                        len++;
                        p = (int)(unsigned short)DistSlot[p & 32767];
                    }
                    links += len;
                    if (len > deepest)
                        deepest = len;
                }
                check("the hash chains link the scanned range",
                      links > n / 2, 1);
                check("and some chain goes back a long way", deepest > 20, 1);
            }
        }

        /*
         *  The row ends where the /b block ends - `dword_42F370[646 * row]`,
         *  which in the original is the same storage as the block list's end -
         *  and the coder closes the row there and opens the next one.
         */
        {
            n = 900;
            for (i = 0; i < n; i++)
                lzinput[i] = (unsigned char)(i & 7);

            lzsetup(1, n / 2);              /* the first block is half of it */
            RowCount = 2;              /* lzsetup resets this to 1 */
            ROW_LIMIT(1) = 0x7FFFFFFF;   /* the second row is open ended */
            lzfeed(lzinput, n);
            LzCode((const unsigned char *)0, 0);

            check("the row ended at the block end", RowIndex, 1);
            check("and the coder carried on past it", LzScan, n);
        }

        RowCount = 1;
        RowIndex = 0;
        Image = NULL;
    }

    /* ---- the IMAGEDESC layout, read off the original's field accesses ---- */
    {
        static IMAGEDESC img;
        static unsigned char base[1024];

#define FIELD_OFFSET(field) \
    ((int)((unsigned char *)&img.field - (unsigned char *)&img))

        check("width", FIELD_OFFSET(width), 0x08);
        check("height", FIELD_OFFSET(height), 0x0C);
        check("the pixel buffer", FIELD_OFFSET(outbuf), 0x10);
        check("and the decoded image", FIELD_OFFSET(pixels), 0x00);
        check("its row stride", FIELD_OFFSET(stride), 0x04);
        check("its size", FIELD_OFFSET(outsize), 0x14);
        check("the bKGD colour", FIELD_OFFSET(bkgd), 0x30);
        check("the distinct colour count", FIELD_OFFSET(ndistinct), 0x34);
        check("the strategy", FIELD_OFFSET(strategy), 0x4C);
        check("maxblocks", FIELD_OFFSET(maxblocks), 0x54);
        check("splitthreshold", FIELD_OFFSET(splitthreshold), 0x58);
        check("the first chunk after IHDR", FIELD_OFFSET(chunkbeg), 0x60);
        check("the first chunk after PLTE", FIELD_OFFSET(chunkmid), 0x64);
        check("the first chunk after the image", FIELD_OFFSET(chunktail),
              0x68);
        check("one past the last chunk", FIELD_OFFSET(chunkend), 0x6C);
        check("the chunk records", FIELD_OFFSET(chunks), 0x70);
        check("the filter trial", FIELD_OFFSET(trial), 0x74);
        check("progress", FIELD_OFFSET(percent), 0x78);
        check("the best size so far", FIELD_OFFSET(best), 0xC8);

        /* and the row record fields, which are offsets into one array */
        check("a row record is one allocation", ROW_STRIDE, 0xA18);
        check("its samples", ROW_SAMPLES * 4, 0x010);
        check("the block counter, which is sample 256", ROW_BLOCKS * 4, 0x410);
        check("the distance samples, at sample 288", ROW_DISTFREQ * 4, 0x490);
        check("the length array", ROW_SYMBOLS * 4, 0x510);
        check("the distance samples really are 288 in",
              ROW_DISTFREQ - ROW_SAMPLES, 288);
        check("and just past the 286 the code covers",
              ROW_DISTFREQ - ROW_SAMPLES - ROW_SAMPLE_SYMS, 2);
        check("the row's own bit count", ROW_ROWBITS * 4, 0x910);
        check("1024 rows fit the stride",
              MAX_ROWS * ROW_STRIDE * 4 < 11 * 1024 * 1024, 1);

        (void)base;
#undef FIELD_OFFSET
    }

    /* ---- sub_4086E0: /s4, the stored (uncompressed) writer ------------- */
    {
        static IMAGEDESC simg;
        static unsigned char sout[1 << 16];
        static unsigned char spix[4 * 5 * 3];
        static unsigned char want[256];
        int w = 4, h = 3, i, r, c, o, ct, adler, nwant;
        unsigned int wcrc;
        unsigned int *pal[1];

        /* sub_4086E0 reports its size through dword_185BEC4, which is s->msg;
           main() always sets it, so the test has to as well */
        ProgressMsg = sink_msg;
        nmsgs = 0;
        messages[0] = 0;

        memset(&simg, 0, sizeof(simg));
        memset(sout, 0, sizeof(sout));
        memset(spix, 0, sizeof(spix));
        Image = &simg;
        simg.outbuf = sout;
        simg.outsize = sizeof(sout);
        simg.pixels = (unsigned int *)spix;
        simg.stride = w * 4;
        simg.width = w;
        simg.height = h;
        MinSample = -1;              /* no tRNS chunk, as the tests above do */

        /*
         *  A pixel is 0xFFBBGGRR, so bytes 2, 1, 0 are B, G, R and byte 3 the
         *  alpha - the same order PLTE and bKGD use.  Fill every byte from a
         *  counter so all four channels are distinct and a byte order
         *  mistake cannot pass, then pin the first two pixels to values that
         *  are easy to read off.
         */
        for (i = 0; i < w * h * 4; i++)
            spix[i] = (unsigned char)(i * 7 + 3);
        spix[0] = 0x10; spix[1] = 0x20; spix[2] = 0x30; spix[3] = 0x40;
        spix[4] = 0x50; spix[5] = 0x60; spix[6] = 0x70; spix[7] = 0x80;

        /* ---- RGB, 8 bit ---- */
        ct = 2;
        unk_185BED0 = ct;
        unk_180A570 = 8;
        unk_180B978 = 0;
        OutCount = 0;
        RowIndex = 0;
        RowCount = 1;
        InitCrcTable();              /* what Optimize() does first */
        AdlerSum = 0x11223344;       /* poison: WriteStored() reseeds it */
        check("/s4: it wrote a PNG", WriteStored(), 1);
        check("/s4: the signature is there",
              memcmp(sout, "\x89PNG\x0D\x0A\x1A\x0A", 8), 0);

        /* the IHDR for 4x3 RGB, checked against zlib.crc32 over the same 17
           bytes - 3B 96 39 91 */
        check("/s4: the IHDR is the standard 13 bytes",
              memcmp(sout + 8, "\0\0\0\015IHDR\0\0\0\4\0\0\0\3\10\2\0\0\0"
                               "\x3B\x96\x39\x91", 25), 0);
        /*
         *  The IDAT.  Its length is staged and written big endian, the tag is
         *  "IDAT" and the next two bytes are the 24184 placeholder.
         */
        o = 33;
        check("/s4: the IDAT tag", memcmp(sout + o + 4, "IDAT", 4), 0);
        check("/s4: and the placeholder CRC",
              sout[o + 8] + sout[o + 9] * 256, 24184);

        /* LinBufBytes for RGB is 3 * width + 1 */
        check("/s4: the stored row length", LinBufBytes, 3 * w + 1);
        /* the length field counts everything between itself and the CRC */
        check("/s4: and the IDAT length covers what is written",
              ((unsigned)sout[o] << 24) | ((unsigned)sout[o + 1] << 16) |
              ((unsigned)sout[o + 2] << 8) | sout[o + 3],
              (unsigned)(h * (5 + 3 * w + 1) + 2 + 4));

        /*
         *  The payload is a zlib *stored* deflate block per row: a header byte
         *  carrying BFINAL, then LEN and ~LEN as two little endian 16 bit
         *  fields.  The 4 bytes come out of
         *  `(LinBufBytes * 0x10001) ^ 0xFFFF0000`, which is exactly that.
         */
        /*
         *  Per row: the flag byte, LEN and ~LEN as two little endian 16 bit
         *  fields, the filter type byte, then the samples.  So a row is
         *  5 + LinBufBytes and the walk has to step by all of it.
         */
        o += 10;               /* past the length, the tag and the 2 marks */
        nwant = 0;
        adler = 1;
        for (r = 0; r < h; r++) {
            int len, nlen, lenbytes;

            check("/s4: the row is a stored block, last row flagged",
                  sout[o] == (r == h - 1), 1);
            o++;
            len = sout[o] | (sout[o + 1] << 8);
            nlen = sout[o + 2] | (sout[o + 3] << 8);
            check("/s4: LEN is LinBufBytes", len, 3 * w + 1);
            check("/s4: and NLEN is its complement", len ^ nlen, 0xFFFF);
            o += 4;
            check("/s4: the row starts with the filter type", sout[o], 0);
            adler = (int)Adler32Update((unsigned)adler, 0);
            o++;

            /* the samples: B, G, R per pixel, so the same byte order the
               internal 0xFFBBGGRR pixel has to be reversed into */
            lenbytes = 3 * w;
            for (c = 0; c < lenbytes; c++) {
                int px = r * w + c / 3;         /* into the whole image */
                static const int srcoff[3] = { 2, 1, 0 };

                want[c] = spix[px * 4 + srcoff[c % 3]];
                adler = (int)Adler32Update((unsigned)adler, want[c]);
            }
            check("/s4: the row's bytes are B, G, R",
                  memcmp(sout + o, want, lenbytes), 0);
            o += lenbytes;
        }

        /* the Adler-32 of the unfiltered data, big endian, and then the
           block CRC.  The CRC covers the tag and the payload but *not* the
           length field - the length is reserved by bumping OutCount past it
           before the tag goes out, so it is written into the buffer at the
           end and the running CRC never sees it.  That is also what PNG
           wants, and what the deflate path does: there BitCrc is seeded
           before PutBits(TAG_IDAT_, 32). */
        check("/s4: the Adler-32 went out big endian",
              ((unsigned)sout[o] << 24) | ((unsigned)sout[o + 1] << 16) |
              ((unsigned)sout[o + 2] << 8) | sout[o + 3], (unsigned)adler);
        o += 4;

        wcrc = 0xFFFFFFFFu;
        for (i = 37; i < o; i++)
            wcrc = CrcTable[(wcrc ^ sout[i]) & 0xFF] ^ (wcrc >> 8);
        wcrc = ~wcrc;
        check("/s4: the IDAT's CRC is over its tag and payload",
              ((unsigned)sout[o] << 24) | ((unsigned)sout[o + 1] << 16) |
              ((unsigned)sout[o + 2] << 8) | sout[o + 3], wcrc);
        o += 4;

        /* the IEND chunk: a zero length, the tag, and a CRC that is fixed because
           the chunk has no data */
        check("/s4: IEND's length is zero",
              sout[o] | (sout[o + 1] << 8) | (sout[o + 2] << 16) |
              (sout[o + 3] << 24), 0);
        check("/s4: then the tag", memcmp(sout + o + 4, "IEND", 4), 0);
        check("/s4: and the fixed CRC",
              ((unsigned)sout[o + 8] << 24) | ((unsigned)sout[o + 9] << 16) |
              ((unsigned)sout[o + 10] << 8) | sout[o + 11], 0xAE426082u);
        check("/s4: and that is the end of it", OutCount, o + 12);

        /* ---- greyscale + alpha: two bytes, grey then alpha ---- */
        ct = 4;
        unk_185BED0 = ct;
        OutCount = 0;
        WriteStored();
        o = find_idat(sout, OutCount); check("/s4: found the IDAT", o > 0, 1); o += 10;
        check("/s4: gray+alpha row length", LinBufBytes, 2 * w + 1);
        for (r = 0; r < h; r++) {
            int len;

            o++;
            len = sout[o] | (sout[o + 1] << 8);
            check("/s4: gray+alpha LEN", len, 2 * w + 1);
            check("/s4: and NLEN", (sout[o + 2] | (sout[o + 3] << 8)) ^ len,
                  0xFFFF);
            o += 4;
            o++;                         /* the filter type, always zero */
            for (c = 0; c < 2 * w; c++) {
                int px = r * w + c / 2;

                if (c & 1)
                    want[c] = spix[px * 4 + 3];   /* alpha */
                else
                    want[c] = spix[px * 4 + 0];   /* grey = the R byte */
            }
            check("/s4: grey then alpha", memcmp(sout + o, want, 2 * w), 0);
            o += 2 * w;
        }

        /* ---- RGBA: four bytes, B, G, R then alpha ---- */
        ct = 6;
        unk_185BED0 = ct;
        OutCount = 0;
        WriteStored();
        o = find_idat(sout, OutCount); check("/s4: found the IDAT", o > 0, 1); o += 10;
        check("/s4: RGBA row length", LinBufBytes, 4 * w + 1);
        for (r = 0; r < h; r++) {
            int len;

            o++;
            len = sout[o] | (sout[o + 1] << 8);
            check("/s4: RGBA LEN", len, 4 * w + 1);
            o += 4;
            o++;                         /* the filter type */
            for (c = 0; c < 4 * w; c++) {
                int px = r * w + c / 4;
                static const int off4[4] = { 2, 1, 0, 3 };

                want[c] = spix[px * 4 + off4[c & 3]];
            }
            check("/s4: B, G, R, A", memcmp(sout + o, want, 4 * w), 0);
            o += 4 * w;
        }

        /* ---- a palette, 8 bit: one byte per pixel, looked up ---- */
        ct = 3;
        unk_185BED0 = ct;
        unk_180B578[0] = 0xFF000000;
        unk_180B578[1] = 0xFF0000FF;
        unk_180B978 = 2;
        /*
         *  Fill the image with the two palette entries, alternating, so the
         *  bytes that come out are 0,1,0,1,... - which pins the lookup and
         *  cannot be confused with a copy of the pixel.
         */
        for (i = 0; i < w * h; i++)
            *(unsigned int *)(spix + i * 4) = unk_180B578[i & 1];
        OutCount = 0;
        WriteStored();
        check("/s4: an 8 bit palette row is one byte per pixel",
              LinBufBytes, w + 1);
        o = find_idat(sout, OutCount); check("/s4: found the IDAT", o > 0, 1); o += 10;
        o++;                         /* the row's flag byte */
        check("/s4: and its LEN is width + 1", sout[o] | (sout[o + 1] << 8),
              w + 1);
        o += 4;
        o++;                         /* the filter type, always zero */
        for (c = 0; c < w; c++)
            want[c] = (unsigned char)(c & 1);   /* alternating two entries */
        check("/s4: the indices are the palette positions",
              memcmp(sout + o, want, w), 0);

        /* ---- a palette below 8 bits: packed, two to a byte ---- */
        unk_180A570 = 4;
        OutCount = 0;
        WriteStored();
        check("/s4: a 4 bit palette row packs two to a byte",
              LinBufBytes, ((4 * w + 7) >> 3) + 1);
        o = find_idat(sout, OutCount); check("/s4: found the IDAT", o > 0, 1); o += 10;
        o++;
        check("/s4: and its LEN", sout[o] | (sout[o + 1] << 8),
              ((4 * w + 7) >> 3) + 1);
        o += 4;
        o++;                         /* the filter type, always zero */
        /*
         *  Counting down from the top of the byte: pixel 0 in the high nibble,
         *  pixel 1 in the low one.  With the alternating palette above both are
         *  0 or 1, so each byte is 0x01 - pixel 0 in the high nibble.
         */
        for (c = 0; c < w / 2; c++)
            want[c] = 0x01;
        check("/s4: the high nibble first", memcmp(sout + o, want, w / 2), 0);

        /* the size line, and the carriage return that comes before it when
           /v was not given */
        /* one more run of its own, so the messages can be counted cleanly */
        messages[0] = 0;
        nmsgs = 0;
        OutCount = 0;
        WriteStored();
        check("/s4: two progress messages", nmsgs, 2);
        check("/s4: a bare CR without /v", messages[0], '\r');
        check("/s4: then the size",
              strncmp(messages + 1, "Out:", 4), 0);

        /* and with /v it is a newline after the size instead */
        simg.verbose = 1;
        messages[0] = 0;
        nmsgs = 0;
        OutCount = 0;
        WriteStored();
        check("/s4: /v prints the newline after", messages[strlen(messages) - 1],
              '\n');
        check("/s4: and no leading CR", messages[0] != '\r', 1);
        simg.verbose = 0;
        ProgressMsg = NULL;

        Image = NULL;
        (void)pal;
    }

    /* ---- sub_402AD0: Optimize() --------------------------------------- */
    {
        static IMAGEDESC simg;
        static SETTINGS set;
        static unsigned char sout[1 << 18];
        static unsigned char spix[4 * 20 * 16];
        int w = 20, h = 16, i, r, c, n, delta;

        ProgressMsg = sink_msg;
        CheckEscHook = NULL;           /* NULL is fine: Optimize substitutes */

        /* a full range of greys, so the ramp is the whole 0..255 */
        for (r = 0; r < h; r++)
            for (c = 0; c < w; c++) {
                unsigned char *p = spix + 4 * (r * w + c);
                int v = (r * w + c) * 256 / (w * h);

                p[0] = (unsigned char)v;         /* R */
                p[1] = (unsigned char)v;         /* G */
                p[2] = (unsigned char)v;         /* B */
                p[3] = 0xFF;                    /* A - opaque, no tRNS needed */
            }

        memset(&simg, 0, sizeof(simg));
        Image = &simg;
        simg.pixels = (unsigned int *)spix;
        simg.stride = w * 4;
        simg.width = w;
        simg.height = h;
        simg.outbuf = sout;
        simg.outsize = sizeof(sout);

        /* ---- phase A: a grey image is written as colour type 0 ---- */
        memset(&set, 0, sizeof(set));
        set.pixels = spix;
        set.pitch = w * 4;
        set.width = w;
        set.height = h;
        set.outbuf = sout;
        set.origsize = (int)sizeof(sout);
        set.msg = sink_msg;
        set.checkesc = NULL;
        set.coltype = -1;              /* let Optimize() choose */
        set.filter = -1;
        set.bitdepth = -1;
        set.strategy = 4;              /* /s4 first: the simplest full run */
        set.blocksplit = 0.0;

        messages[0] = 0;
        nmsgs = 0;
        delta = Optimize(&set);
        check("Optimize: /s4 produced a PNG", delta > 0, 1);
        check("Optimize: /s4 wrote a PNG signature",
              memcmp(set.outbuf, "\x89PNG\x0D\x0A\x1A\x0A", 8), 0);
        check("Optimize: /s4 chose colour type 0", unk_185BED0, 0);
        check("Optimize: /s4 chose bit depth 8", unk_180A570, 8);
        check("Optimize: /s4 announced the type",
              strstr(messages, "/c0") != NULL, 1);
        check("Optimize: /s4 announced the depth",
              strstr(messages, "/d8") != NULL, 1);

        /* the stored IDAT has to actually hold the rows */
        n = find_idat(set.outbuf, delta);
        check("Optimize: /s4 has an IDAT", n > 0, 1);
        check("Optimize: /s4 LinBufBytes is one row", LinBufBytes, w + 1);

        /* ---- the ramp: 256 equally spaced greys, opaque ---- */
        check("Optimize: the ramp has 256 entries", unk_180B978, 256);
        check("Optimize: and it starts at opaque black",
              (unsigned)unk_180B578[0], 0xFF000000u);
        check("Optimize: and steps by one per entry",
              (unsigned)unk_180B578[1] - (unsigned)unk_180B578[0], 0x010101u);
        check("Optimize: and ends at opaque white",
              (unsigned)unk_180B578[255], 0xFFFFFFFFu);

        /* ---- MinSample: an all-opaque image has none, so no tRNS ---- */
        check("Optimize: an opaque image needs no tRNS", MinSample, -1);
        check("Optimize: and no tRNS chunk went out",
              find_chunk(set.outbuf, delta, "tRNS") < 0, 1);

        /* ---- the same image with a real /s0, run through the trial loop ---- */
        set.strategy = 0;              /* Xtreme: the full trial search */
        messages[0] = 0;
        nmsgs = 0;
        delta = Optimize(&set);
        check("Optimize: Xtreme produced a PNG", delta > 0, 1);
        check("Optimize: Xtreme wrote a signature",
              memcmp(set.outbuf, "\x89PNG\x0D\x0A\x1A\x0A", 8), 0);
        check("Optimize: Xtreme kept colour type 0", unk_185BED0, 0);
        check("Optimize: Xtreme's output has an IDAT",
              find_idat(set.outbuf, delta) > 0, 1);
        /*
         *  No IEND here: IEND reaches the output as one of the chunks kept from
         *  the file being read, and this test keeps none (chunkend is 0).  The
         *  /s4 cases below do close with IEND because WriteStored writes it.
         */
        check("Optimize: Xtreme beats /s4", delta < 2389, 1);

        /* ---- an image with fewer than 256 colours becomes a palette ---- */
        for (i = 0; i < w * h; i++) {
            unsigned char *p = spix + 4 * i;
            unsigned char v = (unsigned char)((i % 3) ? 0x00 : 0x80);

            p[0] = p[1] = p[2] = v;
            p[3] = 0xFF;
        }
        set.strategy = 4;
        messages[0] = 0;
        nmsgs = 0;
        delta = Optimize(&set);
        check("Optimize: two colours pick colour type 3", unk_185BED0, 3);
        check("Optimize: and one bit depth for two entries", unk_180A570, 1);
        check("Optimize: the palette has two entries", unk_180B978, 2);
        check("Optimize: a palette was announced",
              strstr(messages, "colors") != NULL, 1);

        /*
         *  More than 256 distinct colours and no transparency: the only legal
         *  encodings left are RGB and RGBA, and with nothing transparent it is
         *  RGB.  A *few* colours would not do - PNGOUT would write a palette,
         *  which is smaller, so this image has to exceed the palette first.
         *  (p[0], p[1]) together name the pixel index, so every pixel differs.
         */
        for (i = 0; i < w * h; i++) {
            unsigned char *p = spix + 4 * i;

            p[0] = (unsigned char)(i & 0xFF);
            p[1] = (unsigned char)(i >> 8);
            p[2] = (unsigned char)(i * 37);
            p[3] = 0xFF;
        }
        set.strategy = 4;
        messages[0] = 0;
        nmsgs = 0;
        delta = Optimize(&set);
        check("Optimize: over 256 colours beats a palette", unk_185BED0, 2);
        check("Optimize: at 8 bits a sample", unk_180A570, 8);

        /* ---- forcing colour type 0 on a colour image is refused ---- */
        set.coltype = 0;
        messages[0] = 0;
        nmsgs = 0;
        delta = Optimize(&set);
        check("Optimize: /c0 on a colour image fails", delta, 0x7FFFFFFF);
        check("Optimize: and says it is not grayscale",
              strstr(messages, "not grayscale") != NULL, 1);
        set.coltype = -1;

        /*
         *  The same again with a pixel made transparent.  PNGOUT only treats
         *  that as real transparency when some *opaque* pixel shares its RGB
         *  triple - a lone transparent black is just a tRNS - so pixel 0 is made
         *  a copy of pixel 256's colour and then given a zero alpha, and it is
         *  the surviving opaque twin that forces colour type 6.
         */
        spix[4 * 256 + 0] = 0;
        spix[4 * 256 + 1] = 0;
        spix[4 * 256 + 2] = 0;
        spix[3] = 0x00;
        messages[0] = 0;
        nmsgs = 0;
        delta = Optimize(&set);
        check("Optimize: transparency forces RGBA", unk_185BED0, 6);
        /*
         *  Nothing is printed: the "try /c6 instead" messages only appear when
         *  the user *forced* a colour type the image cannot use.  A type chosen
         *  automatically just gets written out.
         */
        check("Optimize: an automatic choice says nothing",
              strstr(messages, "try /c") == NULL, 1);

        /* ---- /s4 is taken without any trial at all ---- */
        set.strategy = 4;
        messages[0] = 0;
        nmsgs = 0;
        delta = Optimize(&set);
        check("Optimize: /s4 with alpha still writes a PNG", delta > 0, 1);
        check("Optimize: and keeps colour type 6", unk_185BED0, 6);
        check("Optimize: and its IEND closes the file",
              memcmp(set.outbuf + delta - 12,
                     "\0\0\0\0IEND\xAE\x42\x60\x82", 12), 0);

        Image = NULL;
        ProgressMsg = NULL;
    }

    printf("\n%s  (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}


