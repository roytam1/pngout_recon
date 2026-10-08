/*
 *  decode.c   -   0x409560 DecodeImage(), the PNG path and the 8bpp one
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
 *  geometry does not match Adam7 as the spec lays it out, and it is not a
 *  partition of the image, so no valid interlaced file could be checked against
 *  a transcription of it.
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

/*
 *  The pump indexes the anchor in *bytes* ([esi+8], [esi+0Ch], [esi+10h]) while
 *  the bit reader's WordAt() takes a bit offset and divides by eight.  Mixing
 *  the two up is silent: +8 lands one byte along the data, so the staged length
 *  comes out as noise and the next anchor points into the heap.
 */
static unsigned DwordAt(int off)
{
    return *(const unsigned *)(Anchor + off);
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
     *  Stage the next chunk.  Relative to the current anchor, +8 is the length
     *  of the *following* chunk, +0xC its tag and +0x10 its first four data
     *  bytes.  `lea ecx, [esi+eax+0Ch]` is the anchor for the chunk after that
     *  one - its *last* four data bytes, the same role this chunk's anchor has,
     *  which is what lets the walk continue backwards across the boundary.
     *  The two staging dwords sit next to each other on purpose: a read
     *  crossing the boundary simply walks from one to the other.  PendingLen is
     *  read once and never refreshed, so every IDAT after the first is walked
     *  with the first one's length - that is in the original.
     */
    PendingLen = (int)Swap32(DwordAt(8));
    NextHdr = Anchor + PendingLen + 12;
    Stage[0] = DwordAt(0);
    if (DwordAt(12) == 0x54414449u)          /* "IDAT" */
        Stage[1] = DwordAt(16);
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

/*
 *  sub_40A4A0.  The table is keyed by the *bit reversed* canonical code, so
 *  `code` has to walk the canonical codes in order - 2^(maxbits - len) slots
 *  for every symbol of that length - and each slot is filled with the reversal
 *  of the code it sits at.  Leaving `code` at zero fills the same slots over
 *  and over, which happens to look right for a table where one length holds
 *  everything and is completely wrong for anything else.
 */
static void BuildDecodeTables(const int *offs, const int *symbols,
                              int *table, int *codes, int maxbits)
{
    int code = 0, len, i;

    for (len = 1; len <= maxbits; len++) {
        int slots = MaskTable[maxbits - len] + 1;

        for (i = offs[len - 1]; i < offs[len]; i++) {
            int j;

            for (j = 0; j < slots; j++) {
                int t = ReverseBits(code + j, maxbits);

                table[t] = symbols[i];
                codes[t] = len;
            }
            code += slots;
        }
    }

    {   /* whatever is left over is an invalid code */
        for (; code <= MaskTable[maxbits]; code++)
            codes[ReverseBits(code, maxbits)] = 0;
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
static unsigned char RowUp[0x9000]; /* dword_18FA1C0 */

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

static int AbsTab(int v)            /* 0x18DAA60, for the inlined predictor */
{
    return v < 0 ? -v : v;
}

static void NoteRowFilter(int f)
{
    if (Block && RowBase < RowLimit)
        Block[RowBase] = (unsigned char)f;
}

/*
 *  0x40A91B and 0x40AED7.  The row record keeps the byte the file actually
 *  carried; only the running filter turns the Paeth byte into PNGOUT's own.
 */
static void StartFilterByte(int f)
{
    CurFilter = f;
    NoteRowFilter(f);
    if (f == PaethByte)
        CurFilter = 5;
}

/*
 *  Start a pass.  0x40A72B clears RowBytes + 1 bytes of the row buffer, but
 *  only here: the row itself is what the Up, Average and Paeth branches read as
 *  "the row above", so clearing it between rows would make every row the first
 *  one.
 */
static void ResetPass(void)          /* 0x40AE33 */
{
    RowLeft = RowBytes;
    RowA[0] = RowA[1] = RowA[2] = RowA[3] = 0;
    RowC[0] = RowC[1] = RowC[2] = RowC[3] = 0;
}

/*
 *  Sample out of a packed (depth < 8) row, 0x40ABEF / 0x40AC34 / 0x40AC7A.
 *
 *  The staging row is mirrored: file byte 0 sits at Up[RowBytes] and file byte
 *  RowBytes-1 at Up[1], so index 0 goes unused.  The three packed cases all
 *  read one byte and shift it, the shift being v & (8 - depth), and none of
 *  them masks the result: the palette is pre-widened to 256 entries with
 *  pal[j] = pal[j & mask] so an unmasked byte is a legal index.
 */
static int PackedSample(int v, int depth)
{
    return Up[v >> 3] >> (v & (8 - depth));
}

static void ExpandRow(void)
{
    unsigned char *d = DestRow;
    int v = ExpStart;

    if (ExpStart <= ExpEnd)
        return;

    switch (OutColType) {
    case 2:
        /*
         *  loc_40ADB0.  One unaligned DWORD at Up[v - 2] with its top byte
         *  replaced, so the pixel is the row's B, G and R low to high and the
         *  internal DWORD is 0xFFBBGGRR.  v steps down three a pixel.
         */
        while (v > ExpEnd) {
            unsigned p = (unsigned)Up[v] | ((unsigned)Up[v + 1] << 8)
                       | ((unsigned)Up[v + 2] << 16);

            if (TrnsRgb && (p | 0xFF000000u) == (unsigned)TrnsRgb)
                p &= 0x00FFFFFFu;
            *(unsigned int *)d = p | 0xFF000000u;
            d += 4;
            v -= 3;
        }
        break;

    case 4:                          /* loc_40AD5A: grey then alpha */
        while (v > ExpEnd) {
            unsigned grey = (unsigned)(Pal[Up[v]] & 0x00FFFFFF);

            *(unsigned int *)d = grey | ((unsigned)Up[v - 1] << 24);
            d += 4;
            v -= 2;
        }
        break;

    case 6:                          /* loc_40AD20 */
        while (v > ExpEnd) {
            d[0] = Up[v];
            d[1] = Up[v + 1];
            d[2] = Up[v + 2];
            d[3] = Up[v - 1];
            d += 4;
            v -= 4;
        }
        break;

    default:
        /*
         *  Colour types 0 and 3 both land in the depth switch at 0x40ABD7.
         *  Depth 8 is the plain byte loop at loc_40ACC5; 1, 2 and 4 are the
         *  three packed cases, and every one of them goes through the table.
         */
        if (OutBitDepth == 8) {
            while (v > ExpEnd) {
                *(unsigned int *)d = (unsigned int)Pal[Up[v]];
                d += 4;
                v--;
            }
        } else {
            while (v > ExpEnd) {
                *(unsigned int *)d =
                    (unsigned int)Pal[PackedSample(v, OutBitDepth)];
                d += 4;
                v -= OutBitDepth;     /* v counts bits, one pixel per depth */
            }
        }
        break;
    }
}

static void Unfilter(unsigned char *buf, int len)      /* sub_40A8D0 */
{
    int i = 0;
    int f = CurFilter;

    if (f < 0) {                     /* 0x40A8E3: take the row's filter byte */
        if (len <= 0)
            return;
        StartFilterByte(buf[0]);
        f = CurFilter;               /* StartFilterByte just resolved it */
        i = 1;
    }

    while (i < len) {
        int end = i + RowLeft;

        if (end > len)
            end = len;

        switch (f) {
        case 0:                      /* loc_40A990 */
            while (i < end) {
                Up[RowLeft] = buf[i++];
                RowLeft--;
            }
            break;

        case 1:                      /* loc_40A9B4: Sub */
            while (i < end) {
                int ch = CurChannel;

                RowA[ch] = (unsigned char)(RowA[ch] + buf[i]);
                Up[RowLeft] = RowA[ch];
                CurChannel = Ring[ch];
                RowLeft--;
                i++;
            }
            break;

        case 2:                      /* loc_40A9E9: Up, in place */
            while (i < end) {
                Up[RowLeft] = (unsigned char)(Up[RowLeft] + buf[i]);
                RowLeft--;
                i++;
            }
            break;

        case 3:                      /* loc_40AA0D: Average */
            while (i < end) {
                int ch = CurChannel;
                unsigned char v;

                v = (unsigned char)(buf[i]
                        + ((Up[RowLeft] + RowA[ch]) >> 1));
                Up[RowLeft] = v;
                RowA[ch] = v;
                CurChannel = Ring[ch];
                RowLeft--;
                i++;
            }
            break;

        case 4:                      /* loc_40AA5B: FilterPredict */
            while (i < end) {
                int ch = CurChannel;
                unsigned char up = Up[RowLeft];
                unsigned char v;

                v = (unsigned char)(buf[i]
                        + FilterPredict(RowA[ch], up, RowC[ch]));
                Up[RowLeft] = v;
                RowA[ch] = v;
                RowC[ch] = up;       /* the row above, kept for the next pixel */
                CurChannel = Ring[ch];
                RowLeft--;
                i++;
            }
            break;

        case 5:                      /* loc_40AAE0: FilterPredict, inlined */
            while (i < end) {
                int ch = CurChannel;
                int a = RowA[ch], b = Up[RowLeft], c = RowC[ch];
                /*
                 *  loc_40AB04.  The table is indexed off one base, so the
                 *  three distances only appear as differences:
                 *
                 *      esi = T[c - a]            edi = T[2c - a - b]
                 *      third = T[c - b]
                 *
                 *  and edi is folded down first.  These are the same three as
                 *  FilterPredict()'s - T[2c-a-b] is |a+b-2c| and T[c-a] and
                 *  T[c-b] are its |a-c| and |b-c| - so the inlined copy and
                 *  the function agree over the whole byte domain, which is what
                 *  makes the Paeth byte interchangeable with this one.
                 */
                int d1 = AbsTab(c - a);
                int d2 = AbsTab(2 * c - a - b);
                int d3 = AbsTab(c - b);
                int t = (d2 >= d1) ? b : c;
                unsigned char up = (unsigned char)b;
                unsigned char v;

                v = (unsigned char)(buf[i]
                        + ((d2 >= d1 ? d1 : d2) < d3 ? t : a));
                Up[RowLeft] = v;
                RowA[ch] = v;
                RowC[ch] = up;
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
         *  The jnb at 0x40AB9D skips the expansion once the row index has
         *  reached the height, and with it the destination step at 0x40AE1A.
         *  The row advance below always happens.
         */
        if (RowBase < RowEnd) {
            ExpandRow();
            DestRow += DestPitch;
        }
        RowBase += RowStride;
        ResetPass();

        if (i >= len) {               /* loc_40AEE6: no filter byte left */
            CurFilter = -1;
            return;
        }
        StartFilterByte(buf[i++]);
        f = CurFilter;
    }
}

/* ------------------------------------------------------------------ */
/*  The inflate body.                                                   */
/* ------------------------------------------------------------------ */

static unsigned char Window[0x8000];
static int WinWrite;
static int WinFlush;                 /* the next 16 KB boundary to hand over */
static int ImageDone;

/*
 *  Hand over the 16 KB the write index has just passed.  The length is the
 *  literal 0x4000 (0x40A0B4, and 0x409D09 for the stored path), *not*
 *  "write index minus start": a match is copied without passing the check, so
 *  the index can be several bytes past the boundary and the bytes in between
 *  are unflitered by the next hand-over.  Using the difference here unflilters
 *  them twice, which corrupts the row they belong to and every row after it.
 */
static void FlushWindow(int start)
{
    Unfilter(&Window[start & 0x7FFF], 0x4000);
    WinFlush += 0x4000;
}

/*
 *  Hand the window over whenever the write index has reached the next 16 KB
 *  boundary.  Returns non-zero once the image is done, which is where both
 *  call sites bail out.
 *
 *  The check has to sit at the top of the *symbol* loop, not only where a
 *  literal is stored: a highly compressible stream is mostly matches, and the
 *  match copy writes straight into the window without passing the literal
 *  store.  Checking only on literals leaves the boundary unhandled and the
 *  tail flush then reads tens of thousands of bytes past a 32 KB array.
 *  0x40A09C is the check, and the match loop at 0x40A268 jumps back to it.
 */
static int FlushIfDue(void)
{
    if (WinWrite >= WinFlush) {
        FlushWindow(WinFlush - 0x4000);
        if (RowBase >= RowEnd) {
            ImageDone = 1;            /* 0x40A300: out of rows */
            return 1;
        }
    }
    return 0;
}

static void EmitByte(int byte)
{
    if (FlushIfDue())
        return;
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

                if (FlushIfDue())
                    break;

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
    Up = RowUp;
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

            /*
             *  Sub-byte depths index the table by a raw byte, so the table is
             *  widened to 256 entries at IDAT time rather than here: for colour
             *  type 3 the real entries arrive with the PLTE and their alphas
             *  with the tRNS, both of which come after IHDR.  See the IDAT
             *  branch below.
             */
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
            /* Type 4 has no palette; the ramp above is only its grey source. */
            if (OutColType == 4)
                PalCount = 0;

            /*
             *  0x409AF1.  The table has to be widened to 256 entries here, not
             *  at IHDR: for colour type 3 the colours arrive with the PLTE and
             *  their alphas with the tRNS, and both chunks come after IHDR, so
             *  widening earlier would spread an empty table over the whole
             *  index range.
             */
            switch (OutBitDepth) {
            case 1: for (i = 2; i < 256; i++) Pal[i] = Pal[i & 1]; break;
            case 2: for (i = 4; i < 256; i++) Pal[i] = Pal[i & 3]; break;
            case 4: for (i = 16; i < 256; i++) Pal[i] = Pal[i & 15]; break;
            default: break;
            }

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
            CurChannel = 0;           /* 0x40A777, once a pass */
            ImageDone = 0;
            memset(RowUp, 0, (size_t)RowBytes + 1);
            ResetPass();

            WinWrite = 0;
            WinFlush = 0x4000;

            i = InflateBlocks();
            if (i < 0)
                return i;

            /* Flush whatever the loop did not hand over.  The wrap test is
             * `test ah,ah / js` at 0x40A2B1, so it is bit *15* of the XOR:
             * `start` is always a multiple of 0x4000 and contributes
             * nothing there, leaving "has the write index passed 0x8000".
             *
             *  The first length is `(-start) & 0x7FFF` at 0x40A2CF, not
             *  0x4000 - (start & 0x7FFF): the two agree only while start is
             *  0x4000.  At 0x8000 the original's first call gets length zero
             *  and only the second one runs, because the window is a 32 KB
             *  ring and half of it is the part still being written. */
            {
                int start = WinFlush - 0x4000;

                if ((start ^ WinWrite) & 0x8000) {
                    Unfilter(&Window[start & 0x7FFF], (-start) & 0x7FFF);
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

/*
 *  loc_40D150: the 8bpp palettised format, format code 4.
 *
 *  Header, all little endian, recognised by sub_409260 (which reads the width
 *  and height from the same two words) and by the dispatch in sub_409560:
 *
 *      +0   19 91          magic
 *      +2   word           width
 *      +4   word           height
 *      +A   byte           8, bits per pixel
 *      +B   byte           0
 *      +20  256 * 3 bytes  RGB palette
 *      +320 width*height   one palette index per pixel, row major
 *
 *  The palette transform is the odd one out and is in the original: the first
 *  channel is biased by 0x40 and the result is scaled by four, so
 *
 *      pal[i] = 4 * (((((p[0] - 64) << 8) + p[1]) << 8) + p[2])
 *
 *  (`sub edx, 40h` / `shl edx, 8` / `add edx, ebx` / `shl edx, 8` /
 *  `add edx, ebx` / `shl edx, 2`).  There is no clamp, so a first channel below
 *  64 wraps the entry; that is what the original does.
 *
 *  It fills the same dword_190A420 the PNG path uses and sets the three globals
 *  main() reads, so nothing downstream has to know the format came from here.
 *  The pixel loop clips against the width and height it was given at both ends
 *  of the inner test rather than trusting the header, and the index is read
 *  straight through the palette with no range check - 256 entries and an 8 bit
 *  index, so there is nothing to check.
 */
static int DecodePal8(const unsigned char *data, unsigned char *pixels,
                      int pitch, unsigned int width, unsigned int height,
                      int xorigin, int roworigin)
{
    const unsigned char *p;
    unsigned char *base;
    unsigned row, col;
    int i;

    OutColType = 3;                 /* byte_190A824 */
    OutBitDepth = 8;                /* byte_190A825 */
    unk_190A820 = 256;              /* dword_190A820 */

    if (*(const unsigned short *)(data + 2) == 0)
        return -1;
    if (*(const unsigned short *)(data + 4) == 0)
        return -1;

    p = data + 0x20;
    for (i = 0; i < 256; i++, p += 3)
        Pal[i] = ((((((int)p[0] - 0x40) << 8) + p[1]) << 8) + p[2]) << 2;

    if ((unsigned)(height - roworigin) == 0)
        return 0;

    base = pixels + (size_t)pitch * roworigin;
    for (row = roworigin; row < height; row++) {
        unsigned char *line = base + (size_t)pitch * row;

        for (col = 0; col < width; col++)
            *(int *)(line + (size_t)col * 4) = Pal[*p++];
    }

    return 0;
}

/*
 *  sub_40DAC0: PCX, format code 6.  The header is recognised by its first four
 *  bytes as one dword, 0x0801050A, i.e. 0A 05 01 08 - version 5, RLE encoding,
 *  8 bits a pixel - and the image size comes from the window bounds at +4..+0B,
 *  `xmax - xmin + 1` by `ymax - ymin + 1`, exactly as GetImageSize() computes
 *  it.
 *
 *  Two very different cases share the RLE reader:
 *
 *    - `nplanes` 0 or 1: palettised.  With nplanes 0 the 256 entry palette is
 *      read from the *end* of the file (`lea eax, [edi+eax-300h]`, where eax is
 *      the file size) and `dword_190A820` is set to 256.  Its transform is not
 *      the 8bpp format's: there is no `<< 2` and the bias is 256 rather than 64,
 *
 *          pal[i] = ((p[0] - 256) << 16) + (p[1] << 8) + p[2]
 *
 *      which is the identity on the RGB triple, because subtracting 256 borrows
 *      into the top byte and leaves it at 0xFF for every r <= 255.  So the entry
 *      is the dword whose bytes are B, G, R, FF going up - opaque, as it should
 *      be.
 *
 *    - `nplanes` 3: RGB, one RLE stream per plane, and the plane number selects
 *      *which byte of the pixel* gets the sample.  The loop starts at plane
 *      `nplanes - 1` = 2 and counts down to -1, where it resets to 2, so the
 *      planes arrive in PCX's own order R, G, B and land in bytes 2, 1, 0 of
 *      `0xAARRGGBB`.  PCX has no alpha, so `0x40DBF0` fills byte 3 of every
 *      pixel in the clipped rectangle with 0xFF first and `byte_190A824` becomes
 *      2.
 *
 *  The RLE reader is `0x40DC7A`: a byte below 0xC0 is a literal with a count of
 *  one, a byte at or above it has `& 0x3F` repeats of the byte after it.  A
 *  count of zero (`0xC0`) advances to the next scanline without writing
 *  anything.
 *
 *  The clipping is the odd part and is the original's: the store tests the row
 *  against the height it was passed and the *byte* offset against four times the
 *  width, and the scanline length comes from `bytesPerLine` when that is longer
 *  than the row count - two quantities in different units, compared as though
 *  they were the same.  It is reproduced as measured.  The row loop runs to
 *  `height + yorigin` in rows and lets the store clip.
 */
static int DecodePcx(const unsigned char *data, int size, unsigned char *pixels,
                     int pitch, unsigned int width, unsigned int height,
                     int xorigin, int yorigin)
{
    const unsigned char *src;
    int w, h, nplanes, bytesperline;
int rowend, rowlimit, colstart, startcol, newh, colend4, row, col;
    int plane;

    if (*(const unsigned int *)data != 0x0801050Au)
        return -1;

    w = (short)(*(const short *)(data + 8) - *(const short *)(data + 4)) + 1;
    if (w <= 0)
        return -1;
    h = (short)(*(const short *)(data + 10) - *(const short *)(data + 6)) + 1;
    if (h <= 0)
        return -1;

    nplanes      = data[0x41];
    bytesperline = (short)*(const unsigned short *)(data + 0x42);

    src = data + 0x80;

    if (nplanes == 0) {
        const unsigned char *p = data + size - 0x300;
        int i;

        for (i = 0; i < 256; i++, p += 3)
            Pal[i] = (((int)p[0] - 0x100) << 16) + ((int)p[1] << 8) + p[2];

        OutColType = 3;
        OutBitDepth = 8;
        unk_190A820 = 256;
    }

    /* ---- 0x40DBB3: the row count the store is clipped against ---- */

    rowend = h + yorigin;
    if ((int)height < rowend)
        rowend = (int)height;

if (nplanes == 3) {
        int colend = (int)width, rowfirst, colfirst, c;

        OutColType = 2;

        colfirst = (xorigin > 0) ? xorigin : 0;      /* setle / dec / and ecx */
        rowfirst = (yorigin > 0) ? yorigin : 0;
        if (w + xorigin < colend)
            colend = w + xorigin;

        for (row = rowfirst; row < rowend; row++) {
            unsigned char *p = pixels + (size_t)pitch * row + 3;

            for (c = colfirst; c < colend; c++)
                p[(size_t)c * 4] = 0xFF;
        }
    } else if (nplanes != 1) {
        return 0;             /* 0x40DCFC / 0x40DD75: neither, so nothing runs */
    }

    /* ---- 0x40DC00: the common tail ---- */

    rowlimit = rowend + xorigin;
    if (bytesperline > rowend) {
        if ((int)width < rowlimit)
            width = (unsigned)rowlimit;
        rowlimit += bytesperline - rowend;
    }

    colstart = xorigin * 4;                        /* 0x40DC5E */
    newh     = h + yorigin;                         /* var_4 at 0x40DC1D */
    colend4  = (int)width * 4;                      /* 0x40DC50 */
    rowlimit *= 4;                                  /* 0x40DC55 */
    startcol = (nplanes - 1) + colstart;            /* 0x40DC6D */

    row = yorigin;                                  /* 0x40DC38 */

    for (;;) {
        unsigned char *rowbase = pixels + (size_t)pitch * row;
        int count, value;

        if (nplanes == 1)
            col = colstart;
        else
            col = startcol, plane = 2;

        /*
         *  0x40DC7A / 0x40DD01: one run.  A byte below 0xC0 is a literal with
         *  a count of one; at or above it, `& 0x3F` repeats of the byte after.
         *  A count of zero advances to the next scanline writing nothing.
         */
        for (;;) {
            if (*src < 0xC0) {
                count = 1;
                value = *src++;
            } else {
                count = *src++ & 0x3F;
                value = *src++;
            }

            while (count > 0) {
                if (row < (int)height && col < colend4) {
                    if (nplanes == 1)
                        *(int *)(rowbase + col) = Pal[value & 0xFF];
                    else
                        rowbase[col] = (unsigned char)value;
                }

                col += 4;

                if (col >= rowlimit) {
                    if (nplanes == 1) {
                        row++;
                        rowbase = pixels + (size_t)pitch * row;
                        col = colstart;
                    } else if (--plane < 0) {
                        plane = 2;                 /* 0x40DD5A */
                        row++;
                        rowbase = pixels + (size_t)pitch * row;
                        col = plane + colstart;
                    } else {
                        col = plane + colstart;
                    }
                }
                count--;
            }

            if (row >= newh)
                goto done;              /* 0x40DCE8 / 0x40DD6B: row == limit */
        }
    }

done:
    return 0;
}

int DecodeImage(unsigned char *fileimg, int filesize, unsigned char *pixels,
                int pitch, unsigned int width, unsigned int height,
                int unused7, int unused8)
{
    (void)unused7;
    (void)unused8;

    /*
     *  sub_409560's dispatch, in its order.  The format codes it returns are
     *  1 PNG, 2 JPEG, 3 GIF, 4 the 8bpp palette below, 5 BMP, 6 PCX, 7 DDS and
     *  8 Targa; of those only 1 and 4 are transcribed.
     */
    if (fileimg && filesize >= 2) {
        if (fileimg[0] == 0x89 && fileimg[1] == 'P')
            return DecodePng(fileimg, filesize, pixels, pitch, width, height);

        if (fileimg[0] == 0x19 && fileimg[1] == 0x91 &&
            fileimg[10] == 8 && fileimg[11] == 0)
            return DecodePal8(fileimg, pixels, pitch, width, height,
                              unused7, unused8);

        if (*(const unsigned int *)fileimg == 0x0801050Au)
            return DecodePcx(fileimg, filesize, pixels, pitch, width, height,
                             unused7, unused8);
    }

    printf("\n*** That input format has not been reconstructed yet.\n"
           "*** This build decodes PNG and the 8bpp palettised format;"
           " see README.md.\n\n");
    exit(3);
    return -1;
}
