/*
 *  pngout.c   -   command line driver
 *
 *  Reconstructed from pngout.exe:
 *      0x401000  Msg()         - printf to the current Stream
 *      0x401020  CheckEsc()    - has the user hit ESC?
 *      0x401050  ShowUsage()   - the /? banner
 *      0x401180  main()        - argument parsing, file I/O, reporting
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <conio.h>
#include <io.h>
#include <fcntl.h>
#include <math.h>

#include "pngout.h"

/* The 8 byte PNG signature. */
#define PNG_MAGIC  "\x89PNG\r\n\x1a\n"

#ifndef O_BINARY
#define O_BINARY   0x8000
#endif

/* ------------------------------------------------------------------ */
/*  0x401000 - Msg                                                    */
/*                                                                     */
/*  vfprintf() to the global Stream.  main() points Stream at stderr   */
/*  or stdout, or at NULL for /q, in which case nothing is printed.    */
/* ------------------------------------------------------------------ */

void Msg(const char *fmt, ...)
{
    va_list ap;

    if (Stream == NULL)
        return;

    va_start(ap, fmt);
    vfprintf(Stream, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/*  0x401020 - CheckEsc                                               */
/*                                                                     */
/*  Non zero as soon as a key is waiting and that key is ESC.         */
/* ------------------------------------------------------------------ */

int CheckEsc(int unused)
{
    (void)unused;

    if (!_kbhit())
        return 0;

    return _getch() == 27;
}

/* ------------------------------------------------------------------ */
/*  0x401050 - ShowUsage                                              */
/* ------------------------------------------------------------------ */

void ShowUsage(void)
{
    Msg("PNGOUT [In:{PNG,JPG,GIF,TGA,PCX,BMP}] (Out:PNG) (options...)        %s\n",
        "Jul  2 2011");
    Msg("by Ken Silverman (http://advsys.net/ken)\n");
    Msg("PNGOUT optimizes PNG size losslessly using my own deflate algorithm (not Zlib)\n");
    Msg("With the right options, it can often beat other programs by 5-10%%. Options:\n");
    Msg("   /c# PNG output color: 0=Gray, 2=RGB, 3=Pal, 4=Gray+Alpha, 6=RGB+Alpha\n");
    /*
     *  This one line is a single .rdata literal that IDA only showed the
     *  first 36 bytes of, so the tail was recovered from the raw byte dump.
     *  The DEL (0x7F) bytes are really in the binary; they stand in for the
     *  author's filter direction markers: x = across the row, y = down the
     *  column, "x&y" = both.
     */
    Msg("   /f# PNG output filter: 0=none, 1=\x7fx, 2=\x7fy, 3=\x7fx&y,"
        " 4=Paeth, 5=mixed, 6=reuse\n");
    Msg("   /d# Override default bitdepth: 0(min),1,2,4,8 (valid only in /c0,/c3 modes)\n");
    Msg("   /s# Select strategy. 0:Xtreme(default), 1:Intense, 2:Longest Match,\n");
    Msg("       3:Huffman Only, 4:Uncompressed\n");
    Msg("   /b# Block split threshold (lower=more blocks, 0=1 block/file, default=256)\n");
    Msg("       Use trial&error! Suggested values to try are: 0,128,192,256,512,1024,..\n");
    Msg("   /n# Exact number of Huffman blocks (overrides /b#)\n");
    Msg("   /r  Randomized initial tables (good for many trials with same options)\n");
    Msg("   /i  Use image on windows clipboard as input (specify only output filename)\n");
    Msg("   /k? 0=Remove optional chunks (default), 1=Keep all\n");
    Msg("       p=Keep palette indices, s=Keep settings for /c,f,d,b, t=Keep file time\n");
    Msg("   /k(Chunk,Chunk,..) Preserve only named chunk(s). Example: /kgAMA,bKGD,tEXt\n");
    Msg("   /v,q,l Verbose,Quiet,List mode (use '-' as filename to specify stdin/stdout)\n");
    Msg("   /y  Assume Yes at the 'overwrite file?' prompt\n");
    Msg("   /force  Write file even if bigger.\n");
    Msg("   /mincodes#  Workaround for buggy decoders. 1:Zlib 1.2.1 bug, 2:buggy mobiles\n");
    Msg("The 1st filename is the input (except when using /i). If you omit the output\n");
    Msg("filename, PNGOUT will use the same filename with a .PNG extension. Examples:\n");
    Msg("   >pngout inlarge.bmp outsmall.png           <- writes outsmall.png\n");
    Msg("   >pngout duke3d.png /c2 /f3 /b128 /kbKGD /v <- writes duke3d.png if smaller\n");
    Msg("   >pngout /i clipboard.png                   <- writes clipboard.png\n");
    Msg("   >for %%i in (*.gif) do pngout \"%%i\" /kp      <- convert all GIFs in dir to PNG\n");
    Msg("\nFor an easy-to-use GUI version, try PNGOUTWin! (http://pngoutwin.com)\n");
}

/* ------------------------------------------------------------------ */
/*  small helpers split out of main()                                 */
/* ------------------------------------------------------------------ */

/*
 *  The banner picks ".PNG" unless the name already contains a lower case
 *  letter, in which case ".png" looks less out of place.
 */
static int HasLower(const char *s)
{
    for ( ; *s ; s++) {
        if (*s >= 'A' && *s <= 'Z')
            return 1;
    }
    return 0;
}

static void AppendPngExt(char *name)
{
    strcat(name, HasLower(name) ? ".png" : ".PNG");
}

/*
 *  /b# is a block split threshold stored as a double; a negative value is
 *  really a request for the "uncompressed" strategy.
 */
static void ParseBlockSplit(SETTINGS *s, const char *arg)
{
    s->nblocks = 0;                       /* /b# overrides any /n# */
    s->blocksplit = atof(arg + 2);

    if (s->blocksplit < 0.0) {
        s->strategy = 4;                  /* uncompressed */
        s->blocksplit = -s->blocksplit;
    }
}

/*
 *  /s# and /mincodes# clamp their argument the same way: anything at or
 *  above `hi` becomes `hi`, anything <= 0 becomes `lo`.
 */
static void ClampArg(const char *arg, int lo, int hi, int *out)
{
    int n = atol(arg);

    if (n > 0 && n >= hi)
        *out = hi;
    else if (n <= 0)
        *out = lo;
    else
        *out = n;
}

/*
 *  /c# and /f# read their digits one at a time and reject anything that
 *  is not a plain number rather than trusting atoi().
 *  Returns the value, or -1 if a non digit turned up.
 */
static int ParseDigits(const char *arg)
{
    int v = 0, i = 2;

    for ( ; arg[i] ; i++) {
        if (arg[i] < '0' || arg[i] > '9')
            return -1;
        v = arg[i] + 10 * v - '0';
    }
    return v;
}

static void TooManyFilenames(void)
{
    Msg("Too many filenames specified on cmdline\n");
    ShowUsage();
    exit(1);
}
/* ------------------------------------------------------------------ */
/*  0x401180 - main                                                    */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv, char **envp)
{
    unsigned char *fileimg = NULL;       /* the whole input file           */
    unsigned char *trns = NULL;          /* the tRNS chunk payload        */
    SETTINGS s;
    unsigned char *clip = NULL;          /* clipboard pixels               */
    char  infilename[MAX_PATH];
    char  outfilename[MAX_PATH];
    int   inarg = -1, outarg = -1;
    int   haveout = 0, instdin = 0;
    int   keepsettings = 0, listmode = 0, quiet = 0;
    int   fromclip = 0, assumeYes = 0, force = 0;
    int   keeplistarg = -1, outnamearg = -1;
    int   optbitdepth = -1, useblocks = 0;
    int   i, insize = 0, trnscoltype = -1, coltype;
    int   fmt, delta, wrote = 0;
    FILE *in;
    FILETIME lastaccesstime;
    HANDLE h;

    /*
     *  The option defaults the SETTINGS layout documents.  Without these the
     *  three "automatic" fields are whatever was on the stack, and
     *  Optimize() reads s.coltype straight into the output colour type - so
     *  a run with no /c on the command line picks an arbitrary one.  The
     *  memset has to come *before* them, not after: a second one further down
     *  quietly puts all three back to zero, which reads as "greyscale, no
     *  filter, depth 0" rather than as "decide for me".
     */
    memset(&s, 0, sizeof s);
    s.filter     = -1;                 /* automatic */
    s.coltype    = -1;                 /* automatic */
    s.bitdepth   = -1;                 /* automatic */
    s.blocksplit = 256.0;
    s.msg        = Msg;
    s.checkesc   = CheckEsc;

    (void)envp;

    Stream = stderr;                     /* &iob[2] in the original */

    if (argc < 2) {
        ShowUsage();
        exit(1);
    }

    /* ---------------- argument loop ---------------- */

    for (i = 1; i < argc; i++) {
        char *a = argv[i];
        char  c;

        if (a[0] != '-' && a[0] != '/') {
            if (inarg >= 0) {             /* second bare name: the output */
                if (haveout)
                    TooManyFilenames();
                haveout = 1;
                outarg = outnamearg = i;
            } else {
                inarg = i;
            }
            continue;
        }

        c = a[1];
        if (c == 0) {                    /* a lone "-" is a filename */
            if (inarg >= 0) {
                if (haveout)
                    TooManyFilenames();
                haveout = 1;
                outarg = outnamearg = i;
            } else {
                inarg = i;
                instdin = 1;
            }
            continue;
        }

        if (c == '?') {
            ShowUsage();
            exit(1);
        }

        if (!stricmp(a + 1, "FORCE")) {
            force = 1;
            continue;
        }
        if (!strnicmp(a + 1, "MINCODES", 8)) {
            ClampArg(a + 9, 0, 2, &s.mincodes);
            continue;
        }
        if (!stricmp(a + 1, "ZL121")) {
            s.mincodes = 1;
            continue;
        }

        if (c >= 'a' && c <= 'z')
            c -= 32;

        switch (c) {
        case 'B':
            useblocks = 1;
            ParseBlockSplit(&s, a);
            break;

        case 'C': {
            int v;
            s.coltype = 0;
            v = a[2] ? ParseDigits(a) : 0;
            if (v < 0 || v > 6 || (v & 3) == 1) {
                Msg("Invalid coltype\n");
                ShowUsage();
                exit(1);
            }
            s.coltype = v;
            break;
        }

        case 'D':
            optbitdepth = atol(a + 2);
            switch (optbitdepth) {
            case 0: case 1: case 2: case 4: case 8:
                break;
            default:
                optbitdepth = -1;
                break;
            }
            break;

        case 'F': {
            int v;
            s.filter = 0;
            v = a[2] ? ParseDigits(a) : 0;
            if (v < 0 || v >= 7) {
                Msg("Invalid filter\n");
                ShowUsage();
                exit(1);
            }
            s.filter = v;
            break;
        }

        case 'I':
            fromclip = 1;
            break;

        case 'K':
            if (a[2] >= 'a' && a[2] <= 'z')
                a[2] -= 32;
            switch (a[2]) {
            case 'P':
                if (!a[3]) { s.keeppal = 1; continue; }
                break;
            case 'S':
                if (!a[3]) { keepsettings = 1; continue; }
                break;
            case 'T':
                if (!a[3]) { KeepFileTime = 1; continue; }
                break;
            case 0:
                continue;               /* bare /k */
            }
            keeplistarg = i;
            break;

        case 'L':
            listmode = 1;
            break;

        case 'N':
            useblocks = 1;
            s.nblocks = atol(a + 2);
            break;

        case 'Q':
            quiet = 1;
            break;

        case 'R':
            s.randomize = 1;
            break;

        case 'S':
            ClampArg(a + 2, 0, 4, &s.strategy);
            break;

        case 'V':
            s.verbose = 1;
            break;

        case 'Y':
            if (a[2]) {
                Msg("Warning: unrecognized option: %s\n", a);
                break;
            }
            assumeYes = 1;
            break;

        default:
            Msg("Warning: unrecognized option: %s\n", a);
            break;
        }
    }

    if (!haveout)
        Stream = stdout;                 /* &iob[1] */
    if (quiet)
        Stream = NULL;

    if (inarg < 0) {
        Msg("Must specify a source or target image\n");
        ShowUsage();
        exit(1);
    }

    if (instdin) {
        strcpy(infilename, "(stdin)");
        _setmode(0, O_BINARY);
    } else {
        strcpy(infilename, argv[inarg]);
    }

    if (fromclip) {
        if (haveout)
            TooManyFilenames();
        outarg = outnamearg = inarg;
    }

    /* ---------------- work out the output name ---------------- */

    if (haveout) {
        if (instdin) {                   /* "-" was both: in and out */
            strcpy(outfilename, "(stdout)");
            _setmode(1, O_BINARY);
        } else {
            int n;

            strcpy(outfilename, argv[outarg]);
            n = (int)strlen(outfilename);

            /*
             *  0x4019FD: append unless the name already ends in .PNG.  The
             *  original's test is "len < 4 || (stricmp(tail, ".PNG")
             *  && len + 4 < MAX_PATH)", so a name that already carries the
             *  extension is left alone and one that is too long for it to be
             *  added is left alone too.
             */
            if (n < 4 || (stricmp(outfilename + n - 4, ".PNG")
                          && n + 4 < MAX_PATH))
                AppendPngExt(outfilename);
        }
    } else if (instdin) {
        strcpy(outfilename, "stdin.png");
    } else {
        int n = (int)strlen(infilename);

        strcpy(outfilename, infilename);

        if (n >= 4 && !stricmp(outfilename + n - 4, ".PNG")) {
            assumeYes = 1;               /* we would overwrite ourselves */
        } else if (n >= 5 && !stricmp(outfilename + n - 5, ".JPEG")) {
            AppendPngExt(outfilename);
        } else if (n >= 4 &&
                   (!stricmp(outfilename + n - 4, ".JPG")  ||
                    !stricmp(outfilename + n - 4, ".JPE")  ||
                    !stricmp(outfilename + n - 4, ".GIF")  ||
                    !stricmp(outfilename + n - 4, ".PCX")  ||
                    !stricmp(outfilename + n - 4, ".TGA")  ||
                    !stricmp(outfilename + n - 4, ".BMP"))) {
            AppendPngExt(outfilename);
        } else if (n + 4 < MAX_PATH) {
            AppendPngExt(outfilename);
        }
    }

    OutColType = 2;                      /* byte_190A824 */

    /* ---------------- acquire the pixels ---------------- */

    if (fromclip) {
        long   w;
        unsigned ht;
        int    row;

        if (!ReadClipboard(&s.pixels, &s.pitch, &w, &ht)) {
            Msg("ERROR: Clipboard doesn't contain bitmap\n");
            exit(1);
        }
        clip       = s.pixels;
        s.width    = (int)w;
        s.height   = (int)ht;
        insize     = 0;

        /* Pad with blanks where the input filename would have gone. */
        sprintf(infilename, "%*c", (int)strlen(outfilename), ' ');

        for (row = 0; row < s.height; row++) {
            int col;
            for (col = 0; col < s.width; col++)
                ((unsigned int *)s.pixels)[row * (s.pitch / 4) + col]
                    |= 0xFF000000u;
        }
    } else {
        if (instdin) {
            int got = 0, cap = 0, last, n;

            last = (int)GetTickCount();
            for (;;) {
                if (cap - got < 0x80000) {
                    cap = got + 0x100000;
                    fileimg = (unsigned char *)realloc(fileimg, (size_t)cap);
                    if (!fileimg) {
                        Msg("Out of memory\n");
                        exit(1);
                    }
                }
                n = _read(0, fileimg + got, cap - got);
                if (n > 0) {
                    got += n;
                    last = (int)GetTickCount();
                }
                if (_eof(0) && (int)GetTickCount() - last >= 30)
                    break;
            }
            Msg("Read %d byte(s) from stdin\n", got);
            insize = got;
        } else {
            in = fopen(infilename, "rb");
            if (!in) {
                if (strchr(infilename, '.') ||
                    (int)strlen(infilename) + 4 >= MAX_PATH) {
                    Msg("%s not found\n", infilename);
                    exit(1);
                }
                strcat(infilename, ".png");
                in = fopen(infilename, "rb");
                if (!in) {
                    Msg("%s not found\n", infilename);
                    exit(1);
                }
                /* the real input differs from the output: move the output */
                if (!stricmp(infilename, outfilename))
                    AppendPngExt(outfilename);
            }
            fseek(in, 0, SEEK_END);
            insize = (int)ftell(in);
            fseek(in, 0, SEEK_SET);
            if (insize <= 0) {
                fclose(in);
                Msg("%s not found\n", infilename);
                exit(1);
            }
            fileimg = (unsigned char *)malloc((size_t)insize);
            if (!fileimg) {
                fclose(in);
                Msg("Out of memory\n");
                exit(1);
            }
            fread(fileimg, (size_t)insize, 1, in);
            fclose(in);

            if (KeepFileTime) {
                h = CreateFileA(infilename, GENERIC_READ, 0, 0,
                                OPEN_EXISTING, 0, 0);
                if (h != INVALID_HANDLE_VALUE) {
                    GetFileTime(h, &CreationTime, &lastaccesstime,
                                &LastWriteTime);
                    CloseHandle(h);
                }
            }
        }

        fmt = GetImageSize(fileimg, insize, &s.width, &s.height);
        s.pitch = 4 * s.width;
        s.pixels = (unsigned char *)malloc(4 * (size_t)s.width * s.height);
        if (!s.pixels) {
            free(fileimg);
            Msg("Out of memory\n");
            exit(1);
        }
        if (fmt == FMT_PNG) {
            unk_190A1C4 = s.height;
            trns = (unsigned char *)malloc((size_t)s.height);
            /* The decoder records each input row's filter byte here, which is
             * what the tRNS guess further down reads. */
            FilterChoice = trns;
        }

        if (DecodeImage(fileimg, insize, s.pixels, s.pitch,
                        (unsigned)s.width, (unsigned)s.height, 0, 0) < 0) {
            Msg("%s unsupported format\n", infilename);
            free(s.pixels);
            free(fileimg);
            exit(1);
        }

        /*
         *  A tRNS chunk gives a palette image transparency.  Work out which
         *  colour type that really makes it: naming one index and leaving
         *  every other entry opaque still counts as a plain palette.
         */
        if (fmt == FMT_PNG && trns) {
            unsigned char first = trns[0];
            unsigned int   mask = 0;
            int            r;

            for (r = s.height - 1; r > 0; r--)
                mask |= 1u << trns[r];

            if ((~(1u << first) & mask) != 0)
                trnscoltype = (first != 1 || (mask & 0xFFFFFFF7u) != 0) ? 5 : 3;
            else
                trnscoltype = first;
        }

        if (!memcmp(fileimg, PNG_MAGIC, 8) && !force)
            s.origsize = insize;         /* we have to beat the original */

        ScanChunks(fileimg, insize,
                   (keeplistarg < 0) ? EmptyChunkList : argv[keeplistarg] + 2,
                   s.coltype, s.verbose);
        s.unk12 = unk_190A3FC;
    }

    if (s.filter == 6 && !trns)
        s.filter = 5;                    /* no entropy filter with alpha */

    /* ---------------- /l : list mode ---------------- */

    if (listmode) {
        printf("/c%d", OutColType);
        if (trnscoltype >= 0)
            printf(" /f%d", trnscoltype);
        if (OutColType == 0 || OutColType == 3)
            printf(" /d%d", OutBitDepth);
        printf(" /n%d\n", NumBlocks);
        exit(0);
    }

    /* ---------------- confirm the overwrite ---------------- */

    if (!assumeYes && !haveout) {
        in = fopen(outfilename, "rb");
        if (in) {
            int k;
            fclose(in);
            printf("%s already exists. Overwrite? (y/n) ", outfilename);
            do {
                k = _getch();
                if (k == 27 || k == 'N' || k == 'n') {
                    printf("n\nOperation cancelled.\n");
                    exit(1);
                }
                if (!k || k == -32)      /* low byte of an extended key */
                    k = _getch();
            } while (k != 'Y' && k != 'y');
            printf("y\n");
        }
    }

    /* ---------------- fold in what the decoder found ---------------- */

    s.chunkA  = unk_41153C;
    s.nIHDR   = ChunkCountIHDR;
    s.nPLTE   = ChunkCountPLTE;
    s.nchunks = ChunkCount;
    s.chunks  = ChunkOffsets;

    Msg(" In:                             %s /c%d", infilename, OutColType);
    if (trnscoltype < 0)
        Msg("    ");
    else
        Msg(" /f%d", trnscoltype);
    if (OutColType == 0 || OutColType == 3)
        Msg(" /d%d", OutBitDepth);
    if (insize)
        Msg("\r In:%8d bytes\n", insize);
    else
        Msg("\r In: (Windows clipboard)\n");
    Msg("Out:                             %s", outfilename);

    s.palette  = dword_190A420;         /* the ramp itself, not its first entry */
    s.palflag = unk_190A820;
    if (s.coltype == 3 && !s.keeppal)
        s.palflag = 0;

    /*
     *  Reuse the settings of the file we just read when it was a PNG we
     *  wrote ourselves (24184 is one of our own signatures), or when the
     *  user asked for it with /ks.
     */
    if ((NumBlocks > 0 && unk_190A828 == 24184 && !ChunkCount) || keepsettings) {
        if (!useblocks)
            s.nblocks = NumBlocks;

        if (s.coltype >= 0) {
            if ((1 << (s.coltype & 0xFF)) & 0x54)
                OutBitDepth = 8;
            coltype = s.coltype;
        } else {
            coltype = OutColType;
            s.coltype = OutColType;
        }

        if (s.filter < 0)
            s.filter = (trnscoltype >= 0 && OutColType == coltype)
                       ? trnscoltype
                       : (coltype == 3 ? 0 : 5);

        s.bitdepth = OutBitDepth;
        if (OutColType == 3 && coltype == 0)
            s.bitdepth = 0;
    }

    if (optbitdepth >= 0)
        s.bitdepth = optbitdepth;

    /* ---------------- optimise ---------------- */

    delta = Optimize(&s);
    Msg("\n");

    /* ---------------- write it out ---------------- */

    if (fileimg && !memcmp(fileimg, PNG_MAGIC, 8)) {
        if (delta < insize)
            goto writeit;
    } else if (delta <= s.origsize) {
        goto writeit;
    }

    if (!force) {
        if (delta != 0x7FFFFFFF) {
            if (fromclip || !memcmp(fileimg, PNG_MAGIC, 8) ||
                outnamearg < 0 || stricmp(infilename, outfilename) || !insize)
                goto writeit;

            /*
             *  Optimising in place grew the file and no output name was
             *  given, so keep the original bytes.
             */
            /*
             *  The output goes to stdout only when the *input* was "-";
             *  naming an output file must write that file.
             */
            if (instdin) {
                if (fwrite(fileimg, 1, (size_t)insize, stdout) == (size_t)insize)
                    fflush(stdout);
            } else if ((in = fopen(outfilename, "wb")) != NULL) {
                if (fwrite(fileimg, 1, (size_t)insize, in) == (size_t)insize) {
                    if (KeepFileTime)
                        KeepFileTime = 2;
                } else {
                    Msg("ERROR writing to file!\n");
                }
                fclose(in);
            } else {
                Msg("ERROR: Unable to open %s for writing!\n", outfilename);
            }
            Msg("Unable to compress further: copying original file\n");
            wrote = 2;
            goto done;
        }
        wrote = 3;
        goto done;
    }

    if (delta == 0x7FFFFFFF) {
        wrote = 3;
        goto done;
    }

writeit:
    if (!fromclip && !memcmp(fileimg, PNG_MAGIC, 8) &&
        delta >= insize && outnamearg < 0 &&
        !stricmp(infilename, outfilename) && !force) {
        Msg("Unable to compress further\n");
        wrote = 2;
        goto done;
    }

    /*
     *  `delta` is the optimised size and is what the Chg line reports; the
     *  status - 0 written, 1 the write failed, 2 nothing smaller to write, 3 the
     *  optimiser bailed - is a separate variable (`v58` in the original) and is
     *  what main() returns.  Clearing delta on success, as this used to, made
     *  every run report "-<original> bytes, 0% of original".
     */
    if (instdin) {
        if (fwrite(s.outbuf, 1, (size_t)delta, stdout) == (size_t)delta) {
            wrote = 0;
            if (KeepFileTime)
                KeepFileTime = 2;
        } else {
            Msg("ERROR writing to file!\n");
            wrote = 1;
        }
        fflush(stdout);
    } else if ((in = fopen(outfilename, "wb")) != NULL) {
        if (fwrite(s.outbuf, 1, (size_t)delta, in) == (size_t)delta) {
            wrote = 0;
            if (KeepFileTime)
                KeepFileTime = 2;
        } else {
            Msg("ERROR writing to file!\n");
            wrote = 1;
        }
        fclose(in);
    } else {
        Msg("ERROR: Unable to open %s for writing!\n", outfilename);
        wrote = 1;
    }

    if (insize) {
        int diff = delta - insize;
        Msg("Chg:%+8d byte%c (%3d%% of original)\n",
            diff,
            abs(diff) == 1 ? ' ' : 's',
            (int)((double)delta * 100.0 / (double)insize));
    }

done:
    if (KeepFileTime == 2) {
        hObject = CreateFileA(outfilename, GENERIC_WRITE, 0, 0,
                              OPEN_EXISTING, 0, 0);
        if (hObject != INVALID_HANDLE_VALUE) {
            SetFileTime(hObject, &CreationTime, &lastaccesstime, &LastWriteTime);
            CloseHandle(hObject);
        }
    }

    if (s.outbuf) {
        free(s.outbuf);
        s.outbuf = NULL;
    }
    if (clip) {
        free(clip);
        clip = NULL;
    } else {
        if (trns) {
            free(trns);
            trns = NULL;
            unk_190A1C4 = 0;
        }
        if (s.pixels) {
            free(s.pixels);
            s.pixels = NULL;
        }
        if (fileimg)
            free(fileimg);
    }

    return wrote;
}
