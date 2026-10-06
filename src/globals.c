#include "pngout.h"

/*
 *  globals.c   -   the .data segment
 *
 *  Everything here is a static of the original binary.  The comment on each
 *  one gives the address it lived at in pngout.exe so the layout can be
 *  checked against the IDA listing.
 *
 *  Only the initialised part of .data is reconstructed so far; the 22MB of
 *  uninitialised arrays that the optimiser and the deflate engine use are
 *  declared as the corresponding modules are recovered.
 */

/* ------------------------------------------------------------------ */
/*  initialised data, 0x411010 - 0x41112C                              */
/* ------------------------------------------------------------------ */

HANDLE    hObject      = INVALID_HANDLE_VALUE;  /* 0x411010 */

static int unk_411018;               /* 0x411018: FFFFFFFF then 00000000 */

int ThreeBytes[3]      = { 16, 17, 18 };                   /* 0x411020 */

/*
 *  0x41102C: 16 entries.  Read as "index of the i'th shortest code length"
 *  when turning a set of code lengths into canonical codes.
 */
int Bitrev16[16]       = { 0, 8, 7, 9, 6, 10, 5, 11,
                           4, 12, 3, 13, 2, 14, 1, 15 };

static int unk_41106C;               /* 0x41106C: zero */

int Limit411070        = 128;         /* 0x411070 */
int Limit411074        = 128;         /* 0x411074 */
int Limit411078        = 128;         /* 0x411078 */

/* 0x411080: the 16 entry table above with {16,17,18} pushed in front. */
int Bitrev19[19]       = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4,
                           12, 3, 13, 2, 14, 1, 15 };

/* 0x4110CC: sentinel, then the single bit masks. */
unsigned char BitMaskTable[12] = { 0xFE, 0xFF, 0xFF, 0xFF,
                                   0x01, 0x02, 0x04, 0x08,
                                   0x10, 0x20, 0x40, 0x80 };

/* 0x4110D8: seeds for the block/filter trial, all pointing into .bss. */
void *HashSeeds[8];

/* 0x4110F8: indirect hook, initialised to the function at 0x408D80. */
unsigned int (*sub_4110F8)(unsigned int, int, int);

static int Fill4110FC[20] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                              -1, -1, -1, -1, -1, -1, 0, 0, 0, 0 };

FILE     *Stream         = NULL;      /* 0x411120, -> &iob[2] from main() */
FILETIME  CreationTime;              /* 0x411118 */
FILETIME  LastWriteTime;             /* 0x411124 */

/* ------------------------------------------------------------------ */
/*  read-only tables, .rdata                                          */
/* ------------------------------------------------------------------ */

/*
 *  0x4100D8: 64 dwords laid out as one run -
 *      [0..30]  (1 << n) - 1      low n bits set
 *      [31]     0x7FFFFFFF        both the 31 low bits and bit 31
 *      [32..62] 1 << (n - 32)
 *  so it doubles as a mask table and as a single-bit table.
 */
const int BitTable[64] = {
    0,          1,          3,          7,          15,
    31,         63,         127,        255,        511,
    1023,       2047,       4095,       8191,       16383,
    32767,      65535,      131071,     262143,     524287,
    1048575,    2097151,    4194303,    8388607,    16777215,
    33554431,   67108863,   134217727,  268435455,  536870911,
    1073741823, 2147483647,
    1,          2,          4,          8,          16,
    32,         64,         128,        256,        512,
    1024,       2048,       4096,       8192,       16384,
    32768,      65536,      131072,     262144,     524288,
    1048576,    2097152,    4194304,    8388608,    16777216,
    33554432,   67108864,   134217728,  268435456,  536870912,
    1073741824, 2147483648
};

/* ------------------------------------------------------------------ */
/*  published by the input decoders, read by main()                    */
/* ------------------------------------------------------------------ */

unsigned char OutColType;        /* 0x190A824: colour type we will write    */
unsigned char OutBitDepth;       /* 0x190A825: bit depth we will write      */
int NumBlocks;                   /* 0x190A400: how many deflate blocks the
                                 *          best trial used                 */
int dword_190A420[6];            /* 0x190A420: table filled in by the
                                 *          decoders                       */
int unk_190A820;                 /* 0x190A820: number of palette entries    */
int unk_190A1C4;                 /* 0x190A1C4: height of the tRNS chunk    */
int unk_190A828;                 /* 0x190A828: 24184 marks a PNG we wrote  */
int unk_190A3FC;                 /* 0x190A3FC: |0xFF000000 once a tRNS
                                 *          chunk survives                  */

/* ------------------------------------------------------------------ */
/*  state the optimiser and the deflate bit writer share               */
/* ------------------------------------------------------------------ */

IMAGEDESC *Image;              /* 0x42E960: the image being optimised      */
int       OutCount;            /* 0x185BECC: bytes emitted so far          */
int       BitCount;            /* 0x412670: BITS pending, not bytes        */
unsigned int BitBuf;           /* 0x412674: the bit accumulator            */
unsigned int BitCrc;           /* 0x412EB8: running CRC of the output     */

/* s->msg, published so the optimiser can report progress */
void (*ProgressMsg)(const char *fmt);          /* 0x185BEC4 */

/* block state, set up by EmitBlock() */
int      BlockSymbols;         /* 0x41266C */
int      LinBufBytes;          /* 0x411D54 */
int      LinBufUsed;           /* 0x411D50 */
int      BitBufPos;            /* 0x18CBED8 */
unsigned char *LinBuf;         /* 0x18CBEE0, the LZ work buffer           */
int      ChunkStart;           /* 0x412160, where the IDAT header starts  */
/* 0x412E78: the chunk staging area - a length, a tag and up to 29 bytes
   of data, with the CRC written over the top of the tail */
unsigned char Scratch[64];
int      RowIndex;             /* 0x42D7CC */
int      RowCount;             /* 0x6D5570 */
int      AdlerSum;             /* 0x706B84: Adler-32 of the unfiltered data */
int      RowHeaderCode;        /* 0x6F6A78 */
int      RowCost;              /* 0x18CBEEC */
int      LensScratch[ROW_SAMPLE_MAX];    /* 0x412168 */

/* the per byte filter callback, src/optimize.c */
ByteSinkFn ByteSink;           /* 0x6D6668 */
int      TrialFilter;          /* 0x6D5664 */
int      FilterLeft;           /* 0x6D496C, the previous raw byte */
int      FilterHistogram[256]; /* 0x42E560: unk_42E55C is four bytes below */
int      FilterByteCount;      /* 0x180A568 */
void   (*BlockFullHook)(void); /* where FilterByte()'s jump to loc_4075B0 goes */
void   (*FlushLz)(void);      /* the LZ coder's own flush, sub_4065A0(0, 0) */

/* the Huffman work area, src/optimize.c */
int      Freq[HUFF_MAXSYMS];           /* 0x42CEC8 */
int      Heap[HUFF_MAXSYMS * 2];       /* 0x42D7D0 (0x42D7D4 is Heap[1]) */
int      HeapCount;                    /* 0x42E558 */
int      Left[HUFF_MAXSYMS * 2];       /* 0x42DC58 */

unsigned CrcTable[256];                /* 0x185BAB8 */
int      ColorHashHead[HASH_BUCKETS];   /* 0x1806F88 */
int      ColorHashNext[MAX_COLORS];     /* 0x1806B88 */
int      ColorHashCount;                /* 0x180BA88 */
int     *ColorHashPalette;              /* 0x180A56C */

/* deflate block state */
int      CodePairs[CODE_PAIRS * 2];  /* 0x1809390: 59 (extra, base) pairs */
unsigned char LenCodeToSlotStore[259];    /* 0x180B97D .. 0x180BA7F */
unsigned char DistCodeToSlot[32768];    /* 0x414EC0 */
int      LenHuffIndex;                  /* 0x1809560 */
int      LenHuffCount;                  /* 0x1809564 */
int      DistTablesDone;                /* 0x18CBEF0 */

/* 0x412678 and 0x41267C are one array of 512 dwords; see the macros in
   pngout.h.  MatchCost[] is it, and CurMatch/HashPrev are entry 0 and the
   other 511. */
int      MatchCost[512];               /* 0x412678 */
int      MatchLimit;                   /* 0x412668 */
int      HashHead[4096];                /* 0x412EC0, used as shorts */
int      DistSlot[32768];               /* 0x41CEC8, used as shorts */
int      MatchArea[0x8000];             /* 0x6B4968, the LZ work area */

/* the /b block list, 0x42F364: five fields per block record */
int      BlockRecord[MAX_BLOCKS * BLOCK_STRIDE];
int      NumBlockList;                /* dword_6D5570 */
unsigned char *LinBufData;            /* dword_18CBEE4: LinBuf + 4 */

/* per row match slots */
int      DistSlots[64];                 /* 0x6D5578 */
unsigned char SlotOrder[32];              /* 0x185BA90 */
int      LitCodes[286];                 /* 0x6F6678 */
int      DistCodes[32];                /* 0x6F6AF8 */

/* the LZ coder, src/lz.c */
unsigned char LzWindow[65540];           /* 0x6F6B80, +0x10000 is 0x706B80 */
int      LzWrite;                       /* 0x6D4968 */
int      LzScan;                        /* 0x6F6B78 */
int      RunEnd;                        /* 0x41CEC0 */
int      RunCount;                      /* 0x411D58 */
unsigned short RunLen[65536];           /* 0x185BED8 */
int      MatchStart[32768];             /* 0x6D6670 */
int      PrevReach;                     /* 0x185BEC8 */
int      PrevMark;                      /* 0x185BEB8 */
int      MatchList[0x80000];            /* 0x706B88 */
unsigned short StepTable[LZ_STEPCAP + 260]; /* 0x806B82 */
int      BestCost[256];                 /* 0x6D5170 */

/* 0x42E560: the histogram the filter trials fill, 256 dwords */
int      Entropy[256];
int      CurFilter;                   /* dword_185BEBC: 0 .. 4, the filter in use */
int      EntropyMax;                   /* dword_180A568: the largest count in it   */
int     *EntropyLut;                   /* dword_18CBEE8: count * ln(count) * scale */
int      EntropyLutSize;               /* dword_18CBEE4                        */

int     (*CheckEscHook)(int);            /* dword_6F6670: polled by the trials  */
int      MinSample;                      /* dword_185BEC0: darkest sample in the image */
int      unk_185BEC8, unk_180BA84, unk_185BEB8, unk_411D58, unk_41CEC0;
int      unk_42D7C8;
unsigned char unk_42E55C;

/* ------------------------------------------------------------------ */
/*  the row records, 0x42E958                                            */
/* ------------------------------------------------------------------ */

/*
 *  One 2584 dword record per row, with the fields at the offsets the original
 *  puts them at - see ROW_STRIDE and friends in pngout.h.  It is a single
 *  allocation because the fields overlap on purpose.
 */
int      RowRecord[MAX_ROWS * ROW_STRIDE];
