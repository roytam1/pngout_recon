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

#include <math.h>
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


/* one row record: the fields are picked out by the offsets in pngout.h */
#define ROWOF(row, field)   (&RowRecord[ROW_STRIDE * (row) + (field)])

int  BuildDistanceSlots(int row);
void FlushRow(int *row);
int  WritePngHeader(int width, int height, int coltype, int bitdepth,
                    int *palette, int palcount);

int EmitBlock(unsigned width, unsigned height, int coltype, int bitdepth,
              int *palette, int palcount)
{
    BlockSymbols = (int)height;

    /* one row's worth of filtered samples, and the work buffer is 8 bytes
       per output byte */
    LinBufBytes = (int)(bitdepth * width + 7) >> 3;
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

    if (!WritePngHeader((int)width, (int)height, coltype, bitdepth,
                        palette, palcount))
        return 0;

    /* the chunk header */
    ChunkStart = OutCount;
    EmitBytes(Scratch, 4);                   /* the length, staged for us  */

    BitCrc = 0xFFFFFFFFu;
    BitCount = 0;
    BitBuf = 0;

    PutBits(TAG_IDAT_, 32);
    PutBits(PNGOUT_MARK >> 8, 8);              /* 0x78 */
    PutBits(PNGOUT_MARK & 0xFF, 8);            /* 0x5E */

    AdlerSum = 1;                              /* the Adler-32 seed */
    InitBlockTables();

    /* deflate's BFINAL, then the end-of-block code 256 in 2 bits */
    if (RowIndex >= RowCount - 1)
        PutBits(1, 1);
    else
        PutBits(0, 1);
    PutBits(2, 2);

    FlushRow(ROWOF(RowIndex, ROW_SYMBOLS));
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

            /*
             *  Reverse the low v bits of r.  The order matters: the original
             *  does `shr edx,1` and then `adc eax,eax`, so each bit is taken
             *  *after* the shift, which is the same as taking it before and
             *  shifting afterwards.  Reversing after the shift instead reads
             *  bit 1 upwards, throws the low bit away and pads a zero at the
             *  top - which makes half the codes come out wrong.
             */
            acc = 0;
            n = v;
            while (n--) {
                acc = acc * 2 + (r & 1);
                r >>= 1;
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
    RowRecord[ROW_STRIDE * RowCount + ROW_SLOT] = 0x7FFFFFFF;

    return BuildDistanceSlots(0);
}

/* ------------------------------------------------------------------ */
/*  per row match slots                                               */
/* ------------------------------------------------------------------ */

/*
 *  sub_405A60: turn one row's slot values into the table the encoder's match
 *  search walks.  The first 30 slots come from the length half and the next
 *  29 from the distance half, and every slot that has no entry at all is
 *  4096 - larger than any real value, so it simply loses every comparison.
 *
 *  Both halves are arrays of *code lengths*, not of lengths or distances:
 *  sub_4065A0 emits a match with
 *
 *      PutBits(dword_6F6A04[code], dword_42F1F4[646*row + code]);  // length
 *      PutBits(value - LenBase[code], LenExtra[code]);             // extras
 *      PutBits(dword_6F6AF8[code], dword_42F2E8[646*row + code]);  // distance
 *      PutBits(value - DistBase[code], DistExtra[code]);
 *
 *  so row field +0x89C and row field +0x990
 *  hold how many bits each symbol costs in this row, and what is added here
 *  is the number of extra bits it takes - which is why the entries stay
 *  small enough to be coded by the row's own 19 symbol code.
 *
 *  Note that the second loop's base pointer is written in the listing as
 *  `dword_1809390[eax * 8]` with eax = 30, which is exactly where the linker
 *  put DistPairs - the two tables are adjacent - so `DistPairs[]` is right
 *  even though the disassembly reads LenPairs.
 *
 *  When the strategy is Xtreme (0) it also builds SlotOrder[], each entry
 *  holding the index of the next strictly smaller slot, so a trial can walk
 *  down the candidates instead of scanning all 30.
 */

int RankValues(int *vals, int count, int *out);

int BuildDistanceSlots(int row)
{
    int n = 0;

    memset(ROWOF(row, ROW_SAMPLES), 0, 4 * MAX_WIDTH);

    do {
        if (ROWOF(row, ROW_DISTBITS)[n])
            DistSlots[n] = ROWOF(row, ROW_DISTBITS)[n] + LenPairs[2 * n];
        else
            DistSlots[n] = 4096;                /* 0x1000, "no match" */
        n++;
    } while (n < 30);

    if (n < 59) {
        int *slot = &DistSlots[n];
        int *pair = DistPairs;
        int *in = ROWOF(row, ROW_LENBITS) + n;

        do {
            if (*in)
                *slot = *in + *pair;
            else
                *slot = 4096;
            pair += 2;
            in++;
            slot++;
        } while (pair < &DistPairs[58]);        /* 0x1809480 .. 0x1809568 */
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

    RankValues(ROWOF(row, ROW_SYMBOLS), 286, &FreqA);
    return RankValues(ROWOF(row, ROW_DISTBITS), 30, &FreqB);
}

/* ------------------------------------------------------------------ */
/*  the row writer                                                     */
/* ------------------------------------------------------------------ */

/*
 *  sub_4053D0: write one row's symbol stream out.
 *
 *  `row` is field +0x510 of a row record (0x42EE68) and holds
 *
 *      row[0 .. 255]   256 literal slots, values 0 .. 15
 *      row[256 .. 285]  30 length slots, zero = not used
 *      row[288 .. 317]  30 distance slots, zero = not used
 *
 *  which is row field +0x510 laid over the neighbouring fields: row + 288 is
 *  field +0x990, the distance symbols' bit counts, so the "distance slots"
 *  below are small codes (1 .. 15) and can go through the row's own code
 *  along with everything else.  The length slots are the samples the encoder
 *  matched instead.
 *
 *  and what goes on the wire is
 *
 *      5 bits   how many length slots are in use, minus 1   (1 .. 30)
 *      5 bits   how many distance slots are in use, minus 1 (1 .. 30)
 *      4 bits   how many code lengths follow, minus 4      (4 .. 15)
 *      3 bits each: that many code lengths, in Bitrev19[] order
 *      the symbol stream itself
 *
 *  The 19 symbols are 16 literals plus three run codes, and each row builds
 *  its own code for them - the code is transmitted here and never stored -
 *  with a length limit of 8 bits, so the rows never cost more than they can.
 *
 *  The run codes are what makes a row cheap: PNGOUT's own deflate, unlike
 *  RFC 1951, codes the *symbol stream* with a per row Huffman code, and
 *
 *      16   repeat the previous symbol, 2 extra bits, count 3 .. 6
 *      17   a run of zeroes,           3 extra bits, count 3 .. 10
 *      18   a run of zeroes,           7 extra bits, count 11 .. 138
 *
 *  Codes 17 and 18 are only worth it for zeroes, which is why a run of one
 *  or two zeroes falls back to plain literals: the 2 bit code 16 could not
 *  express a count of 1.
 *
 *  Note the two passes.  The original walks the row twice with the same run
 *  analysis, once to count the symbols and once to emit them, and the emit
 *  pass overwrites the counts with the codes RankValues() hands back.  That
 *  is reproduced here by ClassifyRun() being called from both loops.
 */

#define ROW_HIST       19             /* 16 literals + the three run codes */

/*
 *  Decide what one run of the symbol stream costs.  Returns the code to
 *  emit - a literal 0 .. 15, or 16, 17 or 18 - and how many symbols of the
 *  stream it covers in `count`.
 *
 *  A run of more than three of a value that is *not* already being repeated
 *  is charged as a literal plus a code 16 repeat, which is what the original
 *  does inside one iteration.  Letting the next iteration find that repeat
 *  instead gives the same answer, because the run it sees is one shorter and
 *  still equal to the symbol before it, so ClassifyRun() returns code 16
 *  with exactly the count the original computed inline.
 */
static int ClassifyRun(int *row, int total, int pos, int *count)
{
    int i, val, run, code;

    for (i = pos + 1; i < total && row[i] == row[pos]; i++)
        ;
    run = i - pos;
    val = row[pos];

    /* a run of zeroes, as long as the 3 or 7 bit count can hold it */
    if (val == 0 && run >= 3) {
        if (run > 138)
            run = 138;
        if (run > 10)
            code = 18;
        else if (pos > 0 && row[pos - 1] == 0 && run <= 6)
            code = 16;             /* the run is already being repeated */
        else
            code = 17;
        *count = run;
        return code;
    }

    /* more of the symbol before this one: 3 .. 6, no longer */
    if (pos > 0 && val == row[pos - 1] && run >= 3) {
        if (run % 6 == 2)
            run = 5;      /* charged as five whatever the real length is */
        if (run > 6)
            run = 6;
        *count = run;
        return 16;
    }

    *count = 1;
    return val;
}

void FlushRow(int *row)
{
    int hist[ROW_HIST];
    int lens[ROW_HIST];
    int nlen = ROW_LITERALS + ROW_LENSLOTS;      /* var_A4, starts at 286 */
    int ndist = ROW_DISTSLOTS;
    int total, pos, i, nsent;

    /*
     *  The length slots are packed against index 285 and the distance slots
     *  against index 317, so the number in use is found by walking down over
     *  the first zero - and the count is one past the slot that stopped it,
     *  so a hole anywhere below the top slot is taken as in use.  Both walks
     *  stop one slot short, which is why a row always reports at least one
     *  length slot and one distance slot.
     *
     *  The distance slots are then moved to sit right after the literals so
     *  the stream is contiguous, which is where the run analysis expects
     *  them, and moved back at the end for the next row.
     */
    if (row[285] == 0) {
        int *p = &row[285];

        while (nlen > ROW_LITERALS + 1) {
            int v = *--p;

            nlen--;
            if (v)
                break;
        }
    }

    if (row[317] == 0) {
        int *p = &row[317];

        while (ndist > 1) {
            int v = *--p;

            ndist--;
            if (v)
                break;
        }
    }

    PutBits(nlen - ROW_LITERALS - 1, 5);
    PutBits(ndist - 1, 5);

    if (ndist > 0)
        for (i = 0; i < ndist; i++)
            row[nlen + i] = row[288 + i];

    total = nlen + ndist;

    /* pass one: how often does each of the 19 symbols occur? */
    memset(hist, 0, sizeof(hist));

    for (pos = 0; pos < total; ) {
        int count;

        hist[ClassifyRun(row, total, pos, &count)]++;
        pos += count;
    }

    /* a code for them, transmitted with the row */
    BuildCodeLengths(hist, ROW_HIST, lens, 8);
    RankValues(lens, ROW_HIST, hist);   /* hist now holds the codes */

    /*
     *  Only a suffix of Bitrev19[] is sent.  The scan walks the table
     *  backwards looking for the last symbol that has a length at all, and
     *  the count it lands on is the number of entries to send, so symbols
     *  16, 17, 18 and 0 always go out whether they are used or not.
     */
    nsent = 19;
    for (i = 18; i > 3; i--) {
        if (lens[Bitrev19[i]] != 0)
            break;
        nsent--;
    }

    PutBits(nsent - 4, 4);

    for (i = 0; i < nsent; i++)
        PutBits(lens[Bitrev19[i]], 3);

    /* pass two: and now the symbols themselves */
    for (pos = 0; pos < total; ) {
        int count;
        int code = ClassifyRun(row, total, pos, &count);

        switch (code) {
        case 16:
            PutBits(hist[16], lens[16]);
            PutBits(count - 3, 2);
            break;
        case 17:
            PutBits(hist[17], lens[17]);
            PutBits(count - 3, 3);
            break;
        case 18:
            PutBits(hist[18], lens[18]);
            PutBits(count - 11, 7);
            break;
        default:
            PutBits(hist[code], lens[code]);
            break;
        }

        pos += count;
    }

    /* put the distance slots back where the next row will look for them */
    for (pos = 0; pos < ndist; pos++) {
        int *dst = &row[288 + ndist - 1 - pos];
        int *src = &row[nlen + ndist - 1 - pos];

        *dst = *src;
        *src = 0;
    }
}

/* ------------------------------------------------------------------ */
/*  the PNG in front of the image data                                */
/* ------------------------------------------------------------------ */

/*
 *  sub_405B80 is not the compressor: it writes everything that comes before
 *  the IDAT, and returns 0 only when a bKGD index will not fit the bit depth.
 *  `EmitBlock()` calls it first and only goes on to the IDAT when it succeeds.
 *
 *      the signature and a fresh IHDR
 *      the kept chunks from before PLTE, minus bKGD, with sBIT rebuilt
 *      for a palette: PLTE, and a tRNS holding the alpha of each entry
 *      for other colour types: a tRNS holding the darkest sample
 *      bKGD, if the image has a background colour
 *      the kept chunks from after PLTE, minus bKGD and hIST
 *
 *  Two byte orders meet here and it is worth keeping them apart.  The decoded
 *  image is 0xFFBBGGRR - bytes R, G, B, FF - while the palette and the bKGD
 *  colour are 0xAARRGGBB, so PLTE takes bytes 2, 1, 0 of a palette entry and
 *  tRNS takes byte 3, which is the alpha.  0xFF in that byte means opaque,
 *  which is how the tRNS scan finds where the transparency stops.
 *
 *  Every chunk is staged in Scratch[] (dword_412E78) and the CRC is computed
 *  over the tag and the data, never over the length.
 */

#define TAG_IHDR   0x52444849u      /* the tag as it is written, LE dword */
#define TAG_bKGD   0x44474B62u
#define TAG_sBIT   0x54494273u
#define TAG_PLTE   0x45544C50u
#define TAG_tRNS   0x534E5274u
#define TAG_hIST   0x54534968u

/* the length of a chunk record: four bytes, big endian */
static int ChunkLen(unsigned char *c)
{
    return ((int)c[0] << 24) | ((int)c[1] << 16) | ((int)c[2] << 8) | c[3];
}

/* how many samples sBIT and bKGD carry for each colour type: nibbles of
   0x04023301 and 0x06021602 indexed by the colour type */
#define SBIT_LEN(coltype)   (((0x04023301u >> (4 * (coltype))) & 0xFu))
#define BKGD_LEN(coltype)   (((0x06021602u >> (4 * (coltype))) & 0xFu))

static void PutBE(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

/* the PNG chunk CRC: over [from, to), seeded and finished as usual */
static unsigned ChunkCrc(unsigned char *p, int from, int to)
{
    unsigned c = 0xFFFFFFFFu;
    int i;

    for (i = from; i < to; i++)
        c = CrcTable[(unsigned char)(c ^ p[i])] ^ (c >> 8);

    return ~c;
}

/* one sample of the bKGD / tRNS chunk, as two big endian bytes */
static void PutSample(unsigned char *p, int value, int bitdepth)
{
    if (bitdepth != 8)
        value = value * ((1 << bitdepth) - 1) / 255;
    p[0] = (unsigned char)(value >> 8);
    p[1] = (unsigned char)value;
}

int WritePngHeader(int width, int height, int coltype, int bitdepth,
                   int *palette, int palcount)
{
    int i, n, last, crc;

    OutCount = 0;

    /* ---------------- the signature and IHDR ---------------- */

    memcpy(Scratch, "\x89PNG\x0D\x0A\x1A\x0A", 8);
    PutBE(Scratch + 8, 13);
    memcpy(Scratch + 12, "IHDR", 4);
    PutBE(Scratch + 16, (unsigned)width);
    PutBE(Scratch + 20, (unsigned)height);
    Scratch[24] = (unsigned char)bitdepth;
    Scratch[25] = (unsigned char)coltype;
    Scratch[26] = 0;                 /* compression method */
    Scratch[27] = 0;                 /* filter method      */
    Scratch[28] = 0;                 /* interlace          */
    PutBE(Scratch + 29, ChunkCrc(Scratch, 12, 29));
    EmitBytes(Scratch, 33);

    /* ---------------- the chunks from before PLTE ---------------- */

    for (i = Image->chunkbeg; i < Image->chunkmid; i++) {
        unsigned char *c = (unsigned char *)Image->chunks[i];
        unsigned tag = *(unsigned int *)(c + 4);
        unsigned char sbit[4];

        if (tag == TAG_bKGD)
            continue;               /* written again further down */

        if (tag != TAG_sBIT) {
            EmitBytes(c, ChunkLen(c) + 12);
            continue;
        }

        /*
         *  sBIT has to be rebuilt, because the colour type may have changed
         *  and the number of samples with it.  The values are kept - they
         *  say how many bits each sample really uses - but a chunk too short
         *  for the new colour type has its missing entries filled with the
         *  first one, which is what the original does.
         */
        sbit[0] = (unsigned char)width;   /* the argument slot is the scratch,
                                            so an empty chunk starts out with
                                            the low byte of the width */
        n = ChunkLen(c);
        for (last = 0; last < n && last < 4; last++)
            sbit[last] = c[8 + last];
        if (n < 4)
            sbit[3] = sbit[0];
        if (n < 3) {
            sbit[2] = sbit[0];
            sbit[1] = sbit[0];
        }

        PutBE(Scratch, SBIT_LEN(coltype));
        memcpy(Scratch + 4, "sBIT", 4);
        n = 8;
        Scratch[n++] = sbit[0];
        if ((coltype & 3) != 0) {
            Scratch[n++] = sbit[1];
            Scratch[n++] = sbit[2];
        }
        if (coltype >= 4)
            Scratch[n++] = sbit[3];
        PutBE(Scratch + n, ChunkCrc(Scratch, 4, n));
        EmitBytes(Scratch, n + 4);
    }

    /* ---------------- PLTE and tRNS ---------------- */

    if (coltype == 3) {
        unsigned char sample[2];

        /*
         *  A palette image's background colour is an index, so the colour has
         *  to be in the palette: look for it and add it if it is not there.
         */
        if (Image->bkgd != 0) {
            for (last = palcount - 1; last >= 0; last--)
                if (((palette[last] ^ Image->bkgd) & 0xFFFFFF) == 0)
                    break;

            if (last < 0) {
                if (palcount >= (1 << bitdepth)) {
                    if (ProgressMsg != NULL)
                        ProgressMsg("\rbKGD chunk can't fit palette index. "
                                    "Try higher bitdepth, /c2 or /c6 instead\n");
                    return 0;
                }
                palette[palcount++] = Image->bkgd | 0xFF000000;
            }
        }

        PutBE(Scratch, 3 * (unsigned)palcount);
        memcpy(Scratch + 4, "PLTE", 4);
        crc = ChunkCrc(Scratch, 4, 8);
        EmitBytes(Scratch, 8);

        for (i = 0; i < palcount; i++) {
            /* bytes 2, 1, 0 of the entry: R, G, B */
            sample[0] = (unsigned char)(palette[i] >> 16);
            sample[1] = (unsigned char)(palette[i] >> 8);
            sample[2] = (unsigned char)palette[i];
            EmitBytes(sample, 3);
            crc = CrcTable[(unsigned char)(crc ^ sample[0])] ^ (crc >> 8);
            crc = CrcTable[(unsigned char)(crc ^ sample[1])] ^ (crc >> 8);
            crc = CrcTable[(unsigned char)(crc ^ sample[2])] ^ (crc >> 8);
        }

        PutBE(Scratch, ~crc);
        EmitBytes(Scratch, 4);

        /*
         *  tRNS for a palette is one alpha per entry, taken from byte 3, and
         *  only as far as the last entry that is not opaque.
         */
        for (last = palcount - 1; last >= 0; last--)
            if ((unsigned)palette[last] < 0xFF000000u)
                break;

        if (last >= 0) {
            PutBE(Scratch, last + 1);
            memcpy(Scratch + 4, "tRNS", 4);
            crc = ChunkCrc(Scratch, 4, 8);
            EmitBytes(Scratch, 8);

            for (i = 0; i <= last; i++) {
                /* byte 3 of the entry: the alpha */
                sample[0] = (unsigned char)((unsigned)palette[i] >> 24);
                EmitBytes(sample, 1);
                crc = CrcTable[(unsigned char)(crc ^ sample[0])] ^ (crc >> 8);
            }

            PutBE(Scratch, ~crc);
            EmitBytes(Scratch, 4);
        }
    } else if ((coltype == 0 || coltype == 2) && MinSample != -1) {
        /*
         *  Anything else gets a tRNS holding the darkest sample in the image,
         *  which is how a greyscale or RGB image ends up with a transparent
         *  colour: everything darker than this is fully transparent.
         */
        PutBE(Scratch, coltype ? 6u : 2u);
        memcpy(Scratch + 4, "tRNS", 4);
        crc = ChunkCrc(Scratch, 4, 8);
        EmitBytes(Scratch, 8);

        n = 8;
        for (i = 8 * (coltype & 3); i >= 0; i -= 8) {
            unsigned char hi;

            PutSample(Scratch + n, (int)(((unsigned)MinSample >> i) & 0xFF),
                      bitdepth);
            hi = 0;
            crc = CrcTable[(unsigned char)(crc ^ hi)] ^ (crc >> 8);
            crc = CrcTable[(unsigned char)(crc ^ Scratch[n + 1])] ^ (crc >> 8);
            n += 2;
        }

        EmitBytes(Scratch + 8, n - 8);
        PutBE(Scratch + n, ~crc);
        EmitBytes(Scratch + n, 4);
    }

    /* ---------------- bKGD ---------------- */

    if (Image->bkgd != 0) {
        PutBE(Scratch, BKGD_LEN(coltype));
        memcpy(Scratch + 4, "bKGD", 4);
        crc = ChunkCrc(Scratch, 4, 8);
        EmitBytes(Scratch, 8);

        if (coltype == 3) {
            /* the index of the background colour, which is in the palette */
            for (last = palcount - 1; last > 0; last--)
                if ((((unsigned)palette[last] ^ (unsigned)Image->bkgd)
                     & 0xFFFFFFu) == 0)
                    break;
            Scratch[0] = (unsigned char)last;
            crc = CrcTable[(unsigned char)(crc ^ Scratch[0])] ^ (crc >> 8);
            PutBE(Scratch + 1, ~crc);
            EmitBytes(Scratch, 5);
        } else {
            n = 0;
            for (i = 8 * (coltype & 3); i >= 0; i -= 8) {
                PutSample(Scratch + n,
                          (int)(((unsigned)Image->bkgd >> i) & 0xFF), bitdepth);
                crc = CrcTable[(unsigned char)(crc ^ Scratch[n])] ^ (crc >> 8);
                crc = CrcTable[(unsigned char)(crc ^ Scratch[n + 1])] ^ (crc >> 8);
                n += 2;
            }
            PutBE(Scratch + n, ~crc);
            EmitBytes(Scratch, n + 4);
        }
    }

    /* ---------------- the chunks from after PLTE ---------------- */

    for (i = Image->chunkmid; i < Image->chunkend; i++) {
        unsigned char *c = (unsigned char *)Image->chunks[i];
        unsigned tag = *(unsigned int *)(c + 4);

        if (tag == TAG_bKGD)
            continue;
        if (tag == TAG_hIST && coltype == 3)
            continue;               /* a palette's histogram is not ours */

        EmitBytes(c, ChunkLen(c) + 12);
    }

    return 1;
}

/* ------------------------------------------------------------------ */
/*  the /b block splitter                                              */
/* ------------------------------------------------------------------ */

/*
 *  sub_408000: cut the filtered image into deflate blocks.
 *
 *  The block list is another 0xA18-strided record array, five fields per
 *  block, and `PlanBlocks()` fills in three of the five the optimiser needs:
 *
 *      BlockSplit[b]   where the block was cut, absolute
 *      BlockStart[b]   first byte
 *      BlockEnd[b]     one past the last byte
 *      BlockNext[b]    the next block in the automatic mode's walk
 *      BlockScore[b]   the saving BestSplit() found there
 *
 *  Two modes, chosen by `Image->maxblocks`:
 *
 *    - 0 or less, automatic: keep cutting whatever block offers the biggest
 *      saving, as long as it beats `Image->splitthreshold`, never going over
 *      1024 blocks.  Each block is looked at twice per round, because cutting
 *      one block changes its neighbour's score;
 *    - more than 1: cut the block with the biggest saving until there are
 *      that many, clamped to 1024.
 *
 *  Both end with a shell sort pass over the block *ends* alone - the starts
 *  are left where they are - so the blocks come out close to equal in size.
 *  The original's sort is a single pass per gap, halving the gap each round,
 *  and it walks each window backwards, swapping as long as it can.
 *
 *  Returns 0, or -1 if ESC was pressed.
 */
/*
 *  Sort the block ends ascending, one shell sort gap at a time.  Only the
 *  ends are sorted - the starts are left where the search put them, because
 *  the finished list is read as start[0], end[0], end[1], end[2] ... in that
 *  order, so the ends *are* the block boundaries.
 *
 *  The walk is backwards from the pair being compared and steps by whole
 *  records, which is why the pointers move gap * BLOCK_STRIDE dwords at a
 *  time and not gap.
 */
void SortBlockEnds(int count)
{
    int gap, j, k, t;

    for (gap = count >> 1; gap != 0; gap >>= 1) {
        for (j = 0; j < count - gap; j++) {
            int *outer = BlockEnd(j);
            int *inner = BlockEnd(j + gap);

            for (k = j; k >= 0; k -= gap,
                 outer -= gap * BLOCK_STRIDE, inner -= gap * BLOCK_STRIDE) {
                if (*outer <= *inner)
                    break;
                t = *outer;
                *outer = *inner;
                *inner = t;
            }
        }
    }
}

int PlanBlocks(int start, int end)
{
    double score, best;
    int i, n, pass, twice, cut, bestblock;

    /* the byte buffer keeps its base pointer one dword below the data */
    unsigned char *data = LinBufData;

    *BlockStart(0) = start;
    *BlockEnd(0) = end;
    NumBlockList = 1;

    if (Image->maxblocks <= 0) {
        *BlockNext(0) = -1;

        /*
         *  Each pass starts at the block whose index is the pass number and
         *  walks the list twice, because cutting a block changes its
         *  neighbour's score.  The list grows while it is walked, so a later
         *  pass reaches the blocks the earlier ones created.
         */
        for (pass = 0; pass < NumBlockList; pass++) {
            i = pass;
            twice = 2;

            while (twice-- != 0) {
                if (i < 0)
                    break;

                cut = BestSplit(data + *BlockStart(i),
                                *BlockEnd(i) - *BlockStart(i), &score);
                if (cut < 0)
                    return -1;

                if (cut > 0 && score > Image->splitthreshold) {
                    n = NumBlockList;
                    *BlockStart(n) = *BlockStart(i) + cut;
                    *BlockEnd(n) = *BlockEnd(i);
                    *BlockEnd(i) = *BlockStart(n);
                    *BlockNext(n) = i;
                    NumBlockList = n + 1;
                    if (n + 1 >= MAX_BLOCKS)
                        goto balance;
                }

                i = *BlockNext(i);
            }
        }
        goto balance;
    }

    if (Image->maxblocks == 1)
        return 0;

    if (Image->maxblocks > MAX_BLOCKS)
        Image->maxblocks = MAX_BLOCKS;

    cut = BestSplit(data + start, end - start, BlockScore(0));
    if (cut < 0)
        return -1;
    *BlockSplit(0) = start + cut;

    while (NumBlockList < Image->maxblocks) {
        /* the block with the biggest saving, and where it was cut */
        best = *BlockScore(0);
        bestblock = 0;

        for (i = NumBlockList - 1; i >= 0; i--) {
            if (*BlockScore(i) > best) {
                best = *BlockScore(i);
                bestblock = i;
            }
        }

        if (*BlockEnd(bestblock) - *BlockStart(bestblock) < 2)
            break;

        n = NumBlockList;
        *BlockStart(n) = *BlockSplit(bestblock);
        *BlockEnd(n) = *BlockEnd(bestblock);

        cut = BestSplit(data + *BlockStart(n),
                        *BlockEnd(n) - *BlockStart(n), BlockScore(n));
        if (cut < 0)
            return -1;
        *BlockSplit(n) = *BlockStart(n) + cut;

        /* and the front half, which the cut just shortened */
        *BlockEnd(bestblock) = *BlockSplit(bestblock);
        cut = BestSplit(data + *BlockStart(bestblock),
                        *BlockEnd(bestblock) - *BlockStart(bestblock),
                        BlockScore(bestblock));
        if (cut < 0)
            return -1;
        *BlockSplit(bestblock) = *BlockStart(bestblock) + cut;

        NumBlockList = n + 1;
    }

balance:
    SortBlockEnds(NumBlockList);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  where to cut a block                                               */
/* ------------------------------------------------------------------ */

/*
 *  sub_408390: given a run of bytes, where is the best place to cut it in
 *  two?  That is the question `sub_408000` (the /b block splitter) asks over
 *  and over, so the answer has to be cheap as well as good, and this is a
 *  single pass with everything it needs kept per symbol.
 *
 *  The cost it accumulates is the -log of a symbol's probability with a prior
 *  folded in, so *lower is better*: the caller keeps the cut with the smallest
 *  cost.  What it reports is that cost negated and scaled by 1/8, so the score
 *  is a *saving* and **higher is better** - `sub_408000` splits when the score
 *  beats the /b threshold.  Two running totals are kept per symbol, for the
 *  bytes of that symbol seen so far and for the bytes still ahead, and each new
 *  byte updates both plus the running totals for the whole range.
 *
 *  The prior is why there are two magic numbers: the per-byte cost is
 *
 *      (n(n+1) + 0.03160502036941208) * (n + 0.5)
 *      -------------------------------------------------
 *      (n(n+1) + 0.19823983692604) * n(n+1)
 *
 *  for n bytes of one symbol, which is a KT/Laplace style estimator - the
 *  constant keeps it finite at n = 0, where the term is skipped entirely.
 *
 *  Returns the offset of the best cut, 1 .. len - 1, and the saving there.  A
 *  range of one byte or less has no interior cut: it returns 0 and leaves
 *  `*score` alone.  -1 means the user pressed ESC.
 */
#define PRIOR_NUM  0.03160502036941208
#define PRIOR_DEN  0.19823983692604

/* the cost of one more byte of a symbol that has been seen n times */
static double SplitCost(double n)
{
    double nn = n * (n + 1.0);

    if (nn == 0.0)
        return 0.0;

    return ((nn + PRIOR_NUM) * (n + 0.5)) / ((nn + PRIOR_DEN) * nn);
}

int BestSplit(unsigned char *data, int len, double *score)
{
    static int   Total[256];        /* dword_411950: how often, in all   */
    static int   Seen[256];         /* dword_411550: how often so far     */
    static double LogCount[256];    /* dbl_6D5E68: ln of the two counts  */
    static double PartCost[256];    /* dbl_6D5668: the running cost      */
    static double Running[256];     /* dbl_6D4970: the running total      */

    double best = 1.0e32;
    double acc = 0.0;
    double prev = 0.0;              /* the last candidate, left on the
                                       x87 stack by the original */
    double tail, total, before, part, rest;
    int i, sym, cut = 0, left = 0, remaining = len;

    if (len <= 1)
        return 0;

    memset(Seen, 0, sizeof(Seen));
    memset(Total, 0, sizeof(Total));

    for (i = 0; i < len; i++)
        Total[data[i]]++;

    memset(PartCost, 0, sizeof(PartCost));

    for (i = 0; i < 256; i++) {
        LogCount[i] = Total[i] ? log((double)Total[i]) : 0.0;
        Running[i] = (double)Total[i] * LogCount[i];
    }

    /* the original builds both of these with fldln2/fyl2x, which is exactly
       log(), so log() is what it computes */
    tail = log((double)(len - 1));
    total = log((double)len) * (double)len;

    for (i = 0; i + 1 < len; i++) {
        sym = data[i];

        /* what the cost would be if the range ended here, before this byte
           has been accounted for */
        before = prev + Running[sym] - total;

        Seen[sym]++;
        part = (double)Seen[sym];
        rest = (double)(Total[sym] - Seen[sym]);

        PartCost[sym] += SplitCost(part);
        LogCount[sym] -= SplitCost(rest);
        Running[sym] = part * PartCost[sym] + rest * LogCount[sym];

        remaining--;
        left++;

        total = (double)remaining * tail + (double)left * acc;
        acc += SplitCost((double)left);
        tail -= SplitCost((double)remaining);

        /* and now if the range started after it */
        prev = before - (Running[sym] - total);

        if (prev < best) {
            best = prev;
            cut = i + 1;
        }

        if ((i & 0xFFFF) == 0 && CheckEscHook != NULL && CheckEscHook(1))
            return -1;
    }

    *score = (-0.125 / 1.0) * best;      /* the original: /log2(2.0) */
    return cut;
}

/* ------------------------------------------------------------------ */
/*  the entropy estimate the trials are scored with                    */
/* ------------------------------------------------------------------ */

/*
 *  sub_4063F0: how many bits would this symbol histogram cost?
 *
 *  The answer is built from `sum(count * ln(count))` over the histogram,
 *  scaled so that the commonest symbol - whose count is one more than
 *  `EntropyMax` - comes out at exactly 0x7FFFFFFF, i.e. the whole 31 bit
 *  range.  So the result is a *score* and not a byte count: the bigger the
 *  sum, the smaller the answer, and the trial loops keep the smallest.
 *
 *  The table of `count * ln(count)` values is built once per distinct maximum
 *  count and cached, because the filter trials ask the same question once per
 *  candidate and only the histogram changes.  0x80000000 is the failure value:
 *  the message goes out, the LZ work buffer is freed (so the caller has to
 *  stop) and the estimate is impossible.
 *
 *  Two things worth keeping as they are:
 *
 *    - the first thing it does is undo one entry of the shared histogram at
 *      index `CurFilter`, which is the filter the trial just used.  What that
 *      is for only becomes clear with the filter trials, so it is reproduced
 *      rather than explained.
 *    - `EntropyMax` is decremented *before* it is compared with the cached
 *      size, and it is the size minus one that sizes the new table, so a
 *      histogram whose largest count is 1 gives a table of -1 entries and
 *      every read lands on the zero at index 0.
 */
unsigned int EstimateEntropy(int *hist, int buckets)
{
    int *lut = EntropyLut;
    int n = --EntropyMax;
    unsigned int result = 0x7FFFFFFEu;
    int i;

    Entropy[CurFilter]--;

    if (n != EntropyLutSize || lut == NULL) {
        double scale;

        EntropyLutSize = n;

        if (lut != NULL) {
            free(lut);
            EntropyLut = NULL;
        }

        lut = (int *)malloc(4 * (size_t)n + 4);
        EntropyLut = lut;

        if (lut == NULL) {
            if (ProgressMsg != NULL)
                ProgressMsg("\rmalloc failed (entropylut)\n");
            if (LinBuf != NULL)
                free(LinBuf);
            return 0x80000000u;
        }

        if (n > 0) {
            scale = 2147483647.0 / ((double)n * log((double)n));
            for (i = n; i > 0; i--)
                lut[i] = (int)((log((double)i) * (double)i) * scale);
        }

        lut[0] = 0;
    }

    for (i = buckets - 1; i >= 0; i--)
        result -= (unsigned int)lut[hist[i]];

    return result;
}

/* ------------------------------------------------------------------ */
/*  colour hash                                                       */
/* ------------------------------------------------------------------ */

/*  2048 bucket heads over a 256 entry chain, so looking a colour up costs
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

/* ------------------------------------------------------------------ */
/*  sub_407D80: filter one byte and hand it to the trial''s sink         */
/* ------------------------------------------------------------------ */

/*
 *  One image byte in, one filtered byte out - and the trial''s sink decides
 *  where the result goes.  The sink is a function pointer (dword_6D6668) which
 *  is one of
 *
 *      loc_406510    a bare histogram: FilterHistogram[b]++, FilterByteCount++
 *      sub_406530    Adler-32 plus the LZ coder, i.e. real compression
 *
 *  so the same walk over the image can either measure or compress, and that is
 *  how the filter trials work: they run the image through with one filter type
 *  each and compare what comes out.
 *
 *  The raw byte is written into the work buffer as it goes, one dword per byte,
 *  because the filters need the byte above.  Two row slots are used in turn -
 *  the row being written and the row above it - and LinBufUsed''s low bit says
 *  which slot this row owns, so the store always lands in the slot the filter
 *  is not reading.  Both slots are dword-aligned, which is why the second one
 *  starts a dword past the end of the first.
 *
 *  `above` is therefore a dword index into the data area and the predictor's
 *  left-of-above byte is one below it.  On the first byte of an even row that
 *  lands in the word in front of the data - the original reads it too.
 */
void FilterByte(int byte)
{
    int *words = (int *)LinBufData;
    int above;

    if (BitBufPos == 0) {
        /*
         *  Start of a row: PNG wants the filter type first, so it goes to the
         *  sink before any data byte.  Filter 3 is the average, which reads the
         *  byte above, and there is no row above the first one - the original
         *  substitutes filter 1 there rather than read whatever is in the slot.
         */
        CurFilter = TrialFilter;
        if (CurFilter == 3 && LinBufUsed == 0)
            CurFilter = 1;
        ByteSink(CurFilter);
        FilterLeft = 0;
    }

    if (LinBufUsed & 1) {
        words[BitBufPos] = byte;
        above = LinBufBytes + BitBufPos + 1;
    } else {
        words[LinBufBytes + BitBufPos + 1] = byte;
        above = BitBufPos;
    }

    switch (CurFilter) {
    case 0:                                       /* none */
        ByteSink(byte);
        break;
    case 1:                                       /* sub: the byte to the left */
        ByteSink(byte - FilterLeft);
        break;
    case 2:                                       /* up: the byte above */
        ByteSink(byte - words[above]);
        break;
    case 3:                                       /* average of the two */
        ByteSink(byte - (((words[above] & 0xFF) + (FilterLeft & 0xFF)) >> 1));
        break;
    case 4:                                       /* the predictor */
        ByteSink(byte - FilterPredict(FilterLeft, words[above] & 0xFF,
                                      words[above - 1] & 0xFF));
        break;
    }

    /*
     *  The byte to the left is the *raw* one, not the filtered one, and it is
     *  taken from the argument after the filter has run.
     */
    FilterLeft = byte;
    BitBufPos++;
    if (BitBufPos < LinBufBytes)
        return;
    BitBufPos = 0;

    /*
     *  Row finished.  The histogram sink is driven row by row by its own loop,
     *  so only the other sinks count rows here - and only the compressing one
     *  cares that a block has filled up.
     */
    if (ByteSink == CountByte)
        return;

    LinBufUsed++;
    if (ByteSink == CompressByte && LinBufUsed >= BlockSymbols &&
        BlockFullHook != NULL)
        BlockFullHook();
}

/*
 *  loc_406510: the measuring sink.  All it does is count, so that the filter
 *  trials can see how compressible a filtered row is without compressing it.
 */
int CountByte(int byte)
{
    FilterHistogram[byte & 0xFF]++;
    FilterByteCount++;
    return 0;
}

/*
 *  sub_406560: one Adler-32 step.  Both halves are mod 65521, and the halves
 *  are tested against the modulus separately so the sums never overflow.
 */
unsigned int Adler32Update(unsigned int sum, int byte)
{
    unsigned int lo = (sum & 0xFFFF) + (unsigned int)(byte & 0xFF);
    unsigned int hi;

    if (lo >= 65521)
        lo -= 65521;

    hi = lo + (sum >> 16);
    if (hi >= 65521)
        hi -= 65521;

    return lo | (hi << 16);
}