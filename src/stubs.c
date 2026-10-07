/*
 *  stubs.c   -   entry points that are not reconstructed yet
 *
 *  DecodeImage() - sub_409560, the PNG / JPEG / GIF / BMP / PCX / DDS / Targa
 *  decoders - now lives in src/decode.c, which implements the PNG path and
 *  refuses the other six rather than guessing at them.
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