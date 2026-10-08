/*
 *  dumpdec.c   -   read a PNG, print what the reconstructed decoder produced
 *
 *  pngcheck.cmd checks Optimize()'s output end to end, but the decoder is
 *  easier to look at on its own: this links the same sources, calls
 *  DecodeImage() on a file and writes the pixel buffer out raw, so a round
 *  trip through pngout.exe can be compared sample by sample without having to
 *  re-derive what the optimiser decided to write.
 *
 *      build\dumpdec.exe in.png > raw.bin
 *
 *  Four bytes a pixel, little endian, 0xAARRGGBB internally - which is what
 *  Optimize() is handed, so this is the buffer the reconstruction believes the
 *  image is.  Also prints a hex grid to stderr so a small image can be read
 *  without a hex editor.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>

#include "pngout.h"

int main(int argc, char **argv)
{
    FILE          *f;
    unsigned char *fileimg, *pixels;
    long           size;
    unsigned int   w, h;
    int            i, x, y, fmt;
    int            iw = 0, ih = 0;          /* GetImageSize wants int * */

    if (argc < 2) {
        fprintf(stderr, "usage: dumpdec <file.png>\n");
        return 1;
    }
    /* a decoder that walks off the end of the buffer should still leave a trace */
    setvbuf(stderr, NULL, _IONBF, 0);

    /*
     *  The pixel buffer goes to stdout, so stdout has to be binary.  Left in
     *  text mode the CRT turns every 0x0A sample into CR LF, which shifts the
     *  rest of the dump by a byte - and because one of the values in a smooth
     *  gradient is usually 0x0A, that looks exactly like a decoding fault.
     */
    _setmode(1, O_BINARY);

    if ((f = fopen(argv[1], "rb")) == NULL) {
        fprintf(stderr, "dumpdec: cannot open %s\n", argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    fileimg = (unsigned char *)malloc((size_t)size);
    if (fileimg == NULL || fread(fileimg, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "dumpdec: cannot read %s\n", argv[1]);
        return 1;
    }
    fclose(f);

    fmt = GetImageSize(fileimg, (int)size, &iw, &ih);
    w = (unsigned)iw;
    h = (unsigned)ih;
    if (fmt != FMT_PNG && fmt != FMT_PAL8) {
        fprintf(stderr, "dumpdec: format %d is not reconstructed yet\n", fmt);
        return 1;
    }

    unk_190A1C4 = h;
    FilterChoice = (unsigned char *)malloc(h ? (size_t)h : 1);
    pixels = (unsigned char *)malloc(4 * (size_t)w * h);
    if (pixels == NULL || FilterChoice == NULL) {
        fprintf(stderr, "dumpdec: out of memory\n");
        return 1;
    }
    memset(FilterChoice, 0, h ? (size_t)h : 1);

    if (DecodeImage(fileimg, (int)size, pixels, 4 * (int)w, w, h, 0, 0) < 0) {
        fprintf(stderr, "dumpdec: DecodeImage refused the file\n");
        return 1;
    }

    if (fmt == FMT_PNG) {
        /* the decoder records each input row's filter byte here; the 8bpp
           format has none, so this stays as the memset left it */
        fprintf(stderr, "%dx%d, input row filters:", w, h);
        for (i = 0; i < (int)h; i++)
            fprintf(stderr, " %d", FilterChoice[i]);
        fprintf(stderr, "\n");
    } else {
        fprintf(stderr, "%dx%d, %d colours, coltype %d depth %d\n", w, h,
                unk_190A820, OutColType, OutBitDepth);
    }

    /* width, height, then w*h pixels of 0xAARRGGBB - little endian, so a
       reader can unpack the buffer without knowing the internal layout */
    fwrite(&w, 4, 1, stdout);
    fwrite(&h, 4, 1, stdout);

    for (y = 0; y < (int)h; y++) {
        fprintf(stderr, "%3d:", y);
        for (x = 0; x < (int)w; x++) {
            unsigned p = *(const unsigned int *)(pixels + 4 * (y * w + x));

            fprintf(stderr, " %02X%02X%02X%02X", p & 0xFF, (p >> 8) & 0xFF,
                    (p >> 16) & 0xFF, (p >> 24) & 0xFF);
        }
        fprintf(stderr, "\n");
    }

    fwrite(pixels, 4, (size_t)w * h, stdout);
    return 0;
}