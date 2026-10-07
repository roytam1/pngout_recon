/*
 *  stubs.c   -   entry points that are not reconstructed yet
 *
 *  PNGOUT is one large engine left on top of a driver that is finished:
 *
 *    sub_409560  DecodeImage()  - PNG / JPEG / GIF / BMP / PCX / DDS / TGA
 *                                decoders plus the tRNS and bit depth
 *                                analysis.  ~4.4KB of x86.
 *
 *  It aborts loudly rather than quietly producing a wrong PNG, so nothing built
 *  from this tree can be mistaken for a working reconstruction.
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

/* ---------------------------------------------------------------- */
/*  0x40E584 - the MSVC user math error handler.  VC6's startup
    (_setusermatherr in start+7C) picks this up by name.      */
/* ---------------------------------------------------------------- */


int _UserMathErrorFunction(void)
{
    return 0;
}
