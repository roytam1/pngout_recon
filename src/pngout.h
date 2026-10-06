/*
 *  pngout.h
 *
 *  Shared declarations for the PNGOUT reconstruction.
 *
 *  Everything here was recovered from pngout.exe.lst (IDA listing) and
 *  pngout.exe.c (Hex-Rays).  Names marked "(?)" are descriptive guesses made
 *  from call sites and the string table; names without a comment keep the
 *  original IDA symbol so nothing is invented silently.
 *
 *  Original image base 0x400000, .text 0x401000, .data 0x411000.
 *  Original globals therefore start at 0x411000; every symbol below records
 *  the address it occupied in the original binary.
 */

#ifndef PNGOUT_H
#define PNGOUT_H

#include <windows.h>
#include <stdio.h>

#include "defs.h"

/* ------------------------------------------------------------------ */
/*  Fixed limits taken from the original code                          */
/* ------------------------------------------------------------------ */

#define MAX_WIDTH      320     /* internal row length, from `320 * rows` in sub_402AD0 */
#define MAX_COLORS     256     /* sub_4026E0 clamps the chunk table at 256 */
#define HASH_BUCKETS   2048    /* dword_1806F88[2048] in sub_407FC0        */

/*
 *  The optimiser works on one *row record* per row, and every per row array
 *  is a field of that record, which is why they all share one stride.  The
 *  stride is the immediate 0xA18 = 2584 dwords, e.g. sub_405150 at 0x4052A3
 *
 *      mov eax, dword_42D7CC
 *      imul eax, 0A18h
 *      add eax, offset dword_42EE68
 *      push eax
 *      call sub_4053D0
 *
 *  which is 646 * 4: Hex-Rays printed that as `646 * dword_42D7CC`, which
 *  reads like a dword count of 646 and is what earlier revisions of this
 *  reconstruction used.  The records start at dword_42E958 and the fields
 *  sit at these offsets inside one - which is also how they are modelled here,
 *  as one array with the fields picked out by the offsets below.
 *
 *  Modelled as separate arrays the reconstruction needed 21MB of .bss, two
 *  full records, and the linker ran out of image: a record really is one
 *  allocation, and the fields inside it do overlap on purpose.
 */
#define ROW_STRIDE     2584            /* 0xA18 dwords per row record */
#define MAX_ROWS       1024            /* the original's .bss allows 1331 */

/* the fields of one record, as dword indices into it */
#define ROW_SLOT      (0x000 / 4)      /* 1 dword, 0x7FFFFFFF per block    */
#define ROW_SAMPLES   (0x010 / 4)      /* 320 dwords: the row's samples    */
#define ROW_BLOCKS    (0x410 / 4)      /* 1 dword, bumped once per block;
                                           it is also sample 256, the first
                                           length code                       */
/*
 *  dword_42ECF4 is at +0x39C, not +0x44C: 0x42ECF4 - 0x42E958 = 0x39C and the
 *  LZ coder bumps it once per length code it emits (0x406F64).  It is indexed
 *  by the *pair* index 30..58, so it needs 59 entries, which reach +0x484 -
 *  still short of the distance frequencies at +0x490.
 */
#define ROW_LENCNT    (0x39C / 4)      /* 59: a counter per length code    */
#define ROW_DISTFREQ  (0x490 / 4)      /* 32: frequency per distance code,
                                           sitting just past the 286 literal
                                           and length samples               */
#define ROW_SYMBOLS   (0x510 / 4)      /* 318: the symbol stream           */
#define ROW_LENBITS   (0x89C / 4)      /* 30, then 29 more: bits a length
                                           symbol costs in this row         */
#define ROW_ROWBITS   (0x910 / 4)      /* 1 dword: bits the row needs      */
#define ROW_DISTBITS  (0x990 / 4)      /* 30: bits a distance symbol costs*/
/*
 *  dword_42F370[646 * row] is the position in the window at which this row's
 *  data stops.  In the original that address is *also* the `/b` block list's
 *  `end` field - row index and block index are the same counter (dword_6D5570
 *  is both NumBlockList and RowCount) - which is why the LZ coder closes a row
 *  exactly when a block ends.
 */
#define ROW_LIMIT     (0x1018 / 4)     /* 1 dword: the end of this row     */
#define ROW_SAMPLE_MAX 320             /* the samples really are this long */
#define ROW_SAMPLE_SYMS 286            /* how many of them the code covers */

/* the address of one field of one record */
#define ROWOF(row, field)   (&RowRecord[ROW_STRIDE * (row) + (field)])

/* the symbol stream at field +0x510, i.e. sub_4053D0's argument layout */
#define ROW_LITERALS   256             /* literal slots, values 0 .. 15   */
#define ROW_LENSLOTS   30              /* length slots, at +256          */
#define ROW_DISTSLOTS  30              /* distance slots, at +288        */

/* ------------------------------------------------------------------ */
/*  Settings block                                                     */
/*                                                                     */
/*  main() builds this on the stack at ebp-0x290 and hands the address  */
/*  to sub_402AD0 (`lea eax,[esp+2DCh+var_290] ; call sub_402AD0`).    */
/*  Offects below are dword indices into that 136-byte block.           */
/* ------------------------------------------------------------------ */

typedef struct SettingsStruct {
    /* --- image produced by the input decoder (sub_409260) / clipboard --- */
    unsigned char *pixels;   /* +00 idx 0  32bpp pixel buffer            */
    int           pitch;     /* +04 idx 1  bytes per row, 4*width         */
    int           width;     /* +08 idx 2                                */
    int           height;    /* +0C idx 3                                */

    /* --- buffers owned by main() --- */
    unsigned char *outbuf;   /* +10 idx 4  filled in by Optimize()       */
    int           origsize;  /* +14 idx 5  size of the original file     */
    int           unk6;      /* +18 idx 6  <- dword_190A420              */
    int           palflag;   /* +1C idx 7  <- dword_190A820, /ks         */

    /* --- user options --- */
    int           filter;    /* +20 idx 8  /f#, -1 = automatic            */
    int           coltype;   /* +24 idx 9  /c#, -1 = automatic            */
    int           bitdepth;  /* +28 idx 10 /d#, -1 = automatic            */
    int           keeppal;   /* +2C idx 11 /kp                            */
    int           unk12;     /* +30 idx 12 <- dword_190A3FC              */
    int           mincodes;  /* +34 idx 13 /mincodes#, /zl121            */

    int           pad14[5];  /* +38 .. +48                                 */

    int           strategy;  /* +4C idx 19 /s#, 4 = uncompressed          */
    int           randomize; /* +50 idx 20 /r                             */
    int           nblocks;   /* +54 idx 21 /n#                            */
    double        blocksplit;/* +58 idx 22,23 /b#, default 256.0          */

    /* --- results of ScanChunks() --- */
    int           chunkA;    /* +60 idx 24 <- dword_41153C                */
    int           nIHDR;     /* +64 idx 25 <- dword_411540                */
    int           nPLTE;     /* +68 idx 26 <- dword_411544                */
    int           nchunks;   /* +6C idx 27 <- dword_411548                */
    unsigned int *chunks;    /* +70 idx 28 <- dword_411130                */

    int           pad29;     /* +74 idx 29                                 */

    /* --- filled in by Optimize() --- */
    int           outsize;   /* +78 idx 30                                 */

    void        (*msg)(const char *fmt, ...);  /* +7C idx 31 -> sub_401000 */
    int         (*checkesc)(int);               /* +80 idx 32 -> sub_401020 */
    int           verbose;   /* +84 idx 33 /v                             */
} SETTINGS;

/* Globals main() reads that sub_409560/Optimize publish about the image. */
extern unsigned char OutColType;    /* byte_190A824 */
extern unsigned char OutBitDepth;   /* byte_190A825 */
extern int  NumBlocks;              /* dword_190A400 */
extern int  dword_190A420[];        /* table written by sub_409560 */
extern int  unk_190A820;            /* dword_190A820 */
extern int  unk_190A1C4;            /* dword_190A1C4: height of the tRNS chunk */
extern int  unk_190A828;            /* dword_190A828 == 24184 marks a PNG we wrote */

/* ------------------------------------------------------------------ */
/*  The image descriptor that the optimiser and the bit writer share   */
/*                                                                     */
/*  Reached through dword_42E960.  The offsets below are the ones the */
/*  original reads: sub_4052D0 and sub_405320 use +0x10 and +0x14,     */
/*  sub_405B80 uses +0x30 (the bKGD colour) and +0x60 .. +0x70 (the   */
/*  kept chunk list), sub_4065A0 tests +0x4C (the strategy) and       */
/*  sub_402AD0 drives +0x74 (the filter trial) and +0x78 (progress).   */
/* ------------------------------------------------------------------ */

typedef struct ImageDescStruct {
    unsigned int *pixels;   /* +00  the decoded 32bpp image              */
    int           stride;   /* +04  bytes per row                        */
    int           width;    /* +08                                      */
    int           height;   /* +0C                                      */

    unsigned char *outbuf;  /* +10  the PNG being assembled               */
    int           outsize;  /* +14  its size in bytes                     */

    int           pad0[6];  /* +18 .. +2C */

    int           bkgd;     /* +30  the bKGD colour, 0xAARRGGBB, or 0    */
    int           ndistinct;/* +34  how many distinct values the image has */

    int           pad1[5];  /* +38 .. +48 */

    int           strategy; /* +4C  0 Xtreme, 1 Intense, 2 Longest Match,
                               3 Huffman only, 4 uncompressed           */

    int           pad2[1];  /* +50 */

    int           maxblocks;/* +54  /b#, or 0 to pick the cuts by saving */
    double        splitthreshold; /* +58: cut a block when the saving beats
                                     this                                    */

    /* the chunks ScanChunks decided to keep, as records of
       [length][tag][data][crc] - split in two runs around PLTE */
    int           chunkbeg; /* +60  first chunk after IHDR               */
    int           chunkmid; /* +64  first chunk after PLTE               */
    int           chunktail;/* +68  the chunks that follow the image data*/
    int           chunkend; /* +6C  one past the last chunk              */
    unsigned int **chunks;  /* +70  the records themselves               */

    int           trial;    /* +74  which filter trial is running        */
    int           percent;  /* +78  progress, 0 .. 100                   */

    int           pad4;     /* +7C */
    int           pad5[18]; /* +80 .. +C4 */

    int           best;     /* +C8  best compressed size found so far     */
} IMAGEDESC;

extern IMAGEDESC *Image;      /* dword_42E960 */

/* ------------------------------------------------------------------ */
/*  Entry points                                                       */
/* ------------------------------------------------------------------ */

int  main(int argc, char **argv, char **envp);

/* sub_401000: printf to the current Stream (0 = silently discarded) */
void Msg(const char *fmt, ...);
/* sub_401020: returns 1 when the user pressed ESC */
int  CheckEsc(int unused);
/* sub_401050: print the usage banner */
void ShowUsage(void);

/* sub_4026E0: decide which chunks of `fileimg` survive; `listmode` prints them */
void ScanChunks(unsigned char *fileimg, int filesize, char *keeplist,
                int coltype, int listmode);
/* sub_409260: pull width/height out of IHDR; returns 1 if tRNS is present */
/* sub_409260: sniff the container and read width/height.  Format codes: */
#define FMT_NONE   0
#define FMT_PNG    1
#define FMT_JPEG   2
#define FMT_GIF    3
#define FMT_PAL8   4
#define FMT_BMP    5
#define FMT_PCX    6
#define FMT_DDS    7
#define FMT_TGA    8
int  GetImageSize(unsigned char *fileimg, int filesize, int *width, int *height);
/* sub_4029C0: fetch a 32bpp DIB off the windows clipboard */
int  ReadClipboard(unsigned char **pixels, int *pitch, long *width, unsigned *height);
/* sub_409540: swap the bytes of a 16 bit word */
unsigned short Swap16(unsigned short x);
/* sub_404D60: always 0 - used as a "did anything change" predicate */
int  NothingChanged(void);
/* sub_408690: append one byte to the byte buffer at dword_18CBEF4 */
int  PutByte(int c);
/* sub_407F40: reset the 2048 bucket / 256 chain colour hash */
int  InitColorHash(int *palette);
/* sub_407FC0: index of `color` in the palette, or -1 */
int  FindColor(int color);

/* sub_40AF40: PNGOUT's "minimum sum" row predictor (not the PNG Paeth) */
int  FilterPredict(int a1, int a2, int a3);
/* sub_40B060: CPUID feature mask, used once to gate the MMX/SSE2 paths */
unsigned CpuFeatures(void);

/* --- optimiser primitives, src/optimize.c ------------------------- */
void InitCrcTable(void);
void InitHuff(void);
void HuffPush(int sym);
int  HuffPop(void);
int  EmitBytes(unsigned char *src, int count);
unsigned int PutBits(unsigned int value, int bits);
int  InitColorHash(int *palette);
int  AddColor(int color, int index);

/* the Huffman node pool, so tests can seed frequencies directly */
#define HUFF_SENTINEL   575
#define HUFF_MAXSYMS    576
int  BuildCodeLengths(int *counts, int nsyms, int *lengths, int maxlen);
int  EmitBlock(unsigned width, unsigned height, int coltype, int bitdepth,
              int *palette, int palcount);
int  WritePngHeader(int width, int height, int coltype, int bitdepth,
                    int *palette, int palcount);
void (*ProgressMsg)(const char *fmt);   /* dword_185BEC4, from s->msg */

/* --- state the optimiser and the bit writer share ----------------- */
extern int         OutCount;      /* dword_185BECC */
extern int         BitCount;      /* dword_412670, a BIT count */
extern unsigned int BitBuf;       /* dword_412674 */
extern unsigned int BitCrc;       /* dword_412EB8 */
extern IMAGEDESC  *Image;         /* dword_42E960 */
extern void      (*ProgressMsg)(const char *fmt);   /* dword_185BEC4 */

/* --- block state, set up by EmitBlock() -------------------------- */
extern int      BlockSymbols;     /* dword_41266C */
extern int      LinBufBytes;      /* dword_411D54 */
extern int      LinBufUsed;       /* dword_411D50 */
extern int      BitBufPos;        /* dword_18CBED8 */
extern unsigned char *LinBuf;     /* dword_18CBEE0, the LZ work buffer */
extern int      ChunkStart;       /* dword_412160, start of the IDAT header */
extern unsigned char Scratch[64];  /* dword_412E78: the chunk staging area  */
extern int      RowIndex;         /* dword_42D7CC */
extern int      RowCount;         /* dword_6D5570 */
extern int      AdlerSum;         /* dword_706B84: Adler-32 of the raw data */
extern int      RowHeaderCode;    /* dword_6F6A78, the code for one row tag */
extern int      RowCost;          /* dword_18CBEEC, the rows' cost so far  */
/*
 *  dword_412168: 320 code lengths as dwords, a mirror of a row's sample
 *  array.  The distance lengths are built 0x480 bytes further in, which is
 *  sample 288 - and the original's `dword_412668` is the loop's end sentinel,
 *  one past the last entry.
 */
extern int      LensScratch[ROW_SAMPLE_MAX];

/* --- the per byte filter callback, sub_407D80 ---------------------- */
/*
 *  dword_6D6668 points at whichever of the sinks below the trial wants, and
 *  sub_407D80 both filters the byte and hands the result to it.  It returns
 *  int because the original does, but every caller discards the value.
 */
typedef int (*ByteSinkFn)(int byte);
extern ByteSinkFn ByteSink;       /* dword_6D6668 */
extern int      TrialFilter;      /* dword_6D5664, the filter this trial is on */
extern int      FilterLeft;       /* dword_6D496C, the previous raw byte */
extern int      FilterHistogram[256];    /* dword_42E560 */
extern int      FilterByteCount;  /* dword_180A568 */
/*
 *  The original jumps straight into the row loop's block-full handler
 *  (loc_4075B0, inside sub_4071D0) when a compressing sink has taken as many
 *  bytes as the block allows.  That handler is not reconstructed yet, so the
 *  jump is a hook the row loop installs.
 */
extern void   (*BlockFullHook)(void);

/* --- the Huffman work area --------------------------------------- */
extern unsigned CrcTable[256];
extern int      Freq[HUFF_MAXSYMS];
extern int      Heap[HUFF_MAXSYMS * 2];   /* 0x42D7D0; 0x42D7D4 is Heap[1] */
extern int      HeapCount;
extern int      Left[HUFF_MAXSYMS * 2];   /* dword_42DC58 */

/* --- the colour hash --------------------------------------------- */
extern int ColorHashHead[HASH_BUCKETS];
extern int ColorHashNext[MAX_COLORS];
extern int ColorHashCount;
extern int *ColorHashPalette;

/* --- the row records ------------------------------------------------- */
/*  One 2584 dword record per row, laid out exactly as the original's fields
    are; see the offsets above.  The symbol stream a row is coded from is
    field +0x510: 256 literal slots, 30 length slots, 30 distance slots. */
extern int      RowRecord[MAX_ROWS * ROW_STRIDE];

/* --- deflate block state, src/optimize.c -------------------------- */
/*
 *  0x1809390 is ONE array of 59 (extra bit count, smallest value) pairs:
 *
 *      pairs  0 .. 29   the RFC 1951 *distance* codes, bases 1 .. 24577
 *      pairs 30 .. 58   the RFC 1951 *length* codes,   bases 3 .. 258
 *
 *  sub_4058A0 builds the two halves in two loops - 0x1809390..0x180947F with
 *  the extra-bit count stepping up every second code, and 0x1809480..0x180955F
 *  with it stepping up every fourth code from the eighth - and then stores the
 *  last length pair by hand:
 *
 *      mov dword_1809564, 102h      ; pair 58's base, 258
 *      mov dword_1809560, ebx       ; and its extra bit count, 0
 *
 *  Pair 58 is deflate length symbol 285, which is why 258 is coded with no
 *  extra bits at all.  IDA splits the halves into `dword_1809390` and
 *  `unk_1809480` and the two stores into `dword_1809560`/`dword_1809564`.
 *
 *  Getting this the right way round matters: the length half really is RFC's
 *  length table and the distance half RFC's distance table, and the values are
 *  the standard ones, so a reconstruction that swaps them produces a stream no
 *  decoder can read.  `tools\selftest.cmd` now pins both against RFC 1951.
 */
#define CODE_PAIRS     59
#define DIST_CODES     30
#define LEN_CODES      29              /* pairs 30..58, symbols 257..285 */
extern int CodePairs[CODE_PAIRS * 2];

#define DistCodeBase(i)  CodePairs[2 * (i) + 1]
#define DistCodeExtra(i) CodePairs[2 * (i)]
#define LenCodeBase(i)   CodePairs[2 * (30 + (i)) + 1]
#define LenCodeExtra(i)  CodePairs[2 * (30 + (i))]

/*
 *  sub_4058A0 fills the two code tables backwards: `byte_180B97D` is the array
 *  of length codes indexed by the length itself and `byte_414EC0` the array of
 *  distance codes indexed by the distance, and IDA reaches both at -1 because
 *  it names the symbol one byte below the array.
 *
 *  A *length* code is stored as its **pair** index, 30..58, because that is
 *  what indexes DistSlots[] and the row's bit counts; a *distance* code is
 *  0..29.  The length array therefore has to be 259 entries, not 256: the
 *  original writes it from `unk_180BA7F` *downwards*, which for length 258
 *  lands on `unk_180BA7F` itself - one byte past the byte_180B980 IDA names -
 *  and it is the pair's `base`, not its byte count.
 */
extern unsigned char LenCodeToSlotStore[259];   /* 0x180B97D .. 0x180BA7F */
#define LenCodeToSlot   LenCodeToSlotStore
extern unsigned char DistCodeToSlot[32768];     /* 0x414EC0 */
extern int      LenHuffIndex, LenHuffCount, DistTablesDone;


/*
 *  dword_412678[] is one array of 512 dwords, indexed by position & 0x1FF.
 *  sub_4058A0 seeds entry 0 with 0x80000000 and the other 511 with 0x7FFFFFFF,
 *  which is why IDA shows `dword_412678` and `unk_41267C` as two symbols.
 */
#define CurMatch     MatchCost[0]     /* 0x412678 */
#define HashPrev     (MatchCost + 1)  /* 0x41267C, 511 entries */
extern int      HashHead[4096];  /* 0x412EC0, used as shorts */
extern int      DistSlot[32768]; /* 0x41CEC8, used as shorts */
extern int      MatchArea[0x8000];   /* 0x6B4968 */

extern int      Entropy[256];        /* dword_42E560, the trial histogram */
extern int      CurFilter;           /* dword_185BEBC, the filter in use    */
extern int      EntropyMax;           /* dword_180A568, its largest count    */
extern int     *EntropyLut;           /* dword_18CBEE8                        */
extern int      EntropyLutSize;       /* dword_18CBEE4                        */
unsigned int EstimateEntropy(int *hist, int buckets);

extern int     (*CheckEscHook)(int);    /* dword_6F6670, polled by the trials */
/*
 *  The /b block list, another 0xA18-strided record array with five fields
 *  per block - split, start, end, next and a double score.  Only 0x18 bytes
 *  of each record are touched, so it gets a compact stride here; see
 *  PlanBlocks() in src/optimize.c for what each field is.
 */
#define MAX_BLOCKS     1024            /* the original's cap, `cmp eax, 400h` */
#define BLOCK_STRIDE   32              /* dwords per block record            */
extern int BlockRecord[MAX_BLOCKS * BLOCK_STRIDE];
/*
 *  dword_6D5570 is ONE variable in the original.  sub_408000 (PlanBlocks)
 *  counts blocks into it and sub_407090 (AdvanceRow) counts rows into it, and
 *  the LZ coder indexes the *block* ends by row index - so a row and a block
 *  are the same thing here.  It used to be declared twice, once under each
 *  name, which quietly gave the build two counters.
 */
#define NumBlockList   RowCount
extern unsigned char *LinBufData;      /* dword_18CBEE4, the buffer's data   */

#define BlockSplit(n)   (&BlockRecord[(n) * BLOCK_STRIDE + 0])
#define BlockStart(n)   (&BlockRecord[(n) * BLOCK_STRIDE + 1])
#define BlockEnd(n)     (&BlockRecord[(n) * BLOCK_STRIDE + 2])
#define BlockNext(n)    (&BlockRecord[(n) * BLOCK_STRIDE + 3])
#define BlockScore(n)   ((double *)&BlockRecord[(n) * BLOCK_STRIDE + 4])

int  PlanBlocks(int start, int end);
void SortBlockEnds(int count);

/* sub_408390: the best place to cut a run of bytes, and the saving there */
int  BestSplit(unsigned char *data, int len, double *score);
extern int      MinSample;        /* dword_185BEC0, the darkest sample */
extern int      unk_185BEC8, unk_180BA84, unk_185BEB8, unk_411D58, unk_41CEC0;
extern int      unk_42D7C8;
extern unsigned char unk_42E55C;

int  BuildDeflateTables(void);
int  InitBlockTables(void);
int  BuildDistanceSlots(int row);
extern int      DistSlots[64];        /* dword_6D5578 */
extern unsigned char SlotOrder[32];      /* byte_185BA90 */
/*
 *  dword_6F6678 and dword_6F6AF8 are the two arrays sub_405800 (RankValues)
 *  fills: the codes for all 286 symbols of the row's code, and the codes for
 *  the 30 distance codes.  dword_6F6A04, the third symbol, is dword_6F6678 + 227
 *  dwords - see LenCodeOut() - so there are only two of them.
 */
extern int      LitCodes[286];         /* dword_6F6678 */
extern int      DistCodes[32];        /* dword_6F6AF8 */
int  RankValues(int *vals, int count, int *out);
void FlushRow(int *row);

/* ------------------------------------------------------------------ */
/*  sub_4065A0, the LZ coder, and the state it keeps                   */
/* ------------------------------------------------------------------ */

/*
 *  The window is 64KB, one byte per position, indexed by position & 0xFFFF.
 *  dword_706B80 is the dword *overlapping* the last three bytes of it - the
 *  original keeps window[0..3] there so it can slide - and the only thing the
 *  coder ever does with it is that store; nothing in .text reads it back.
 */
extern unsigned char LzWindow[65540];  /* 0x6F6B80 */
extern int      LzScan;               /* 0x6F6B78: how far the matcher got  */
extern int      LzWrite;              /* 0x6D4968: where the next byte goes */
extern int      RunEnd;               /* 0x41CEC0: end of the current run   */
extern int      RunCount;             /* 0x411D58: its length, saturating   */
extern unsigned short RunLen[65536];  /* 0x185BED8: the run each position is
                                            part of                            */
extern int      MatchCost[512];       /* 0x412678: best cost per position   */
extern int      MatchLimit;           /* 0x412668: the furthest one so far  */
extern int      MatchStart[32768];    /* 0x6D6670: where a position's matches
                                            start in the step table            */
extern int      PrevReach;            /* dword_185BEC8 */
extern int      PrevMark;             /* dword_185BEB8 */
extern int      MatchList[0x80000];   /* 0x706B88: length, then distance    */
/*
 *  The step table is indexed by MatchStart[] plus a length, and MatchStart is
 *  the match list count, which the coder only clamps at `cmp eax, offset
 *  unk_7FFF00` before wrapping it to zero.  So the array really does have to
 *  be LZ_STEPCAP entries - the original's runs from 0x806B82 to 0x1806B86,
 *  just short of the colour hash that follows it, and IDA's guess of 331559
 *  entries for word_806D86 is simply wrong.  It is 16MB of .bss that nothing
 *  touches until a block actually gets that big.
 */
#define LZ_STEPCAP    0x7FFF00         /* `cmp eax, offset unk_7FFF00` */
extern unsigned short StepTable[LZ_STEPCAP + 260];
extern int      BestCost[256];        /* dword_6D5170 */
#define MatchReach MatchArea          /* 0x6B4968: how far a position's list
                                            is still good for                    */
#define MatchCount unk_180BA84
#define unk_6D4968 LzWrite
#define unk_6F6B78 LzScan

/* one window byte, the dword that starts there, and the saved first dword */
#define LzByte(i)      (LzWindow[(i) & 0xFFFF])
#define LzDword(i)     (*(unsigned int *)(LzWindow + ((i) & 0xFFFF)))
#define LzHead         (*(unsigned int *)(LzWindow + 0x10000))

/*
 *  The step table.  The recorded distance for match-list entry k lands at
 *  0x806B88 + 2k; the reads are one and 1026 entries either side of that, and
 *  that is exactly what IDA's three symbols word_806B82, word_806B88 and
 *  word_806D86 are - they are one array, not three.
 */
#define LzStepWrite(k)  (StepTable[(k) + 3])
#define LzStepPairAt(k) ((unsigned)StepTable[(k) + 2] | \
                         ((unsigned)StepTable[(k) + 3] << 16))

/*
 *  Which code a length or a distance costs.  byte_180B980 and byte_414EC0 are
 *  both one below the array sub_4058A0 built, which is why the LZ coder reaches
 *  them at index -1.  A length's code is a *pair* index, 30..58.
 */
#define LenCodeOf(len)   LenCodeToSlot[len]
#define LenSlotOf(len)   (LenCodeToSlot[len] - 30)     /* 0..28 */
#define DistCodeOf(dist) DistCodeToSlot[(dist) - 1]

/* the code that goes on the wire for one length slot.  dword_6F6A04 is
   dword_6F6678 + 227 dwords, so it is the same array one code on. */
#define LenCodeOut(slot) LitCodes[227 + (slot)]

/* the match list: length in the low half, distance (or the literal itself) in
   the high half, written as two separate 16 bit stores as the original does */
#define SetMatch(p, l, v)   (MatchList[2 * ((p) & 0x3FFFF)]     = \
                              (unsigned short)(l),                 \
                              MatchList[2 * ((p) & 0x3FFFF) + 1] = \
                              (unsigned short)(v))
#define MatchLenAt(p)       ((int)MatchList[2 * ((p) & 0x3FFFF)])
#define MatchValAt(p)       ((int)MatchList[2 * ((p) & 0x3FFFF) + 1])

int  LzCode(const unsigned char *data, int len);


/* sub_407D80: filter one image byte and hand it to the trial''s sink */
void FilterByte(int byte);
int  CountByte(int byte);                  /* loc_406510, the histogram sink */
unsigned int Adler32Update(unsigned int sum, int byte);   /* sub_406560 */
int  CompressByte(int byte);               /* sub_406530, the LZ coder''s sink */

/* sub_407090: close the row just filled and start the next one */
int  AdvanceRow(void);

/* loc_4075B0: flush the LZ coder, close the block, write its trailer */
void FinishBlock(void);
extern void   (*FlushLz)(void);   /* where step 1 above goes: sub_4065A0(0,0) */
unsigned int Swap32(unsigned int x);

/* fill `dest` with a 32 bit pattern; the CRT memset only does bytes */
void memset32(void *dest, int value, int count);

/* sub_409560: decode `fileimg` (any supported format) into 32bpp pixels */
int  DecodeImage(unsigned char *fileimg, int filesize, unsigned char *pixels,
                 int pitch, unsigned int width, unsigned int height,
                 int unused7, int unused8);
/* sub_402AD0: the optimiser; returns the size of the smallest PNG it found */
int  Optimize(SETTINGS *s);

/* ------------------------------------------------------------------ */
/*  Globals that live in the initialised part of .data                 */
/* ------------------------------------------------------------------ */

extern FILE     *Stream;            /* 0x411120 */
extern HANDLE    hObject;           /* 0x411010 */
extern FILETIME  CreationTime;      /* 0x411118 */
extern FILETIME  LastWriteTime;     /* 0x411124 */

/* 0x411130: offsets of the chunks ScanChunks decided to keep */
extern unsigned int ChunkOffsets[MAX_COLORS];   /* dword_411130 */
extern int ChunkCount;              /* dword_411548 */
extern int ChunkCountIHDR;          /* dword_411540 */
extern int ChunkCountPLTE;          /* dword_411544 */
extern int unk_41153C;              /* dword_41153C */

/* 0x411538: /kt - 0 = no, 1 = remember the timestamps, 2 = stamped written */
extern int KeepFileTime;

/* 0x40FAD4: four zero bytes; main() uses it as the default /k chunk list */
extern char EmptyChunkList[];

/* 0x41102C: 16-entry bit-reversed ordering table {0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15} */
extern int Bitrev16[16];
extern int Bitrev19[19];            /* dword_411080 */
extern int ThreeBytes[3];           /* dword_411020 = {16,17,18} */
extern int Limit411070, Limit411074, Limit411078;   /* all 128 */

/* 0x4110CC: byte table {0xFE,0xFF,0xFF,0xFF,1,2,4,8,0x10,0x20,0x40,0x80} */
extern unsigned char BitMaskTable[12];

/* 0x4110D8: 8 hash seeds */
extern void *HashSeeds[8];
/* 0x4110F8: indirect hook, initialised to sub_408D80 */
extern unsigned int (*sub_4110F8)(unsigned int, int, int);

/* ------------------------------------------------------------------ */
/*  Large uninitialised (.bss) arrays                                  */
/* ------------------------------------------------------------------ */

extern int  unk_190A3FC;            /* set |= 0xFF000000 when tRNS seen  */
extern int  unk_190A820, unk_190A1D0;

#endif /* PNGOUT_H */
