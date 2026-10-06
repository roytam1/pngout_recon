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
 *  sit at these offsets inside one:
 *
 *      +0x000  dword_42E958   one dword, set to 0x7FFFFFFF per block
 *      +0x010  dword_42E968   320 dwords: the row's samples
 *      +0x410  dword_42ED68   one dword: a counter the row coder bumps
 *      +0x44C  dword_42ECF4   30 dwords: a counter per length code
 *      +0x480  dword_42EDE8   30 dwords: frequency per distance code
 *      +0x510  dword_42EE68   318 dwords: the symbol stream sub_4053D0 eats
 *      +0x89C  dword_42F1F4   bits per length symbol, then 29 more entries
 *      +0x910  dword_42F268   one dword: how many bits the row needs
 *      +0x990  dword_42F2E8   30 dwords: bits per distance symbol
 *
 *  The record is 0xA18 dwords but only the first ~0x930 of them are touched,
 *  so the small fields are modelled here with a compact stride of their own.
 *  Nothing in the reconstruction relies on the fields overlapping.
 */
#define ROW_STRIDE     2584            /* 0xA18 dwords per row record */
#define MAX_ROWS       1024            /* the original's .bss allows 1331 */
#define ROW_SLOTS      32              /* 30 slots + slack                */

/*  the symbol stream at field +0x510, i.e. sub_4053D0's argument layout */
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
/*  Reached through dword_42E960.  sub_4052D0 and sub_405320 use only  */
/*  the output buffer at +0x10 and its size at +0x14; sub_402AD0's      */
/*  image scan reads +0x00, +0x04, +0x08, +0x0C, +0x30 and +0x74.     */
/* ------------------------------------------------------------------ */

typedef struct ImageDescStruct {
    unsigned int *pixels;   /* +00  row data, one plane after another     */
    int           stride;   /* +04  bytes between planes                  */
    int           nplanes;  /* +08  entries in the current plane          */
    int           nrows;    /* +0C  number of planes in the image         */

    unsigned char *outbuf;  /* +10  the PNG being assembled               */
    int           outsize;  /* +14  its size in bytes                     */

int           pad0[3];  /* +18 .. +20 */
    unsigned char *unused; /* +24 */
    int           pad1;     /* +28 */
    int           ndistinct;/* +34  how many distinct values the image has */
    int           pad2[15]; /* +38 .. +70 */

    int           trial;    /* +74  which filter trial is running        */
    int           pad3[3];  /* +78 .. +84 */

    int           strategy; /* +4C  0 Xtreme, 1 Intense, 2 Longest Match,
                               3 Huffman only, 4 uncompressed          */
    int           pad4[16]; /* +50 .. +70 */

    int           pad5[17]; /* +84 .. +C4 */

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
int  EmitBlock(unsigned Number, unsigned symbols, int bits, int bitspersym,
              int a5, int a6);
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
extern int      Scratch;          /* dword_412E78, a 4 byte scratchpad */
extern int      RowIndex;         /* dword_42D7CC */
extern int      RowCount;         /* dword_6D5570 */
extern int      CrcPending;       /* dword_706B84 */

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

/* --- the symbol stream one row is coded from ------------------------ */
/*  0x42EE68, field +0x510 of the row record: 256 literal slots, then 30
 *  length slots at +256, then 30 distance slots at +288.  sub_4053D0 eats
 *  it; see FlushRow() in src/optimize.c. */
extern int      ImageBuffer[MAX_ROWS * ROW_STRIDE];

/* --- deflate block state, src/optimize.c -------------------------- */
extern int      LenPairs[60];   /* 0x1809390: 30 (base, extra) pairs */
extern int      DistPairs[56];  /* 0x1809480: 28 (base, extra) pairs */
extern unsigned char LenCodeToSlot[256];
extern unsigned char DistCodeToSlot[32768];
extern int      LenHuffIndex, LenHuffCount, DistTablesDone;

extern unsigned CurMatch;        /* 0x412678 */
extern int      HashPrev[511];   /* 0x41267C */
extern int      HashHead[4096];  /* 0x412EC0, used as shorts */
extern int      DistSlot[32768]; /* 0x41CEC8, used as shorts */
extern int      MatchArea[0x8000];   /* 0x6B4968 */
extern int      RowSlot[(MAX_ROWS + 1) * ROW_SLOTS];  /* 0x42E958 */

extern int      unk_185BEC8, unk_180BA84, unk_185BEB8, unk_411D58, unk_41CEC0;
extern int      unk_6D4968, unk_6F6B78, unk_412668, unk_42D7C8, unk_18CBEEC;
extern unsigned char unk_42E55C;

int  BuildDeflateTables(void);
int  InitBlockTables(void);
int  BuildDistanceSlots(int row);
extern int      DistSlots[64];        /* dword_6D5578 */
extern int      DistWork[MAX_ROWS * ROW_STRIDE];  /* dword_42E968 */
extern int      DistSymBits[(MAX_ROWS + 1) * ROW_SLOTS]; /* dword_42F2E8 */
extern int      LenSymBits[(MAX_ROWS + 1) * ROW_SLOTS];  /* dword_42F1F4 */
extern unsigned char SlotOrder[32];      /* byte_185BA90 */
extern int      FreqA, FreqB;          /* dword_6F6678, dword_6F6AF8 */
int  RankValues(int *vals, int count, int *out);
void FlushRow(int *row);

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
