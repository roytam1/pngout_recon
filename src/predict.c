/*
 *  predict.c   -   the row predictor used by PNGOUT's own filters
 *
 *  0x40AF40  FilterPredict()
 *
 *  Used by sub_4086E0 (the "minimum sum" filter) and by sub_40A8D0.  It is
 *  *not* the Paeth predictor from the PNG spec: instead of clamping
 *  p = a + b - c it picks whichever of the three candidates is closest.
 */

#include "pngout.h"

int FilterPredict(int a1, int a2, int a3)
{
    int ab = a1 - a2;             /* esi */
    int cb = a3 - a2;             /* ecx */
    int mid = abs(a1 + a3 - 2 * a2);
    int da = abs(ab);
    int dc = abs(cb);

    if (dc <= da && dc <= mid)
        return a1;

    if (da > mid)
        return a2;

    return a3;
}