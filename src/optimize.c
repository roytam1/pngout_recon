/*
 *  optimize.c   -   primitives of the trial-and-error optimiser
 *
 *  Recovered from sub_402AD0 (`Optimize`) and the helpers it calls.  The
 *  optimiser itself is still being recovered; everything in this file is
 *  self contained and is what the rest of the optimiser will be built on.
 *
 *      0x404D70  InitCrcTable()
 *      0x405020  InitHuff()
 *      0x405050  HuffPush()
 *      0x4050C0  HuffPop()
 *      0x4052D0  EmitBytes()
 *      0x405320  PutBits()
 *      0x407F40  InitColorHash()
 *      0x407F80  AddColor()
 *      0x407FC0  FindColor()
 */

#include <string.h>

#include "pngout.h"

/* ------------------------------------------------------------------ */
/*  one deflate block -> one IDAT chunk                              */
/* ------------------------------------------------------------------ */

/*
 *  sub_405150, the wrapper around the compressor:
 *
 *    - sizes the LZ work buffer from the symbol count and the bits per
 *      symbol, 8 bytes per byte of output;
 *    - lets sub_405B80 compress into it;
 *    - then writes the PNG chunk: the 4 byte length straight out of the
 *      scratch dword, the tag, and a *placeholder* CRC;
 *    - emits the final block: the last-block flag, then the end-of-block
 *      code 256 in 2 bits, then sub_4053D0 writes the rows out.
 *
 *  The placeholder CRC is 0x78 0x5E, i.e. 24184 little endian.  That is not a
 *  checksum at all: it is how PNGOUT marks a file as its own, and main()
 *  keys off exactly this value (`unk_190A828 == 24184`) to decide whether it
 *  can reuse the settings it found in the file.
 *
 *  Still to recover: sub_405B80 (the compressor), sub_4053D0 (writes the
 *  filtered rows out) and sub_4058A0 (deflate distance tables).
 */
#define TAG_IDAT_   0x54414449u      /* "IDAT" read as a little endian DWORD */
#define PNGOUT_MARK 0x5E78           /* the placeholder CRC, = 24184       */


/* dword_42EE68: the filtered image, 320 samples a row with a small header,
   giving the 646 byte stride sub_402AD0 walks. */
extern unsigned char ImageBuffer[MAX_WIDTH * 646];

int CompressBlock(unsigned Number, unsigned symbols, int a3, int bitspersym,
                  int a5, int a6);
int  BuildDistanceSlots(int row);
void FlushBlock(unsigned char *rows);

int EmitBlock(unsigned Number, unsigned symbols, int a3, int bitspersym,
              int a5, int a6)
{
    BlockSymbols = (int)symbols;

    /* a4 * Number bits of payload, rounded up to whole bytes, and the
       work buffer is 8 bytes per output byte */
    LinBufBytes = (int)(bitspersym * Number + 7) >> 3;
    LinBufUsed = 0;
    BitBufPos = 0;

    if (LinBuf != NULL) {
        free(LinBuf);
        LinBuf = NULL;
    }

    LinBuf = (unsigned char *)malloc(8 * (size_t)LinBufBytes + 8);
    if (LinBuf == NULL) {
        if (ProgressMsg != NULL)
            ProgressMsg("\rmalloc failed (olinbuf)\n");
        return 0;
    }
    memset(LinBuf, 0, 4 * (size_t)LinBufBytes + 8);

    if (!CompressBlock(Number, symbols, a3, bitspersym, a5, a6))
        return 0;

    /* the chunk header */
    ChunkStart = OutCount;
    EmitBytes((unsigned char *)&Scratch, 4);   /* the length, staged for us */

    BitCrc = 0xFFFFFFFFu;
    BitCount = 0;
    BitBuf = 0;

    PutBits(TAG_IDAT_, 32);
    PutBits(PNGOUT_MARK >> 8, 8);              /* 0x78 */
    PutBits(PNGOUT_MARK & 0xFF, 8);            /* 0x5E */

    CrcPending = 1;                            /* "the CRC has to be patched" */
    InitBlockTables();

    /* deflate's BFINAL, then the end-of-block code 256 in 2 bits */
    if (RowIndex >= RowCount - 1)
        PutBits(1, 1);
    else
        PutBits(0, 1);
    PutBits(2, 2);

    FlushBlock(&ImageBuffer[646 * RowIndex]);
    return 1;
}

/*
 *  The reflected CRC-32 table, polynomial 0xEDB88320, built backwards from
 *  i = 255 down to 0.
 *
 *  Note: in the listing the single writer of this table (sub_404D70 at
 *  0x404D95) is shown four bytes above the array that every reader indexes
 *  (0x405392 and 17 other sites all read `dword_185BAB8`, the writer stores
 *  to `dword_185BABC`).  Taken literally that shifts every entry by one and
 *  would corrupt the CRC of every PNG PNGOUT emits, so the table is modelled
 *  here at the address the readers use.  Worth re-checking against the real
 *  bytes if the original ever turns up.
 */

void InitCrcTable(void)
{
    int i;
    unsigned c;

    for (i = 255; i >= 0; i--) {
        int n;
        c = (unsigned)i;
        for (n = 0; n < 8; n++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        CrcTable[i] = c;
    }
}

/* ------------------------------------------------------------------ */
/*  rank / bit-reverse                                                */
/* ------------------------------------------------------------------ */

/*
 *  sub_405800: assign each element of vals[] its rank among equal-or-smaller
 *  values, bit-reversed over the width of its own code, ready to emit as an
 *  LSB-first deflate code.
 *
 *  The histogram counts DOWN from 0 (dec, not inc), and the cumulative step
 *  doubles, making it a prefix sum in bit-reversed space.  Only the low v
 *  bits of hist[v] survive the reversal, so the doubling is harmless.
 */
int RankValues(int *vals, int count, int *out)
{
    int hist[32];
    int i, j, running;

    for (i = 0; i < 32; i++)
        hist[i] = 0;

    for (j = count - 1; j >= 0; j--)
        hist[vals[j]]--;

    running = 0;
    for (i = 1; i < 32; i++) {
        int t = hist[i];
        hist[i] = running;
        running = 2 * (running - t);
    }

    if (count > 0) {
        for (i = 0; i < count; i++) {
            int v = vals[i];
            int r, acc, n;

            if (v == 0)
                continue;

            r = hist[v];
            hist[v] = r + 1;

            acc = 0;
            n = v;
            while (n--) {
                r >>= 1;
                acc = acc * 2 + (r & 1);
            }
            out[i] = acc;
        }
    }

    return running;
}

/* ------------------------------------------------------------------ */
/*  colour hash                                                       */
/* ------------------------------------------------------------------ */

/*
 *  A textbook binary heap over symbol frequencies, 1-based, with the
 *  sentinel HUFF_EMPTY (575) parked in slot 0 and pre-written one slot ahead
 *  on every push so the sift-down can always read both children.
 *
 *  There is only ONE array.  IDA shows it twice, as `dword_42D7D0` and
 *  `dword_42D7D4`, because those are Heap[0] and Heap[1] respectively.
 *
 *      Freq[s]     frequency of symbol s, dword_42CEC8
 *      Heap[i]     the node indices, dword_42D7D0
 *      HeapCount   how many are in the heap, dword_42E558
 */

void InitHuff(void)
{
    HeapCount = 0;
    Heap[0] = HUFF_SENTINEL;
    Heap[1] = HUFF_SENTINEL;
}

void HuffPush(int sym)
{
    int n = HeapCount + 1;
    int parent = n >> 1;
    int moved = Heap[parent];
    int movedfreq = Freq[moved];
    int myfreq = Freq[sym];
    int slot = n;

    HeapCount = n;

    while (movedfreq > myfreq) {
        Heap[slot] = moved;
        slot = parent;
        parent >>= 1;
        moved = Heap[parent];
        movedfreq = Freq[moved];
    }

    Heap[slot] = sym;
    Heap[n + 1] = HUFF_SENTINEL;       /* == dword_42D7D4[n] */
}

int HuffPop(void)
{
    int result = Heap[1];              /* == dword_42D7D4[0] */
    int last = Heap[HeapCount];
    int n = HeapCount - 1;
    int slot = 1;

    HeapCount = n;

    if (n >= 2) {
        for (;;) {
            int child = slot * 2;
            int pick;

            pick = (Freq[Heap[child + 1]] < Freq[Heap[child]]) ? 1 : 0;
            child += pick;

            if (Freq[Heap[child]] >= Freq[last])
                break;

            Heap[slot] = Heap[child];
            slot = child;

            if (slot * 2 > n)
                break;
        }
    }

    Heap[slot] = last;
    return result;
}

/* ------------------------------------------------------------------ */
/*  deflate style bit and byte output                                 */
/* ------------------------------------------------------------------ */

/* The CRT memset only fills bytes; several of these tables want a dword
   pattern, which the compiler turned into a 32 bit store loop. */
void memset32(void *dest, int value, int count)
{
    int *p = (int *)dest;

    while (count-- > 0)
        *p++ = value;
}

/*
 *  The output buffer is reached through the image descriptor:
 *      +0x10  buffer
 *      +0x14  size of the buffer
 *  and writes past the end are silently dropped, so a trial that overruns
 *  just produces a size that will lose.
 */
extern IMAGEDESC *Image;
extern int         OutCount;            /* dword_185BECC */

int EmitBytes(unsigned char *src, int count)
{
    int i;

    if (count <= 0)
        return 0;

    for (i = 0; i < count; i++) {
        int pos = OutCount;

        if (pos < Image->outsize)
            Image->outbuf[pos] = src[i];

        OutCount = pos + 1;
    }

    return i;
}

/*
 *  BitCount is a *bit* count and BitBuf a 32 bit accumulator shifted left as
 *  bits arrive; whole bytes fall out of the bottom and are CRC'd as they go.
 *  dword_412EB8 is the running CRC.
 */
extern int          BitCount;           /* dword_412670 */
extern unsigned int BitBuf;             /* dword_412674 */
extern unsigned int BitCrc;             /* dword_412EB8 */

unsigned int PutBits(unsigned int value, int bits)
{
    int total = BitCount + bits;
    unsigned int buf = BitBuf | (value << BitCount);
    unsigned int crc = BitCrc;

    BitBuf = buf;
    BitCount = total;

    while (total >= 8) {
        int pos = OutCount;

        if (pos < Image->outsize)
            Image->outbuf[pos] = (unsigned char)buf;

        crc = CrcTable[(unsigned char)(buf ^ crc)] ^ (crc >> 8);
        buf >>= 8;
        pos++;
        OutCount = pos;
        BitCrc = crc;
        BitBuf = buf;
        total -= 8;
        BitCount = total;
    }

    return buf;
}

/* ------------------------------------------------------------------ */
/*  length limited Huffman code length solver                         */
/* ------------------------------------------------------------------ */

/*
 *  dword_42DC58: the parent of each node, left as a one-child parent list
 *  because both children of a merge get the same parent index.  After the
 *  tree is built the array is walked backwards to turn parent links into
 *  depths, which is why both children may share a value.
 */

/*
 *  sub_404DA0: choose a code length for every symbol.
 *
 *  In the spirit of zlib's gen_bitlen(): build a Huffman tree, look at the
 *  longest code it produced, and if that is too long, flatten the frequency
 *  distribution (bias += 1 << shift, then shift++) and build it again.  Each
 *  round pushes low frequencies further down, so the code gets shallower.
 *
 *  `counts[i] <= 0` means symbol i is unused: it is given frequency
 *  0x7FFFFFFF so the heap always sinks it to the bottom, and a length of 0.
 *
 *  Finally, symbols whose *flattened* frequency came out equal must be
 *  given lengths in non-increasing order or the resulting code is not
 *  canonical.  Runs of such symbols get their lengths bubble sorted.
 *
 *  Returns nsyms, or 0 when the data degenerates to fewer than two symbols.
 */
int BuildCodeLengths(int *counts, int nsyms, int *lengths, int maxlen)
{
    int bias = 0;
    int shift = 0;
    int nused = 0;

    for (;;) {
        int top, s, j, n, k, longest;

        InitHuff();
        nused = 0;
        s = 0;

        if (nsyms <= 0)
            break;

        do {
            if (counts[s] <= 0) {
                Freq[s] = 0x7FFFFFFF;
            } else {
                nused++;
                Freq[s] = (counts[s] + bias) >> shift;
            }
            HuffPush(s);
            s++;
        } while (s < nsyms);

        if (nused < 2)
            break;

        /* Build the tree: two pops make one parent. */
        top = nused + nsyms - 2;

        for (j = nsyms; j <= top; j++) {
            int a = HuffPop();
            int b;

            Left[a] = j;
            b = HuffPop();
            Left[b] = j;
            Freq[j] = Freq[a] + Freq[b];
            HuffPush(j);
        }

        /* Parent links to depths, deepest node first. */
        Freq[top] = 0;
        for (n = top - 1; n >= nsyms; n--)
            Freq[n] = Freq[Left[n]] + 1;

        /* Emit the lengths back to front, tracking the longest. */
        longest = 0;

        for (k = nsyms - 1; k >= 0; k--) {
            if (counts[k] <= 0) {
                lengths[k] = 0;
            } else {
                int v = Freq[Left[k]] + 1;

                lengths[k] = v;
                if (v > longest)
                    longest = v;
            }
        }

        if (longest < maxlen)
            goto canonicalise;

        bias  += 1 << shift;
        shift++;
    }

    /*
     *  Fewer than two symbols: every code gets a single bit, which is what
     *  the caller needs to emit a literal of one kind and still leave a
     *  second code point assigned.
     */
    memset(lengths, 0, 4 * nsyms);

    if (nused != 0) {
        int only = HuffPop();

        lengths[only] = 1;
        if (Image != NULL && Image->ndistinct >= 2)
            lengths[(~only) & 1] = 1;
    } else {
        if (Image != NULL && Image->ndistinct >= 1)
            lengths[0] = 1;
        if (Image != NULL && Image->ndistinct >= 2)
            lengths[1] = 1;
    }

    return 0;

canonicalise:
    {
        int i;
        int *lp = lengths + 1;
        const int *cp = counts;

        for (i = 1; i - 1 < nsyms; i++, lp++, cp++) {
            int end, a, p;

            if (*cp <= 0)
                continue;
            if (i >= nsyms)
                continue;

            /* how far does the run of equal flattened frequency reach? */
            for (end = i; end < nsyms; end++) {
                if (((counts[end] + bias) >> shift)
                    != ((*cp + bias) >> shift))
                    break;
            }

            /* sort lengths[i-1 .. end-1] into non-increasing order */
            for (p = 0; p + i < end; p++) {
                for (a = i - 1; a < i + p; a++) {
                    int t = lengths[a];
                    int v = lp[p];
                    if (v > t) {
                        lengths[a] = v;
                        lp[p] = t;
                    }
                }
            }
        }
    }

    return nsyms;
}

/* ------------------------------------------------------------------ */
/*  deflate length and distance tables                                */
/* ------------------------------------------------------------------ */

/*
 *  sub_4058A0.  The base/extra-bit tables are generated rather than stored,
 *  once, guarded by dword_18CBEF0.  They are the standard deflate tables:
 *
 *      LenBase[i]  / LenExtra[i]   30 length codes, lengths 3 .. 258
 *      DistBase[j] / DistExtra[j]  56 distance codes, distances 1 .. 32768
 *
 *  LenCodeToSlot maps a length code back to its position and
 *  DistCodeToSlot does the same for distance codes, giving O(1) decoding in
 *  the trial loops.
 *
 *  The arrays themselves live in globals.c; see the extern declarations in
 *  pngout.h for the addresses.
 */

/*
 *  Generate the length table: 30 codes, one extra bit more for every other
 *  code from the third onwards, which is what makes the groups come out in
 *  the RFC 1951 pattern.
 */
/*
 *  dword_1809390 is 30 (base, extra) pairs: base at [2*i], extra at
 *  [2*i+1], which IDA also names dword_1809394[2*i].
 *
 *  The loop variables are volatile on purpose.  VC6 /O2 miscompiles the
 *  block-scoped form of this loop - it stops honouring the store after the
 *  fourth pair, which the BuildDeflateTables checks in tools\selftest.cmd
 *  caught.  See README.md.
 */
static void BuildLengthTable(void)
{
    volatile int base;
    volatile int extra;
    int i;

    LenPairs[0] = 0;
    LenPairs[1] = 1;

    for (i = 1; i < 30; i++) {
        base = LenPairs[2 * i - 2];
        extra = LenPairs[2 * i - 1];
        extra = extra + (1 << base);
        if (((i - 1) & 1) != 0 && (i - 1) >= 2)
            base = base + 1;
        LenPairs[2 * i] = base;
        LenPairs[2 * i + 1] = extra;
    }
}

/*
 *  Generate the distance table: 28 code pairs.  The original tests the loop
 *  counter *and* the loop pointer against a threshold (unk_18094B8), which
 *  is the same as "every fourth code, but not before the eighth", so the
 *  extra-bit count steps up at i = 7, 11, 15, ...
 *
 *  Note the first group of zero-extra-bit codes is wider than RFC 1951
 *  allows.  PNGOUT's deflate is its own, so these tables are NOT assumed to
 *  match the RFC; see README.md.
 */
static void BuildDistanceTable(void)
{
    int i;

    DistPairs[0] = 3;
    DistPairs[1] = 0;

    for (i = 1; i < 28; i++) {
        int base = DistPairs[2 * (i - 1)];
        int extra = DistPairs[2 * (i - 1) + 1];

        base += 1 << extra;
        if (((i - 1) & 3) == 3 && (i - 1) >= 7)
            extra++;
        DistPairs[2 * i] = base;
        DistPairs[2 * i + 1] = extra;
    }
}

/* Which length code covers a given length, walking the table backwards. */
static void BuildLenCodeToSlot(void)
{
    int slot = 258;
    int code = 58;

    while (slot >= 3) {
        int extra = LenPairs[2 * code + 1];

        LenCodeToSlot[slot - 258] = (unsigned char)code;
        slot--;
        if (slot < extra)
            code--;
        if (code < 0)
            break;
    }
}

/* Which distance code covers a given distance, walking the table backwards. */
static void BuildDistCodeToSlot(void)
{
    int slot = 32768;
    int code = 29;

    while (slot > 0) {
        int extra = LenPairs[2 * code + 1];

        DistCodeToSlot[slot - 1] = (unsigned char)code;
        slot--;
        if (slot < extra)
            code--;
        if (code < 0)
            break;
    }
}

/*
 *  The one-shot part of sub_4058A0, split out so it can be exercised on its
 *  own.  Returns 1 if it actually built them.
 */
/*
 *  Order matters, and it is not arbitrary.
 *
 *  The original's two reverse-slot builders index the length table with codes
 *  up to 58 (dword_1809394[2 * 58]) while that table only holds 30 pairs, so
 *  those reads deliberately fall through into the *distance* table that the
 *  linker has placed immediately after.  Reproduced verbatim they overwrite
 *  the length table instead, because this build lays the arrays out
 *  differently, so the length table is built last.  The values produced are
 *  the ones the loops generate:
 *
 *      (0,1) (0,2) (0,3) (0,4) (1,5) (1,7) (2,9) (2,13) (3,17) (3,25) ...
 *
 *  which is the RFC 1951 length pattern shifted so that the group width is
 *  2^extra.  Verified by the BuildDeflateTables checks in
 *  tools\selftest.cmd; see README.md for the aliasing note.
 */
int BuildDeflateTables(void)
{
    if (DistTablesDone)
        return 0;

    DistTablesDone = 1;
    BuildLenCodeToSlot();
    BuildDistCodeToSlot();
    BuildDistanceTable();
    BuildLengthTable();
    LenHuffCount = 258;
    LenHuffIndex = 0;
    return 1;
}

/*  0x4058A0 */
int InitBlockTables(void)
{
    BuildDeflateTables();

    /* per block reset */
    CurMatch = 0x80000000u;                        /* dword_412678 */
    memset32(HashPrev, 0x7FFFFFFF, 511);            /* unk_41267C */
    memset(HashHead, 0, 0x2000);                   /* word_412EC0 */
    memset(DistSlot, 0, 0x10000);                  /* word_41CEC8 */

    /*
     *  Strategies 3 (Huffman only) and 4 (uncompressed) never build the LZ
     *  work area, so only the first three strategies reset it.
     */
    if (Image->strategy < 3) {
        memset(MatchArea, 0, 0x20000);             /* dword_6B4968 */
        unk_185BEC8 = -1;
        unk_180BA84 = 0;
        unk_185BEB8 = 0;
        unk_42E55C = 0;
        unk_411D58 = -1;
        unk_41CEC0 = 0;
    }

    unk_6D4968 = 0;
    unk_6F6B78 = 0;
    unk_412668 = 0;
    unk_42D7C8 = 0;
    unk_18CBEEC = 0;
    RowIndex = 0;
    RowSlot[646 * RowCount] = 0x7FFFFFFF;          /* dword_42E958[] */

    return BuildDistanceSlots(0);
}

/* ------------------------------------------------------------------ */
/*  per row match slots                                               */
/* ------------------------------------------------------------------ */

/*
 *  sub_405A60: turn one row's LZ distances into the slot table the encoder
 *  searches.  The first 30 slots are length slots, the rest distance slots,
 *  and every slot that has no match is 4096.
 *
 *  When the strategy is Xtreme (0) it also builds SlotOrder[], each entry
 *  holding the index of the next strictly smaller slot, so a trial can walk
 *  down the candidates instead of scanning all 30.
 */
int DistSlots[64];                  /* dword_6D5578, 59 entries used */
int DistWork[MAX_WIDTH * 646];      /* dword_42E968, per-row scratch */
int DistIn[MAX_WIDTH * 646];        /* dword_42F2E8, per-row distances */
unsigned char SlotOrder[32];        /* byte_185BA90 */
int FreqA;                          /* dword_6F6678 */
int FreqB;                          /* dword_6F6AF8 */

int RankBytes(unsigned char *vals, int count, int *out);
int RankInts(int *vals, int count, int *out);

int BuildDistanceSlots(int row)
{
    int base = 646 * row;
    int n = 0;
    int *dist = &DistIn[base];

    memset(&DistWork[base], 0, 0x500);

    do {
        if (*dist)
            DistSlots[n] = *dist + LenPairs[2 * n];
        else
            DistSlots[n] = 4096;
        n++;
        dist++;
    } while (n < 30);

    if (n < 59) {
        int *slot = &DistSlots[n];
        int *pair = &DistPairs[2 * n];
        int *work = &DistWork[n + base];

        do {
            if (*work)
                *slot = *work + *pair;
            else
                *slot = 4096;
            pair += 2;
            work++;
            slot++;
        } while ((int)pair < 0x1809568);
    }

    if (Image->strategy == 0) {
        int v8 = 1;
        int *v9 = DistSlots;

        do {
            int i;

            for (i = v8; i < 30; i++) {
                if (DistSlots[i] < *v9)
                    break;
            }
            SlotOrder[v8 - 1] = (unsigned char)i;
            v8++;
            v9++;
        } while (v8 - 1 < 30);
    }

    RankValues((int *)&ImageBuffer[base], 286, &FreqA);
    return RankValues(dist - 30, 30, &FreqB);
}

/*
 *  2048 bucket heads over a 256 entry chain, so looking a colour up costs
 *  one hash and a short walk.  The hash is
 *      (((color ^ (color >> 11)) >> 11) & 0xFFFF) - (color & 0xFFFF)
 *  masked to 11 bits.
 */

static unsigned ColorHash(int color)
{
    unsigned t = ((unsigned)((color ^ (color >> 11)) >> 11)) & 0xFFFFu;

    return (t - (unsigned)(color & 0xFFFF)) & (HASH_BUCKETS - 1);
}

int InitColorHash(int *palette)
{
    memset(ColorHashHead, 0xFF, sizeof(ColorHashHead));
    memset(ColorHashNext, 0xFF, sizeof(ColorHashNext));
    ColorHashCount = 0;
    ColorHashPalette = palette;
    return palette != NULL;
}

int AddColor(int color, int index)
{
    unsigned h = ColorHash(color);
    int old = ColorHashHead[h];

    ColorHashHead[h] = index;
    ColorHashNext[index] = old;
    ColorHashCount++;
    return ColorHashCount;
}

int FindColor(int color)
{
    unsigned h = ColorHash(color);
    int i = ColorHashHead[h];

    if (i < 0)
        return -1;

    while (ColorHashPalette[i] != color) {
        i = ColorHashNext[i];
        if (i < 0)
            return -1;
    }

    return i;
}