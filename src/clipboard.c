/*
 *  clipboard.c   -   read an image off the windows clipboard
 *
 *  0x4029C0  ReadClipboard()
 *
 *  Grabs whatever bitmap is on the clipboard as a top-down 32bpp DIB and
 *  hands back the pixel buffer together with its pitch and dimensions.
 *  Every pixel's alpha byte is set to 0xFF by the caller, because clipboard
 *  bitmaps have no meaningful alpha.
 */

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "pngout.h"

#define CF_BITMAP  2

int ReadClipboard(unsigned char **pixels, int *pitch, long *width,
                  unsigned *height)
{
    HBITMAP  hbm;
    BITMAP   bm;
    BITMAPINFO bi;
    HDC      hdc;
    unsigned char *bits;

    if (!OpenClipboard(NULL))
        return 0;

    hbm = (HBITMAP)GetClipboardData(CF_BITMAP);
    if (!hbm) {
        CloseClipboard();
        return 0;
    }

    /* A 24 byte BITMAP starts with the type, the size and the dimensions. */
    GetObjectA(hbm, sizeof(BITMAP), &bm);

    bits = (unsigned char *)malloc(4 * (size_t)bm.bmWidth * bm.bmHeight);
    if (!bits) {
        CloseClipboard();
        return 0;
    }

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = bm.bmWidth;
    bi.bmiHeader.biHeight      = -bm.bmHeight;   /* negative: top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    hdc = CreateCompatibleDC(NULL);

    if (GetDIBits(hdc, hbm, 0, (UINT)bm.bmHeight, bits, &bi, DIB_RGB_COLORS)) {
        DeleteDC(hdc);
        CloseClipboard();
        *pixels = bits;
        *width  = (long)bm.bmWidth;
        *pitch  = 4 * bm.bmWidth;
        *height = (unsigned)bm.bmHeight;
        return 1;
    }

    DeleteDC(hdc);
    CloseClipboard();
    free(bits);
    return 0;
}