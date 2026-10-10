/*
 *  stubs.c   -   entry points that are not reconstructed yet
 *
 *  Nothing decodable is stubbed any more: DecodeImage() - sub_409560, the
 *  PNG / JPEG / GIF / BMP / PCX / DDS / Targa / 8bpp-palettised decoders -
 *  lives in src/decode.c, which implements all eight paths (JPEG: sequential
 *  with and without restart, plus progressive; interlaced PNG, subset JPEG
 *  scans, DRI+progressive and exotic sampling refuse cleanly rather than
 *  guessing).  Its dispatch is the original's, so the format tests are the
 *  real ones.
 */

#include <stdio.h>
#include <stdlib.h>

#include "pngout.h"

/* ---------------------------------------------------------------- */
/*  0x40E584 - the MSVC user math error handler.  VC6's startup      */
/*  (_setusermatherr in start+7C) picks this up by name.             */
/* ---------------------------------------------------------------- */

int _UserMathErrorFunction(void)
{
    return 0;
}