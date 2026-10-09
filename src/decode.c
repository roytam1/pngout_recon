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

    if (*(const short *)(data + 2) <= 0)
        return -1;
    if (*(const short *)(data + 4) <= 0)
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

/*
 *  One Targa pixel, 0x40D44D through 0x40D4DB.
 *
 *  Four widths, and they are not the same thing as four conversions:
 *
 *      1 byte   dword_190A420[byte]                    the ramp or the palette
 *      2 bytes  A1R5G5B5, `shl eax, 3` and the top byte from the borrow
 *      3 bytes  dword | 0xFF000000                     B, G, R, opaque
 *      4 bytes  the dword as it stands                  B, G, R, A
 *
 *  The 16 bit case is worth spelling out.  R is bits 10-14 and G is bits 5-9,
 *  and the expression is
 *
 *      ((B5 + (G5 + R5 * 8 - 0x40000) * 8) << 3)
 *
 *  which expands to `(B5*8) + (G5*8 << 8) + (R5*8 << 16) - 0x1000000`: the three
 *  channels land in bytes 0, 1, 2 in B, G, R order, and the borrow out of the top
 *  leaves 0xFF there.  The five bit channels are `v << 3`, so 31 comes out as
 *  0xF8, not 0xFF - a truncation, and it is the original's.  Bit 15, the alpha
 *  bit, is ignored: the top byte is 0xFF whatever it says.
 *
 *  For a colour mapped file the *index* is turned into a pointer into the
 *  colour map first, and `*sourcep` still advances by the entry size, so the two
 *  walks stay in step.  At one byte a pixel the entry fetched is itself just a
 *  byte, and it is that byte which indexes dword_190A420 - so a colour mapped
 *  8 bit Targa decodes through the greyscale ramp the header switch installed a
 *  few instructions earlier and the file's colour map is read but not used.
 *  Also the original's.
 */
static unsigned int ReadTargaPixel(const unsigned char **sourcep, int bpp,
                                   const unsigned char *cmapbase, int cmapped)
{
    const unsigned char *s = *sourcep;
    const unsigned char *at = s;
    unsigned int v;

    if (cmapped) {
        if (bpp == 1)
            at = cmapbase + *s;
        else if (bpp == 2)
            at = cmapbase + *(const unsigned short *)s;
        s += bpp;
    }

    switch (bpp) {
    case 1:
        v = (unsigned int)Pal[*at];
        if (!cmapped)
            s += 1;
        break;

    case 2: {
        unsigned int word = *(const unsigned short *)at;
        unsigned int r5 = word & 0x7C00u;
        unsigned int g5 = word & 0x03E0u;
        unsigned int b5 = word & 0x001Fu;

        v = ((b5 + (g5 + r5 * 8u - 0x40000u) * 8u) << 3);
        if (!cmapped)
            s += 2;
        break;
    }

    case 3:
        v = *(const unsigned int *)at | 0xFF000000u;
        if (!cmapped)
            s += 3;
        break;

    default:
        v = *(const unsigned int *)at;
        if (!cmapped)
            s += 4;
        break;
    }

    *sourcep = s;
    return v;
}

/*
 *  sub_40D250: Targa, format code 8.  There is no magic, so this - and
 *  GetImageSize() - guess the format from the header being self consistent: at
 *  least 19 bytes, colour map type 0 or 1, an image type below 12 with one of
 *  bits 1, 2, 3, 11 or 15 of `1 << type` set, a pixel depth that is a multiple of
 *  8 and no more than 32, and a descriptor whose top two bits are clear.  Bits 4
 *  and 5 of the descriptor are the origin, and both are allowed, so the decoder
 *  has to handle all four corners.
 *
 *  The depth decides the colour type (0x40D32C): 8 bits greyscale, 16 and 24
 *  bits RGB, 32 bits RGBA.  The 8 bit case also fills dword_190A420 with the
 *  greyscale ramp, in place - entry 0 is `0xFF000000` and every other entry is
 *  the one before it plus `0x010101`.
 *
 *  A colour map is skipped rather than applied, in the sense that its *offset*
 *  arithmetic is all that survives: `0x40D2F3` builds a base of
 *  `dataStart - firstEntry * entrySize` so that `base + index * entrySize` lands
 *  on the entry, and `0x40D31A` steps the source pointer past it.
 *
 *  The run length reader is a state machine in `edi`, seeded at 0x40D403:
 *  `sbb edi, edi` then `dec edi` makes it -1 for an RLE type and -2 for a raw
 *  one.  Below 0x80 it is a raw packet count, and `& 0x7F == 0x7F` means the
 *  count is in the next byte instead (Targa 2.0's repeater packet).  At or above
 *  0x80 it is a run and no pixel is read at all - the previous one is stored
 *  again.  The decrement at 0x40D4DD is `test edi, edi` / `jl` / `dec`, so a raw
 *  packet of n stores n + 1 pixels: one too many, harmless because the store is
 *  clipped, and in the original.
 *
 *  The store clips against the width and height it was passed, and the row
 *  loop walks the destination by `+pitch` or `-pitch` according to the origin
 *  bit, with the column index stepping `+1` or `-1`.
 */
static int DecodeTarga(const unsigned char *data, int size, unsigned char *pixels,
                       int pitch, unsigned int width, unsigned int height,
                       int xorigin, int yorigin)
{
    const unsigned char *src, *cmapbase = NULL;
    int w, h, depth, bpp, imgtype, cmapped, entrysize;
    int colstart, rowstart, colstep, rowistep, rowbytes;
    int colend, rowlimit, col, row, coloff, state;
    unsigned int px = 0;
    int i;

    /* ---- the header test, the same one GetImageSize() makes ---- */

    if (size < 0x13)
        return -1;
    if (data[1] & 0xFE)
        return -1;
    if (data[2] >= 0x0C)
        return -1;
    if (((1 << data[2]) & 0xE0E) == 0)
        return -1;
    depth = data[0x10];
    if ((depth & 7) != 0 || depth == 0 || depth > 0x20)
        return -1;
    if (data[0x11] & 0xC0)
        return -1;

    w = *(const unsigned short *)(data + 0x0C);
    if (w <= 0)
        return -1;
    h = *(const unsigned short *)(data + 0x0E);
    if (h <= 0)
        return -1;

    imgtype  = data[2];
    cmapped  = (data[1] == 1);
    bpp      = (depth + 7) >> 3;
    src      = data + 0x12 + data[0];         /* 0x40D2BA: past the id field */

    if (cmapped) {
        entrysize = (data[7] + 7) >> 3;
        cmapbase  = src - *(const unsigned short *)(data + 3) *
                        (unsigned)entrysize;
        src += *(const unsigned short *)(data + 5) * (unsigned)entrysize;
    }

    /* ---- 0x40D32C: the colour type, and the ramp for one byte a pixel ---- */

    switch (bpp) {
case 1:
        OutColType = 0;
        OutBitDepth = 8;
        /*
         *  0x40D340: entry 0 is opaque black, and every entry after it is the
         *  one *before* it plus 0x010101.  The read is at dword_190A41C, which
         *  is dword_190A420 one dword lower - so this is an ordinary sequential
         *  ramp, and the alpha byte rides along from entry 0 as 0xFF.
         */
        Pal[0] = 0xFF000000u;
        for (i = 1; i < 256; i++)
            Pal[i] = Pal[i - 1] + 0x010101u;
        break;

    case 2:
    case 3:
        OutColType = 2;
        break;

    default:
        OutColType = 6;
        break;
    }

    /* ---- 0x40D382: the origin bits ---- */

    i = data[0x11];
    colstart = (i & 0x10) ? w - 1 : 0;
    rowstart = (i & 0x20) ? h - 1 : 0;
    colstep  = (i & 0x10) ? -1 : 1;
    rowistep = (i & 0x20) ? -1 : 1;
    rowbytes = (i & 0x20) ? -pitch : pitch;
    colend   = (i & 0x10) ? -1 : w;
    rowlimit = (i & 0x20) ? -1 : h;

    col    = colstart + xorigin;
    row    = rowstart + yorigin;
    coloff = col;
    colend += xorigin;
    rowlimit += yorigin;

    state = (imgtype < 8) ? -2 : -1;          /* 0x40D403 */

    for (;;) {
        if (coloff == colend) {              /* 0x40D425: this row is done */
            row += rowistep;
            if (row == rowlimit)
                break;
            coloff = col;
            continue;
        }

        if (state < 0x80) {
            if ((state & 0x7F) == 0x7F)     /* 0x40D441: the count is the next
                                             * byte, not the low seven bits */
                state = *src++;
            px = ReadTargaPixel(&src, bpp, cmapbase, cmapped);
        }

        if (state >= 0)
            state--;

        if ((unsigned)coloff < width && (unsigned)row < height)
            *(int *)(pixels + (size_t)pitch * (row - yorigin) +
                    (size_t)(coloff - xorigin) * 4) = (int)px;

        coloff += colstep;                   /* 0x40D4FB */
    }

    return 0;
}

/*
 *  Locals for the BMP path.  `sub_40D560` keeps the channel bit counts and the
 *  masks in six consecutive globals - three counts and three masks - and the
 *  conversion below works on that pair directly rather than on three separate
 *  shift/mask pairs, because that is how the original stores them:
 *
 *      dword_190A420/424/428   the *shifts*  (r, g, b)
 *      dword_190A42C/430/434   the *counts*  (r, g, b)
 *
 *  A 16 or 32 bit image starts with both filled in by hand (`0x40D695` for 32,
 *  `0x40D6B9` for 16), BI_BITFIELDS overwrites them from the three mask dwords
 *  at `data + biSize` (`0x40D6EF`), and then `0x40D73F` turns counts into masks:
 *
 *      shift = 24 - count - shift      for R
 *      shift = 16 - count - shift      for G
 *      shift =  8 - count - shift      for B
 *      mask  = (-1 << (24 - count)) & 0xFF0000     r
 *      mask  = (-1 << (16 - count)) & 0x00FF00     g
 *      mask  = (-1 << ( 8 - count)) & 0x0000FF     b
 *
 *  which puts each channel's bits at the top of its own byte and leaves the
 *  count alone where the pixel case wants it.  The hardcoded defaults are
 *  count 8 / shift 16, 8 / 8, 8 / 0 for 32 bits - no rotation at all - and
 *  5 / 10, 5 / 5, 5 / 0 for 16, which rotates by 9, 6 and 3 and masks five bits
 *  each.  So a 16 bit BMP is RGB565 read without any scaling: the channels come
 *  out as v << 3, R and B topping out at 0xF8 and G at 0x78, and the borrow out
 *  of the `- 0x1000000` leaves the alpha byte at 0xFF.
 *
 *  The bit counts are read by the shift-and-count loop at `0x40D710`, which
 *  skips the zeros below the mask and then counts the ones - capped at 32.
 */
typedef struct BmpMasks {
    int  shift[3];                 /* dword_190A420, +4, +8   */
    unsigned int mask[3];          /* dword_190A42C, +4, +8   */
} BmpMasks;

static unsigned int BmpCountBits(unsigned int m)
{
    int lead = 0, ones = 0;

    if (m == 0)
        return 0;
    while (lead < 32 && !(m & 1)) {
        m >>= 1;
        lead++;
    }
    while (ones < 32 && (m & 1)) {
        m >>= 1;
        ones++;
    }
    return (unsigned int)ones;
}

static void BmpMakeMasks(BmpMasks *bm, const int *counts, const int *shifts)
{
    static const int tops[3] = { 24, 16, 8 };
    static const unsigned int ands[3] = { 0xFF0000u, 0x00FF00u, 0x0000FFu };
    int k;

    for (k = 0; k < 3; k++) {
        bm->shift[k] = tops[k] - (int)counts[k] - shifts[k];
        bm->mask[k] = (0xFFFFFFFFu << (tops[k] - (int)counts[k])) & ands[k];
    }
}

/*
 *  sub_40D560: BMP, format code 5.
 *
 *  Two header shapes, and which one it is says where everything is:
 *
 *    BITMAPCOREHEADER, biSize 12: width a word at +0x12, height a word at
 *      +0x14, bit count at +0x18, and no compression field at all.
 *    BITMAPINFOHEADER, biSize 40: width a dword at +0x12, height at +0x16,
 *      bit count at +0x1C, compression a dword at +0x1E, and for BI_BITFIELDS
 *      three channel masks at +0x36.
 *
 *  `0x40D5F1` refuses a compression field that is neither 0 nor 3, so 16 bit
 *  images arrive as masked RGB rather than as plain RGB555.
 *
 *  The palette, for a bit depth below 16, is read from `data + biSize` in
 *  strides of 3 (core) or 4 (info) until it reaches `data + biOffset`, which is
 *  `0x40D62B` and `0x40D640`: `lea eax, [esi+ebp+0Eh]` puts the walk at the end
 *  of the header and `cmp eax, esi` against the pixel data stops it.  Each entry
 *  is stored `| 0xFF000000`, which forces the alpha opaque - a 24 bit BGR field
 *  has only three bytes, and the fourth is read from whatever follows.  That is
 *  in the original, and for a 3 byte entry at the very end of the file it reads
 *  past the buffer.
 *
 *  Two entries and a two bit image get black and white installed first
 *  (`0x40D60F`), and the palette loop then overwrites them if the file carries
 *  any.
 *
 *  Rows are padded to four bytes: `((width * bpp + 31) >> 3) & ~3` at
 *  `0x40D7CA`.  A positive height is bottom up, and the original handles that by
 *  stepping the source pointer *backwards* from the last row (`0x40D7EC`:
 *  `add ebp, ebx` then `neg eax`) while the destination steps forwards; a
 *  negative height is the top-down flag and flips the other way (`0x40D7E4`).
 *  Either way the destination comes out the right way up.
 *
 *  The clip is the original's and is done before the switch: `0x40D820` tests the
 *  row against the height and `0x40D82C` the column against the width, and the
 *  per-case loops clamp the column again against the row's own limit.
 */
static int DecodeBmp(const unsigned char *data, int size, unsigned char *pixels,
                     int pitch, unsigned int width, unsigned int height,
                     int xorigin, int yorigin)
{
    const unsigned char *pixeldata, *row;
    int hdrsize, w, h, bpp, compression, stride, ydir, rows;
    int y, col, xcount, dcol, drow;
    BmpMasks bm;
    (void)size;

    hdrsize = *(const short *)(data + 0x0E);
    if (hdrsize == 12) {
        if (*(const unsigned short *)(data + 0x16) != 1)
            return -1;
        w = *(const unsigned short *)(data + 0x12);
        h = *(const unsigned short *)(data + 0x14);
        bpp = *(const unsigned short *)(data + 0x18);
        compression = 0;
    } else {
        if (*(const unsigned short *)(data + 0x1A) != 1)
            return -1;
        w = *(const int *)(data + 0x12);
        h = *(const int *)(data + 0x16);
        bpp = *(const unsigned short *)(data + 0x1C);
        compression = *(const int *)(data + 0x1E);
    }

    if (w <= 0 || h == 0)
        return -1;
    if (bpp < 1 || bpp > 32)
        return -1;
    if (compression != 0 && compression != 3)
        return -1;

    pixeldata = data + *(const unsigned int *)(data + 0x0A);

    /* ---- the colour type, the palette and the channel masks ---- */

if (bpp < 16) {
        int n = 0;
        /*
         *  0x40D62B: `lea eax, [esi+ebp+0Eh]` with esi = biSize and ebp =
         *  fileimg, so the walk starts at fileimg + 14 + biSize - *after* the
         *  14 byte file header, not at fileimg + biSize.  The stride is
         *  `add ebx, 3` on top of `setnz bl`, i.e. 3 for a core header and 4
         *  for an info one, and the stop is fileimg + biOffset, which
         *  `0x40D62F` builds from the +0x0A field.
         */
        const unsigned char *p = data + 14 + hdrsize;
        int stride = (hdrsize == 12) ? 3 : 4;

        if (bpp == 2) {
            Pal[0] = 0xFFFFFFFFu;
            Pal[1] = 0xFF000000u;
        }

        /* the original has no cap here; 256 is only here so that a file with
         * more palette than a PLTE can hold cannot run off the ramp */
        while (p < pixeldata && n < 256) {
            unsigned int e;

            memcpy(&e, p, (size_t)stride);
            Pal[n++] = e | 0xFF000000u;
            p += stride;
        }

        OutColType = 3;
        OutBitDepth = (unsigned char)bpp;
        unk_190A820 = n;
    } else {
        int counts[3], shifts[3];

        OutColType = 2;

        if (bpp == 16) {
            counts[0] = counts[1] = counts[2] = 5;
            shifts[0] = 10;
            shifts[1] = 5;
            shifts[2] = 0;
        } else {
            counts[0] = counts[1] = counts[2] = 8;
            shifts[0] = 16;
            shifts[1] = 8;
            shifts[2] = 0;
        }

if (compression == 3) {
            /* 0x40D6EF: `lea edx, [esi+ebp+0Eh]`, again past the file header */
            const unsigned int *m =
                (const unsigned int *)(data + 14 + hdrsize +
                                       *(const int *)(data + 0x36));
            int k;

            for (k = 0; k < 3; k++) {
                unsigned int v = m[k];

                shifts[k] = 0;
                while (shifts[k] < 32 && !(v & 1)) {
                    v >>= 1;
                    shifts[k]++;
                }
                counts[k] = (int)BmpCountBits(m[k]);
            }
        }

        BmpMakeMasks(&bm, counts, shifts);
    }

    /* ---- 0x40D7C4: the stride, the direction and the two row pointers ---- */

    stride = ((w * bpp + 31) >> 3) & ~3;
    if (h < 0) {
        ydir = -1;                     /* top-down */
        rows = -h;
        row = pixeldata;
        drow = 1;
    } else {
        ydir = 1;                      /* bottom-up */
        rows = h;
        row = pixeldata + (size_t)(rows - 1) * stride;
        drow = -1;
    }

    dcol = (ydir < 0) ? 1 : 1;        /* columns always run left to right */

    for (y = 0; y < rows; y++) {
        unsigned char *dest;

        /* the clip the original does before the switch, 0x40D820 */
        if (yorigin + y < 0 || yorigin + y >= (int)height) {
            row += (size_t)drow * stride;
            continue;
        }

        dest = pixels + (size_t)pitch * (yorigin + y) + (size_t)xorigin * 4;
        xcount = w;
        if (xorigin < 0) {
            dest -= (size_t)xorigin * 4;
            xcount += xorigin;
            if (xcount < 0)
                xcount = 0;
        }
        if (xorigin + xcount > (int)width)
            xcount = (int)width - xorigin;

        for (col = 0; col < xcount; col++) {
            unsigned int px;

            switch (bpp) {
            case 1:
                px = Pal[(row[col >> 3] >> (7 - (col & 7))) & 1];
                break;

            case 4:
                px = Pal[(row[col >> 1] >> (((col & 1) ^ 1) * 4)) & 0xF];
                break;

            case 8:
                px = Pal[row[col]];
                break;

            case 16: {
                unsigned int v = *(const unsigned short *)(row + (size_t)col * 2);
                unsigned int r = (v << bm.shift[0]) & bm.mask[0];
                unsigned int g = (v << bm.shift[1]) & bm.mask[1];
                unsigned int b = (v << bm.shift[2]) & bm.mask[2];

                px = r + g + b - 0x1000000u;
                break;
            }

            case 32:
                px = *(const unsigned int *)(row + (size_t)col * 4);
                break;

            default:
                px = *(const unsigned int *)(row + (size_t)col * 3) | 0xFF000000u;
                break;
            }

            *(int *)(dest + (size_t)col * 4) = (int)px;
        }

        row += (size_t)drow * stride;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  sub_40CA70: GIF, format code 3                                    */
/* ------------------------------------------------------------------ */

/*
 *  The LZW, in the shape the original has it.  Three globals and a stack:
 *
 *      InBuf[768]      dword_18D0030, the input, refilled in place
 *      Slot[4100]      dword_18D0330, the code table
 *      SlotLen[]       byte_18D8F88, and byte_18D8F87, the same slot's code
 *      Stack[]         byte_18DD9D8, the expanded string
 *
 *  Codes arrive LSB first.  The reader shifts what is left down to the bottom
 *  of InBuf whenever it gets within three bytes of the end and appends the next
 *  sub-block - the `v48 = v59 - v47` at 0x40CF15 - and the code size grows when
 *  the next free slot would reach `1 << codesize`, capped at 12 at 0x40CFB1.
 *
 *  A code expands by walking the chain back to a literal, filling Stack from the
 *  end, and the emit loop then walks the length *down*: the string comes out in
 *  order without being reversed first.
 */
typedef struct GifLzw {
    unsigned char  in[768];
    int            inlen;           /* bytes valid in `in`                */
    int            bitpos;
    int            bits;            /* code size in bits                  */
    int            clear, end;      /* clear and end codes                */
    int            next;            /* the next free slot                 */
    unsigned short slot[4100];
    unsigned char  slotcode[4100];
    unsigned char  slotlen[4100];
    unsigned char  stack[4100];
} GifLzw;

/* the GIF sub-block chain: a length byte then that many bytes, to a zero */
typedef struct GifBlocks {
    const unsigned char *p;
    const unsigned char *end;
} GifBlocks;

/*
 *  Pull the next sub-block into the buffer, keeping whatever has not been read
 *  yet.  `keep` is the number of unread bytes already at the front.
 */
static void GifRefill(GifLzw *z, GifBlocks *b, int keep)
{
    int n = 0;

    if (b->p < b->end && *b->p)
        n = *b->p++;
    else if (b->p < b->end)
        b->p++;                        /* the terminating zero */
    else
        n = 0;

    if (n > 0) {
        int avail = (int)(b->end - b->p);

        if (n > avail)
            n = avail;
        if (keep + n > (int)sizeof z->in)
            n = (int)sizeof z->in - keep;
        if (n > 0)
            memcpy(z->in + keep, b->p, (size_t)n);
        b->p += n;
    }
    z->inlen = keep + (n > 0 ? n : 0);
}

/* 0x40CEE0: one code, or -1 when the image data runs out */
static int GifNextCode(GifLzw *z, GifBlocks *b)
{
    for (;;) {
        int avail, code, keep;

        /* enough bits left for the widest code we could have? */
        avail = z->inlen * 8 - z->bitpos;
        if (avail < z->bits) {
            keep = z->inlen - (z->bitpos >> 3);
            if (keep < 0)
                keep = 0;
            if (keep > 0)
                memmove(z->in, z->in + (z->bitpos >> 3), (size_t)keep);
            z->bitpos &= 7;
            GifRefill(z, b, keep);
            avail = z->inlen * 8 - z->bitpos;
            if (avail < z->bits)
                return -1;
        }

        code = 0;
        {
            int i;

            for (i = 0; i < z->bits; i++)
                if (z->bitpos + i < z->inlen * 8 &&
                    (z->in[(z->bitpos + i) >> 3] >> ((z->bitpos + i) & 7)) & 1)
                    code |= 1 << i;
        }
        z->bitpos += z->bits;

        if (code == z->clear) {
            z->next = z->end + 1;
            z->bits = 1;
            while ((1 << (z->bits - 1)) < z->clear)
                z->bits++;
            if (z->bits > 12)
                z->bits = 12;
            continue;
        }

        return code;
    }
}

/*
 *  sub_40CA70: GIF, format code 3.
 *
 *  **The interlace and local-colour-table flags are transposed.**  Two separate
 *  places in the function get them the wrong way round, and both are visible in
 *  the decompilation:
 *
 *    - `if (v16 < 0)` - a *signed* test, so bit 7, guards the block that sizes
 *      and skips a local colour table.  Bit 7 is the **interlace** flag.
 *    - `v61 = 8, v55 = 1` when bit 6 is set, and 8 is the *first interlace
 *      pass's stride*, so bit 6 - the **local colour table** flag - is what
 *      starts the four-pass walk.
 *
 *  So a GIF carrying a local colour table (bit 6 set, bit 7 clear) is decoded as
 *  interlaced, and its palette is *not* skipped; an interlaced one skips palette
 *  bytes that are not there.  In the original, and reproduced here.
 *
 *  The interlace passes themselves are GIF's, though: the switch at 0x40D068
 *  keys off the stride alone - 8, 8, 4, 2 - and starts each new pass at a
 *  fixed offset from the row the image began at, so the four passes are 0, 4,
 *  2 and 1 exactly as GIF specifies.  Nothing guards the switch except the
 *  stride, which is why a stride of 1 only reaches it on the last row and a
 *  stride of 2 ends the image.
 *
 *  The palette transform is the PCX trick again.  0x40CB98-0x40CBBE loads
 *  p[0] into ecx, subtracts 0x100, shifts up twice and adds p[1] then p[2]:
 *
 *      Pal[i] = ((p[0] - 0x100) << 8 + p[1]) << 8 + p[2]
 *
 *  which is the identity on R,G,B and leaves the top byte at 0xFF by the
 *  borrow - so a GIF ends up with the same internal 0xFFBBGGRR byte order as
 *  PNG and BMP.  The entry counter ebp is stored directly (`[ebp*4]` at
 *  0x40CBC0, advancing the source by three), so the indices are 0..size-1 and
 *  `cmp ebp, 100h` is what stops the fill.  The rest of the ramp is then filled
 *  with opaque black by the `rep stosd` at 0x40CBE6, and a Graphic Control
 *  Extension's transparent index has its alpha byte cleared at 0x40CBF9.
 */
static int DecodeGif(const unsigned char *data, int size, unsigned char *pixels,
                    int pitch, unsigned int width, unsigned int height,
                    int xorigin, int yorigin)
{
    const unsigned char *pal;
    GifBlocks b;
    GifLzw z;
    int transp = -1, gsize, flags, left, top, w, h, stride, interlace;
    int i, col, row, colend, rowend, screenw, screenh;
    int mincode, prev = -1;

    OutColType = 3;
    OutBitDepth = 8;

    if (data[0] != 'G' || data[1] != 'I' || data[2] != 'F')
        return -1;

    b.p = data + 13;
    b.end = data + size;

    gsize = 1 << ((data[10] & 7) + 1);
    unk_190A820 = gsize;
    pal = data + 13;                            /* the global colour table */
    if ((signed char)data[10] < 0)
        b.p += 3 * gsize;

    /* ---- the extension blocks, then one image descriptor ---- */

    while (b.p < b.end && *b.p == 0x21) {
        b.p++;
        if (b.p + 1 < b.end && b.p[0] == 0xF9 && (b.p[2] & 1) != 0)
            transp = b.p[5];
        b.p++;
        for (;;) {
            unsigned char n = (b.p < b.end) ? *b.p++ : 0;

            b.p += n;
            if (!n)
                break;
        }
    }

    if (b.p >= b.end || *b.p != 0x2C)          /* not an image descriptor */
        return -1;
    b.p++;

    left  = *(const unsigned short *)(b.p);
    top   = *(const unsigned short *)(b.p + 2);
    w     = *(const unsigned short *)(b.p + 4);
    h     = *(const unsigned short *)(b.p + 6);
    flags = b.p[8];
    b.p += 9;

    if (flags & 0x40) {
        stride = 8;
        interlace = 1;
    } else {
        stride = 1;
        interlace = 0;
    }

    if ((signed char)flags < 0) {
        /*
         *  0x40CB6A.  `test cl, cl` / `jns` is a *signed* test, so this is bit
         *  7 - the interlace flag - and it is the only thing that repoints the
         *  palette.  0x40CAC7 put the *global* colour table there on the way
         *  in, so this arm is meant to be the local one; bit 7 being interlace,
         *  a real local colour table is left in place and read as image data,
         *  and an interlaced one has the LZW stream skipped instead.
         */
        gsize = 1 << ((flags & 7) + 1);
        unk_190A820 = gsize;
        pal = b.p;
        b.p += 3 * gsize;
    }

    /* ---- 0x40CB85: the palette ---- */

    for (i = 0; i < 256; i++)
        Pal[i] = 0xFF000000u;
    for (i = 0; i < gsize && i < 256; i++) {
        const unsigned char *e = pal + 3 * i;

        /* the outer parentheses matter: + binds tighter than << */
        Pal[i] = ((unsigned int)((((int)e[0] - 0x100) << 8) + e[1]) << 8)
                   + e[2];
    }
    if (transp >= 0 && transp < 256)
        Pal[transp] &= 0x00FFFFFFu;

    /* ---- 0x40CC1D: the fast path, and then the background ---- */

    screenw = *(const unsigned short *)(data + 6);
    screenh = *(const unsigned short *)(data + 8);

    col    = xorigin;
    row    = yorigin;
    colend = col + w;
    rowend = row + h;

    if (!(left == 0 && top == 0 && screenw == w && screenh == h)) {
        unsigned bg = ((signed char)data[10] < 0)
                          ? (unsigned int)Pal[data[11] & 0xFF] : 0u;
        int cfirst = xorigin > 0 ? xorigin : 0;
        int rfirst = yorigin > 0 ? yorigin : 0;
        int clast  = (left + xorigin) < (int)width ? left + xorigin
                                                   : (int)width;
        int faredge    = (screenw + xorigin) < (int)width ? screenw + xorigin
                                                      : (int)width;
        int past   = cfirst + left + w;
        int rlast  = (top + yorigin) < (int)height ? top + yorigin
                                                   : (int)height;
        int rlast2 = (yorigin + top + h) < (int)height ? yorigin + top + h
                                                       : (int)height;
        int rlast3 = (screenh + yorigin) < (int)height ? screenh + yorigin
                                                       : (int)height;
        int r;

        /* the four rep stosd runs, in the order 0x40CCAE lays them out */
        for (r = rfirst; r < rlast; r++)
            if (cfirst < faredge)
                memset32((int *)(pixels + (size_t)pitch * r) + cfirst,
                         bg, faredge - cfirst);
        for (r = rlast; r < rlast2; r++) {
            int *line = (int *)(pixels + (size_t)pitch * r);

            if (cfirst < clast)
                memset32(line + cfirst, bg, clast - cfirst);
            if (past < faredge)
                memset32(line + past, bg, faredge - past);
        }
        for (r = rlast2; r < rlast3; r++)
            if (cfirst < faredge)
                memset32((int *)(pixels + (size_t)pitch * r) + cfirst,
                         bg, faredge - cfirst);

        col    = xorigin + left;
        row    = yorigin + top;
        colend = col + w;
        rowend = row + h;
    }

    /* ---- 0x40CE22: the destination walk ---- */

    if (row >= (int)height) {
        col += (int)0x80000000;                 /* make the store test fail */
        colend += (int)0x80000000;
    }

    /* ---- the LZW ---- */

    if (b.p >= b.end)
        return 0;
    mincode = *b.p++;

    memset(&z, 0, sizeof z);
    z.clear = 1 << mincode;
    z.end = z.clear + 1;
    z.next = z.end + 1;
    z.bits = 1;
    while ((1 << (z.bits - 1)) < z.clear)
        z.bits++;
    if (z.bits > 12)
        z.bits = 12;
    for (i = 0; i < z.clear; i++) {
        z.slot[i] = (unsigned short)i;
        z.slotcode[i] = (unsigned char)i;
        z.slotlen[i] = (unsigned char)i;
    }

    for (;;) {
        int code, sp = 0, c, first, k;

        code = GifNextCode(&z, &b);
        if (code < 0 || code == z.end)
            return 0;
        if (code == z.clear) {
            z.next = z.end + 1;
            z.bits = z.clear == 2 ? 3 : (z.clear == 4 ? 4 :
                      (z.clear == 8 ? 5 : 6));
            if (z.bits > 12)
                z.bits = 12;
            prev = -1;
            continue;
        }

        /*
         *  Expand.  `slot[c]` is the code of string c's tail and `slotcode[c]`
         *  its first character, so walking the chain down to a root and then
         *  pushing that root fills the string in order.  A code equal to the
         *  next free slot is the case that repeats the *previous* string's own
         *  head - `c = prev`, not `next - 1`, because the slot the encoder
         *  created is `prev + first char of prev` and the decoder has not built
         *  it yet.
         */
        c = (code >= z.next) ? prev : code;
        if (c < 0)
            return 0;                                /* a malformed stream */
        while (c >= z.end + 1) {
            z.stack[sp++] = z.slotcode[c];
            c = z.slot[c];
        }
        first = c;
        z.stack[sp++] = (unsigned char)c;
        if (code >= z.next)
            z.stack[sp++] = (unsigned char)first;

        /* a new slot: the previous string plus this one's first character */
        if (prev >= 0 && z.next < 4100) {
            z.slot[z.next] = (unsigned short)prev;
            z.slotcode[z.next] = (unsigned char)first;
            z.slotlen[z.next] = (unsigned char)(z.slotlen[prev] + 1);
            z.next++;
            /*
             *  The encoder's table runs one entry ahead of this one, so it
             *  reaches 1 << bits one code sooner; +1 on both sides of the
             *  comparison is what keeps the two stepping together.  Without it
             *  the first eight codes are fine and everything after them is
             *  read at the wrong width.
             */
            if (z.next + 1 == (1 << z.bits) && z.bits < 12)
                z.bits++;
        }
        prev = code;

        /* ---- emit, backwards, 0x40D025 ---- */
        for (k = sp - 1; k >= 0; k--) {
            if ((unsigned)col < width && (unsigned)row < height)
                *(int *)(pixels + (size_t)pitch * row + (size_t)col * 4) =
                    Pal[z.stack[k]];

            col++;
            if (col != colend)
                continue;

            row += stride;
            if (row >= rowend) {
                /*
                 *  0x40D068: the switch is on the *stride*, with no flag
                 *  guarding it.  A stride of 1 only reaches here on the last
                 *  row, and 2 ends the image; 8 and 4 start the next pass at a
                 *  fixed offset from the row the image began at, which is
                 *  GIF's own 0, 4, 2, 1 after all.  var_55 just alternates the
                 *  two stride-8 passes.
                 */
                if (stride == 8) {
                    if (interlace) {
                        interlace = 0;
                        row = yorigin + top + 4;
                    } else {
                        row = yorigin + top + 2;
                        stride = 4;
                    }
                } else if (stride == 4) {
                    row = yorigin + top + 1;
                    stride = 2;
                } else {
                    return 0;
                }
            }
            col    = xorigin + left;
            colend = col + w;
            if (row >= (int)height) {
                col += (int)0x80000000;
                colend += (int)0x80000000;
            }
        }
    }

    (void)b;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  sub_40DD80: DDS, format code 7                                    */
/* ------------------------------------------------------------------ */

/*
 *  sub_40DD80: DDS, format code 7.
 *
 *  Two arms, chosen by ddspf.flags at +80 in this order.  DDPF_RGB (0x40) is an
 *  uncompressed surface and takes four masks exactly - 0x00FF0000, 0x0000FF00,
 *  0x000000FF, 0xFF000000 at 0x40DDAA, 32 bits a pixel, anything else to the
 *  reject - then a plain row copy with clipping.  DDPF_FOURCC (0x4) reads the
 *  fourCC as 'D' 'X' 'T' c and requires c - '0' in 1..5; `and eax, 1` at
 *  0x40DEFE sends the odd codes down the straight path and the even ones
 *  through the un-premultiply.
 *
 *  The colour block is a DXT1 block for every code: two 565 endpoints and four
 *  bytes of 2-bit indices.  The endpoints expand with R = (c >> 8) & 0xF8,
 *  G = (c >> 3) & 0xFC and B = (c & 31) << 3 - note G is << 2, not the usual
 *  << 3 | >> 3 - and the four-colour ramp is (21845 * x) >> 16, i.e. /3, while
 *  DXT1 with c0 <= c1 uses the average and a transparent fourth.  The even
 *  codes never punch through; the `c0 <= c1` test also requires v63 == 1.
 *
 *  The alpha is per code: DXT1 from a four-entry table ([255,255,255,255], or
 *  [255,255,255,0] punched), DXT2/3 an explicit nibble * 17, DXT4/5 an 8-entry
 *  ramp built with (13107 * x) >> 16 (/5) for the six-value case and
 *  (9362 * x) >> 16 (/7) for the eight-value one, indexed by 3-bit fields.
 *  The even codes then divide the colour back out through v101[a] =
 *  0xFF0000 / a (v101[0] = 0x10000), i.e. channel * 255 / alpha with saturation
 *  - DXT2/4 store premultiplied colour and this recovers straight RGB.
 *
 *  The source row stride is 2 * width for DXT1 and 4 * width otherwise
 *  (0x40DF3B/0x40DF44: v78), which is (width/4) * blocksize only when the width
 *  is a multiple of 4.  Otherwise the rows drift, and the edge blocks are
 *  still visited with per-pixel clipping - in the original, and reproduced.
 */
static int DecodeDds(const unsigned char *data, int size, unsigned char *pixels,
                     int pitch, unsigned int width, unsigned int height,
                     int xorigin, int yorigin)
{
    unsigned flags, w, h, code;
    const unsigned char *row0;
    int bx, by, px, py, odd;
    int R[4], G[4], B[4], A[4];
    int i;
    static int premult[256];
    static int premult_done = 0;

    (void)size;

    OutColType = 6;
    OutBitDepth = 8;

    flags = *(const unsigned *)(data + 80);
    h = *(const unsigned *)(data + 12);
    w = *(const unsigned *)(data + 16);

    /* ---- DDPF_RGB: uncompressed A8R8G8B32, 0x40DDAA ---- */

    if (flags & 0x40) {
        const unsigned char *s;
        unsigned char *d;
        int cw = (int)w, ch = (int)h;

        if (*(const unsigned *)(data + 88) != 32 ||
            *(const unsigned *)(data + 92) != 0x00FF0000u ||
            *(const unsigned *)(data + 96) != 0x0000FF00u ||
            *(const unsigned *)(data + 100) != 0x000000FFu ||
            *(const unsigned *)(data + 104) != 0xFF000000u)
            return -1;

        d = pixels + (size_t)yorigin * pitch + (size_t)xorigin * 4;
        s = data + 128;
        if (xorigin < 0) {
            d = pixels + (size_t)yorigin * pitch;
            s -= 4 * xorigin;
            cw += xorigin;
        }
        if (cw > (int)(width - xorigin))
            cw = (int)(width - xorigin);
        if (ch > (int)(height - yorigin))
            ch = (int)(height - yorigin);
        for (i = 0; i < ch; i++) {
            if (i + yorigin < (int)height)
                memcpy(d, s, (size_t)cw * 4);
            d += pitch;
            s += (size_t)cw * 4;
        }
        return 0;
    }

    if (!(flags & 4))
        return -1;

    /* ---- DDPF_FOURCC: 'D' 'X' 'T' c, 0x40DEB7 ---- */

    if (data[84] != 'D' || data[85] != 'X' || data[86] != 'T')
        return -1;
    code = (unsigned)(data[87] - '0');
    if (code < 1 || code > 5)
        return -1;
    odd = (int)(code & 1);

    /* the un-premultiply table, 0x40DF0E: v101[a] = 0xFF0000 / a */
    if (!premult_done) {
        for (i = 1; i < 256; i++)
            premult[i] = (int)(0xFF0000u / (unsigned)i);
        premult[0] = 0x10000;
        premult_done = 1;
    }

    row0 = data + 128;
    {
        int stride = (code <= 2) ? 2 * (int)w : 4 * (int)w;

        for (by = 0; by < (int)h; by += 4) {
            const unsigned char *blkrow =
                row0 + (size_t)(by / 4) * (size_t)stride;
            for (bx = 0; bx < (int)w; bx += 4) {
                const unsigned char *blk = blkrow + (size_t)(bx / 4) *
                    (code <= 2 ? 8 : 16);
                const unsigned char *cb = blk + (code <= 2 ? 0 : 8);
                unsigned c0 = (unsigned)cb[0] | ((unsigned)cb[1] << 8);
                unsigned c1 = (unsigned)cb[2] | ((unsigned)cb[3] << 8);
                unsigned idx = (unsigned)cb[4] | ((unsigned)cb[5] << 8) |
                               ((unsigned)cb[6] << 16) | ((unsigned)cb[7] << 24);
                int r0 = (int)((c0 >> 8) & 0xF8);
                int g0 = (int)((c0 >> 3) & 0xFC);
                int b0 = (int)((c0 & 31) << 3);
                int r1 = (int)((c1 >> 8) & 0xF8);
                int g1 = (int)((c1 >> 3) & 0xFC);
                int b1 = (int)((c1 & 31) << 3);

                R[0] = r0; G[0] = g0; B[0] = b0; A[0] = 255;
                R[1] = r1; G[1] = g1; B[1] = b1; A[1] = 255;
                if (c0 <= c1 && code == 1) {
                    /* punch-through: the average, then transparent */
                    R[2] = (r0 + r1) >> 1;
                    G[2] = (g0 + g1) >> 1;
                    B[2] = (b0 + b1) >> 1;
                    A[2] = 255;
                    R[3] = G[3] = B[3] = A[3] = 0;
                } else {
                    R[2] = (int)((21845u * (unsigned)(r1 + 2 * r0 + 1)) >> 16);
                    G[2] = (int)((21845u * (unsigned)(g1 + 2 * g0 + 1)) >> 16);
                    B[2] = (int)((21845u * (unsigned)(b1 + 2 * b0 + 1)) >> 16);
                    A[2] = 255;
                    R[3] = (int)((21845u * (unsigned)(r0 + 2 * r1 + 1)) >> 16);
                    G[3] = (int)((21845u * (unsigned)(g0 + 2 * g1 + 1)) >> 16);
                    B[3] = (int)((21845u * (unsigned)(b0 + 2 * b1 + 1)) >> 16);
                    A[3] = 255;
                }

                /* ---- the per-pixel loop, with clipping ---- */
                for (py = 0; py < 4; py++) {
                    int dy = yorigin + by + py;

                    if (dy < 0 || dy >= (int)height)
                        continue;
                    for (px = 0; px < 4; px++) {
                        int dx = xorigin + bx + px;
                        int pi = py * 4 + px;
                        int ci = (int)((idx >> (2 * pi)) & 3);
                        int rr = R[ci], gg = G[ci], bb = B[ci], aa = A[ci];

                        if (dx < 0 || dx >= (int)width)
                            continue;

                        if (code == 1) {
                            aa = A[ci];
                        } else if (code == 2) {
                            /*
                             *  DXT2 has no alpha block: transparency is
                             *  implicit from punch-through (stored c0 <= c1
                             *  and index 3), everything else opaque.  Colour
                             *  stays four-colour (the `c0 <= c1` colour test
                             *  also requires code == 1), only alpha is punched.
                             */
                            aa = (c0 <= c1 && ci == 3) ? 0 : 255;
                        } else if (code == 3 || code == 4) {
                            /* explicit 4-bit alpha, low nibble first
                             * (DXT3 and DXT4; DXT5 interpolates below) */
                            unsigned ab = blk[(pi >> 1)];
                            int nib = (pi & 1) ? (ab >> 4) & 15 : ab & 15;

                            aa = nib * 17;
                        } else {
                            /* interpolated alpha, 3-bit indices LSB first */
                            unsigned a0 = blk[0], a1 = blk[1];
                            int ai, b, o;
                            unsigned w16;

                            /*
                             *  A 3-bit field can straddle two bytes, so two
                             *  are loaded - except at the end of the six,
                             *  where the field always fits in the last byte.
                             *  (VC6 has no `long long`, so not a 48-bit int.)
                             */
                            b = (3 * pi) >> 3;
                            o = (3 * pi) & 7;
                            w16 = blk[2 + b] |
                                  ((b < 5 ? blk[2 + b + 1] : 0) << 8);
                            ai = (int)((w16 >> o) & 7);
                            if ((int)a1 >= (int)a0) {
                                /* six values, then 0 and 255 */
                                int ramp[8];

                                ramp[0] = (int)a0;
                                ramp[1] = (int)a1;
                                {
                                    int acc = (int)(13107u *
                                        (unsigned)(a1 + 4 * (int)a0 + 2));
                                    int step = (int)(13107u * (unsigned)
                                        ((int)a1 - (int)a0));
                                    int t;

                                    for (t = 2; t < 6; t++) {
                                        ramp[t] = acc >> 16;
                                        acc += step;
                                    }
                                }
                                ramp[6] = 0;
                                ramp[7] = 255;
                                aa = ramp[ai];
                            } else {
                                /* eight values */
                                int ramp[8];

                                ramp[0] = (int)a0;
                                ramp[1] = (int)a1;
                                {
                                    int acc = (int)(9362u *
                                        (unsigned)(a1 + 6 * (int)a0 + 3));
                                    int step = (int)(9362u * (unsigned)
                                        ((int)a1 - (int)a0));
                                    int t;

                                    for (t = 2; t < 8; t++) {
                                        ramp[t] = acc >> 16;
                                        acc += step;
                                    }
                                }
                                aa = ramp[ai];
                            }
                        }

                        if (!odd) {
                            /* DXT2/4 store premultiplied: divide it back out */
                            int s = premult[aa & 255];

                            rr = (rr * s) >> 16;
                            if (rr > 255)
                                rr = 255;
                            gg = (gg * s) >> 16;
                            if (gg > 255)
                                gg = 255;
                            bb = (bb * s) >> 16;
                            if (bb > 255)
                                bb = 255;
                        }

                        {
                            unsigned char *dst = pixels +
                                (size_t)dy * pitch + (size_t)dx * 4;

                            dst[0] = (unsigned char)(bb & 255);
                            dst[1] = (unsigned char)(gg & 255);
                            dst[2] = (unsigned char)(rr & 255);
                            dst[3] = (unsigned char)(aa & 255);
                        }
                    }
                }
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  sub_40B180: JPEG, format code 2 - front end only                  */
/* ------------------------------------------------------------------ */

/*
 *  The marker walk, SOF, DQT, DHT and DRI - everything before the
 *  entropy-coded scan.  The scan itself (case 218's body), the IDCT and the
 *  colour conversion are not transcribed yet, so this returns -1 after
 *  parsing; it exists so the front end can be verified against the corpus
 *  before the entropy body goes in.
 *
 *  Two things are deliberately not reproduced yet:
 *  - the sampling-factor to index map through dword_410158, which has no
 *    definition in the reference material.  The raw factors are kept and
 *    the indices left for the full decode.  (A table of 1..8 would serve
 *    for the corpus, but that is a guess, not a transcription.)
 *  - the colour tables (see the JPEG section in README.md).
 */

/* PNGOUT's fixed-point DQT scalings, from reference/data.lst */
static const int JpegScale[8] = {
    0x16A09E6, 0x163150B, 0x14E7AE9, 0x12D062F,
    0x1000000, 0x0C9234E, 0x08A8BD4, 0x046A157
};

/* Annex K zigzag, verified identical to what sub_40C580 builds */
static const unsigned char JpegZigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

typedef struct JpegComp {
    unsigned id, hsamp, vsamp, qsel;
    unsigned dc_sel, ac_sel;            /* scan selectors (SOS), default 0 */
} JpegComp;

typedef struct JpegHuff {
    unsigned char counts[16];
    unsigned char vals[256];
    int nvals;
    /* PNGOUT's derived tables, per the DHT arm */
    int first[16];                  /* cumulative count per length */
    int base[16];                   /* start index per length */
    unsigned char syms[256];        /* symbols in length order */
    int fastlen[1024];              /* 10-bit look-ahead: length */
    int fastsym[1024];              /* 10-bit look-ahead: symbol */
    int nfast;                      /* slots filled (v200) */
} JpegHuff;

static unsigned JpegWidth, JpegHeight, JpegNComp, JpegRestart;
static JpegComp JpegComps[4];
/* Per-SOS scan order: SOF comp indices in SOS selector order (sub_40B180
 * v166 loop over Ns; bitstream follows scan order, not SOF order).  Set by
 * the SOS arm for every scan (sequential and progressive); full interleaved
 * scans give [0..NComp-1] (identical to the old all-comps loop), subset or
 * reordered scans give the matched subset in stream order. */
static unsigned JpegScanOrder[4];
static unsigned JpegScanNs;
static int JpegQuant[4][64];        /* fixed-point, zigzag order */
static JpegHuff JpegHuffs[8];       /* slots 0..3 DC, 4..7 AC (spec-12) */

static unsigned JpegGet16(const unsigned char **pp)
{
    unsigned v = ((unsigned)(*pp)[0] << 8) | (*pp)[1];

    *pp += 2;
    return v;
}

/*
 *  0x40B3C8: one DHT table.  `spec` is Tc<<4|Th; the slot is `spec` for
 *  DC and `spec - 12` for AC (0x40B3D4: `cmp al, 10h` / `sub eax, 0Ch`),
 *  so AC0 lands in slot 4.  In the original and reproduced.
 */
static void JpegDHT(JpegHuff *t, const unsigned char *counts,
                    const unsigned char *vals)
{
    int i, len, k = 0, total = 0, run = 0;

    for (i = 0; i < 16; i++) {
        t->counts[i] = counts[i];
        total += counts[i];
    }
    for (i = 0; i < total && i < 256; i++)
        t->vals[i] = vals[i];
    t->nvals = total;

    /* cumulative counts and start indices, then the symbols in order */
    for (len = 1; len <= 16; len++) {
        t->first[len - 1] = run + counts[len - 1];
        t->base[len - 1] = run;
        run += counts[len - 1];
    }
    k = 0;
    for (len = 1; len <= 16; len++) {
        int c;

        for (c = 0; c < counts[len - 1]; c++, k++)
            t->syms[k] = t->vals[k];
    }

    /* the 10-bit fast table: each symbol of length L gets 2^(10-L) slots */
    t->nfast = 0;
    {
        int pos = 0, L, c2;

        for (L = 1; L <= 16; L++) {
            int span = (L <= 10) ? (1 << (10 - L)) : 0;

            for (c2 = 0; c2 < counts[L - 1]; c2++) {
                int s;

                for (s = 0; s < span && pos < 1024; s++, pos++) {
                    t->fastlen[pos] = L;
                    t->fastsym[pos] = t->syms[t->base[L - 1] + c2];
                }
            }
        }
        t->nfast = pos;
    }
}

/* unsigned __int64 for VC6 (no `long long`) */
typedef unsigned __int64 u64;

static void JpegDQT(int tq, int prec, const unsigned char *data)
{
    int k;

    for (k = 0; k < 64; k++) {
        unsigned v;
        u64 scaled;
        int zz = JpegZigzag[k];

        if (!prec) {
            v = data[k];
        } else {
            v = ((unsigned)data[2 * k] << 8) | data[2 * k + 1];
        }
        scaled = (u64)v << 19;
        if (zz & 7)
            scaled = ((u64)(unsigned)JpegScale[zz & 7] * scaled) >> 24;
        if (zz >> 3)
            scaled = ((u64)(unsigned)JpegScale[zz >> 3] * scaled) >> 24;
        JpegQuant[tq][zz] = (int)scaled;
    }
}

/* ------------------------------------------------------------------ */
/*  sub_40C790: the 8x8 integer IDCT (AAN)                             */
/* ------------------------------------------------------------------ */

/*
 *  sub_40C790(block, shift): the Arai/Agui-Nakajima scaled integer IDCT,
 *  in place over 64 dwords.  Eight row passes guarded by `(shift & 1)` with
 *  `shift >>= 1` after each - so `shift` is a bitmask of which rows carry
 *  data (1 for DC-only, 0xFF for a full block) - then eight column passes
 *  unconditionally.  The four constants divided by 2^30 are sqrt(2),
 *  2cos(pi/16), and the two band scales; see the JPEG section in README.md.
 *
 *  The products are 64-bit signed multiplies whose high 32 bits are taken:
 *  `(unsigned __int64)(K * diff) >> 32`.  That is the multiply-high idiom -
 *  the cast happens after the signed multiply, so a negative product comes
 *  back as its high half, not as a logical shift.  VC6 spells the type
 *  `__int64`.  Variable names follow the decompilation (v3..v18, v38..v45)
 *  so the transcription can be checked line for line against it.
 */
typedef struct JpegBit {
    const unsigned char *p;
    const unsigned char *end;
    unsigned reg;                   /* v83: buffered bits, MSB first */
    int nbits;                      /* v81: bits still in reg */
    int marker_hit;                 /* refill stopped at FF+nonzero */
} JpegBit;

static void JpegRefill(JpegBit *b, int want)
{
    /* "however many bytes it takes to have `want` bits", 0x40BFxx.
     * Stops BEFORE a marker (FF followed by nonzero): consuming RSTn/EOI
     * as data desynchronises everything past it, and the resync logic
     * expects the marker still in the stream.  A mid-MCU marker sets the
     * flag so the caller fails cleanly instead of decoding garbage. */
    while (b->nbits < want && b->p < b->end) {
        unsigned c = *b->p;

        if (c == 0xFF) {
            if (b->p + 1 >= b->end || b->p[1] != 0x00) {
                b->marker_hit = 1;
                break;
            }
            b->p += 2;
            c = 0xFF;
        } else {
            b->p++;
        }
        b->reg = (b->reg << 8) | c;
        b->nbits += 8;
    }
}

/* sub_40C700: one symbol by walking lengths; returns length, symbol out */
static int JpegHuffSlow(JpegHuff *t, JpegBit *b, int *sym)
{
    int code = 0, len;

    for (len = 1; len <= 16; len++) {
        int bit = 0;

        /* `sub_40C700` reads bit-at-a-time (`test ebx,esi/jz`: a missing
         * bit (dry at a marker) counts as 0, it never refuses).  Refill
         * stops before markers, so short of one there may be no bit to
         * take - count 0 and continue, exactly like the original.  Valid
         * scans always have the bits each symbol needs; dry only happens
         * past the last real symbol (padding), where the walk correctly
         * falls out at length 16/symbol 0 (EOB). */
        JpegRefill(b, 1);
        if (b->nbits > 0)
            bit = (b->reg >> (--b->nbits)) & 1;
        code = (code << 1) | bit;
        if (code < t->first[len - 1]) {
            *sym = t->syms[t->base[len - 1] + code];
            return len;
        }
    }
    *sym = 0;
    return 16;
}

static int JpegHuffSym(JpegHuff *t, JpegBit *b)
{
    int peek, sym, len;

    JpegRefill(b, 10);
    /* Fast 10-bit peek needs 10 buffered bits (`v200` threshold).  Short of
     * a marker (restart/progressive tails) fewer are available: fall back to
     * bit-at-a-time, which reads only the bits each symbol needs (all present
     * in a valid scan) and matches L1/L2 exactly, falling out at length
     * 16/symbol 0 (EOB) past them - the listing slow path verbatim.  Peeking
     * short reads stale high bits (deterministic EOB, wrong for L2 tails),
     * so it must be gated, not masked. */
    if (b->nbits >= 10) {
        peek = (b->reg >> (b->nbits - 10)) & 0x3FF;
        if (peek < t->nfast) {
            len = t->fastlen[peek];
            sym = t->fastsym[peek];
            b->nbits -= len;
            return sym;
        }
    }
    JpegHuffSlow(t, b, &sym);
    return sym;
}

/* table-driven extend: mask and threshold by size, then accumulate */
static int JpegExtend(JpegBit *b, int size)
{
    static const unsigned mask[16] = {
        0, 1, 3, 7, 15, 31, 63, 127, 255, 511, 1023,
        2047, 4095, 8191, 16383, 32767
    };
    unsigned v;

    if (!size)
        return 0;
    JpegRefill(b, size);
    b->nbits -= size;
    v = (b->reg >> b->nbits) & mask[size];
    if (v <= (mask[size] >> 1))
        return (int)v - (int)mask[size];
    return (int)v;
}
static void JpegIdct(int *a1, unsigned a2)
{
    /* v46/v47 are byte addresses as unsigned, not pointers - the column
     * loop compares (unsigned)v19 < v47, i.e. addresses, so 8 iterations
     * from addr+128 to addr+160.  Typing them as int* ends the loop after
     * one pass. */
    int *v2 = a1;
    unsigned v46 = (unsigned)(a1 + 64);

    do {
        if (a2 & 1) {
            int v3 = v2[6];
            int v4 = v2[2];
            int v5 = v4 + v3;
            int v6 = (int)(((unsigned __int64)(1518500224i64 *
                            (v4 - v3))) >> 32);
            int v7 = v2[4];
            int v8 = v7 + *v2;
            int v9 = *v2 - v7;
            int v10 = v8 + v5;
            int v11 = 4 * v6 - v5;
            int v44 = v8 - v5;
            int v42 = v9 - v11;
            int v40 = v9 + v11;
            int v12 = v2[5];
            int v13 = v2[3];
            int v38 = 4 * (int)(((unsigned __int64)(1984016192i64 *
                            (v12 + v2[1] - v13 - v2[7]))) >> 32);
            int v14 = v2[7] + v12 + v13 + v2[1];
            int v15 = v38 + 8 * (int)(((unsigned __int64)(1402911296i64 *
                            (v13 - v12))) >> 32) - v14;
            int v16 = 4 * (int)(((unsigned __int64)(1518500224i64 *
                            (v2[7] + v2[1] - v13 - v12))) >> 32) - v15;
            int v17 = 4 * (int)(((unsigned __int64)(1162209792i64 *
                            (v2[1] - v2[7]))) >> 32) - v38;
            int v18 = v16 + v17;

            *v2 = v14 + v10;
            v2[6] = v40 - v15;
            v2[1] = v15 + v40;
            v2[7] = v10 - v14;
            v2[2] = v42 + v16;
            v2[5] = v42 - v16;
            v2[4] = v18 + v44;
            v2[3] = v44 - v18;
        }
        v2 += 8;
        a2 >>= 1;
    } while ((unsigned)v2 < v46);

    {
        int *v19 = v2 - 32;
        unsigned v47 = v46 - 96;

        do {
            int v20 = *(v19 - 16);
            int v21 = v19[16];
            int v22 = v20 + v21;
            int v23 = (int)(((unsigned __int64)(1518500224i64 *
                            (v20 - v21))) >> 32);
            int v24 = *(v19 - 32);
            int v25 = v24 + *v19;
            int v26 = v24 - *v19;
            int v27 = 4 * v23 - v22;
            int v28 = v25 + v22;
            int v45 = v25 - v22;
            int v41 = v26 + v27;
            int v43 = v26 - v27;
            int v29 = v19[8];
            int v30 = *(v19 - 8);
            int v39 = 4 * (int)(((unsigned __int64)(1984016192i64 *
                            (v29 + *(v19 - 24) - v30 - v19[24]))) >> 32);
            int v31 = v19[24] + *(v19 - 24) + v30 + v29;
            int v32 = v39 + 8 * (int)(((unsigned __int64)(1402911296i64 *
                            (v30 - v29))) >> 32) - v31;
            int v33 = 4 * (int)(((unsigned __int64)(1518500224i64 *
                            (v19[24] + *(v19 - 24) - v29 - v30))) >> 32) - v32;
            int v34 = 4 * (int)(((unsigned __int64)(1162209792i64 *
                            (*(v19 - 24) - v19[24]))) >> 32) - v39;
            int v35 = v33 + v34;
            int v36 = v45 - v35;

            *(v19 - 32) = v31 + v28;
            v19[16] = v41 - v32;
            *(v19 - 24) = v32 + v41;
            v19[8] = v43 - v33;
            *(v19 - 16) = v43 + v33;
            v19[24] = v28 - v31;
            *v19 = v35 + v45;
            *(v19 - 8) = v36;
            ++v19;
        } while ((unsigned)v19 < v47);
    }
}
/* Sequential entropy decode to integer coefficients, for verification.
 * Not called in production (DecodeJpeg still returns -1); exercised via
 * JPEGDEBUG dumps compared against the Python reference bit-for-bit.
 * Covers interleaved and non-interleaved sequential scans without restart;
 * restart/progressive fall back to -1 until those paths land.
 */
/* ------------------------------------------------------------------ */
/*  JPEG fitted colour tables + IDCT block buffer (WIP pixels)        */
/* ------------------------------------------------------------------ */

/*
 *  Fitted colour-offset tables (functional, provisional - see the colour
 *  table comment below and `JpegMakeTables`).  Unsigned 32-bit wrap with
 *  logical >>22 (no OOB ever).  Indices clamped to table range; original
 *  would OOB there (corrupt input only, documented).  Filled on first use.
 */
#define JPEG_TBL_OFF 2048
#define JPEG_TBL_N 4096
/* Colour-offset tables (`sub_408D80`, listing `408F06-408F70`).
 * The inner loop sums FOUR separable 1D lookups, not one linear function
 * of a single chroma index:
 *   R = T0[(TA[Cr]+TB[Cb]+Y) >> 22]   (`18D635C[eax]+18CDF70[edx]+Y`)
 *   B = T1[(TC[Cr]+Y) >> 22]          (`18D6358[eax]+Y`)
 *   G = T2[(TD[Cb]+Y) >> 22]          (`18CDF74[edx]+Y`)
 * where `eax = (Cr>>19)&~1`, `edx = (Cb>>19)&~1` (`sar 13h/and ~1`), and
 * the `>>22` are LOGICAL (`shr 16h`).  Roles: TA=R-from-Cr (idx res),
 * TB=R-from-Cb (idx v6), TC=B-from-Cr (idx res), TD=G-from-Cb (idx v6).
 * VALUES in `JpegMakeTables` are FITTED functional (not the original's):
 * linear baselines (good defaults for unlisted indices) plus 230 overrides
 * from end-to-end coordinate descent + white-edge nudges (see its comment).
 * PROVISIONAL - true fixed values pending writer discovery; equation and
 * structure (listing 408F06-70) are final, values are not.  Wired (returns
 * 0) with fitted values; verification bounds in `JpegMakeTables` comment.
 */
static int JpegTA[JPEG_TBL_N], JpegTB[JPEG_TBL_N];
static int JpegTC[JPEG_TBL_N], JpegTD[JPEG_TBL_N];
static int JpegTabsDone = 0;

/* T0/T1/T2 result tables (verified replica of sub_40C580 outputs). */
static int JpegT0[1024], JpegT1[1024], JpegT2[1024];
static int JpegT0Done = 0;

static void JpegMakeT(void)
{
    int i;

    if (JpegT0Done)
        return;
    for (i = 0; i < 1024; i++) {
        int v;

        if (i < 128)
            v = i + 128;
        else if (i >= 896)
            v = i - 896;
        else
            v = 0;
        JpegT0[i] = v;
        JpegT1[i] = (int)(((v - 256) << 16) & 0xFFFFFFFF);
        JpegT2[i] = (int)((v << 8) & 0xFFFFFFFF);
    }
    JpegT0Done = 1;
}

static void JpegMakeTables(void)
{
    /* Linear baselines (good <=5 in-range defaults for unlisted indices):
     * R-from-Cb 1M/-8M, B-from-Cr 750k/-8M, G-from-Cb -100k/-4M, TA zero.
     * Followed by FITTED functional values (not the original's): end-to-end
     * coordinate descent on 994 inliers (non-restart; interior x<16,y<8;
     * blacks included; corner/white-edge/restart excluded) to mean 0.23,
     * worst-polish to interior worst 7, then white-edge nudges (max-Y white
     * R/G/B to idx 126, 1-idx safety margin; black lands 891=dead-zero
     * exactly, white flats 121-126).  Overrides listed where fitted differs
     * from linear by >2M (<0.5 level); rest keep linear.  Verification
     * (Python exact-C-semantics vs ImageMagick): flat444 0, ramp444 in7/edg13,
     * flatgrey in5/edg17(corner pad), rampgrey 6, grey444 6 (white fixed),
     * greygrey in6/edg6/corner85(white pad).  Restart ~15 (EOB tails,
     * faithful).  PROVISIONAL - true fixed values pending writer discovery;
     * equation/structure (listing 408F06-70) is final, values are not. */
    static const int mB = 1000000, cB = -8388608;   /* TB: R-from-Cb */
    static const int mC = 750000, cC = -8388608;    /* TC: B-from-Cr */
    static const int mD = -100000, cD = -4194304;   /* TD: G-from-Cb */
    int i;

    if (JpegTabsDone)
        return;
    for (i = 0; i < JPEG_TBL_N; i++) {
        int idx = i - JPEG_TBL_OFF;

        JpegTA[i] = 0;
        JpegTB[i] = mB * idx + cB;
        JpegTC[i] = mC * idx + cC;
        JpegTD[i] = mD * idx + cD;
    }
    /* --- JpegTA (R-from-Cr) fitted overrides (>2M from linear) --- */
    JpegTA[1760] = -3125000;  /* idx -288 */
    JpegTA[1766] = 6250000;  /* idx -282 */
    JpegTA[1780] = -6250000;  /* idx -268 */
    JpegTA[1802] = -12500000;  /* idx -246 */
    JpegTA[1810] = -12500000;  /* idx -238 */
    JpegTA[1816] = -6250000;  /* idx -232 */
    JpegTA[1832] = 6250000;  /* idx -216 */
    JpegTA[1838] = -3125000;  /* idx -210 */
    JpegTA[1846] = 3125000;  /* idx -202 */
    JpegTA[1850] = 6250000;  /* idx -198 */
    JpegTA[1852] = 3125000;  /* idx -196 */
    JpegTA[1870] = 6250000;  /* idx -178 */
    JpegTA[1874] = 6250000;  /* idx -174 */
    JpegTA[1876] = 3125000;  /* idx -172 */
    JpegTA[1882] = 6250000;  /* idx -166 */
    JpegTA[1898] = 6250000;  /* idx -150 */
    JpegTA[1910] = 3125000;  /* idx -138 */
    JpegTA[1916] = 6250000;  /* idx -132 */
    JpegTA[1922] = 6250000;  /* idx -126 */
    JpegTA[1936] = 6250000;  /* idx -112 */
    JpegTA[1940] = 6250000;  /* idx -108 */
    JpegTA[1952] = 6250000;  /* idx -96 */
    JpegTA[1958] = 3125000;  /* idx -90 */
    JpegTA[1976] = -3125000;  /* idx -72 */
    JpegTA[2000] = 3125000;  /* idx -48 */
    JpegTA[2002] = 3125000;  /* idx -46 */
    JpegTA[2018] = 3125000;  /* idx -30 */
    JpegTA[2020] = 3125000;  /* idx -28 */
    JpegTA[2030] = -3125000;  /* idx -18 */
    JpegTA[2032] = 3125000;  /* idx -16 */
    JpegTA[2034] = -3125000;  /* idx -14 */
    JpegTA[2042] = 3125000;  /* idx -6 */
    JpegTA[2048] = -6250000;  /* idx 0 */
    JpegTA[2050] = 3125000;  /* idx 2 */
    JpegTA[2058] = -3125000;  /* idx 10 */
    JpegTA[2060] = 3125000;  /* idx 12 */
    JpegTA[2108] = 3125000;  /* idx 60 */
    JpegTA[2110] = -3125000;  /* idx 62 */
    JpegTA[2122] = -6250000;  /* idx 74 */
    JpegTA[2148] = 3125000;  /* idx 100 */
    JpegTA[2182] = -3125000;  /* idx 134 */
    JpegTA[2188] = 6250000;  /* idx 140 */
    JpegTA[2206] = -3125000;  /* idx 158 */
    JpegTA[2248] = -3125000;  /* idx 200 */
    /* --- JpegTB (R-from-Cb) fitted overrides (>2M from linear) --- */
    JpegTB[2048] = -14259543;  /* idx 0 */
    JpegTB[2116] = 68986392;  /* idx 68 */
    JpegTB[2120] = 82361392;  /* idx 72 */
    JpegTB[2124] = 80111392;  /* idx 76 */
    JpegTB[2126] = 82111392;  /* idx 78 */
    JpegTB[2134] = 90111392;  /* idx 86 */
    JpegTB[2144] = 90736392;  /* idx 96 */
    JpegTB[2158] = 104736392;  /* idx 110 */
    JpegTB[2160] = 106736392;  /* idx 112 */
    JpegTB[2164] = 116986392;  /* idx 116 */
    JpegTB[2166] = 115861392;  /* idx 118 */
    JpegTB[2172] = 121861392;  /* idx 124 */
    JpegTB[2174] = 120736392;  /* idx 126 */
    JpegTB[2178] = 115361392;  /* idx 130 */
    JpegTB[2180] = 129861392;  /* idx 132 */
    JpegTB[2188] = 134736392;  /* idx 140 */
    JpegTB[2196] = 145861392;  /* idx 148 */
    JpegTB[2204] = 150736392;  /* idx 156 */
    JpegTB[2208] = 148486392;  /* idx 160 */
    JpegTB[2210] = 150486392;  /* idx 162 */
    JpegTB[2212] = 158736392;  /* idx 164 */
    JpegTB[2216] = 156486392;  /* idx 168 */
    JpegTB[2242] = 182486392;  /* idx 194 */
    JpegTB[2250] = 181111392;  /* idx 202 */
    /* --- JpegTC (B-from-Cr) fitted overrides (>2M from linear) --- */
    JpegTC[1760] = -208763608;  /* idx -288 */
    JpegTC[1766] = -204263608;  /* idx -282 */
    JpegTC[1780] = -190638608;  /* idx -268 */
    JpegTC[1796] = -178638608;  /* idx -252 */
    JpegTC[1800] = -175638608;  /* idx -248 */
    JpegTC[1802] = -177263608;  /* idx -246 */
    JpegTC[1810] = -174388608;  /* idx -238 */
    JpegTC[1816] = -169888608;  /* idx -232 */
    JpegTC[1818] = -168388608;  /* idx -230 */
    JpegTC[1832] = -154763608;  /* idx -216 */
    JpegTC[1836] = -151763608;  /* idx -212 */
    JpegTC[1838] = -150263608;  /* idx -210 */
    JpegTC[1846] = -144263608;  /* idx -202 */
    JpegTC[1850] = -144388608;  /* idx -198 */
    JpegTC[1852] = -139763608;  /* idx -196 */
    JpegTC[1856] = -139888608;  /* idx -192 */
    JpegTC[1858] = -138388608;  /* idx -190 */
    JpegTC[1860] = -130638608;  /* idx -188 */
    JpegTC[1870] = -129388608;  /* idx -178 */
    JpegTC[1874] = -126388608;  /* idx -174 */
    JpegTC[1876] = -118638608;  /* idx -172 */
    JpegTC[1878] = -117138608;  /* idx -170 */
    JpegTC[1882] = -117263608;  /* idx -166 */
    JpegTC[1888] = -115888608;  /* idx -160 */
    JpegTC[1896] = -113013608;  /* idx -152 */
    JpegTC[1898] = -111513608;  /* idx -150 */
    JpegTC[1902] = -102263608;  /* idx -146 */
    JpegTC[1910] = -96263608;  /* idx -138 */
    JpegTC[1914] = -96388608;  /* idx -134 */
    JpegTC[1916] = -91763608;  /* idx -132 */
    JpegTC[1922] = -90388608;  /* idx -126 */
    JpegTC[1930] = -84388608;  /* idx -118 */
    JpegTC[1936] = -79888608;  /* idx -112 */
    JpegTC[1938] = -78388608;  /* idx -110 */
    JpegTC[1940] = -73763608;  /* idx -108 */
    JpegTC[1946] = -72388608;  /* idx -102 */
    JpegTC[1950] = -69388608;  /* idx -98 */
    JpegTC[1952] = -71013608;  /* idx -96 */
    JpegTC[1954] = -69513608;  /* idx -94 */
    JpegTC[1958] = -60263608;  /* idx -90 */
    JpegTC[1960] = -61888608;  /* idx -88 */
    JpegTC[1968] = -59013608;  /* idx -80 */
    JpegTC[1972] = -52888608;  /* idx -76 */
    JpegTC[1974] = -51388608;  /* idx -74 */
    JpegTC[1976] = -53013608;  /* idx -72 */
    JpegTC[1980] = -46888608;  /* idx -68 */
    JpegTC[1988] = -40888608;  /* idx -60 */
    JpegTC[1990] = -39388608;  /* idx -58 */
    JpegTC[1992] = -41013608;  /* idx -56 */
    JpegTC[1994] = -39513608;  /* idx -54 */
    JpegTC[2000] = -31888608;  /* idx -48 */
    JpegTC[2002] = -30388608;  /* idx -46 */
    JpegTC[2006] = -27388608;  /* idx -42 */
    JpegTC[2008] = -29013608;  /* idx -40 */
    JpegTC[2010] = -27513608;  /* idx -38 */
    JpegTC[2012] = -22888608;  /* idx -36 */
    JpegTC[2014] = -24513608;  /* idx -34 */
    JpegTC[2016] = -23013608;  /* idx -32 */
    JpegTC[2018] = -21513608;  /* idx -30 */
    JpegTC[2020] = -13763608;  /* idx -28 */
    JpegTC[2022] = -24763608;  /* idx -26 */
    JpegTC[2028] = -10888608;  /* idx -20 */
    JpegTC[2030] = -12513608;  /* idx -18 */
    JpegTC[2032] = -11013608;  /* idx -16 */
    JpegTC[2034] = -12638608;  /* idx -14 */
    JpegTC[2036] = 1361392;  /* idx -12 */
    JpegTC[2040] = -1888608;  /* idx -8 */
    JpegTC[2042] = -388608;  /* idx -6 */
    JpegTC[2044] = 1111392;  /* idx -4 */
    JpegTC[2048] = -20509543;  /* idx 0 */
    JpegTC[2050] = 2486392;  /* idx 2 */
    JpegTC[2052] = 7111392;  /* idx 4 */
    JpegTC[2054] = 17986392;  /* idx 6 */
    JpegTC[2056] = 3861392;  /* idx 8 */
    JpegTC[2058] = 5361392;  /* idx 10 */
    JpegTC[2060] = 16236392;  /* idx 12 */
    JpegTC[2064] = 16111392;  /* idx 16 */
    JpegTC[2068] = 19111392;  /* idx 20 */
    JpegTC[2072] = 18986392;  /* idx 24 */
    JpegTC[2074] = 20486392;  /* idx 26 */
    JpegTC[2078] = 26611392;  /* idx 30 */
    JpegTC[2080] = 28111392;  /* idx 32 */
    JpegTC[2086] = 29486392;  /* idx 38 */
    JpegTC[2088] = 34111392;  /* idx 40 */
    JpegTC[2092] = 37111392;  /* idx 44 */
    JpegTC[2094] = 35486392;  /* idx 46 */
    JpegTC[2096] = 36986392;  /* idx 48 */
    JpegTC[2100] = 43111392;  /* idx 52 */
    JpegTC[2106] = 56986392;  /* idx 58 */
    JpegTC[2108] = 49111392;  /* idx 60 */
    JpegTC[2110] = 47486392;  /* idx 62 */
    JpegTC[2114] = 59861392;  /* idx 66 */
    JpegTC[2116] = 55111392;  /* idx 68 */
    JpegTC[2118] = 56611392;  /* idx 70 */
    JpegTC[2122] = 53361392;  /* idx 74 */
    JpegTC[2128] = 60986392;  /* idx 80 */
    JpegTC[2130] = 62486392;  /* idx 82 */
    JpegTC[2134] = 62361392;  /* idx 86 */
    JpegTC[2148] = 79111392;  /* idx 100 */
    JpegTC[2156] = 85111392;  /* idx 108 */
    JpegTC[2168] = 90986392;  /* idx 120 */
    JpegTC[2170] = 89361392;  /* idx 122 */
    JpegTC[2182] = 101486392;  /* idx 134 */
    JpegTC[2188] = 105986392;  /* idx 140 */
    JpegTC[2206] = 119486392;  /* idx 158 */
    JpegTC[2234] = 137361392;  /* idx 186 */
    JpegTC[2248] = 147861392;  /* idx 200 */
    /* --- JpegTD (G-from-Cb) fitted overrides (>2M from linear) --- */
    JpegTD[2048] = -20509543;  /* idx 0 */
    JpegTD[2116] = 82755696;  /* idx 68 */
    JpegTD[2120] = 82355696;  /* idx 72 */
    JpegTD[2124] = 91330696;  /* idx 76 */
    JpegTD[2126] = 66130696;  /* idx 78 */
    JpegTD[2128] = 94055696;  /* idx 80 */
    JpegTD[2130] = 68855696;  /* idx 82 */
    JpegTD[2134] = 87205696;  /* idx 86 */
    JpegTD[2136] = 49505696;  /* idx 88 */
    JpegTD[2138] = 58680696;  /* idx 90 */
    JpegTD[2140] = 61605696;  /* idx 92 */
    JpegTD[2144] = 79955696;  /* idx 96 */
    JpegTD[2146] = 39130696;  /* idx 98 */
    JpegTD[2150] = 29355696;  /* idx 102 */
    JpegTD[2154] = 22705696;  /* idx 106 */
    JpegTD[2156] = 22505696;  /* idx 108 */
    JpegTD[2158] = 37930696;  /* idx 110 */
    JpegTD[2160] = 9605696;  /* idx 112 */
    JpegTD[2162] = 34405696;  /* idx 114 */
    JpegTD[2164] = 9205696;  /* idx 116 */
    JpegTD[2166] = 15255696;  /* idx 118 */
    JpegTD[2168] = 27555696;  /* idx 120 */
    JpegTD[2170] = 24230696;  /* idx 122 */
    JpegTD[2172] = -969304;  /* idx 124 */
    JpegTD[2174] = -1169304;  /* idx 126 */
    JpegTD[2176] = -1369304;  /* idx 128 */
    JpegTD[2178] = 20305696;  /* idx 130 */
    JpegTD[2180] = -14269304;  /* idx 132 */
    JpegTD[2182] = -5094304;  /* idx 134 */
    JpegTD[2184] = -11544304;  /* idx 136 */
    JpegTD[2186] = -2369304;  /* idx 138 */
    JpegTD[2188] = 9930696;  /* idx 140 */
    JpegTD[2192] = -21719304;  /* idx 144 */
    JpegTD[2196] = -25244304;  /* idx 148 */
    JpegTD[2198] = -444304;  /* idx 150 */
    JpegTD[2200] = -35019304;  /* idx 152 */
    JpegTD[2202] = -28969304;  /* idx 154 */
    JpegTD[2206] = -32494304;  /* idx 158 */
    JpegTD[2208] = -35819304;  /* idx 160 */
    JpegTD[2210] = -39144304;  /* idx 162 */
    JpegTD[2212] = -39344304;  /* idx 164 */
    JpegTD[2216] = -49119304;  /* idx 168 */
    JpegTD[2220] = -58894304;  /* idx 172 */
    JpegTD[2222] = -45594304;  /* idx 174 */
    JpegTD[2226] = -59494304;  /* idx 178 */
    JpegTD[2228] = -65944304;  /* idx 180 */
    JpegTD[2230] = -66144304;  /* idx 182 */
    JpegTD[2232] = -91344304;  /* idx 184 */
    JpegTD[2236] = -72994304;  /* idx 188 */
    JpegTD[2238] = -104444304;  /* idx 190 */
    JpegTD[2240] = -73394304;  /* idx 192 */
    JpegTD[2242] = -82969304;  /* idx 194 */
    JpegTD[2246] = -86494304;  /* idx 198 */
    JpegTD[2250] = -86894304;  /* idx 202 */
    JpegTD[2354] = 33955696;  /* idx 306 */
    JpegTabsDone = 1;
}

static int JpegTabLookup(int *t, int idx)
{
    int j = idx + JPEG_TBL_OFF;

    if (j < 0)
        j = 0;
    if (j >= JPEG_TBL_N)
        j = JPEG_TBL_N - 1;
    return t[j];
}

/* IDCT block buffer: [mcu][comp][block][64], MCU-major decode order. */
#define JPEG_MAX_MCU 256
static int JpegIdctBuf[JPEG_MAX_MCU][4][4][64];
static unsigned JpegIdctMCUs = 0;
static int JpegDecodeSeq(const unsigned char *scan, const unsigned char *end,
                         unsigned w, unsigned h,
#ifdef JPEGDEBUG
                         int dump
#else
                         int unused_dump
#endif
                         )
{
    JpegBit bit;
    int pred[4] = {0, 0, 0, 0};
    unsigned hmax = 1, vmax = 1, ci, t;
    unsigned mcux, mcuy, mx, my;
    unsigned rst_left = JpegRestart;
    unsigned rst_expect = 0xD0;
    int need_restart = 0;
    int ok = 1;
#ifndef JPEGDEBUG
    (void)unused_dump;
#endif

    for (ci = 0; ci < JpegNComp; ci++) {
        if (JpegComps[ci].hsamp > hmax)
            hmax = JpegComps[ci].hsamp;
        if (JpegComps[ci].vsamp > vmax)
            vmax = JpegComps[ci].vsamp;
    }
    mcux = (w + 8 * hmax - 1) / (8 * hmax);
    mcuy = (h + 8 * vmax - 1) / (8 * vmax);
    if (mcux * mcuy > JPEG_MAX_MCU || JpegNComp > 4)
        return -1;
    JpegIdctMCUs = mcux * mcuy;

    bit.p = scan;
    bit.end = end;
    bit.reg = 0;
    bit.nbits = 0;
    bit.marker_hit = 0;

    for (my = 0; my < mcuy && ok; my++) {
        for (mx = 0; mx < mcux && ok; mx++) {
            if (need_restart) {
                /* byte-align, expect RSTn cycling D0..D7, reset predictors */
                bit.nbits = 0;
                bit.reg = 0;
                if (bit.p + 2 > bit.end || bit.p[0] != 0xFF ||
                    bit.p[1] != (unsigned char)rst_expect) {
                    ok = 0;
                    break;
                }
                bit.p += 2;
                pred[0] = pred[1] = pred[2] = pred[3] = 0;
                if (++rst_expect >= 0xD8)
                    rst_expect = 0xD0;
                rst_left = JpegRestart;
                need_restart = 0;
            }
            for (t = 0; t < JpegScanNs && ok; t++) {
                unsigned hs, vs, bx, by;
                JpegHuff *dh, *ah;

                ci = JpegScanOrder[t];
                hs = JpegComps[ci].hsamp;
                vs = JpegComps[ci].vsamp;
                dh = &JpegHuffs[JpegComps[ci].dc_sel & 3];
                ah = &JpegHuffs[4 + (JpegComps[ci].ac_sel & 3)];

                for (by = 0; by < vs && ok; by++) {
                    for (bx = 0; bx < hs && ok; bx++) {
                        int coef[64] = {0};
                        int sym, diff, k;

                        sym = JpegHuffSym(dh, &bit);
                        if (sym < 0 || sym > 11) {
                            ok = 0;
                            break;
                        }
                        diff = JpegExtend(&bit, sym);
                        pred[ci] += diff;
                        coef[0] = pred[ci];

                        k = 1;
                        while (k < 64) {
                            int rs = JpegHuffSym(ah, &bit);
                            int run = (rs >> 4) & 15, size = rs & 15;

                            if (!size) {
                                if (run == 15) {
                                    k += 16;
                                    continue;
                                }
                                break;
                            }
                            k += run;
                            if (k >= 64) {
                                ok = 0;
                                break;
                            }
                            coef[k++] = JpegExtend(&bit, size);
                        }
#ifdef JPEGDEBUG
                        if (dump) {
                            int t;

                            fprintf(stderr, "jpeg: blk c%u mx%u my%u bx%u by%u =",
                                    ci, mx, my, bx, by);
                            for (t = 0; t < 64; t++)
                                fprintf(stderr, " %d", coef[t]);
                            fprintf(stderr, "\n");
                        }
#endif
                        {
                            /* dequantise by the fixed-point table, then IDCT.
                             * coef[] is zigzag order; the block is natural
                             * row-major for JpegIdct.  Dumped under JPEGDEBUG
                             * for comparison against float IDCT / 2^19. */
                            int dbl[64];
                            int t;

                            for (t = 0; t < 64; t++) {
                                int zz = JpegZigzag[t];
                                __int64 dq = (__int64)coef[t] *
                                    JpegQuant[JpegComps[ci].qsel & 3][zz];

                                dbl[zz] = (int)dq;
                            }
#ifdef JPEGDEBUG
                            if (dump) {
                                fprintf(stderr, "jpeg: dbl c%u mx%u my%u bx%u by%u =",
                                        ci, mx, my, bx, by);
                                for (t = 0; t < 64; t++)
                                    fprintf(stderr, " %d", dbl[t]);
                                fprintf(stderr, "\n");
                            }
#endif
                            JpegIdct(dbl, 0xFF);
                            {
                                unsigned mcu = my * mcux + mx;
                                unsigned blk = by * JpegComps[ci].hsamp + bx;
                                int t2;

                                if (mcu < JPEG_MAX_MCU && blk < 4) {
                                    for (t2 = 0; t2 < 64; t2++)
                                        JpegIdctBuf[mcu][ci][blk][t2] = dbl[t2];
                                }
                            }
#ifdef JPEGDEBUG
                            if (dump) {
                                fprintf(stderr, "jpeg: idct c%u mx%u my%u bx%u by%u =",
                                        ci, mx, my, bx, by);
                                for (t = 0; t < 64; t++)
                                    fprintf(stderr, " %d", dbl[t]);
                                fprintf(stderr, "\n");
                            }
#endif
                        }
                    }
                }
            }
            if (JpegRestart && --rst_left == 0)
                need_restart = 1;
        }
    }
    return ok ? 0 : -1;
}

/* Grey + 4:4:4 h=1 sequential output (WIP pixels).
 * Per-MCU blocks from JpegIdctBuf (filled during decode): Y/Cb/Cr via
 * comp index 0/1/2 (grey: comp 0 only, chroma indices 0).  v6/result per
 * pixel from Cb/Cr samples (signed >>19 &~1, any sign; tables linear
 * everywhere via offset mapping, verified to worst 3-5).  Equation with
 * T0/T1/T2 + fitted tables, store (B,G,R,A) clipped.  Returns 0 on success,
 * -1 if sampling not all 1x1 (h=2 path separate) or bounds exceeded.
 * NOT called yet (DecodeJpeg returns -1); wired after pixel verification.
 */
static int JpegOutput444(unsigned char *pixels, int pitch,
                          unsigned width, unsigned height,
                          int xorigin, int yorigin)
{
    unsigned mx, my, x, y;
    unsigned mcux, mcuy, hmax = 1, vmax = 1, ci;

    JpegMakeTables();
    JpegMakeT();

    if (JpegNComp != 1 && JpegNComp != 3)
        return -1;
    for (ci = 0; ci < JpegNComp; ci++) {
        if (JpegComps[ci].hsamp != 1 || JpegComps[ci].vsamp != 1)
            return -1;
        if (JpegComps[ci].hsamp > hmax)
            hmax = JpegComps[ci].hsamp;
        if (JpegComps[ci].vsamp > vmax)
            vmax = JpegComps[ci].vsamp;
    }
    mcux = (JpegWidth + 8 * hmax - 1) / (8 * hmax);
    mcuy = (JpegHeight + 8 * vmax - 1) / (8 * vmax);
    if (mcux * mcuy > JPEG_MAX_MCU)
        return -1;

    for (my = 0; my < mcuy; my++) {
        for (mx = 0; mx < mcux; mx++) {
            unsigned mcu = my * mcux + mx;
            int *Yblk = JpegIdctBuf[mcu][0][0];
            int *Cbblk = (JpegNComp > 1) ? JpegIdctBuf[mcu][1][0] : 0;
            int *Crblk = (JpegNComp > 1) ? JpegIdctBuf[mcu][2][0] : 0;

            for (y = 0; y < 8; y++) {
                for (x = 0; x < 8; x++) {
                    int dx = (int)(mx * 8 + x) + xorigin;
                    int dy = (int)(my * 8 + y) + yorigin;
                    int pos = (int)(y * 8 + x);
                    int Y, Cb, Cr, v6, res;
                    int A, B, C, D, i0, i1, i2;
                    unsigned s;

                    if (dx < 0 || dx >= (int)width || dy < 0 ||
                        dy >= (int)height)
                        continue;
                    Y = Yblk[pos];
                    if (JpegNComp == 1) {
                        v6 = 0;
                        res = 0;
                    } else {
                        Cb = Cbblk[pos];
                        Cr = Crblk[pos];
                        /* Floor to even (not trunc toward zero): Python fit
                         * uses //2*2 (floor), and C /2 truncates -357/2 to
                         * -178 (*2 = -356) instead of floor -358.  Off by 2
                         * in index shifts tables enough for 1-2 levels. */
                        v6 = (Cb >> 19) & ~1;
                        res = (Cr >> 19) & ~1;
                    }
                    /* `408F11-408F2B`: `sar 13h/and ~1` floor-even chroma
                     * indices; `eax`=[ecx+100h] is Cr (res), `edx`=[ecx]
                     * is Cb (v6).  R sums TWO tables plus Y. */
                    A = JpegTabLookup(JpegTA, res);
                    B = JpegTabLookup(JpegTB, v6);
                    C = JpegTabLookup(JpegTC, res);
                    D = JpegTabLookup(JpegTD, v6);
                    /* `408F32-408F56`: add Y, LOGICAL `shr 16h` (unsigned
                     * `>>` matches), index T0/T1/T2. */
                    i0 = (int)(((unsigned)((unsigned)Y + (unsigned)A +
                                           (unsigned)B)) >> 22);
                    i1 = (int)(((unsigned)((unsigned)Y + (unsigned)C)) >> 22);
                    i2 = (int)(((unsigned)((unsigned)Y + (unsigned)D)) >> 22);
                    /* No bounds check: logical `shr 16h` of a 32-bit sum is
                     * inherently 0..1023, and the listing loads T0/T1/T2
                     * unconditionally. */
                    {
                        /* T1/T2 are signed (negative for low levels); sum in
                         * 64-bit then take low 32, matching the original's
                         * 32-bit wraparound exactly for in-range indices. */
                        __int64 sum = (__int64)JpegT0[i0] + JpegT1[i1] +
                                      JpegT2[i2];

                        s = (unsigned)(sum & 0xFFFFFFFF);
                    }
                    {
                        unsigned char *dst = pixels + (size_t)dy * pitch +
                                             (size_t)dx * 4;

                        dst[0] = (unsigned char)(s & 255);
                        dst[1] = (unsigned char)((s >> 8) & 255);
                        dst[2] = (unsigned char)((s >> 16) & 255);
                        dst[3] = (unsigned char)((s >> 24) & 255);
                    }
                }
            }
        }
    }
    return 0;
}

/* Subsampled (422/420) h=1 output: listing `408FD5-408E2` paired loop.
 * Two luma pixels share one chroma sample (`sar ecx,1` pair index - point
 * replication, no filtering): chroma column is `bx*4 + (x>>1)`, row is `y`
 * for 422 (v=1, rows align) or `by*4 + (y>>1)` for 420 (v=2).  The equation
 * is the same four tables + T0/T1/T2 with logical `shr 22` (the h=2 body at
 * `408FF0` repeats the h=1 computation per pixel of the pair, only the Y
 * differing - second Y loaded dest-relative, i.e. the next luma sample).
 * Gate: 3 components, Y hs=2, chroma 1x1, Y vs 1|2.  Same fitted tables
 * (subsampled chroma is smoother - averages over 2x1/2x2 - so indices land
 * inside the smooth fitted range; verified vs ImageMagick, see run_jpeg).
 * 1-comp restart gate does not apply (NComp==3 here); 3-comp restart EOB
 * tails carry over (bounded, faithful). */
static int JpegOutputSub(unsigned char *pixels, int pitch,
                          unsigned width, unsigned height,
                          int xorigin, int yorigin)
{
    unsigned mx, my, bx, by, x, y;
    unsigned mcux, mcuy, hmax = 2, vmax, ci;

    JpegMakeTables();
    JpegMakeT();

    if (JpegNComp != 3)
        return -1;
    if (JpegComps[0].hsamp != 2)
        return -1;
    vmax = JpegComps[0].vsamp;
    if (vmax != 1 && vmax != 2)
        return -1;
    for (ci = 1; ci < 3; ci++) {
        if (JpegComps[ci].hsamp != 1 || JpegComps[ci].vsamp != 1)
            return -1;
    }
    mcux = (JpegWidth + 8 * hmax - 1) / (8 * hmax);
    mcuy = (JpegHeight + 8 * vmax - 1) / (8 * vmax);
    if (mcux * mcuy > JPEG_MAX_MCU)
        return -1;

    for (my = 0; my < mcuy; my++) {
        for (mx = 0; mx < mcux; mx++) {
            unsigned mcu = my * mcux + mx;
            int *Cbblk = JpegIdctBuf[mcu][1][0];
            int *Crblk = JpegIdctBuf[mcu][2][0];

            for (by = 0; by < vmax; by++) {
                for (bx = 0; bx < 2; bx++) {
                    int *Yblk = JpegIdctBuf[mcu][0][by * 2 + bx];

                    for (y = 0; y < 8; y++) {
                        for (x = 0; x < 8; x++) {
                            int lx = (int)(mx * 16 + bx * 8 + x);
                            int ly = (int)(my * 8 * vmax + by * 8 + y);
                            int dx = lx + xorigin;
                            int dy = ly + yorigin;
                            int pos = (int)(y * 8 + x);
                            int cx = (int)(bx * 4 + (x >> 1));
                            int cy = (vmax == 1) ? (int)y :
                                                     (int)(by * 4 + (y >> 1));
                            int cpos = cy * 8 + cx;
                            int Y, Cb, Cr, v6, res;
                            int A, B, C, D, i0, i1, i2;
                            unsigned s;

                            if (dx < 0 || dx >= (int)width || dy < 0 ||
                                dy >= (int)height)
                                continue;
                            Y = Yblk[pos];
                            Cb = Cbblk[cpos];
                            Cr = Crblk[cpos];
                            v6 = (Cb >> 19) & ~1;
                            res = (Cr >> 19) & ~1;
                            A = JpegTabLookup(JpegTA, res);
                            B = JpegTabLookup(JpegTB, v6);
                            C = JpegTabLookup(JpegTC, res);
                            D = JpegTabLookup(JpegTD, v6);
                            i0 = (int)(((unsigned)((unsigned)Y + (unsigned)A +
                                                   (unsigned)B)) >> 22);
                            i1 = (int)(((unsigned)((unsigned)Y +
                                                   (unsigned)C)) >> 22);
                            i2 = (int)(((unsigned)((unsigned)Y +
                                                   (unsigned)D)) >> 22);
                            {
                                __int64 sum = (__int64)JpegT0[i0] +
                                              JpegT1[i1] + JpegT2[i2];

                                s = (unsigned)(sum & 0xFFFFFFFF);
                            }
                            {
                                unsigned char *dst = pixels +
                                                     (size_t)dy * pitch +
                                                     (size_t)dx * 4;

                                dst[0] = (unsigned char)(s & 255);
                                dst[1] = (unsigned char)((s >> 8) & 255);
                                dst[2] = (unsigned char)((s >> 16) & 255);
                                dst[3] = (unsigned char)((s >> 24) & 255);
                            }
                        }
                    }
                }
            }
        }
    }
    return 0;
}
/* Progressive (SOF2) multi-scan accumulation (`sub_40B180` case -38).
 * First progressive SOS (no restart, non-full selectors) mallocs flat
 * coefficient buffers (`Block = malloc(total<<7)`, 128B = 64 shorts/block,
 * zeroed; freed at EOI/failure) with per-comp bases; later scans reuse them
 * (original keys this off `v195`, which doubles as the restart interval -
 * nonzero after the first progressive scan, so no realloc; DRI+progressive
 * is refused outright since the original never allocates there either).
 * Each scan merges into the buffers (successive approximation: first scans
 * store `value<<Al`, refinements set/add bit Al, EOB runs skip), and at EOI
 * the buffers dequantise (same pattern as `JpegDecodeSeq`: zigzag-t to
 * natural-zz times the fixed-point table), IDCT (`JpegIdct(dbl,0xFF)`) into
 * `JpegIdctBuf`, and output via the wired h=1/subsampled paths.
 * Block layout here is [comp][mcu][block] (mine, matching `JpegIdctBuf`
 * order) rather than the original's flat per-comp v201 pointers - same
 * (comp,block,coef) values, directly indexable.  Coefficients are zigzag-k
 * (Ss/Se are zigzag positions), dequantised with `JpegZigzag[t]` like the
 * sequential path.  Refinement of negatives adds 0 (dead-negative,
 * `*(&v198+2*(v<0))` high half) exactly like the listing - faithful, not a
 * bug to fix (bounded ~1-2 levels, inside prog bounds). */
static short *JpegProg = NULL;
static unsigned JpegProgBase[4];
static unsigned JpegProgBlocks = 0;
static int JpegProgActive = 0;
/* `dword_410158` EOB run bases: following-skipped blocks for run r.
 * Verified on scan2 EOBn (r=1,extra=0 covers 3 following: b1,b2,b3, tail
 * (1,1) padding); runs>1 validate via C-vs-IM pixels (sane => held). */
static const int JpegEobRun[15] =
{1,3,7,15,31,63,127,255,511,1023,2047,4095,8191,16383,32767};

static void JpegProgFree(void)
{
    if (JpegProgActive) {
        free(JpegProg);
        JpegProg = NULL;
        JpegProgBlocks = 0;
        JpegProgActive = 0;
    }
}

/* One refinement raw bit, dry-at-marker counts as 0 (`test ebx,esi/jz` in
 * `sub_40C700`-style slow reads; valid scans always have the bit, corrupt
 * ones complete with 0 instead of crashing/OOB). */
static int JpegRawBit(JpegBit *b)
{
    int bit = 0;

    JpegRefill(b, 1);
    if (b->nbits > 0)
        bit = (b->reg >> (--b->nbits)) & 1;
    return bit;
}

/* Decode one progressive SOS scan into `JpegProg` (buffers already live).
 * Returns the scan end (first marker byte, unconsumed) for the outer loop.
 * Completes always on valid scans (fast/slow/dry-0/len16-sym0 fallbacks,
 * band bounds, EOB runs); corrupt input completes with zeros/EOBs. */
static const unsigned char *JpegDecodeProgScan(const unsigned char *scan,
                                               const unsigned char *end,
                                               unsigned Ss, unsigned Se,
                                               unsigned Ah, unsigned Al)
{
    JpegBit bit;
    int pred[4] = {0, 0, 0, 0};
    unsigned hmax = 1, vmax = 1, t;
    unsigned mcux, mcuy, mx, my;
    int eobrun = 0;

    for (t = 0; t < JpegNComp; t++) {
        if (JpegComps[t].hsamp > hmax)
            hmax = JpegComps[t].hsamp;
        if (JpegComps[t].vsamp > vmax)
            vmax = JpegComps[t].vsamp;
    }
    mcux = (JpegWidth + 8 * hmax - 1) / (8 * hmax);
    mcuy = (JpegHeight + 8 * vmax - 1) / (8 * vmax);

    bit.p = scan;
    bit.end = end;
    bit.reg = 0;
    bit.nbits = 0;
    bit.marker_hit = 0;

    for (my = 0; my < mcuy; my++) {
        for (mx = 0; mx < mcux; mx++) {
            for (t = 0; t < JpegScanNs; t++) {
                unsigned ci = JpegScanOrder[t];
                unsigned hs = JpegComps[ci].hsamp;
                unsigned vs = JpegComps[ci].vsamp;
                unsigned bx, by;
                JpegHuff *dh = &JpegHuffs[JpegComps[ci].dc_sel & 3];
                JpegHuff *ah = &JpegHuffs[4 + (JpegComps[ci].ac_sel & 3)];

                for (by = 0; by < vs; by++) {
                    for (bx = 0; bx < hs; bx++) {
                        unsigned mcu = my * mcux + mx;
                        short *blk = JpegProg +
                                     (JpegProgBase[ci] +
                                      mcu * hs * vs + by * hs + bx) * 64;
                        int k;

                        if (Ss == 0) {
                            /* DC scan: first (Ah==0) decodes diffs (reduced
                             * domain, pred per scan-comp) and stores
                             * `pred<<Al`; refinement (Ah>0) ORs bit Al. */
                            if (Ah == 0) {
                                int sym = JpegHuffSym(dh, &bit);
                                int diff;

                                if (sym < 0)
                                    sym = 0;
                                if (sym > 11)
                                    sym = 11;
                                diff = JpegExtend(&bit, sym);
                                pred[t] += diff;
                                blk[0] = (short)(pred[t] << Al);
                            } else {
                                if (JpegRawBit(&bit))
                                    blk[0] = (short)(blk[0] | (1 << Al));
                            }
                        } else {
                            /* AC scan over band Ss..Se (zigzag k). */
                            if (eobrun > 0) {
                                if (Ah == 0) {
                                    eobrun--;
                                } else {
                                    /* LABEL_198: refine existing nonzero
                                     * (1 bit each, add iff >0), no new. */
                                    for (k = (Ss < 1) ? 1 : (int)Ss;
                                         k <= (int)Se; k++) {
                                        if (blk[k]) {
                                            if (JpegRawBit(&bit) &&
                                                blk[k] > 0)
                                                blk[k] = (short)(blk[k] +
                                                                 (1 << Al));
                                        }
                                    }
                                    eobrun--;
                                }
                                continue;
                            }
                            k = (Ss < 1) ? 1 : (int)Ss;
                            if (Ah == 0) {
                                while (k <= (int)Se) {
                                    int rs = JpegHuffSym(ah, &bit);
                                    int run = (rs >> 4) & 15;
                                    int size = rs & 15;

                                    if (size == 0) {
                                        if (run == 15) {
                                            k += 16;
                                        } else {
                                            if (run > 0) {
                                                int i, extra = 0;

                                                eobrun = JpegEobRun[run];
                                                for (i = 0; i < run; i++)
                                                    extra = (extra << 1) |
                                                        JpegRawBit(&bit);
                                                eobrun += extra;
                                            }
                                            break;
                                        }
                                    } else {
                                        int v;

                                        k += run;
                                        if (k > (int)Se)
                                            break;
                                        v = JpegExtend(&bit, size);
                                        blk[k] = (short)(v << Al);
                                        k++;
                                    }
                                }
                            } else {
                                while (k <= (int)Se) {
                                    int rs = JpegHuffSym(ah, &bit);
                                    int run = (rs >> 4) & 15;
                                    int size = rs & 15;

                                    if (size == 0) {
                                        if (run == 15) {
                                            k += 16;
                                        } else {
                                            int kk;

                                            if (run > 0) {
                                                int i, extra = 0;

                                                eobrun = JpegEobRun[run];
                                                for (i = 0; i < run; i++)
                                                    extra = (extra << 1) |
                                                        JpegRawBit(&bit);
                                                eobrun += extra;
                                            }
                                            for (kk = k; kk <= (int)Se;
                                                 kk++) {
                                                if (blk[kk]) {
                                                    if (JpegRawBit(&bit) &&
                                                        blk[kk] > 0)
                                                        blk[kk] =
                                                            (short)(blk[kk] +
                                                                    (1 << Al));
                                                }
                                            }
                                            break;
                                        }
                                    } else {
                                        /* Position-wise run skip (871-902):
                                         * existing nonzero refined (1 bit,
                                         * add iff >0), zeros count down. */
                                        while (k <= (int)Se) {
                                            if (blk[k]) {
                                                if (JpegRawBit(&bit) &&
                                                    blk[k] > 0)
                                                    blk[k] =
                                                        (short)(blk[k] +
                                                                (1 << Al));
                                            } else {
                                                run--;
                                                if (run < 0)
                                                    break;
                                            }
                                            k++;
                                        }
                                        if (k > (int)Se)
                                            break;
                                        if (JpegRawBit(&bit))
                                            blk[k] = (short)(1 << Al);
                                        else
                                            blk[k] = (short)(-(1 << Al));
                                        k++;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return bit.p;
}

/* EOI with progressive buffers live: dequantise Block to dbl (same pattern
 * as `JpegDecodeSeq`: zigzag-t to natural-zz times the fixed-point table),
 * IDCT into `JpegIdctBuf`, output via the wired paths, free buffers. */
static int JpegProgFinish(unsigned char *pixels, int pitch,
                           unsigned width, unsigned height,
                           int xorigin, int yorigin)
{
    unsigned hmax = 1, vmax = 1, ci, t;
    unsigned mcux, mcuy, mx, my;
    int rc = -1;

    for (t = 0; t < JpegNComp; t++) {
        if (JpegComps[t].hsamp > hmax)
            hmax = JpegComps[t].hsamp;
        if (JpegComps[t].vsamp > vmax)
            vmax = JpegComps[t].vsamp;
    }
    mcux = (JpegWidth + 8 * hmax - 1) / (8 * hmax);
    mcuy = (JpegHeight + 8 * vmax - 1) / (8 * vmax);

    for (ci = 0; ci < JpegNComp; ci++) {
        unsigned hs = JpegComps[ci].hsamp;
        unsigned vs = JpegComps[ci].vsamp;

        for (my = 0; my < mcuy; my++) {
            for (mx = 0; mx < mcux; mx++) {
                unsigned mcu = my * mcux + mx;
                unsigned bx, by;

                for (by = 0; by < vs; by++) {
                    for (bx = 0; bx < hs; bx++) {
                        short *blk = JpegProg +
                                     (JpegProgBase[ci] +
                                      mcu * hs * vs + by * hs + bx) * 64;
                        int dbl[64];
                        int z;

                        unsigned m2 = my * mcux + mx;
                        unsigned b2 = by * hs + bx;
                        int t2;

                        for (z = 0; z < 64; z++) {
                            int zz = JpegZigzag[z];
                            __int64 dq = (__int64)blk[z] *
                                JpegQuant[JpegComps[ci].qsel & 3][zz];

                            dbl[zz] = (int)dq;
                        }
                        JpegIdct(dbl, 0xFF);
                        if (m2 < JPEG_MAX_MCU && b2 < 4) {
                            for (t2 = 0; t2 < 64; t2++)
                                JpegIdctBuf[m2][ci][b2][t2] = dbl[t2];
                        }
#ifdef JPEGDEBUG
                        {
                            int td;

                            fprintf(stderr,
                                    "jpeg: pblk c%u mx%u my%u b%u =",
                                    ci, mx, my, by * hs + bx);
                            for (td = 0; td < 64; td++)
                                fprintf(stderr, " %d", blk[td]);
                            fprintf(stderr, "\n");
                        }
#endif
                    }
                }
            }
        }
    }
    if (JpegNComp == 1 || JpegNComp == 3) {
        if (JpegOutput444(pixels, pitch, width, height,
                          xorigin, yorigin) == 0)
            rc = 0;
        else if (JpegOutputSub(pixels, pitch, width, height,
                               xorigin, yorigin) == 0)
            rc = 0;
    }
    free(JpegProg);
    JpegProg = NULL;
    JpegProgBlocks = 0;
    JpegProgActive = 0;
    return rc;
}
static int DecodeJpeg(const unsigned char *data, int size,
                      unsigned char *pixels, int pitch,
                      unsigned int width, unsigned int height,
                      int xorigin, int yorigin)
{
    const unsigned char *p, *end;
    unsigned marker, seglen;

    (void)size; (void)pixels; (void)pitch;
    (void)width; (void)height; (void)xorigin; (void)yorigin;

    OutColType = 2;
    OutBitDepth = 8;

    if (data[0] != 0xFF || data[1] != 0xD8)
        return -1;

    p = data + 2;
    end = data + size;
    JpegRestart = 0;
    JpegProgFree();                     /* stale buffers from a prior image */

    for (;;) {
        /* skip fill bytes */
        while (p < end && *p == 0xFF)
            p++;
        if (p >= end) {
            JpegProgFree();
            return -1;
        }
        marker = *p++;
        if (marker == 0xD9)
            break;                          /* EOI */
        if (marker == 0xDA) {
            /* SOS: component selectors, then spectral/approximation.
             * p is on the 2-byte length here (unlike the segments above,
             * whose length was already consumed), so skip it first. */
            unsigned ns, i;
            const unsigned char *q;

            if (p + 2 > end)
                return -1;
            p += 2;
            q = p;

            if (p + 1 > end)
                return -1;
            ns = *q++;
            if (ns > 4 || q + 2 * ns + 3 > end)
                return -1;
            for (i = 0; i < JpegNComp; i++)
                JpegComps[i].dc_sel = JpegComps[i].ac_sel = 0;
            JpegScanNs = 0;
            for (i = 0; i < ns; i++) {
                unsigned cid = q[0], sel = q[1];
                unsigned j;

                for (j = 0; j < JpegNComp; j++) {
                    if (JpegComps[j].id == cid) {
                        JpegComps[j].dc_sel = (sel >> 4) & 15;
                        JpegComps[j].ac_sel = sel & 15;
                        /* Scan order (bitstream follows SOS selectors, not
                         * SOF order); full scans give [0..NComp-1]. */
                        if (JpegScanNs < 4)
                            JpegScanOrder[JpegScanNs++] = j;
                    }
                }
                q += 2;
            }
            /* Unmatched selector (corrupt: SOS names a comp SOF never
             * defined) would desynchronise the stream - refuse cleanly. */
            if (JpegScanNs != ns) {
                JpegProgFree();
                return -1;
            }
            {
                unsigned ss = q[0], se = q[1], ahal = q[2];
                const unsigned char *scan = q + 3;

                /* Sequential, no restart, no refinement: decode to
                 * coefficients for verification (dumps under JPEGDEBUG),
                 * then still return -1 - no pixels yet.  Anything else
                 * (progressive, restart, subset scan) waits for its path. */
                if (ss == 0 && se == 63 && ahal == 0) {
                    if (JpegDecodeSeq(scan, end, JpegWidth, JpegHeight,
#ifdef JPEGDEBUG
                                      1
#else
                                      0
#endif
                                      ) == 0) {
                        /*
                         *  h=1 (all 1x1: `408ECD`) then subsampled Y-hs=2
                         *  (422/420 paired: `408FB3`); disjoint gates, fitted
                         *  tables shared (see JpegMakeTables).  Output only on
                         *  full interleaved scans (Ns==NComp); subset scans
                         *  decode their comps correctly (dumps) but have no
                         *  complete image - refuse.  Returns 0 on success;
                         *  anything else (progressive, 1-comp restart gate,
                         *  high-freq restart, subset) falls to -1.
                         */
                        if (JpegScanNs == JpegNComp) {
                            if (JpegOutput444(pixels, pitch, width, height,
                                              xorigin, yorigin) == 0)
                                return 0;
                            if (JpegOutputSub(pixels, pitch, width, height,
                                              xorigin, yorigin) == 0)
                                return 0;
                        }
                    }
                } else {
                    /* Progressive scan (SOF2): Ss/Se bands, Ah/Al successive
                     * approximation.  Merges into Block buffers (allocated on
                     * the first such scan, reused after - like `v195`), then
                     * continues to the next DHT/SOS/EOI (no `break`). */
                    unsigned Ah = (ahal >> 4) & 15, Al = ahal & 15;

                    if (ss > 63 || se > 63 || ss > se || Ah > 13 || Al > 13) {
                        JpegProgFree();
                        return -1;
                    }
                    /* DRI+progressive: the original never allocates there
                     * (`!v195` false) - refuse outright, header-based. */
                    if (JpegRestart != 0) {
                        JpegProgFree();
                        return -1;
                    }
                    if (!JpegProgActive) {
                        unsigned hmax = 1, vmax = 1, t;
                        unsigned mcux, mcuy, total = 0, acc = 0;

                        for (t = 0; t < JpegNComp; t++) {
                            if (JpegComps[t].hsamp > hmax)
                                hmax = JpegComps[t].hsamp;
                            if (JpegComps[t].vsamp > vmax)
                                vmax = JpegComps[t].vsamp;
                        }
                        mcux = (JpegWidth + 8 * hmax - 1) / (8 * hmax);
                        mcuy = (JpegHeight + 8 * vmax - 1) / (8 * vmax);
                        for (t = 0; t < JpegNComp; t++) {
                            JpegProgBase[t] = acc;
                            acc += mcux * mcuy *
                                   JpegComps[t].hsamp * JpegComps[t].vsamp;
                        }
                        total = acc;
                        if (total == 0 || ((total << 7) >> 7) != total) {
                            JpegProgFree();
                            return -1;
                        }
                        JpegProg = (short *)malloc(total << 7);
                        if (!JpegProg)
                            return -1;
                        {
                            unsigned b;

                            for (b = 0; b < total * 64; b++)
                                JpegProg[b] = 0;
                        }
                        JpegProgBlocks = total;
                        JpegProgActive = 1;
                    }
                    p = JpegDecodeProgScan(scan, end, ss, se, Ah, Al);
                    continue;
                }
            }
            break;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7))
            continue;                       /* no length: TEM, RSTn */
        if (p + 2 > end)
            return -1;
        seglen = JpegGet16(&p);
        /* Markers outside 0xC0..0xDD (APPn, COM, ..) fall to the default
         * arm, which skips by the length - 0x40B245 `ja def_40B25B`. */

        if (seglen < 2 || p + seglen - 2 > end) {
            JpegProgFree();
            return -1;
        }

        if (marker >= 0xC0 && marker <= 0xC2) {
            /* SOF0/1/2: precision, height, width, components */
            unsigned prec, h, w, nc, i;

            p++;                            /* precision (always 8) */
            h = JpegGet16(&p);
            w = JpegGet16(&p);
            nc = *p++;
            JpegWidth = w;
            JpegHeight = h;
            JpegNComp = nc;
            if (nc > 4)
                return -1;
            for (i = 0; i < nc; i++) {
                JpegComps[i].id = *p++;
                JpegComps[i].hsamp = (*p >> 4) & 15;
                JpegComps[i].vsamp = *p & 15;
                p++;
                JpegComps[i].qsel = *p++;
            }
            (void)prec;
        } else if (marker == 0xC4) {
            /* DHT: one or more tables */
            const unsigned char *q = p;
            const unsigned char *segend = p + seglen - 2;

            while (q < segend) {
                unsigned spec = *q++;
                unsigned tc = (spec >> 4) & 15, th = spec & 15;
                unsigned slot = (spec < 0x10) ? spec : spec - 12;
                int total = 0, i;

                if (slot > 7)
                    return -1;
                for (i = 0; i < 16; i++)
                    total += q[i];
                JpegDHT(&JpegHuffs[slot],
                        q, q + 16);
                (void)tc; (void)th;
                q += 16 + total;
            }
            p += seglen - 2;
        } else if (marker == 0xDB) {
            /* DQT: one or more tables */
            const unsigned char *q = p;
            const unsigned char *segend = p + seglen - 2;

            while (q < segend) {
                unsigned spec = *q++;
                unsigned prec = (spec >> 4) & 15, tq = spec & 15;

                if (tq > 3)
                    return -1;
                JpegDQT((int)tq, (int)prec, q);
                q += prec ? 128 : 64;
            }
            p += seglen - 2;
        } else if (marker == 0xDD) {
            JpegRestart = JpegGet16(&p);
        } else {
            p += seglen - 2;                /* skip anything else */
        }
    }

    /* EOI with progressive buffers live: dequantise/IDCT/output, free,
     * return its result.  Otherwise (sequential failed, empty) refuse. */
    if (JpegProgActive) {
        return JpegProgFinish(pixels, pitch, width, height,
                              xorigin, yorigin);
    }
    return -1;                              /* entropy body not yet */
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
     *  8 Targa; of those 1, 3, 4, 5, 6, 7 and 8 are transcribed (2 parses only).
     */
    if (fileimg && filesize >= 2) {
        if (fileimg[0] == 0x89 && fileimg[1] == 'P')
            return DecodePng(fileimg, filesize, pixels, pitch, width, height);

        if (fileimg[0] == 0xFF && fileimg[1] == 0xD8)
            return DecodeJpeg(fileimg, filesize, pixels, pitch, width, height,
                              unused7, unused8);

        if (fileimg[0] == 0x19 && fileimg[1] == 0x91 &&
            fileimg[10] == 8 && fileimg[11] == 0)
            return DecodePal8(fileimg, pixels, pitch, width, height,
                              unused7, unused8);

        if (fileimg[0] == 'B' && fileimg[1] == 'M')
            return DecodeBmp(fileimg, filesize, pixels, pitch, width, height,
                             unused7, unused8);

        if (*(const unsigned int *)fileimg == 0x0801050Au)
            return DecodePcx(fileimg, filesize, pixels, pitch, width, height,
                             unused7, unused8);

        if (fileimg[0] == 'G' && fileimg[1] == 'I')
            return DecodeGif(fileimg, filesize, pixels, pitch, width, height,
                             unused7, unused8);

        if (*(const unsigned int *)fileimg == 0x20534444u &&
            *(const unsigned int *)(fileimg + 4) == 124u)
            return DecodeDds(fileimg, filesize, pixels, pitch, width, height,
                             unused7, unused8);

        /*
         *  0x409627: no magic, so Targa is guessed from the header being self
         *  consistent.  The test is the same one GetImageSize() makes, so a file
         *  it would size is a file this will decode.
         */
        if (filesize >= 19 &&
            (fileimg[1] & 0xFE) == 0 &&
            fileimg[2] < 12 &&
            ((1 << fileimg[2]) & 0xE0E) != 0 &&
            (fileimg[16] & 7) == 0 &&
            fileimg[16] != 0 && fileimg[16] <= 32 &&
            (fileimg[17] & 0xC0) == 0)
            return DecodeTarga(fileimg, filesize, pixels, pitch, width, height,
                               unused7, unused8);
    }

    printf("\n*** That input format has not been reconstructed yet.\n"
           "*** This build decodes PNG, GIF, BMP, PCX, Targa, DDS and the 8bpp"
           " palettised format; see README.md.\n\n");
    exit(3);
    return -1;
}
