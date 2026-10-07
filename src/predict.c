/*
 *  predict.c   -   the row predictor used by PNGOUT's own filters
 *
 *  0x40AF40  FilterPredict()
 *
 *  Used by sub_4086E0 (the "minimum sum" filter) and by sub_40A8D0.  It is
 *  *not* the Paeth predictor from the PNG spec: instead of clamping
 *  p = a + b - c it picks whichever of the three candidates is closest, and
 *  the three distances it compares are not the Paeth ones either.
 *
 *  0x40AF40 takes (a, b, c) and computes three distances:
 *
 *      d1 = |a + b - 2c|      ; eax+edi, the first thing built
 *      d2 = |b - c|
 *      d3 = |a - c|
 *
 *  then returns `a` if d2 is no larger than either of the others, `b` if
 *  d3 <= d1, and `c` otherwise.  Note that it is `c` - not `b` - that the
 *  first distance is measured from, which is what makes this PNGOUT's own
 *  estimator rather than Paeth's: reading it as |a + c - 2b| is the natural
 *  mistake and gives different answers for most triples.
 *
 *  The callers pass the three neighbours in PNG's order, so
 *  FilterPredict(left, up, upleft).
 */

#include "pngout.h"

int FilterPredict(int a, int b, int c)
{
    int d1 = abs(a + b - 2 * c);        /* 0x40AF55: lea eax, [esi+ecx] */
    int d2 = abs(b - c);                /* 0x40AF4F: sub ecx, ebx       */
    int d3 = abs(a - c);                /* 0x40AF51: sub esi, ebx       */

    if (d2 <= d3 && d2 <= d1)           /* 0x40AF70: cmp ecx, eax / edi  */
        return a;                       /* 0x40AF7A: mov eax, ebp       */

    if (d3 <= d1)                       /* 0x40AF7F: cmp eax, edi       */
        return b;                       /* 0x40AF89                      */

    return c;                           /* 0x40AF87: mov eax, ebx       */
}