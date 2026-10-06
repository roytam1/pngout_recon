/*
 *  chunk.c   -   PNG chunk handling
 *
 *  0x4026E0  ScanChunks()  - walk the chunk list of an input PNG, decide which
 *                            chunks survive, and (with /v) print the table.
 *  0x409260  FindTRNS()    - pull width/height out of IHDR, notice tRNS.
 *  0x409540  Swap16()      - byte swap a 16 bit word.
 *
 *  Chunk layout on disk is  length:4  type:4  data:length  crc:4, and both
 *  the length and the CRC are big endian.
 */

#include <stdio.h>
#include <string.h>

#include "pngout.h"

/* Chunk tags as they compare in the disassembly: the raw big endian DWORD. */
#define TAG(a, b, c, d)   ((unsigned int)(d) << 24 | (unsigned int)(c) << 16 | \
                           (unsigned int)(b) << 8  | (unsigned int)(a))

#define TAG_IHDR   TAG('I', 'H', 'D', 'R')
#define TAG_PLTE   TAG('P', 'L', 'T', 'E')
#define TAG_IDAT   TAG('I', 'D', 'A', 'T')
#define TAG_IEND   TAG('I', 'E', 'N', 'D')
#define TAG_tRNS   TAG('t', 'R', 'N', 'S')

/*
 *  The signature is compared as the two DWORDs the original compares:
 *  bytes 89 50 4E 47 read little endian is 0x474E5089 (1196314761), and
 *  0D 0A 1A 0A is 0x0A1A0A0D (169478669).
 */
#define PNG_SIG0   0x474E5089u
#define PNG_SIG1   0x0A1A0A0Du

/* Colour types that already have a real alpha channel, so tRNS is useless. */
#define COLTYPE_GRAY_ALPHA  4
#define COLTYPE_RGB_ALPHA   6

/* dword_411538: 0 = off, 1 = remember timestamps, 2 = timestamps written. */
int      KeepFileTime;

/* dword_411130: addresses of the chunks the /k list asked us to keep. */
unsigned int ChunkOffsets[MAX_COLORS];
int      ChunkCount;          /* dword_411548 */
int      ChunkCountIHDR;      /* dword_411540 */
int      ChunkCountPLTE;      /* dword_411544 */
int      unk_41153C;          /* dword_41153C */

/* 0x40FAD4: four zero bytes, main()'s default /k argument. */
char EmptyChunkList[] = { 0, 0, 0, 0 };

/* ------------------------------------------------------------------ */
/*  0x409540 - Swap16                                                 */
/*                                                                     */
/*  (x << 8) + HIBYTE(x).  PNG stores both the chunk length and the   */
/*  chunk CRC byte swapped relative to the host, so this is on the    */
/*  hot path for every chunk we walk.                                  */
/* ------------------------------------------------------------------ */

unsigned short Swap16(unsigned short x)
{
    return (unsigned short)(((x << 8) & 0xFF00) | (x >> 8));
}

/*
 *  The MSVC intrinsic the original calls through the IAT.  PNG stores the
 *  chunk length, the IHDR width and height and the chunk CRC as 4 byte big
 *  endian values, so these all need the 32 bit version rather than Swap16.
 *  JPEG's SOF dimensions are the 16 bit one.
 */
static unsigned int Swap32(unsigned int x)
{
    return ((x & 0x000000FFu) << 24) | ((x & 0x0000FF00u) <<  8) |
           ((x & 0x00FF0000u) >>  8) | ((x & 0xFF000000u) >> 24);
}

/* ------------------------------------------------------------------ */
/*  0x4026E0 - ScanChunks                                              */
/*                                                                     */
/*  `keeplist` is the text following /k(.  "1" or "ALL" keeps every   */
/*  ancillary chunk; otherwise it is a comma separated list of four   */
/*  character chunk names.  It is modified in place: strtok() writes   */
/*  NULs over the commas and we put the commas back, exactly as the   */
/*  original does.                                                     */
/*                                                                     */
/*  Only chunks the user actually named get recorded in ChunkOffsets. */
/*  The two counters classify *where* in the file the chunk appeared,  */
/*  using the most recent critical chunk that was seen:               */
/*      1  = between IHDR and PLTE   (gAMA, sRGB, ... - PLTE still    */
/*                                     required afterwards)             */
/*      2  = between PLTE and IDAT   (bKGD, hIST - also needs PLTE)    */
/*      3  = after IDAT              (tEXt, tIME, ...)                 */
/*  That is what the original's reuse of one unreset variable does.    */
/*                                                                     */
/*  The original's return value is the result of the last Msg() call   */
/*  and nothing ever looks at it, so this returns void.                */
/* ------------------------------------------------------------------ */

void ScanChunks(unsigned char *fileimg, int filesize, char *keeplist,
                int coltype, int listmode)
{
    unsigned char *p;
    unsigned char *end;
    int trnsseen = 0;
    int class = 0;                        /* the original's unreset v14 */

    unk_41153C     = 0;
    ChunkCountIHDR = 0;
    ChunkCountPLTE = 0;
    ChunkCount     = 0;

    if (*keeplist || listmode) {
        if (fileimg != NULL &&
            *(unsigned int *)fileimg       == PNG_SIG0 &&
            *(unsigned int *)(fileimg + 4) == PNG_SIG1) {

            if (listmode)
                Msg("                Name  Offset   Length     CRC\n");

            p   = fileimg + 8;
            end = fileimg + filesize;

            while (p < end) {
                unsigned int tag = *(unsigned int *)(p + 4);
                unsigned int len = Swap32(*(unsigned int *)p);
                int keep;

                switch (tag) {
                case TAG_IHDR: class = 1; keep = 0; goto critical;
                case TAG_PLTE: class = 2; keep = 0; goto critical;
                case TAG_IDAT: class = 3; keep = 0; goto critical;
                case TAG_tRNS:             keep = 0; goto critical;
                case TAG_IEND:             keep = 0; goto critical;
                default:                    keep = 0; break;
                }

                /* Only ancillary chunks are looked up in the /k list. */
                if (!*keeplist)
                    goto critical;

                if (!strcmp(keeplist, "1") || !stricmp(keeplist, "ALL")) {
                    keep = class;
                } else {
                    /*
                     *  The original uses strtok() and puts each separator
                     *  back with tok[strlen(tok)] = ',', then calls strtok
                     *  again - which steps one byte past the terminating
                     *  NUL after the final token.  Harmless with main()'s
                     *  260 byte buffer, but stopping when the last token has
                     *  been undone gives the same answer without reading or
                     *  writing outside the caller's buffer.
                     */
                    size_t total = strlen(keeplist);
                    char  *tok = strtok(keeplist, ",");

                    while (tok != NULL) {
                        size_t n = strlen(tok);

                        if (n == 4 && *(unsigned int *)tok == tag)
                            keep = class;
                        tok[n] = ',';                 /* undo strtok */
                        if ((size_t)(tok - keeplist) + n + 1 >= total)
                            break;                    /* that was the last one */
                        tok = strtok(NULL, ",");
                    }
                }

                if (keep <= 0 || ChunkCount >= MAX_COLORS)
                    goto critical;

                /*
                 *  The original compares the tag here against 0x44415452,
                 *  which reads back as "RTAD" and is not a PNG chunk type,
                 *  so this flag is never actually raised.  Kept as-is so
                 *  the behaviour is identical.
                 */
                if (tag == 0x44415452u)
                    trnsseen = 1;

                ChunkOffsets[ChunkCount] = (unsigned int)p;

                if (keep == 1)
                    ChunkCountIHDR++;
                else if (keep == 2)
                    ChunkCountPLTE++;
                ChunkCount++;

            critical:
                if (listmode) {
                    if (tag == TAG_PLTE) {
                        if (coltype >= 0 && coltype != 3)
                            Msg("  Remove chunk: ");
                        else if (keep == 0)
                            Msg("Required chunk: ");
                        else
                            Msg("    Keep chunk: ");
                    } else if (tag == TAG_tRNS &&
                               (coltype == COLTYPE_GRAY_ALPHA ||
                                coltype == COLTYPE_RGB_ALPHA)) {
                        Msg("  Remove chunk: ");
                    } else if (keep == 0) {
                        Msg("Required chunk: ");
                    } else {
                        Msg("    Keep chunk: ");
                    }

                    Msg("%c%c%c%c %08x %08x %08x\n",
                        tag & 0xFF, (tag >> 8) & 0xFF,
                        (tag >> 16) & 0xFF, (tag >> 24) & 0xFF,
                        (int)(p - fileimg + 8),
                        len,
                        (int)Swap32(*(unsigned int *)(p + len + 8)));
                }

                if (tag == TAG_IEND)
                    break;
                p += len + 12;
            }

            if (listmode)
                Msg("\n");
        }
    }

    if (trnsseen)
        unk_190A3FC |= (int)0xFF000000u;
    else
        unk_190A3FC = 0;
}

/* ------------------------------------------------------------------ */
/*  0x409260 - GetImageSize                                           */
/*                                                                     */
/*  Sniffs the container format and reads the dimensions out of the    */
/*  header.  Returns a format code, or 0 when the file is not one of   */
/*  the formats PNGOUT understands:                                   */
/*                                                                     */
/*      1  PNG   "\x89PNG" + signature, walk to the first IHDR       */
/*      2  JPEG  FFD8, walk the marker chain to SOF0/SOF1/SOF2        */
/*      3  GIF   "GIF", little endian width@6 height@8                */
/*      4  8bpp palettised image, magic 19 91, 32 byte header and a  */
/*         256 entry RGB palette at offset 32.  PNGOUT's help text    */
/*         does not list this format, and I could not identify it.    */
/*      5  BMP   "BM", both BITMAPCOREHEADER and BITMAPINFOHEADER     */
/*      6  PCX   0A 05 01 08 (v5, RLE, 8bpp), bounds at 4..0A         */
/*      7  DDS   "DDS " with a 124 byte header, height@0C width@10     */
/*      8  Targa, which has no magic, so it is guessed from the       */
/*         header fields being self consistent                          */
/*                                                                     */
/*  The original returns the format code but main() only ever compares */
/*  it against 1, so the codes are kept as-is.                         */
/* ------------------------------------------------------------------ */

#define FMT_NONE     0
#define FMT_PNG      1
#define FMT_JPEG     2
#define FMT_GIF      3
#define FMT_PAL8     4
#define FMT_BMP      5
#define FMT_PCX      6
#define FMT_DDS      7
#define FMT_TGA      8

int GetImageSize(unsigned char *fileimg, int filesize, int *width, int *height)
{
    unsigned char *p;
    unsigned char *end;
    int i;

    *width = *height = 0;

    if (filesize < 16)
        return FMT_NONE;

    /* ---------------- PNG ---------------- */
    if (fileimg[0] == 0x89 && fileimg[1] == 0x50 &&
        *(unsigned int *)fileimg       == PNG_SIG0 &&
        *(unsigned int *)(fileimg + 4) == PNG_SIG1) {

        p   = fileimg + 8;
        end = fileimg + filesize;
while ((unsigned int)(p - fileimg) < (unsigned int)(filesize - 16)) {
                unsigned int len;

                if (*(unsigned int *)(p + 4) == TAG_IHDR) {
                    *width  = (int)Swap32(*(unsigned int *)(p + 8));
                    *height = (int)Swap32(*(unsigned int *)(p + 12));
                    return FMT_PNG;
                }
                len = Swap32(*(unsigned int *)p);
                p += len + 12;
            }
        return FMT_NONE;
    }

    /* ---------------- JPEG ---------------- */
    if (fileimg[0] == 0xFF && fileimg[1] == 0xD8) {
        p   = fileimg + 2;
        end = fileimg + filesize;
        while ((unsigned int)(p - fileimg) < (unsigned int)(filesize - 8)) {
            unsigned int skip;

            if (p[0] != 0xFF || p[1] == 0xFF) {
                p++;                                /* resynchronise */
            } else if ((unsigned int)(p[1] - 0xC0) < 3) {
                /* SOF: precision at +4, height at +5, width at +7 */
                *height = (int)Swap16(*(unsigned short *)(p + 5));
                *width  = (int)Swap16(*(unsigned short *)(p + 7));
                return FMT_JPEG;
            } else {
                skip = (unsigned int)Swap16(*(unsigned short *)(p + 2));
                p += skip + 2;
            }
        }
        return FMT_NONE;
    }

    /* ---------------- GIF ---------------- */
    if (fileimg[0] == 'G' && fileimg[1] == 'I' && fileimg[2] == 'F') {
        *width  = *(unsigned short *)(fileimg + 6);
        *height = *(unsigned short *)(fileimg + 8);
        return FMT_GIF;
    }

    /* ---------------- 8bpp palettised ---------------- */
    if (fileimg[0] == 0x19 && fileimg[1] == 0x91 &&
        fileimg[10] == 8 && fileimg[11] == 0) {
        *width  = *(unsigned short *)(fileimg + 2);
        *height = *(unsigned short *)(fileimg + 4);
        return FMT_PAL8;
    }

    /* ---------------- BMP ---------------- */
    if (fileimg[0] == 'B' && fileimg[1] == 'M') {
        if (*(unsigned short *)(fileimg + 14) == 12 &&
            *(unsigned short *)(fileimg + 22) == 1) {       /* BITMAPCOREHEADER */
            *width  = *(unsigned short *)(fileimg + 18);
            *height = *(unsigned short *)(fileimg + 20);
            return FMT_BMP;
        }
        if (*(unsigned short *)(fileimg + 26) == 1) {       /* biPlanes */
            *width  = *(int *)(fileimg + 18);
            *height = *(int *)(fileimg + 22);
            return FMT_BMP;
        }
        return FMT_NONE;
    }

    /* ---------------- PCX ---------------- */
    if (*(unsigned int *)fileimg == 0x0801050Au) {
        *width  = (short)(*(short *)(fileimg + 8) - *(short *)(fileimg + 4)) + 1;
        *height = (short)(*(short *)(fileimg + 10) - *(short *)(fileimg + 6)) + 1;
        return FMT_PCX;
    }

    /* ---------------- DDS ---------------- */
    if (*(unsigned int *)fileimg == 0x20534444u &&
        *(unsigned int *)(fileimg + 4) == 124u) {
        *width  = *(int *)(fileimg + 16);
        *height = *(int *)(fileimg + 12);
        return FMT_DDS;
    }

    /* ---------------- Targa ---------------- */
    if (filesize >= 19 &&
        (fileimg[1] & 0xFE) == 0 &&              /* colour map type 0 or 1 */
        fileimg[2] < 12 &&
        ((1 << fileimg[2]) & 0xE0E) != 0 &&     /* a plausible image type */
        (fileimg[16] & 7) == 0 &&               /* pixel depth: 8/16/24/32 */
        fileimg[16] != 0 && fileimg[16] <= 32 &&
        (fileimg[17] & 0xC0) == 0) {           /* sane image descriptor */
        *width  = *(unsigned short *)(fileimg + 12);
        *height = *(unsigned short *)(fileimg + 14);
        return FMT_TGA;
    }

    (void)i;
    return FMT_NONE;
}
