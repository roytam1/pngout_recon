/*
 *  decode.c   -   0x409560 DecodeImage(), the PNG path
 *
 *  PNGOUT carries its own inflate, so this file holds the whole of the PNG
 *  decoder: the chunk walk, a bit reader with the input pump, the Huffman
 *  helpers, the inflate body, the unfilter, and the expansion of a finished row
 *  into 32bpp pixels.
 *
 *  Pixels come out one DWORD each, 0xFFBBGGRR, so a pixel reads B, G, R, FF
 *  going up in memory.  That is what the optimiser's row feeders expect; see
 *  FeedRgbRow in src/trials.c.
 *
 *  Four things here are easy to get wrong and are commented where they appear:
 *
 *    - the bit offset counts *up* towards zero from a large negative value and
 *      the anchor is the *tail* of the region, so IDAT setup points it at
 *      data + len - 4;
 *    - the fast path peeks without consuming;
 *    - the deflate extra fields are read least significant bit first;
 *    - the first four distance codes have no extra bits at all.
 *
 *  All of that is checked against zlib - see README.md, which also records the
 *  trap in the unfilter's loop: the jnb at 0x40AB9D skips only the expansion,
 *  never the advance to the next row.
 *
 *  Interlaced input is refused rather than guessed at: sub_40A630's pass
 *  geometry does not match Adam7 as the spec lays it out, and the encoder side
 *  has not been consulted to settle it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pngout.h"

/* ------------------------------------------------------------------ */
/*  0x190A420: 256 entries - the greyscale ramp or the palette - and   */
/*  0x190A820: how many are live.  Both are published to main().       */
/* ------------------------------------------------------------------ */

static int  *Pal = dword_190A420;  /* dword_190A420 */
static int   PalCount;             /* dword_190A820 */
static int   TrnsRgb;              /* dword_18D4350, the tRNS triple */
static int   PaethByte = 4;        /* dword_4110CC: that filter byte means 5 */

/* ------------------------------------------------------------------ */
/*  The bit reader.  dword_18DEE44 is a signed bit offset counting up  */
/*  towards zero, and dword_18DB740 is the byte it is anchored at.     */
/*  The anchor is the *tail* of the region, so the negative shift       */
/*  walks bytes backwards - which is why IDAT setup puts it at          */
/*  data + len - 4.                                                      */
/* ------------------------------------------------------------------ */

static const unsigned char *Anchor;  /* dword_18DB740 */
static const unsigned char *NextHdr; /* dword_18DB478, a header to stage */
static int   BitPos;                /* dword_18DEE44 */
static int   PendingLen;            /* dword_18DEDE8 */
static unsigned Stage[2];           /* dword_18D8F5C, dword_18D8F60 */

/* dword_4100D8: MaskTable[k] == 2^k - 1 */
static const unsigned MaskTable[32] = {
    0x00000000u, 0x00000001u, 0x00000003u, 0x00000007u,
    0x0000000Fu, 0x0000001Fu, 0x0000003Fu, 0x0000007Fu,
    0x000000FFu, 0x000001FFu, 0x000003FFu, 0x000007FFu,
    0x00000FFFu, 0x00001FFFu, 0x00003FFFu, 0x00007FFFu,
    0x0000FFFFu, 0x0001FFFFu, 0x0003FFFFu, 0x0007FFFFu,
    0x000FFFFFu, 0x001FFFFFu, 0x003FFFFFu, 0x007FFFFFu,
    0x00FFFFFFu, 0x01FFFFFFu, 0x03FFFFFFu, 0x07FFFFFFu,
    0x0FFFFFFFu, 0x1FFFFFFFu, 0x3FFFFFFFu, 0x7FFFFFFFu
};

static unsigned WordAt(int pos)
{
    return *(const unsigned *)(Anchor + (pos >> 3));
}

static void Pump(void)              /* sub_40A340 */
{
    if (NextHdr) {
        Anchor = NextHdr;
        BitPos += 32 - 8 * PendingLen;
        NextHdr = 0;
        return;
    }

    /*
     *  Stage the next chunk.  Relative to the current anchor, +8 is the
     *  length of the *following* chunk, +0xC its tag and +0x10 its first
     *  four data bytes.  The two staging dwords sit next to each other on
     *  purpose: a read crossing the boundary simply walks from one to the
     *  other.  PendingLen is read once and never refreshed, so every IDAT
     *  after the first is walked with the first one's length - that is in
     *  the original.
     */
    PendingLen = (int)Swap32(WordAt(8));
    NextHdr = Anchor + 8 + PendingLen + 12;
    Stage[0] = WordAt(0);
    if (WordAt(12) == 0x54414449u)          /* "IDAT" */
        Stage[1] = WordAt(16);
    Anchor = (const unsigned char *)&Stage[1];
    BitPos -= 32;
}

static int GetBits(int n)
{
    int pos = BitPos;
    unsigned v;

    if (n == 0)
        return 0;
    v = WordAt(pos) >> (pos & 7);
    BitPos = pos + n;
    if (BitPos >= 0)
        Pump();
    return (int)(v & ((1u << n) - 1u));
}

static int PeekBits(int n)          /* without consuming */
{
    int pos = BitPos;

    return (int)((WordAt(pos) >> (pos & 7)) & ((1u << n) - 1u));
}

static void SkipToByte(void)
{
    int pad = (-BitPos) & 7;

    if (pad) {
        BitPos += pad;
        if (BitPos >= 0)
            Pump();
    }
}

static void SkipBits(int n)         /* a code of n bits, already looked at */
{
    BitPos += n;
    if (BitPos >= 0)
        Pump();
}

/* ------------------------------------------------------------------ */
/*  Huffman: sub_40A580, sub_40A4A0, sub_40A440.                      */
/* ------------------------------------------------------------------ */

static void BuildHuffmanOffsets(const int *lengths, int n, int *symbols, int *offs)
{
    int h[31];
    int i;

    for (i = 0; i < 31; i++)
        h[i] = 0;
    for (i = n - 1; i >= 0; i--)
        h[lengths[i]]++;
    offs[0] = 0;
    h[0] = 0;
    for (i = 0; i < 31; i++)
        offs[i + 1] = offs[i] + h[i];

    /* This bumps offs[len] as it assigns, so the consumers see the
     * *inclusive* cumulative array. */
    for (i = 0; i < n; i++)
        if (lengths[i]) {
            symbols[offs[lengths[i]]] = i;
            offs[lengths[i]]++;
        }
}

/* sub_40A4A0's inner loop: `shr edx,1 / adc eax,eax`, maxbits times. */
static int ReverseBits(int v, int nbits)
{
    int r = 0, k;

    for (k = 0; k < nbits; k++) {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    return r;
}

static void BuildDecodeTables(const int *offs, const int *symbols,
                              int *table, int *codes, int maxbits)
{
    int code = 0, len = 1, p = 1, arg = maxbits - 1;

    while (len <= maxbits) {
        int i = offs[p - 1];

        while (i < offs[p]) {
            unsigned end = MaskTable[arg] + (unsigned)code, j;

            for (j = (unsigned)code; j <= end; j++) {
                int t = ReverseBits((int)j, maxbits);

                table[t] = symbols[i];
                codes[t] = len;
            }
            i++;
        }
        len++;
        p++;
        arg--;
    }
    {   /* whatever is left over is an invalid code */
        unsigned end = MaskTable[maxbits], j;

        for (j = (unsigned)code; j <= end; j++)
            codes[ReverseBits((int)j, maxbits)] = 0;
    }
}

static int DecodeSymbol(const int *offs, const int *symbols)   /* sub_40A440 */
{
    int len = 0, v = 0;

    for (;;) {
        int bit = GetBits(1);

        v = (bit + offs[len] - offs[len + 1]) + 2 * v;
        len++;
        if (v < 0)
            return symbols[offs[len] + v];
    }
}

/* ------------------------------------------------------------------ */
/*  sub_40AF90's tables.  The length table has 29 entries: the loop    */
/*  stops after symbol 284 and 285, the 258 byte code, is written out  */
/*  by hand just after it.                                              */
/* ------------------------------------------------------------------ */

static const int LenBase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const int LenExtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};

/*
 *  The first four distance codes carry NO extra bits.  The obvious
 *  extra = i / 2 is wrong at 2 and 3, and reading one bit too many there
 *  desynchronises the whole stream with no local symptom.
 */
static const int DistBase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
    12289, 16385, 24577
};
static const int DistExtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/* ------------------------------------------------------------------ */
/*  The unfilter and the row expansion.                                */
/* ------------------------------------------------------------------ */

static unsigned char *Block;        /* caller's: one filter byte a row */
static int   RowLimit;              /* dword_190A1C4, the height */
static int   RowBase;               /* dword_18DA23C, the row we are on */
static int   RowIndex0;             /* dword_18DA240 */
static int   RowEnd;                /* dword_18DA254, the height */
static int   RowStride;             /* dword_18DA234 */
static int   RowBytes;              /* dword_18DB26C */
static int   RowLeft;               /* dword_18D9FA4, bytes left in the row */
static int   CurChannel;            /* dword_18DC38C */
static int   Ring[4];               /* dword_18DB85C[4] */
static unsigned char RowUp[0x9000]; /* dword_18FA1C0; Up is RowUp + 1 */

/*
 *  dword_18CBF00: a running destination pointer, not an indexed base.  The
 *  expansion writes at it directly and loc_40AE2D steps it on by the pitch
 *  afterwards, so it tracks the row rather than being recomputed from a count.
 */
static unsigned char *DestRow;

static unsigned char *Up;
static unsigned char RowA[4];       /* dword_18CBF68, left */
static unsigned char RowC[4];       /* dword_18CBF54, up-left */
static int   CurFilter;             /* dword_18E909C, -1 before the first */
static unsigned char *Dest;         /* dword_18DC4D0 */
static int   DestPitch;             /* dword_18DF880 */
static int   ImageWidth;            /* dword_18D9FA8 */
static int   Samples;               /* samples per pixel */
static int   ExpStart, ExpEnd;      /* dword_18DB270, dword_18DB274 */

static int AbsTab(int v)            /* 0x18D6A40, for the inlined predictor */
{
    return v < 0 ? -v : v;
}

static void NoteRowFilter(int f)
{
    if (Block && RowBase < RowLimit)
        Block[RowBase] = (unsigned char)f;
}

static void StartFilterByte(int f)
{
    if (f == PaethByte)
        f = 5;                       /* the Paeth byte means PNGOUT's own */
    CurFilter = f;
    NoteRowFilter(f);
}

static void ResetPass(void)
{
    RowLeft = RowBytes;
    RowA[0] = RowA[1] = RowA[2] = RowA[3] = 0;
    RowC[0] = RowC[1] = RowC[2] = RowC[3] = 0;
    CurChannel = Ring[0];
    memset(RowUp, 0, (size_t)RowBytes + 2);
}

/*
 *  Sample out of a packed (depth < 8) row.
 *
 *  The staging row holds its filter byte at Up[RowBytes] and the row's bytes
 *  back to front below it, so Up[j] is row byte (RowBytes - j).  v counts bit
 *  positions downward from the first sample, so it has to be mapped back to an
 *  ascending row position before indexing.
 */
static int PackedSample(int v, int depth)
{
    int rowbit = RowBytes - 1 - v;
    int byte = RowUp[1 + RowBytes - 1 - (rowbit >> 3)];

    return (byte >> (8 - depth - (rowbit & 7))) & ((1 << depth) - 1);
}

static void ExpandRow(void)
{
    unsigned char *d = DestRow;
    int v = ExpStart;

    switch (OutColType) {
    case 2:
        /*
         *  The three samples come out of the reversed row *ascending*, at
         *  loc_40ADB0: Up[v], Up[v+1], Up[v+2].  With the row stored back to
         *  front that is the file's B, G, R, so the DWORD lands as
         *  0xFFBBGGRR.
         */
        while (v > ExpEnd) {
            int p = Up[v] | (Up[v + 1] << 8) | (Up[v + 2] << 16);

            if (TrnsRgb && (p | 0xFF000000) == (unsigned)TrnsRgb)
                p &= 0x00FFFFFF;
            *(unsigned int *)d = (unsigned int)p | 0xFF000000u;
            d += 4;
            v -= 3;
        }
        break;

    case 4:                          /* grey then alpha: grey in RGB */
        while (v > ExpEnd) {
            int grey = Pal[Up[v]] & 0x00FFFFFF;

            *(unsigned int *)d = (unsigned int)(grey | (Up[v - 1] << 24));
            d += 4;
            v -= 2;
        }
        break;

    case 6:
        /* loc_40AD20: Up[v], Up[v+1], Up[v+2], then Up[v-1] for the alpha. */
        while (v > ExpEnd) {
            d[0] = Up[v];
            d[1] = Up[v + 1];
            d[2] = Up[v + 2];
            d[3] = Up[v - 1];
            d += 4;
            v -= 4;
        }
        break;

    case 0:                          /* greyscale, one byte per pixel */
        while (v > ExpEnd) {
            *(unsigned int *)d = (unsigned int)Pal[Up[v]];
            d += 4;
            v--;
        }
        break;

    case 3:                          /* palette index, one byte per pixel */
        while (v > ExpEnd) {
            *(unsigned int *)d = (unsigned int)Up[v];
            d += 4;
            v--;
        }
        break;

    default:                         /* sub-byte depths, through the table */
        switch (OutBitDepth) {
        case 1:
            while (v > ExpEnd) {
                *(unsigned int *)d = (unsigned int)Pal[PackedSample(v, 1)];
                d += 4;
                v--;
            }
            break;
        case 2:
            while (v > ExpEnd) {
                *(unsigned int *)d = (unsigned int)Pal[PackedSample(v, 2)];
                d += 4;
                v -= 2;
            }
            break;
        case 4:
            while (v > ExpEnd) {
                *(unsigned int *)d = (unsigned int)Pal[PackedSample(v, 4)];
                d += 4;
                v -= 4;
            }
            break;
        default:
            break;
        }
        break;
    }
}

static void Unfilter(unsigned char *buf, int len)      /* sub_40A8D0 */
{
    int i = 0;
    int f = CurFilter;

    if (f < 0) {
        if (len <= 0)
            return;
        NoteRowFilter(buf[0]);
        StartFilterByte(buf[0]);
        f = CurFilter;               /* StartFilterByte just resolved it */
        i = 1;
    }

    while (i < len) {
        int end = i + RowLeft;

        if (end > len)
            end = len;

        switch (f) {
        case 0:
            while (i < end)
                Up[RowLeft--] = buf[i++];
            break;

        case 1:
            while (i < end) {
                int ch = CurChannel;

                RowA[ch] = (unsigned char)(RowA[ch] + buf[i]);
                Up[RowLeft--] = RowA[ch];
                CurChannel = Ring[ch];
                i++;
            }
            break;

        case 2:
            while (i < end)
                Up[RowLeft--] = (unsigned char)(Up[RowLeft + 1] + buf[i++]);
            break;

        case 3:
            while (i < end) {
                int ch = CurChannel;

                Up[RowLeft] = (unsigned char)
                    (buf[i] + ((Up[RowLeft + 1] + RowA[ch]) >> 1));
                RowA[ch] = Up[RowLeft];
                CurChannel = Ring[ch];
                RowLeft--;
                i++;
            }
            break;

        case 4:
            while (i < end) {
                int ch = CurChannel;

                Up[RowLeft] = (unsigned char)
                    (buf[i] + FilterPredict(RowA[ch], Up[RowLeft + 1],
                                           Up[RowLeft]));
                RowC[ch] = Up[RowLeft + 1];
                RowA[ch] = Up[RowLeft];
                CurChannel = Ring[ch];
                RowLeft--;
                i++;
            }
            break;

        case 5:                      /* FilterPredict, inlined */
            while (i < end) {
                int ch = CurChannel;
                int a = RowA[ch], b = Up[RowLeft + 1], c = Up[RowLeft];
                int d1 = AbsTab(a + b - 2 * c), d2 = AbsTab(b - c);
                int cc = c, p;

                if (d1 >= d2)
                    cc = a;
                p = ((d1 < d2 ? d1 : d2) < AbsTab(a - c)) ? cc : b;
                RowC[ch] = Up[RowLeft + 1];
                Up[RowLeft] = (unsigned char)(buf[i] + p);
                RowA[ch] = Up[RowLeft];
                CurChannel = Ring[ch];
                RowLeft--;
                i++;
            }
            break;

        default:
            break;
        }

        if (RowLeft > 0)
            return;                    /* the row is half built */

        /*
         *  The jnb at 0x40AB9D skips only the expansion once the row index
         *  has reached the height.  The advance always happens, and the
         *  expansion falls straight into it.
         */
        if (RowBase < RowEnd)
            ExpandRow();

        RowBase += RowStride;
        DestRow += DestPitch;
        ResetPass();

        if (i >= len) {
            CurFilter = -1;
            return;
        }
        StartFilterByte(buf[i++]);
    }
}

/* ------------------------------------------------------------------ */
/*  The inflate body.                                                   */
/* ------------------------------------------------------------------ */

static unsigned char Window[0x8000];
static int WinWrite;
static int WinFlush;                 /* the next 16 KB boundary to hand over */
static int ImageDone;

static void FlushWindow(int start)
{
    Unfilter(&Window[start & 0x7FFF], WinWrite - start);
    WinWrite = start + 0x4000;
    WinFlush += 0x4000;
}

static void EmitByte(int byte)
{
    if (WinWrite >= WinFlush) {
        FlushWindow(WinWrite - 0x4000);
        if (RowBase >= RowEnd) {
            ImageDone = 1;            /* 0x40A300: out of rows */
            return;
        }
    }
    Window[WinWrite++ & 0x7FFF] = (unsigned char)byte;
}

static int InflateBlocks(void)
{
    static int lengths[320];
    static int lsym[320], loffs[32], dsym[32], doffs[32];
    static int ltab[512], lcod[512], dtab[64], dcod[64];

    while (!ImageDone) {
        int final = GetBits(1);
        int type = GetBits(2);

        if (type == 0) {
            int len, nlen;

            SkipToByte();
            len = GetBits(16);
            nlen = GetBits(16);
            if (len != ((~nlen) & 0xFFFF))
                return -1;
            if (len == 0)
                break;
            while (len-- > 0)
                EmitByte(GetBits(8));
        } else if (type == 1 || type == 2) {
            int hlit, hdist, i;

            if (type == 1) {
                for (i = 0; i < 144; i++) lengths[i] = 8;
                for (; i < 256; i++) lengths[i] = 9;
                for (; i < 280; i++) lengths[i] = 7;
                for (; i < 288; i++) lengths[i] = 8;
                for (i = 288; i < 320; i++) lengths[i] = 5;
                hlit = 288;
                hdist = 32;
            } else {
                int cl[19];
                int hclen, n = 0;
                int csym[19], coffs[32];

                hlit = GetBits(5) + 257;
                hdist = GetBits(5) + 1;
                hclen = GetBits(4) + 4;

                memset(cl, 0, sizeof cl);
                for (i = 0; i < hclen; i++)
                    cl[Bitrev19[i]] = GetBits(3);
                memset(csym, 0, sizeof csym);
                BuildHuffmanOffsets(cl, 19, csym, coffs);

                while (n < hlit + hdist) {
                    int s = DecodeSymbol(coffs, csym);

                    if (s < 16) {
                        lengths[n++] = s;
                    } else if (s == 16) {
                        int v = lengths[n - 1], r = GetBits(2) + 3;

                        while (r-- > 0)
                            lengths[n++] = v;
                    } else if (s == 17) {
                        int r = GetBits(3) + 3;

                        while (r-- > 0)
                            lengths[n++] = 0;
                    } else {
                        int r = GetBits(7) + 11;

                        while (r-- > 0)
                            lengths[n++] = 0;
                    }
                }
                NumBlocks++;            /* dynamic blocks only */
            }

            memset(lsym, 0, sizeof lsym);
            memset(dsym, 0, sizeof dsym);
            memset(loffs, 0, sizeof loffs);
            memset(doffs, 0, sizeof doffs);
            memset(ltab, 0, sizeof ltab);
            memset(lcod, 0, sizeof lcod);
            memset(dtab, 0, sizeof dtab);
            memset(dcod, 0, sizeof dcod);

            BuildHuffmanOffsets(lengths, hlit, lsym, loffs);
            BuildDecodeTables(loffs, lsym, ltab, lcod, 9);
            BuildHuffmanOffsets(lengths + hlit, hdist, dsym, doffs);
            BuildDecodeTables(doffs, dsym, dtab, dcod, 6);

            for (;;) {
                int peek, sym;

                if (lcod[peek = PeekBits(9)] == 0)
                    sym = DecodeSymbol(loffs, lsym);
                else {
                    sym = ltab[peek];
                    SkipBits(lcod[peek]);
                }

                if (sym < 256) {
                    EmitByte(sym);
                    if (ImageDone)
                        break;
                    continue;
                }
                if (sym == 256)
                    break;
                if (sym > 285)
                    return -1;

                {
                    int e = LenExtra[sym - 257];
                    int length = LenBase[sym - 257] + GetBits(e);
                    int dpe = PeekBits(6);
                    int ds, dist, src;

                    if (dcod[dpe] == 0)
                        ds = DecodeSymbol(doffs, dsym);
                    else {
                        ds = dtab[dpe];
                        SkipBits(dcod[dpe]);
                    }
                    if (ds > 29)
                        return -1;
                    dist = DistBase[ds] + GetBits(DistExtra[ds]);
                    src = WinWrite - dist;
                    if (src < 0)
                        return -1;
                    while (length-- > 0) {
                        Window[WinWrite & 0x7FFF] = Window[(src++) & 0x7FFF];
                        WinWrite++;
                    }
                }
            }
        } else {
            return -1;                  /* btype 3 is not valid deflate */
        }

        if (final)
            break;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  The chunk walk.                                                     */
/* ------------------------------------------------------------------ */

static int DecodePng(const unsigned char *data, int size, unsigned char *pixels,
                     int pitch, unsigned int width, unsigned int height)
{
    const unsigned char *p = data + 8;
    const unsigned char *end = data + size;
    int i;

    TrnsRgb = 0;
    PalCount = 0;
    NumBlocks = 0;
    unk_190A828 = 0;
    Dest = pixels;
    DestPitch = pitch;
    DestRow = pixels;
    ImageWidth = (int)width;
    RowLimit = unk_190A1C4;
    Block = FilterChoice;
    Up = RowUp + 1;
    memset(Pal, 0, 256 * sizeof *Pal);

    while (p + 8 <= end) {
        unsigned len = Swap32(*(const unsigned *)p);
        const unsigned char *tag = p + 4;
        const unsigned char *body = p + 8;

        if (body + (int)len > end)
            break;

        if (tag[0] == 'I' && tag[1] == 'H' && tag[2] == 'D' && tag[3] == 'R') {
            int depth = body[8];
            int coltype = body[9];
            int maxval;

            if (body[10] || body[11] || body[12] >= 2)
                return -1;
            if (!((1 << depth) & 0x116) || !((1 << coltype) & 0x5D))
                return -1;
            if (body[12])
                return -2;              /* interlaced: refused, see the header */

            OutBitDepth = (unsigned char)depth;
            OutColType = (unsigned char)coltype;

            /*
             *  The channel ring.  The unfilter keeps the left-hand neighbour of
             *  each byte in RowA[ch] and walks it with CurChannel, so the ring
             *  has to enumerate exactly one full cycle of the samples per
             *  pixel: 0->1->2->0 for RGB and 0->1->0->1 for grey+alpha.  A ring
             *  that skips a channel reconstructs Sub, Average and Paeth against
             *  stale state, which shows up as rows that are flat or halved.
             */
            switch (coltype) {          /* the channel ring */
            case 0: case 3: Ring[0] = Ring[1] = Ring[2] = Ring[3] = 0; break;
            case 2:         Ring[0] = 1; Ring[1] = 2; Ring[2] = 0; Ring[3] = 0;
                            break;
            case 4:         Ring[0] = 1; Ring[1] = 0; Ring[2] = 1; Ring[3] = 0;
                            break;
            case 6:         Ring[0] = 1; Ring[1] = 2; Ring[2] = 3; Ring[3] = 0;
                            break;
            default: break;
            }

            /* Types 0 and 4 get a greyscale ramp; 4 uses it as scratch. */
            if (coltype == 0 || coltype == 4) {
                maxval = (1 << depth) - 1;
                PalCount = 1 << depth;
                for (i = 0; i < PalCount; i++)
                    Pal[i] = (int)(0xFF000000u
                                   + (unsigned)i * 0x010101u * (255 / maxval));
            }

            /* Sub-byte depths index by raw byte, so widen the table. */
            switch (depth) {
            case 1: for (i = 2; i < 256; i++) Pal[i] = Pal[i & 1]; break;
            case 2: for (i = 4; i < 256; i++) Pal[i] = Pal[i & 3]; break;
            case 4: for (i = 16; i < 256; i++) Pal[i] = Pal[i & 15]; break;
            default: break;
            }

            switch (coltype) {
            case 2: Samples = 3; break;
            case 4: Samples = 2; break;
            case 6: Samples = 4; break;
            default: Samples = 1; break;
            }
            RowBytes = (int)width * Samples;
            switch (depth) {
            case 1: RowBytes = (RowBytes + 7) >> 3; break;
            case 2: RowBytes = (RowBytes + 3) >> 2; break;
            case 4: RowBytes = (RowBytes + 1) >> 1; break;
            default: break;
            }

            /* dword_18DB270 / dword_18DB274, for no interlace. */
            switch (coltype) {
            case 4: ExpStart = 2 * (int)width;         ExpEnd = 0; break;
            case 2: ExpStart = 3 * (int)width - 2;     ExpEnd = -2; break;
            case 6: ExpStart = 4 * (int)width - 2;     ExpEnd = -2; break;
            default:
                switch (depth) {
                case 1: i = (-(int)width) & 7;
                        ExpStart = (int)width + i + 7;  ExpEnd = i + 7; break;
                case 2: i = (-(int)width) & 3;
                        ExpStart = 2 * ((int)width + i) + 6;
                        ExpEnd = 2 * i + 6; break;
                case 4: i = (-(int)width) & 1;
                        ExpStart = 4 * ((int)width + i) + 4;
                        ExpEnd = 4 * i + 4; break;
                default: ExpStart = (int)width;         ExpEnd = 0; break;
                }
                break;
            }
        } else if (tag[0] == 'P' && tag[1] == 'L' && tag[2] == 'T'
                   && tag[3] == 'E') {
            int n = (int)len / 3;

            PalCount = n;
            for (i = n - 1; i >= 0; i--)
                Pal[i] = (int)((Swap32(*(const unsigned *)(body + 3 * i)) >> 8)
                               | 0xFF000000u);
        } else if (tag[0] == 'b' && tag[1] == 'K' && tag[2] == 'G'
                   && tag[3] == 'D') {
            int v = 0;

            switch (OutColType) {
            case 0: case 4:
                v = 0x010101 * (255 * (body[1] + (body[0] << 8))
                                / ((1 << OutBitDepth) - 1)) - 0x1000000;
                unk_190A3FC = v;
                goto publish;
            case 2: case 6:
                if (OutBitDepth == 8) {
                    v = (body[5] + ((body[3] + ((body[1] - 256) << 8)) << 8))
                        | 0xFF000000;
                    unk_190A3FC = v;
                    goto publish;
                }
                for (i = 0; i < 3; i++)
                    v += ((body[2 * i + 1] + (body[2 * i] << 8)) / 257)
                         << (8 * (2 - i));
                unk_190A3FC = v | 0xFF000000;
                goto publish;
            case 3:
                unk_190A3FC = Pal[body[0]];
            publish:
                Limit411078 = unk_190A3FC & 0xFF;
                Limit411074 = (unk_190A3FC >> 8) & 0xFF;
                Limit411070 = (unk_190A3FC >> 16) & 0xFF;
                break;
            default:
                break;
            }
        } else if (tag[0] == 't' && tag[1] == 'R' && tag[2] == 'N'
                   && tag[3] == 'S') {
            if (OutColType == 0) {
                if (len >= 1)
                    Pal[body[1]] &= 0x00FFFFFF;   /* fully transparent */
            } else if (OutColType == 2) {
                if (OutBitDepth == 8 && len >= 6)
                    TrnsRgb = (body[5]
                               + ((body[3] + ((body[1] - 256) << 8)) << 8))
                              | 0xFF000000;
            } else if (OutColType == 3) {
                int n = (int)len < PalCount ? (int)len : PalCount;

                /* In place.  IDA anchors this dword_190A424[--i], which
                 * looks like a second array, but the dec and the +4 base
                 * offset cancel exactly. */
                for (i = n - 1; i >= 0; i--)
                    Pal[i] = (Pal[i] & 0x00FFFFFF) | (body[i] << 24);
            }
        } else if (tag[0] == 'I' && tag[1] == 'D' && tag[2] == 'A'
                   && tag[3] == 'T') {
            Anchor = body + len - 4;
            BitPos = 32 - 8 * (int)len;
            NextHdr = 0;
            Stage[0] = Stage[1] = 0;

            unk_190A828 = GetBits(16);   /* 0x5e78 == 24184 marks our own */

            RowIndex0 = 0;
            RowEnd = (int)height;
            RowStride = 1;
            RowBase = 0;
            CurFilter = -1;
            ImageDone = 0;
            ResetPass();

            WinWrite = 0;
            WinFlush = 0x4000;

            i = InflateBlocks();
            if (i < 0)
                return i;

            /* Flush whatever the loop did not hand over.  The wrap test is
             * `test ah,ah / js` at 0x40A2B1, so it is bit *15* of the XOR:
             * `start` is always a multiple of 0x4000 and contributes
             * nothing there, leaving "has the write index passed 0x8000". */
            {
                int start = WinFlush - 0x4000;

                if ((start ^ WinWrite) & 0x8000) {
                    Unfilter(&Window[start & 0x7FFF], 0x4000 - (start & 0x7FFF));
                    Unfilter(&Window[0], WinWrite & 0x7FFF);
                } else {
                    Unfilter(&Window[start & 0x7FFF], WinWrite - start);
                }
            }

            if (OutColType == 4)
                PalCount = 0;             /* grey + alpha has no palette */
            return 0;
        }

        p = body + len + 4;
    }
    return -1;
}

int DecodeImage(unsigned char *fileimg, int filesize, unsigned char *pixels,
                int pitch, unsigned int width, unsigned int height,
                int unused7, int unused8)
{
    (void)unused7;
    (void)unused8;

    if (fileimg && filesize >= 8 && fileimg[0] == 0x89 && fileimg[1] == 'P')
        return DecodePng(fileimg, filesize, pixels, pitch, width, height);

    printf("\n*** That input format has not been reconstructed yet.\n"
           "*** This build decodes PNG only; see README.md.\n\n");
    exit(3);
    return -1;
}
