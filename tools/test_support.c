/*
 *  test_support.c   -   the few globals the chunk layer needs, so the unit
 *  test can link without dragging in main() or the optimiser stubs.
 */

#include <stdio.h>
#include <stdarg.h>

#include "pngout.h"

FILE *Stream;
int   unk_190A3FC;

void Msg(const char *fmt, ...)
{
    va_list ap;

    if (Stream == NULL)
        return;
    va_start(ap, fmt);
    vfprintf(Stream, fmt, ap);
    va_end(ap);
}