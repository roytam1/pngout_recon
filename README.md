# pngout.exe - reconstruction

Reconstructing Ken Silverman's **PNGOUT** (2 Jul 2011 build) as source that
compiles with Visual C++ 6.0, starting from an IDA listing and a Hex-Rays
decompilation.

## Input files

| file | what it is |
|---|---|
| `pngout.exe.lst` | 924 MB IDA listing. Lines 1-20 461 are the `.text` disassembly; the remaining 22 M lines are the byte dump of the 22 MB `.data` section. |
| `pngout.exe.c` | 12 162 line Hex-Rays decompilation, 60 functions. |

The target: PE32, image base `0x400000`, `.text` `0x401000` (54 712 bytes),
`.data` `0x411000` with **virtual size 0x14F9838 (21 993 528 bytes)** but only
512 bytes of initialised data. Built with VC6, `/MD`, SSE off, `.mmx` on.

## Building

```
tools\build.cmd            # compile + link  ->  build\pngout.exe
tools\build.cmd clean
tools\selftest.cmd         # unit tests for the recovered support code
tools\regen.cmd            # rebuild reference\ from the two IDA exports
tools\run.cmd <command>    # run a tool inside the repo (maps the UNC share)
```

The build needs `C:\Program Files (x86)\Microsoft Visual Studio\VC98` on the
machine. `cl.exe` there imports `mspdb60.dll`, which lives in
`Common\MSDev98\Bin`; `build.cmd` puts both on the path.

`tools\run.cmd` exists because the repository lives on a UNC share and
`cmd.exe` cannot `chdir` into one; `pushd` maps it to a drive letter. It
propagates the command's exit code so it can be used from scripts.

## Reference corpus

`tools\extract_lst.py` splits the 924 MB listing into `reference/` (10 KB to
780 KB files) and is the only step that has to touch the whole thing:

| tool | purpose |
|---|---|
| `extract_lst.py` | split the listing into `reference/{text,idata,rdata,data}.lst` + `strings.tsv` |
| `scan_landmarks.py` | list every function in the listing |
| `split_fn.py` | write one disassembly file per function to `reference/fn/` |
| `split_decomp.py` | write one decompilation file per function to `reference/decomp/` |
| `dis.py` | dump the original disassembly of an address range or a function |
| `str.py` | reassemble a NUL terminated string from the listing's byte dump |
| `bss_refs.py` | every reference `.text` makes into the uninitialised `.data` |
| `callgraph.py` | call graph and call sites |

Examples:

```
python tools\dis.py _main +80          # original disassembly of main+0x80
python tools\dis.py sub_402AD0         # a whole function
python tools\str.py 0040F728 rdata     # recover a string IDA truncated
```

`reference/fn_index.txt` lists all 60 functions with their address ranges and
listing line counts, which is how the remaining work gets sized.

## Status

### Done and building

| original | reconstructed | file |
|---|---|---|
| `0x401000` `sub_401000` | `Msg()` | `src/pngout.c` |
| `0x401020` `sub_401020` | `CheckEsc()` | `src/pngout.c` |
| `0x401050` `sub_401050` | `ShowUsage()` | `src/pngout.c` |
| `0x401180` `_main` | `main()` | `src/pngout.c` |
| `0x4026E0` `sub_4026E0` | `ScanChunks()` | `src/chunk.c` |
| `0x409260` `sub_409260` | `GetImageSize()` | `src/chunk.c` |
| `0x409540` `sub_409540` | `Swap16()` | `src/chunk.c` |
| `0x4029C0` `sub_4029C0` | `ReadClipboard()` | `src/clipboard.c` |
| `0x40AF40` `sub_40AF40` | `FilterPredict()` | `src/predict.c` |
| `0x40B060` `sub_40B060` | `CpuFeatures()` | `src/cpuid.c` |
| `0x404D70` `sub_404D70` | `InitCrcTable()` | `src/optimize.c` |
| `0x405020` `sub_405020` | `InitHuff()` | `src/optimize.c` |
| `0x405050` `sub_405050` | `HuffPush()` | `src/optimize.c` |
| `0x4050C0` `sub_4050C0` | `HuffPop()` | `src/optimize.c` |
| `0x4052D0` `sub_4052D0` | `EmitBytes()` | `src/optimize.c` |
| `0x405320` `sub_405320` | `PutBits()` | `src/optimize.c` |
| `0x407F40` `sub_407F40` | `InitColorHash()` | `src/optimize.c` |
| `0x407F80` `sub_407F80` | `AddColor()` | `src/optimize.c` |
| `0x407FC0` `sub_407FC0` | `FindColor()` | `src/optimize.c` |
| `0x404DA0` `sub_404DA0` | `BuildCodeLengths()` | `src/optimize.c` |
| `0x405150` `sub_405150` | `EmitBlock()` | `src/optimize.c` |
| `0x4053D0` `sub_4053D0` | `FlushRow()` | `src/optimize.c` |
| `0x4058A0` `sub_4058A0` | `InitBlockTables()` | `src/optimize.c` |
| `0x405A60` `sub_405A60` | `BuildDistanceSlots()` | `src/optimize.c` |
| `0x405B80` `sub_405B80` | `WritePngHeader()` | `src/optimize.c` |
| `0x40E584` `UserMathErrorFunction` | `_UserMathErrorFunction()` | `src/stubs.c` |
| `.data` initialised part | 22 globals + `BitTable[64]` | `src/globals.c` |

### Not yet reconstructed

`src/stubs.c` aborts loudly for these two, so a build can never silently
produce a wrong PNG.

| original | name | size | what it does |
|---|---|---|---|
| `0x409560` | `DecodeImage()` | 4 353 B | dispatches to the PNG/JPEG/GIF/BMP/PCX/DDS/TGA decoders and publishes `OutColType`, `OutBitDepth`, `NumBlocks`, `dword_190A420[]`, `unk_190A820` |
| `0x402AD0` | `Optimize()` | 8 954 B | the trial and error search over filters, block splits and Huffman tables; returns the size of the smallest PNG it found |

## Optimize (0x402AD0): how it fits together

Worked through so far. `Optimize()` takes the `SETTINGS *` that `main()`
built and writes the resulting PNG into `s->outbuf`, returning its size, or
`0x7FFFFFFF` when it found nothing better than the original.

On entry it copies the settings into globals, which is how the whole optimiser
gets at them:

| setting | global | meaning |
|---|---|---|
| `s->palflag` `[7]` | `dword_180B978` | `/ks` keep settings |
| `s->filter` `[8]` | `dword_6D5664` | `/f#`, -1 = automatic |
| `s->coltype` `[9]` | `dword_185BED0` | `/c#`, -1 = automatic |
| `s->bitdepth` `[10]` | `dword_180A570` | `/d#` |
| `s->msg` `[31]` | `dword_185BEC4` | progress messages, e.g. `"\nEarly ESC! "` |
| `s->checkesc` `[32]` | `dword_6F6670` | polled in every trial loop |
| `s->outsize` `[30]` | zeroed | filled in at the end |

Then it walks the image (`dword_42E960`) to classify it: the minimum pixel
value into `dword_185BEC0`, whether every channel repeats (grey) into a flag
bit, and how many distinct colours there are into the palette at
`dword_180B578` (kept at `unk_1808F90` so a trial can be undone).

### Pieces recovered

See the table above; `src/optimize.c` plus `src/predict.c` and `src/cpuid.c`
are what the rest of the optimiser will be built on. Three findings worth
recording:

* **`dword_42D7D0` and `dword_42D7D4` are one array, not two.** IDA names them
  separately because they are `Heap[0]` and `Heap[1]`; the heap is 1-based with
  the sentinel 575 in slot 0, pre-written one slot ahead on each push so the
  sift-down can always read both children. Reading them as two arrays makes
  `HuffPop()` appear to return the sentinel forever, which is exactly the bug
  the first version of the unit test hit.
* **`PutBits()` shifts the value, not the accumulator:** `BitBuf | (value <<
  BitCount)`. It is an ordinary deflate LSB-first packer whose whole bytes fall
  out of the bottom and are CRC'd as they go. Reading it as
  `(BitBuf << bits) | value` reverses the order of every pair of values.
* **The CRC table.** `sub_404D70` builds the reflected CRC-32 table
  (polynomial `0xEDB88320`) and the values match the standard table exactly,
  which the tests check. But in the listing the single writer (`0x404D95`)
  stores to `dword_185BABC` while all 18 readers index `dword_185BAB8`, four
  bytes lower. Taken literally that shifts every entry by one and would corrupt
  the CRC of every PNG emitted, so the table is modelled at the address the
  readers use. Worth re-checking against the real bytes if the original ever
  turns up.
* **The output really is a PNG, and it stamps itself.** `sub_405150`
  (`EmitBlock()`) writes the chunk length straight out of the staging area
  (`dword_412E78`, `Scratch[]`), then `PutBits(0x54414449, 32)`, then two bytes
  of CRC. That first constant is `"IDAT"` read as a little endian DWORD. The two
  CRC bytes are `0x78 0x5E` - a **placeholder, not a checksum**. Little endian
  that is **24184**, which is exactly what `main()` tests for:

  ```c
  if (NumBlocks > 0 && unk_190A828 == 24184 && !ChunkCount) ...
  ```

  So 24184 is how PNGOUT marks a file as its own, and `main()` keys off it to
  decide whether it may reuse the settings it found in the file. Worth being
  careful with these constants: `0x474E5089` is `"\x89PNG"`, `0x0A1A0A0D` is
  the signature tail, and `1413563465` is `0x54414449` = `"IDAT"`. The same
  `"IDAT"` test appears in `sub_40A340`'s chunk walk.
* **`sub_4058A0` is the encoder's deflate distance tables** - the same
  base / extra-bit construction `sub_40AF90` does for the decoder, into
  `dword_1809390/394`, gated by a one-shot flag `dword_18CBEF0`.
  `sub_405A60` then fills `dword_6D5578[]` per row from those bases, using
  **4096 as the "no match" sentinel**.
* **`dword_412E78` is a scratch buffer, not a field.** It is seeded with the
  PNG signature at `0x405B8F` and then used as a general byte scratchpad by
  `sub_405B80`, so its value at any point depends on what ran before.

### The code-length solver

`sub_404DA0` is a length-limited Huffman solver in the spirit of zlib's
`gen_bitlen()`:

1. give every unused symbol (`counts[i] <= 0`) frequency `0x7FFFFFFF` so the
   heap sinks it;
2. build the tree with the 1-based heap, two pops per merge;
3. turn parent links into depths by walking the nodes from the top down;
4. if the longest code is too long, flatten the distribution
   (`bias += 1 << shift; shift++`) and go again;
5. finally bubble sort the lengths of symbols whose *flattened* frequency came
   out equal into non-increasing order, without which the code would not be
   canonical.

The tests check this against the property that makes it right: a valid code
must satisfy `sum(2^-length) == 1` exactly. Geometric frequencies give
`1 2 3 ... 15 15`, uniform frequencies give all-4, and a skewed
distribution with `maxlen` 7 comes back flattened to fit.

### The deflate length/distance tables

`sub_4058A0` (`InitBlockTables()`) generates the length and distance base /
extra-bit tables rather than storing them, then does a large per-block reset.
Both tables are now generated correctly and checked by the self test.

```
length  (extra, cumulative base): (0,1) (0,2) (0,3) (0,4) (1,5) (1,7)
                                    (2,9) (2,13) (3,17) (3,25) (4,33) ...
distance (base, extra):           (3,0) (4,0) (5,0) ... (10,0) (11,1)
                                    (13,1) (15,1) (17,1) ...
```

The length table's **second** field is the cumulative base and the first is
the extra-bit count, so the span of code *i* is `base[i+1] - base[i]` and must
equal `2^extra[i]`; the test asserts exactly that. The distance table is
cumulative in its first field instead.

**Build order is load-bearing.** The original's two reverse-slot builders
index the length table with codes up to 58 (`dword_1809394[2 * 58]`) while that
table holds only 30 pairs, so those reads deliberately fall through into the
*distance* table the linker placed next - in the original that aliasing is
intended, and it is why the reverse builders walk the distance extra-bit
counts to decide how fast to step the length code. Reproduced verbatim in
this build's layout the reads land on unrelated data and corrupt
`LenPairs[]`, so the length table is built last. This was found by
reordering the calls and observing the table turn correct.

Also note these are probably *not* RFC 1951 tables: the distance table's first
zero-extra-bit group is wider than the standard permits, which fits PNGOUT
having its own deflate.

`sub_4058A0` also resets, per block: `dword_412678 = 0x80000000`, 511 dwords
of `unk_41267C` to `0x7FFFFFFF`, 8 KB at `word_412EC0`, 64 KB at
`word_41CEC8`, and - only when `Image->strategy < 3` (so Huffman-only and
uncompressed skip it) - 128 KB at `dword_6B4968` plus five counters. It ends
with `dword_42E958[0xA18 * RowCount] = 0x7FFFFFFF` and a call to `sub_405A60`.

### The row record: the stride is 0xA18, not 646

Earlier revisions of this file said the working image has a "646 byte stride".
That is Hex-Rays dividing the immediate by four. Every per row array is indexed
with `imul eax, 0A18h` - 2584 **dwords**, 10 336 bytes - so the optimiser keeps
one 2584 dword record per row and the arrays are fields *inside* it, which is
why they all share the stride and why they overlap:

| field | address | used for |
|---|---|---|
| +0x000 | `dword_42E958` | one dword, `0x7FFFFFFF` per block |
| +0x010 | `dword_42E968` | 320 dwords: the row's samples |
| +0x44C | `dword_42ECF4` | a counter per length code |
| +0x480 | `dword_42EDE8` | frequency per distance code |
| +0x510 | `dword_42EE68` | 318 dwords: the symbol stream `sub_4053D0` eats |
| +0x89C | `dword_42F1F4` | bits per length symbol, then 29 more entries |
| +0x910 | `dword_42F268` | how many bits the row's own code needs |
| +0x990 | `dword_42F2E8` | bits per distance symbol |

`sub_4065A0` settles it: it computes `4 * (0x286 * row + column)` and uses the
result as a *dword* index, and `0x286 * 4 == 0xA18`. The record is 0xA18 dwords
but only the first 0x930 of them are touched, so `src/pngout.h` gives the small
fields a compact stride of their own and keeps 0xA18 for the two big ones
(the samples and the symbol stream). Nothing in the reconstruction relies on
the fields overlapping.

### sub_4053D0, the row writer

`FlushRow()` codes one row's symbol stream. The record is

```
row[0 .. 255]    256 literal slots, values 0 .. 15
row[256 .. 285]   30 length slots, zero = not in use
row[288 .. 317]   30 distance slots, zero = not in use
```

so `row + 288` is field +0x990, the distance symbols' *bit counts* - which is
why those values are small enough to go through the same code as everything
else, and why "distance slots" here means "one small code per distance
symbol". `sub_4065A0` emits the actual distance symbol and its extra bits
separately, from its own table.

What goes on the wire is

```
5 bits    length slots in use, minus 1        (1 .. 30)
5 bits    distance slots in use, minus 1      (1 .. 30)
4 bits    code lengths that follow, minus 4    (4 .. 15)
3 bits each, in Bitrev19[] order
the symbol stream
```

The 19 symbols are 16 literals plus three run codes, and **each row builds its
own Huffman code for them and transmits it here** - 8 bits maximum, so a row
can never cost more than its own entropy:

```
16    repeat the previous symbol, 2 extra bits, count 3 .. 6
17    a run of zeroes,           3 extra bits, count 3 .. 10
18    a run of zeroes,           7 extra bits, count 11 .. 138
```

17 and 18 only pay off for zeroes, which is why one or two zeroes fall back to
plain literals: the 2 bit code 16 cannot express a count of 1. A run of more
than three of a value that is *not* already being repeated is charged as a
literal plus a code 16 repeat. The original walks the row twice with the same
analysis - once to count the symbols, once to emit them - and `ClassifyRun()`
is shared between the two passes here.

How many slots are in use is found by walking down over the first zero from
the top index, and the count is *one past* the slot that stopped it, so a hole
anywhere below the top slot is taken as in use. That is why the slots have to
be packed against index 285 and 317. Both walks stop one slot short, so a row
always reports at least one length slot and one distance slot. The distance
slots are copied up so the stream is contiguous, and copied back at the end.

Only a suffix of `Bitrev19[]` is sent: the scan walks the table backwards for
the last symbol that has a length at all, so symbols 16, 17, 18 and 0 always
go out whether they are used or not.

### sub_405B80, everything in front of the image data

`WritePngHeader()` is not the compressor - it is the other half of `EmitBlock()`
and writes everything before the IDAT. It returns 0 in exactly one case, a
background colour that will not fit the bit depth as a palette index, which is
why `EmitBlock()` calls it first and gives up if it fails:

```
the signature and a fresh IHDR
the kept chunks from before PLTE, minus bKGD, with sBIT rebuilt
for a palette:  PLTE, and a tRNS holding the alpha of each entry
for anything else: a tRNS holding the darkest sample in the image
bKGD, if the image has a background colour
the kept chunks from after PLTE, minus bKGD, and minus hIST for a palette
```

**Two byte orders meet in here.** The decoded image is one DWORD `0xFFBBGGRR`
- bytes R, G, B, FF - while the palette and the bKGD colour are `0xAARRGGBB`.
So PLTE takes bytes 2, 1, 0 of a palette entry, tRNS takes byte 3 (the alpha,
where `0xFF` means opaque), and bKGD extracts its channels from bytes 2, 1, 0
as well. That is why the tRNS scan for a palette looks for the last entry that
is *not* opaque: everything from there on has to be spelled out.

**How a greyscale or RGB image gets a transparent colour.** `tRNS` for those
types names one sample, and everything equal to or darker than it is fully
transparent. PNGOUT uses the darkest sample it found while scanning the image
(`dword_185BEC0`, `MinSample`) and emits that, which makes an image that has
been through a lossy stage transparent in its shadows. `-1` there means "no
such sample" and the chunk is left out. The sample is rescaled when the bit
depth is not 8, and written as two big endian bytes.

**sBIT has to be rebuilt.** It says how many bits each sample really uses, and
the number of samples in it depends on the colour type, so when the colour type
changes the chunk has to be re-laid-out. The counts come out of a table:

| colour type | 0 grey | 2 RGB | 3 palette | 4 grey+alpha | 6 RGBA |
|---|---|---|---|---|---|
| sBIT samples (`0x04023301`) | 1 | 3 | 3 | 2 | 4 |
| bKGD bytes (`0x06021602`) | 2 | 6 | 6 | 2 | 6 |

Nibbles of a constant indexed by `4 * colourtype`. The values themselves are
kept, but a chunk too short for the new colour type has its missing entries
filled in with the first one - which is right, since every sample of a
well-formed sBIT has the same depth anyway.

**A palette background is an index, so the colour has to be in the palette.**
`bKGD` for a palette holds an index, so before PLTE is written PNGOUT looks for
the background colour among the palette entries and *appends* it if it is not
there - which is why a palette can grow during this call. If the palette is
already as long as the bit depth allows, it gives up and says so:

```
bKGD chunk can't fit palette index. Try higher bitdepth, /c2 or /c6 instead
```

The search for the index afterwards stops one entry early (`m > 0`, not
`m >= 0`), which is harmless only because the appended entry is the last one.

### Still open

* `sub_405A60` (`BuildDistanceSlots()`) fills `dword_6D5578[]` with 30 slots
  from the length half and 29 from the distance half, 4096 meaning "no match";
  builds `byte_185BA90[]` when the strategy is Xtreme; and finishes with the two
  `sub_405800` calls. Note that both halves are arrays of *code lengths* - what
  is added is the number of extra bits - so it is a comparison key for the
  match search, not a length or a distance.
* The CRC table's single writer sits 4 bytes above all 18 readers
  (`0x185BABC` vs `0x185BAB8`). Modelled at the readers' address.

### Still to do inside Optimize

`sub_402AD0` itself (5 983 listing lines), the four filter trials
`sub_4071D0`/`4077C0`/`407B00`/`407D80`, the min-sum filter `sub_4086E0`, the
entropy estimate `sub_408000`/`sub_408390`, the timing `sub_4063F0`, and the
`.bss` globals they all use.

## DecodeImage (0x409560): how it fits together

This is the part worked through so far. `DecodeImage` is a chain of tail
jumps, not a switch, after four globals are cleared (`unk_190A820`,
`unk_190A3FC`, `NumBlocks`, `unk_190A828`):

| test | target | format |
|---|---|---|
| `89 'P'` | inline at `0x409680` | **PNG** |
| `FF D8` | `sub_40B180` | **JPEG** |
| `'G' 'I' 'F'` | `sub_40CA70` | **GIF** |
| `19 91` + bpp 8 | chunk at `0x40D150` | 8bpp palettised, unidentified |
| `'B' 'M'` | `sub_40D560` | **BMP** |
| `0A 05 01 08` | `sub_40DAC0` | **PCX** |
| `'DDS '` + hdr size 124 | `sub_40DD80` | **DDS** |
| header fields self consistent | `sub_40D250` | **Targa** |
| otherwise | returns -1 | |

So PNGOUT carries **its own inflate**, not zlib. The help text says so, and
the bit reader confirms it.

### Shared machinery

* `sub_40AF90` (called once, gated by `byte_190A1D0`) builds the deflate
  length and distance tables into `dword_18DC1B0/4` and `unk_18DC2A0...`,
  sets `dword_18DC380 = 0` and `dword_18DC384 = 258`, then calls `sub_40B060`.
* `sub_40B060` is `CpuFeatures()`, reconstructed in `src/cpuid.c`. The
  measurement in `tools\selftest.cmd` shows it returns
  `(CPUID(1).EDX & 0x3FBFFFFF) | 0x00400000`, and the caller then tests
  **bit 15**, which CPUID(1).EDX leaves reserved. So the SSE2 branch is dead
  in practice and PNGOUT always uses its plain MMX code.
* `sub_40A340` is the inflate input pump. `dword_18DEE44` is a **bit** offset
  and `dword_18DB740` a byte pointer; a byte is read as
  `*(unsigned *)(ptr + (bits >> 3))` and a bit as `(word >> (bits & 7)) & 1`.
  Three cases: pull from a `FILE *` into a 64 KB window, skip a pending chunk,
  or stage the next chunk's bytes and rewind the bit position by 32.
* `sub_40A440` is Huffman symbol decode. It walks the bit stream one bit at a
  time, folding in `offs[i] - offs[i+1]` each step and stopping when the
  accumulated value goes negative, then indexes the value table with
  `code + offs[i]`.
* `sub_40A4A0` is
  `BuildDecodeTables(symbols, offsets, table, codes, maxbits)`. It fills a
  full `2^maxbits` table indexed by the **bit-reversed** window:
  `codes[rev(code, maxbits)] = length`, `table[rev(code, maxbits)] = symbol`.
  Slot counts come from walking `BitTable[]` downwards, so length *L* gets
  `2^(maxbits-L)` slots. Called with `maxbits` 9 and 6 from the PNG path.
* `sub_40A580` builds the cumulative offset array that `sub_40A4A0` and
  `sub_40A440` share.
* `sub_40AF40` is `FilterPredict()`, reconstructed in `src/predict.c`. This is
  PNGOUT's "minimum sum" predictor, **not** the PNG Paeth: it returns whichever
  of the three candidates is closest to the centre one.
* `sub_40A630` computes per-colour-type bit geometry from `dword_18DB268`
  (= interlace * 7) and the nibble constant `0x04020100`. This is the Adam7
  interlace path.
* `sub_40A8D0` is the PNG unfiltering, and calls `FilterPredict()`.

### Pixel format

Internally a pixel is one DWORD `0xFFBBGGRR`, i.e. bytes `R,G,B,FF`. The
clipboard path sets alpha with `|= 0xFF000000`, and `ScanChunks`' PLTE parser
builds entries as `bswap32(word) >> 8 | 0xFF000000`. For greyscale PNGs a ramp
of `1 << bitdepth` entries is generated into `dword_190A420` with step
`(0xFFFFFFFF / maxval) * 0x010101`.

### IHDR / PLTE validation in the PNG path

* width and height are `_byteswap_ulong` of a DWORD and must be greater than 0
* bit depth must satisfy `(1 << depth) & 0x116`, so 1, 2, 4 or 8
* colour type must satisfy `(1 << type) & 0x5D`, so 0, 2, 3, 4 or 6
* compression and filter methods must both be 0, interlace must be below 2
* `dword_18DB268 = interlace * 7`
* PLTE: entry count is `len / 3`, read backwards three bytes at a time

### The unidentified 8bpp format (`0x40D150`)

Magic `19 91`, width as a 16 bit word at +2, height at +4, byte +10 must be 8
and byte +11 must be 0, then a 256 entry RGB palette at offset 32. Its palette
transform is:

```c
pal[i] = 4 * (((((p[0] - 64) << 8) + p[1]) << 8) + p[2]);   /* p advances by 3 */
```

The `<< 2` at the end and the `- 64` on the first channel are both in the
original. PNGOUT's own help text does not list this format either, so it stays
named `FMT_PAL8`.

### Still to do inside DecodeImage

The bit-level details of the inflate body (`0x409B40` ... `0x40A31E`), the PNG
unfilter in `sub_40A8D0`, the Adam7 path in `sub_40A630`, the six other
decoders, the baseline JPEG decoder in `sub_40B180` (3 492 lines, by far the
largest), and the entropy coder around `sub_40AF90`. The 22 MB of `.bss` these
use also has to be re-declared.

## Verification so far

Two independent checks, both runnable.

### tools\selftest.cmd

190 checks over the recovered code: `Swap16`, `GetImageSize` for all
eight format codes, the `/k` list handling in `ScanChunks`, `FilterPredict()`,
`CpuFeatures()`, and the optimiser primitives (CRC table, Huffman heap, colour
hash, `PutBits()`/`EmitBytes()`, `RankValues()`, `FlushRow()` and `WritePngHeader()`).
They caught eight real defects, all now fixed:

* PNG dimensions are **4 byte** big endian, not 2 byte. `GetImageSize` was
  using `Swap16` where the original calls `_byteswap_ulong` on a DWORD.
* `ScanChunks` needs the same 32 bit byteswap for the chunk length and CRC.
* `CpuFeatures()` was documented with the wrong mask semantics. Measured
  against real hardware, the mask `0x3FBFFFFF` *keeps* SSE2 (bit 26) and
  clears bits 31, 30 and 22, and bit 22 is then forced on by a test of bit 25,
  which on a modern CPU is simply SSE.
* `PutBits()` was transcribed as `(BitBuf << bits) | value`; the original
  shifts the *value* above the pending bits, `(BitBuf | (value << BitCount))`.
  The wrong version reverses the order of every pair of values in the stream.
* The Huffman heap was written against two arrays, because IDA labels
  `dword_42D7D0` and `dword_42D7D4` separately. They are `Heap[0]` and
  `Heap[1]` of one 1-based array, which is why the first version's `HuffPop()`
  only ever returned the sentinel 575.
* **`RankValues()` reversed its bits in the wrong order.** The original does
  `shr edx,1` then `adc eax,eax`, so each bit is taken *after* the shift; the
  transcription shifted first and then took the *new* low bit, which reads bit
  1 upwards, throws bit 0 away and pads a zero at the top. Every code that was
  not a power of two came out wrong, and roughly half of all the codes in a
  code came out identical to another - two symbols with the same code *and* the
  same length, so the row could not be decoded at all. Nothing noticed until
  `FlushRow()`'s output was fed back through a decoder written from the format
  description above; the fix is checked directly now (no two symbols may share
  a code, and every code must fit its own length).
* **`BuildDistanceSlots()` never filled its distance half.** The second loop's
  base pointer is written in the listing as `dword_1809390[eax * 8]` with
  `eax = 30`, which lands exactly on `DistPairs`, so the loop runs from there to
  the end of `DistPairs` - 29 entries. The transcription read `DistPairs[2*n]`
  instead, which starts past the end of the table and so filled nothing: 30
  slots instead of 59.
* **`FlushRow()`'s two slot-count walks were off by one.** The original reads
  the slot, *then* decrements, then tests the value; the transcription tested
  before decrementing, so a row always claimed one slot too many.

They also exposed that the original's `strtok` loop in `ScanChunks` steps one
byte past the terminating NUL after the last token, because it restores the
comma with `tok[strlen(tok)] = ','` and then calls `strtok` again. That is
harmless against `main()`'s 260 byte buffer; the reconstruction stops at the
last token instead, which is equivalent for every well-formed list.

The row writer is checked by decoding: `tools/test_chunk.c` carries a small
decoder for the format described above - it reads the row header, rebuilds the
code from the transmitted lengths with `RankValues()`, then walks the payload -
so every row shape tried has to give its own symbols back exactly, and one of
them is pinned to the byte: a row of one literal followed by zeroes comes out
as `00 18 10 00 00 E2 3F 1B`.

`WritePngHeader()` is checked against bytes rather than against itself. A 64x32
8 bit greyscale image has to produce exactly the 33 signature and IHDR bytes,
`87 F6 21 58` included - which is the value Python's `zlib.crc32` gives for the
same 17 bytes, so the chunk CRC is the standard one and not a variant. The rest
of the tests walk each colour type: PLTE built from bytes 2, 1, 0 of the
palette, a tRNS whose alphas come from byte 3, a bKGD that appends its own
colour to the palette when it has to, the "won't fit the bit depth" failure,
the darkest-sample tRNS for RGB, the 4 bit rescaling, and a kept gAMA chunk
copied verbatim next to an sBIT that is rebuilt.

### Import table

Everything the original imports from the CRT stays visible in a partial build,
so `dumpbin /imports build\pngout.exe` can be compared against the `extrn` list
in `reference/idata.lst` even with the engines missing:

* **51 of the original's 55 imports are present, with nothing extra.**
* **USER32 and GDI32 match exactly**, in the same order: `OpenClipboard`,
  `GetClipboardData`, `CloseClipboard` / `DeleteDC`, `GetObjectA`,
  `CreateCompatibleDC`, `GetDIBits`.
* **MSVCRT matches exactly**: all 23 runtime functions plus the ten `start`
  helpers (`__getmainargs`, `__p___commode`, `__p__fmode`, `__p___initenv`,
  `__set_app_type`, `__imp___initterm`, `__setusermatherr`, `_adjust_fdiv`,
  `__imp___except_handler3`, `__imp__controlfp`).
* The four missing functions are **`QueryPerformanceCounter`,
  `QueryPerformanceFrequency`, `rand` and `srand`**, precisely the ones used
  only by `Optimize()`: the "Took %.3f sec." timing and the `/r` randomised
  initial tables. **Their appearance is the acceptance test for that
  function**: a correct `Optimize()` takes the import count to 55 with no
  extras.

`ShowUsage()` is byte exact, including the literal `0x7F` bytes in the `/f#`
line that IDA had truncated at 36 characters. The error paths match too:
`/c9`, `/f9`, `/c1`, `/?`, no arguments and a missing input file all print the
original's message and exit 1.

## Notes on the reconstruction

* **The settings block.** `main()` builds a 136 byte block at `ebp-0x290` and
  passes its address to `Optimize()`. It doubles as the decoded-image record:
  `pixels`, `pitch`, `width`, `height`, the output buffer, and then the `/c`,
  `/f`, `/d`, `/kp`, `/mincodes`, `/s`, `/b`, `/n`, `/r`, `/v` options.
  Offsets are confirmed from the option stores in `main`'s disassembly and from
  the `a1[n]` reads in `sub_402AD0`. It is `SETTINGS` in `src/pngout.h`.
* **`ScanChunks` only records chunks the user named.** The original keeps one
  unreset variable holding the class of the last critical chunk seen
  (IHDR = 1, PLTE = 2, IDAT = 3) and uses it to classify kept chunks, so
  `ChunkCountIHDR` really counts kept chunks between IHDR and PLTE, and
  `ChunkCountPLTE` those between PLTE and IDAT. Reconstructed as is.
* **A dead comparison is preserved.** `ScanChunks` tests the chunk tag against
  `0x44415452`, which reads back as `"RTAD"` and is not a PNG chunk type, so
  the flag it guards is never raised. Left in with a comment rather than
  "fixed".
* **`GetImageSize` finds seven formats plus a Targa guess.** PNG, JPEG, GIF,
  BMP, PCX (magic `0A 05 01 08`) and DDS (`"DDS "` with a 124 byte header) are
  all confirmed by their headers. Format 4 is handled correctly but **could
  not be identified**; see above.
* **Format 8 is the headerless one.** Targa has no magic, so it is accepted on
  the strength of its header fields being self consistent. That is the format
  the help text means by `.TGA`.
* **Where the decompiler was wrong.** Hex-Rays merged two unrelated stack
  locals in `main` into the same name (`Block` and `::Block`); one is the input
  file buffer and the other the tRNS chunk payload. It also folded a jump table
  index into a signed variable in the `/c#` and `/f#` parsers. Both are spelled
  out in `src/pngout.c`.
* **More Hex-Rays arithmetic.** It prints a dword index scaled by 0xA18 as
  `646 * n`, having divided the immediate by four - which is right for the
  product and wrong for the stride. See the row record notes above.
* **Names.** Descriptive names are used only where the semantics are
  established from call sites, the string table or the disassembly. Everything
  still unidentified keeps its IDA name (`unk_190A3FC`, `dword_190A420`, ...) so
  nothing is invented silently.
* **`IMAGEDESC` had the wrong offsets.** The comments claimed `+0x74` for the
  filter trial, `+0x4C` for the strategy and `+0x34` for the distinct count,
  but the declared fields did not land there: `ndistinct` came out at `+0x30`,
  `strategy` at `+0x80` and `trial` at `+0x70`. Reading the original settles it -
  `sub_4065A0` tests `+0x4C`, `sub_402AD0` drives `+0x74` and `+0x78`, and
  `sub_405B80` wants `+0x30` and `+0x60 .. +0x70` - so the struct now spells the
  real layout out field by field, the chunk list included.

## Layout

```
src/defs.h        IDA type compatibility layer (_DWORD, qmemcpy, __noreturn, ...)
src/pngout.h      SETTINGS, IMAGEDESC, format codes, globals, prototypes
src/pngout.c      driver: Msg, CheckEsc, ShowUsage, main
src/chunk.c       PNG chunk walk, format sniffer, Swap16
src/clipboard.c   ReadClipboard
src/globals.c     .data
src/optimize.c    CRC table, Huffman heap, colour hash, bit/byte output,
                  deflate tables, per row slots, the row writer, the PNG header
src/predict.c     FilterPredict
src/cpuid.c       CpuFeatures
src/stubs.c       the two unrecovered entry points, plus the math error handler
tools/            extraction scripts, build.cmd, run.cmd, selftest.cmd, regen.cmd
reference/        extracted listing/decompilation/strings (generated, gitignored)
```