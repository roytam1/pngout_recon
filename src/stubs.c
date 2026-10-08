/*
 *  stubs.c   -   entry points that are not reconstructed yet
 *
 *  DecodeImage() - sub_409560, the PNG / JPEG / GIF / BMP / PCX / DDS / Targa /
 *  8bpp-palettised decoders - lives in src/decode.c, which implements the PNG
 *  and 8bpp paths (loc_40D150) and refuses the other six rather than guessing at
 *  them.  Its dispatch is still the original's, so the format tests are the real
 *  ones and only the decode itself is missing.
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