/*
 *  stubs.c   -   entry points that are not reconstructed yet
 *
 *  PNGOUT is two large engines on top of the driver that is finished:
 *
 *    sub_409560  DecodeImage()  - PNG / JPEG / GIF / BMP / PCX / DDS / TGA
 *                                decoders plus the tRNS and bit depth
 *                                analysis.  ~4.4KB of x86.
 *    sub_402AD0  Optimize()     - the trial and error search over filters,
 *                                block splits and Huffman tables that is
 *                                the whole point of the program.
 *                                ~8.9KB of x86.
 *
 *  The three functions below are the guts of the optimiser that EmitBlock()
 *  in src/optimize.c drives:
 *
 *    sub_405B80  CompressBlock()   - the actual LZ77 + entropy coding
 *    sub_4058A0  InitDistanceTables() - deflate distance base/extra tables
 *    sub_4053D0  FlushBlock()      - writes the filtered rows out
 *
 *  All of them abort loudly rather than quietly producing a wrong PNG, so
 *  nothing built from this tree can be mistaken for a working
 *  reconstruction.
 */

#include <stdio.h>
#include <stdlib.h>

#include "pngout.h"

static void NotReconstructed(const char *what, const char *addr)
{
    printf("\n*** %s (originally at %s) has not been reconstructed yet.\n"
           "*** This build can parse the command line but cannot process an image.\n"
           "*** See README.md for the remaining work list.\n\n",
           what, addr);
    exit(3);
}

int DecodeImage(unsigned char *fileimg, int filesize, unsigned char *pixels,
                int pitch, unsigned int width, unsigned int height,
                int unused7, int unused8)
{
    (void)fileimg; (void)filesize; (void)pixels; (void)pitch;
    (void)width; (void)height; (void)unused7; (void)unused8;
    NotReconstructed("DecodeImage()", "0x409560");
    return -1;
}

int Optimize(SETTINGS *s)
{
    (void)s;
    NotReconstructed("Optimize()", "0x402AD0");
    return 0x7FFFFFFF;
}

int CompressBlock(unsigned Number, unsigned symbols, int bits, int bitspersym,
                  int a5, int a6)
{
    (void)Number; (void)symbols; (void)bits; (void)bitspersym;
    (void)a5; (void)a6;
    NotReconstructed("CompressBlock() [sub_405B80]", "0x405B80");
    return 0;
}

void InitDistanceTables(void)
{
    NotReconstructed("InitDistanceTables() [sub_4058A0]", "0x4058A0");
}

void FlushBlock(unsigned char *rows)
{
    (void)rows;
    NotReconstructed("FlushBlock() [sub_4053D0]", "0x4053D0");
}
/* ---------------------------------------------------------------- */
/*  0x40E584 - the MSVC user math error handler.  VC6's startup
   (_setusermatherr in start+7C) picks this up by name.      */
/* ---------------------------------------------------------------- */

int _UserMathErrorFunction(void)
{
    return 0;
}
