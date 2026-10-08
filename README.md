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
tools\dumpdec.cmd          # build\dumpdec.exe, the standalone decoder probe
tools\regen.cmd            # rebuild reference\ from the two IDA exports
tools\run.cmd <command>    # run a tool inside the repo (maps the UNC share)
```

The build needs `C:\Program Files (x86)\Microsoft Visual Studio\VC98` on the
machine. `cl.exe` there imports `mspdb60.dll`, which lives in
`Common\MSDev98\Bin`; `build.cmd` puts both on the path.

`tools\run.cmd` exists because the repository lives on a UNC share and
`cmd.exe` cannot `chdir` into one; `pushd` maps it to a drive letter. It
propagates the command's exit code so it can be used from scripts.

That mapping is a **real one that outlives the process**: a script that exits
without reaching its `popd`s leaks a drive letter permanently, and there are only
twenty six of them. Once they run out, `pushd` silently starts returning 1, and
every `pushd` in these scripts becomes a no-op - so the build lands in the
repository root instead of `build\`, `if exist pngcheck.exe` finds the *old* one
and nothing is rebuilt. If a build here seems to do nothing, check `net use`
first; `net use X: /delete /y` gives the letters back.

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
| `0x4063F0` `sub_4063F0` | `EstimateEntropy()` | `src/optimize.c` |
| `0x406560` `sub_406560` | `Adler32Update()` | `src/optimize.c` |
| `0x407090` `sub_407090` | `AdvanceRow()` | `src/optimize.c` |
| `0x4071D0` `sub_4071D0` | `FilterPixel32()` | `src/optimize.c` |
| `0x4077C0` `sub_4077C0` | `FilterPixel24()` | `src/optimize.c` |
| `0x407B00` `sub_407B00` | `FilterPixel16()` | `src/optimize.c` |
| `0x407D80` `sub_407D80` | `FilterByte()` | `src/optimize.c` |
| `loc_4075B0` | `FinishBlock()` | `src/optimize.c` |
| `0x408000` `sub_408000` | `PlanBlocks()` + `SortBlockEnds()` | `src/optimize.c` |
| `0x408390` `sub_408390` | `BestSplit()` | `src/optimize.c` |
| `0x406530` `sub_406530` | `CompressByte()` | `src/lz.c` |
| `0x4065A0` `sub_4065A0` | `LzCode()` | `src/lz.c` |
| `0x4086E0` `sub_4086E0` | `WriteStored()` | `src/optimize.c` |
| `0x404D60` `sub_404D60` | `NothingChanged()` | `src/optimize.c` |
| `0x408690` `sub_408690` | `PutByte()` | `src/optimize.c` |
| `0x4086B0` `sub_4086B0` | `InitProgressScale()` | `src/optimize.c` |
| `0x402AD0` `sub_402AD0` | `Optimize()` | `src/trials.c` |
| `0x40E584` `UserMathErrorFunction` | `_UserMathErrorFunction()` | `src/stubs.c` |
| `0x40AF40` `sub_40AF40` | `FilterPredict()` | `src/predict.c` |
| `.data` initialised part | 22 globals + `BitTable[64]` | `src/globals.c` |

`Optimize()` and everything it calls is now round-trip verified: `Optimize()`
produces a PNG that `tools\pngcheck.py` inflates with `zlib`, unfilters and
compares sample by sample against the source, over 30 cases covering all five
strategies, colour types 0, 2, 3 and 6, all five filters and both block
splitting modes. See **Verification so far**.

### Not yet reconstructed

`src/stubs.c` aborts loudly for the one that is left, so a build can never
silently produce a wrong PNG.

| original | name | size | what it does |
|---|---|---|---|
| `0x409560` | `DecodeImage()` | 4 353 B | dispatches to the PNG/JPEG/GIF/BMP/PCX/DDS/TGA decoders and publishes `OutColType`, `OutBitDepth`, `NumBlocks`, `dword_190A420[]`, `unk_190A820` |

The inflate inside it is verified byte for byte against `zlib`, and `src/decode.c`
implements the PNG path - chunk walk, bit reader and pump, Huffman, inflate,
unfilter and row expansion - for colour types 0, 2, 3, 4 and 6 at bit depths 1,
2, 4 and 8, plus the 8bpp palettised, GIF, BMP, PCX and Targa decoders.
Interlaced PNG input is refused rather than guessed at, and JPEG and DDS are not
written yet, so a build reports those two as unimplemented rather than guessing.

## Optimize (0x402AD0): how it fits together

`src/trials.c`. `Optimize()` takes the `SETTINGS *` that `main()` built and writes
the resulting PNG into `s->outbuf`, returning its size, or `0x7FFFFFFF` when it
found nothing better than the original.

On entry it copies the settings into globals, which is how the whole optimiser
gets at them:

| setting | global | meaning |
|---|---|---|
| `s->palflag` `[7]` | `dword_180B978` | `/ks` keep settings, and afterwards the palette length |
| `s->filter` `[8]` | `dword_6D5664` | `/f#`, -1 = automatic (becomes 5, or 0 for a palette) |
| `s->coltype` `[9]` | `dword_185BED0` | `/c#`, -1 = automatic |
| `s->bitdepth` `[10]` | `dword_180A570` | `/d#` |
| `s->strategy` `[19]` | `Image->strategy` `+4C` | `/s#`, 4 = `/s4` stored |
| `s->maxblocks` `[21]` | `Image->maxblocks` `+54` | `/n#`, an exact block count |
| `s->blocksplit` `[22]` | `Image->splitthreshold` `+58` | `/b#`, the saving a cut has to beat |
| `s->msg` `[31]` | `dword_185BEC4` | progress messages, e.g. `"\nEarly ESC! "` |
| `s->checkesc` `[32]` | `dword_6F6670` | polled in every trial loop |
| `s->verbose` `[33]` | `Image->verbose` `+84` | `/v`, also the `QueryPerformanceCounter` timing |

It then walks the image (`dword_42E960`) once to classify it, producing the three
flag bits that decide the colour type:

| bit | meaning | set when |
|---|---|---|
| 1 | needs alpha | some pixel is not opaque, and is not the darkest pixel - see `0x402D42` and `0x402D5C` below |
| 2 | not grayscale | some pixel has `R != G != B` |
| 4 | too many colours | a 257th distinct colour turned up |

Bit 1 has two ways to be raised, and both are unsigned comparisons, which matters
because a non-opaque pixel is *negative* as a signed int. The darkest pixel
(`MinSample`, `dword_185BEC0`) is taken with `jnb` at `0x402BF7` and tested
against `0x01000000` with `jnb` at `0x402D09`; if it is at least that, alpha is
needed. Otherwise the image is searched for a pixel that is either the same RGB
with a non-zero alpha or - at `0x402D5C` - simply not opaque and not equal to
`MinSample`. The second test is the one that carries a picture with a fully
transparent background: without it the background's own RGB is the only candidate
and the other see-through colours never get a look in.

and the palette in `dword_180B578`, the number of distinct colours, and
`MinSample` (`dword_185BEC0`), the darkest pixel - which is what decides whether
a tRNS chunk is written. The classification decides the type (`loc_402D8C`):

| flags | type | because |
|---|---|---|
| 0 | 0 if >= 128 colours, else 3 | a handful of greys is cheaper as a PLTE |
| 1, 2, 3 | 3 | a palette is the only encoding under 256 colours |
| 5 | 4 | grey + alpha, palette would need too many entries |
| 6 | 2 | RGB, more than 256 colours and nothing transparent |
| 7 | 6 | RGBA |

If the user *forced* a `/c#` the image cannot use, it says why in the original's
exact wording and gives up with `0x7FFFFFFF`.

It then estimates the output size in closed form (`loc_4034C8`), which sizes the
work buffer and doubles as the size `/s4` would produce, and either takes the
`/s4` shortcut or runs the trials.

### The trial loop

The loop re-runs the same image with the same options; what changes between
iterations is the *row records*, the per block Huffman machinery in
`src/optimize.c`. Trial 0 seeds them, each later trial leaves them primed by the
previous one, so the code lengths improve and the output shrinks until it stops.
`/f5` ("mixed") filters each row once per candidate filter with the histogram
sink, scores it with `EstimateEntropy()`, and re-filters the row with the winner
and the real sink; `/f6` instead reuses a caller-supplied per row table. The loop
knows it has stopped by fingerprinting each trial's filter choices and size into
`dword_1809568`/`dword_180A578` - once a trial reproduces an earlier pair, no
further progress is possible and the best result is restored and returned.

### Pieces recovered

See the table above; `src/optimize.c` plus `src/predict.c` and `src/cpuid.c` are
what `Optimize()` is built on. Findings worth recording:

* **The greyscale test is two shifted halves, not a mask.** `loc_402C0A` builds
  `(v & 0xFFFF) << 8` and `v & 0xFFFF00` and xors them, which is zero exactly
  when `R == G == B`. The tempting single mask,
  `(v & 0xFFFF00FF) == ((v & 0xFF00) << 8)`, is *not* the same test: it also
  requires `B == 0`, so it calls every grey pixel with a non-zero blue a colour
  and sends ordinary greyscale to a palette.
* **The minimum pixel is compared unsigned.** `MinSample` starts at `0xFFFFFFFF`
  and the loop's `jnb` is an unsigned compare, so a transparent black
  (`0x00000000`) wins it. Compared signed, 0 is greater than -1 and the
  transparent background would never be noticed - which is exactly the pixel
  whose presence decides whether the image needs an alpha channel.
* **With /kp off, the colour type 3 pass does not re-seed the hash.**
  `loc_40327E` only calls `FindColor()` and counts; the palette *and* the hash
  were already built by the first pass over the image. Calling
  `InitColorHash()` there would make every pixel look like a new colour and
  append a second copy of the palette.
* **`unk_411F5C` is zeroed `.bss`, not a table.** Section 3 (`.data`) is 512 bytes
  of file image with a `0x14F9838` virtual size, so `dword_411D5C` and everything
  past it reads as zero; the greyscale scan's `or edx, [ecx]` therefore
  contributes nothing and it is really a scan of `dword_411D60` alone. The one
  other thing it does is skip grey level 85, which changes nothing because
  `0x55`'s nibbles match anyway.
* **`dword_180A568` is one variable under two names.** `loc_406510` counts a
  row's bytes into it and `EstimateEntropy()` then decrements it to get the
  largest count to tabulate. Declared as two counters, `EstimateEntropy()`
  keeps decrementing one nothing increments and sizes its table from `-1`.
* **`dword_411D60` and `dword_180B578` are swapped together.** The palette and
  the per-entry counts are the two halves of one exchange, and getting that
  wrong leaves the counts describing a different palette than the one written.
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
* **`dword_6F6A78` is `LitCodes[256]`, not a variable of its own.** It is
  `dword_6F6678 + 256`, so `RankValues()` fills it along with the rest of the
  table, and `sub_407090` writes it with the row's own length for symbol 256 to
  **close every deflate block**. Nothing in the optimiser names an end-of-block:
  the one bit and the two bits written before each `sub_4053D0` are BFINAL and
  BTYPE, so a row *is* a block, and the code that ends it goes out from
  `AdvanceRow()`.
* **`dword_42F370` is the block list's `end` field, not a row field.**
  `0x405FE8`:
  ```
  mov  edx, dword_42D7CC
  imul edx, 0A18h
  mov  eax, dword_42F370[edx]
  ```
`0x42F374` is exactly 0xA18 dwords past the row record's base (`0x42E958`), so
it is the *next* record's field 1 - which is where the `/b` block list's `end`
field lives (`BlockRecord` is at `0x42F36C`, `end` is its second dword). Row index and
  block index are the same counter (`dword_6D5570` is both `RowCount` and
  `NumBlockList`), so the position where a row's data stops is the position where
  its block ends. `sub_4058A0`'s last store, `dword_42E958[0xA18 * RowCount] =
  0x7FFFFFFF`, is therefore the same array one record later: it opens the *last*
  block to the whole window, which is what lets the final row end when the writer
  runs out of bytes instead of at a boundary.
* **`sub_40AF40`'s first distance is measured from `c`, not from `b`.** The
  three are `|a + b - 2c|`, `|b - c|` and `|a - c|`, and the answer is `a` when
  `d2` is no larger than either, `b` when `d3 <= d1`, `c` otherwise. `a`, `b`,
  `c` are the left, above and above-left samples. Reading the first as
  `|a + c - 2b|` is the natural mistake and disagrees on most triples.
* **The four filter routines do not agree on which byte comes first.** They emit
  `2, 1, 0` for 24 bit (`mov cl, byte ptr [arg_0+2]` / `mov dl, bh` /
  `and eax, 0FFh` in `sub_4077C0`) and `2, 1, 0, 3` for 32 bit, because an
  internal pixel is one DWORD with **B, G, R, A** from the bottom up; only the
  1 and 2 byte cases walk upwards. So an RGB image comes out R, G, B per scanline
  and a grey+alpha one grey then alpha, which is PNG's order - getting it wrong
  costs nothing but the colours, and a round trip cannot see it.

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
and only the first 0x930 of them are touched, so `src/pngout.h` defines the
field offsets and `src/globals.c` has the lot as **one** `RowRecord[]` array
rather than one array per field. That is not cosmetic: modelled separately the
reconstruction needed two full records - 21MB of `.bss` - and the fields *do*
overlap on purpose (`sub_407090` reads the previous row's samples as
`samples[i] - 320`, which is inside the record in front of it).

Two of those fields are the same field under two names, and both matter:

* `+0x89C` is `+0x510` + 227, so `ROW_LENBITS[k]` is `ROW_SYMBOLS[227 + k]`; the
  LZ coder indexes it with the *pair* index 30 to 58 and so reads
  `ROW_SYMBOLS[257 .. 285]`, which is where the length code lengths are.
* `+0x990` is `+0x510` + 288, so the distance symbols' bit counts are
  `ROW_SYMBOLS[288 .. 317]` - the same 30 entries `FlushRow()` calls the
  distance slots.
* `+0x910` is `ROW_SYMBOLS[256]`: how many bits the row's end-of-block code
  needs. `sub_407090` reads it *after* rebuilding the code lengths, so it is the
  length the row's own code gives symbol 256, not a separate counter.

And one field is not in the record at all: `dword_42F370[646 * row]`, the row's
limit, is the `/b` block list's `end` field - see the `Pieces recovered` notes.

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

### sub_4063F0, the entropy estimate

`EstimateEntropy()` is how the filter trials are scored: given the symbol
histogram the trial produced, how many bits would it cost? It answers with
`0x7FFFFFFE - sum(count * ln(count))`, scaled so that the commonest symbol -
whose count is one more than `EntropyMax` - comes out at exactly `0x7FFFFFFF`,
the whole 31 bit range. So it is a *score*, not a byte count: a more skewed
histogram sums higher, scores lower, and the trial loops keep the smallest.
It saturates, because the commonest symbol alone can spend the whole range.

The table of `count * ln(count)` values is cached and rebuilt only when
`EntropyMax` changes, since the trials ask the same question once per candidate
and only the histogram moves. `0x80000000` is the failure value: the
"malloc failed (entropylut)" message goes out, the LZ work buffer is freed so
the caller has to stop, and no estimate is possible.

Two details are reproduced rather than explained, because they only make sense
once the filter trials are in:

* the first thing it does is undo one entry of the shared histogram at index
  `CurFilter` - `dword_185BEBC` is the filter in use, 0 .. 4, which the filter
  trials set;
* `EntropyMax` is decremented *before* it is compared against the cached table
  size, and it is that value *minus one* which sizes the new table. A histogram
  whose largest count is 1 therefore asks for a table of -1 entries and every
  read lands on the zero at index 0.

### sub_408390, where to cut a block

`BestSplit()` answers the question the `/b` block splitter (`sub_408000`, now
reconstructed as `PlanBlocks()`) asks over and over: given a run of bytes, where
is the best place to cut it in two? It has to be cheap as well as good, so it is
a single pass keeping, per symbol, the bytes seen so far and the bytes still
ahead.

The cost it accumulates is the −log of a symbol's probability with a prior
folded in:

```
(n(n+1) + 0.03160502036941208) * (n + 0.5)
-----------------------------------------
(n(n+1) + 0.19823983692604) * n(n+1)
```

for *n* bytes of one symbol — a KT/Laplace style estimator, with the constant
keeping it finite at *n* = 0 (where the term is skipped). Internally *lower is
better*, so the search keeps the smallest; what it reports is that cost negated
and scaled by 1/8, so the score is a **saving** and higher is better, which is
what the splitter compares against the `/b#` threshold.

Two details that only show up in the disassembly:

* the previous candidate's cost is left **on the x87 stack** (`fst` rather than
  `fstp`) and picked up at the top of the next iteration, with a dead
  `fld dbl_40FC10` — a zero — putting the seed there before the loop. Reading
  it as anything else makes the search pick the first cut every time;
* `log()` is computed with `fldln2` + `fyl2x`, which is exactly `log()`, and
  the returned saving is divided by `log2(2.0)` to turn the `-0.125` factor
  into `-1/8`.

The tests pin the behaviour that matters: 32 zero bytes followed by 32 `0xFF`
bytes is cut at 32, four quarters of 16 different bytes are cut on a boundary,
a solid byte has almost nothing to gain (0.106 against 5.6 and 5.7), and ESC
is polled every 64K bytes and aborts with −1.

### Settled since

* The CRC table's single writer sits 4 bytes above all 18 readers
  (`0x185BABC` vs `0x185BAB8`). Not an off-by-one: `sub_404D70` writes
  `dword_185BABC[edx*4]` for `edx` counting 255 down to 0 (`mov edx, 0FFh`,
  `dec edx`, `mov [185BABC+edx*4], eax`, `jns`) and every reader uses
  `dword_185BAB8[n*4]` over the same 0..255 range. It is one 256 entry table
  that IDA has named from two different references, four bytes apart. Modelled
  at the readers' address, which is what `CrcTable` is.

`sub_405A60` (`BuildDistanceSlots()`) is described above and is done: it fills
`dword_6D5578[]` with 30 slots from the length half and 29 from the distance
half, 4096 meaning "no match"; builds `byte_185BA90[]` when the strategy is
Xtreme; and finishes by turning the row's code lengths into `LitCodes[]` and
`DistCodes[]`. Note that both halves are arrays of *code lengths* - what is added
is the number of extra bits - so it is a comparison key for the match search, not
a length or a distance.

### sub_408000, the /b block splitter

`PlanBlocks(start, end)` turns one byte range into the list of deflate blocks
the compressor will emit, calling `BestSplit()` on each block to decide whether
cutting it pays. It reads the range out of `dword_18CBEF4` — the buffer
`sub_408690` (`PutByte`) filled on the collecting pass, allocated in
`Optimize()` at `0x403694` as `(height * 4 + 1) * width` — and **not** out of
`dword_18CBEE4` (`LinBufData`), which is a different and much smaller
allocation used by `FilterSample()`. All four `call sub_408390` sites say so:
`0x408071`, `0x408142`, `0x40822A` and `0x40827F`. The list lives in the second
0xA18-strided record array at
`0x42F36C`, and the addressing names the fields: `dword_42F36C[n*0xA18]` is
`start`, `dword_42F370[...]` is `end`, `dword_42F374[...]` is `next`, and
`lea eax, dword_42F378[eax]` hands the *address* of field 3 to `BestSplit()` as
its `double *score` — so the score is a `double` and `end` is the record's second
dword, not its fourth. `NumBlockList` is `dword_6D5570`.

Both modes end with the same shell sort:

* **`/b#` given** (`Image->maxblocks`, `+0x54`) — cut the whole range at its
  best point, then repeatedly split the block with the biggest saving until the
  count is reached, a block is shorter than two bytes, or 1024 (`cmp eax, 400h`)
  blocks exist. The scores are *stale* on purpose: a block's score is only
  refreshed when that block is the one being split, which is why the loop can
  stop with the count below `/b#` — the chosen block may have no interior cut
  left. `/b1` returns before any of this, having left the single whole-range
  block in place.
* **automatic** (`maxblocks <= 0`) — walk the list from `next`, twice per block
  because cutting one changes its neighbour's score, cutting whenever
  `BestSplit()` reports both a cut and a saving above `Image->splitthreshold`
  (`+0x58`). The walk restarts at the block whose index equals the pass number,
  which is how the growing list gets visited: it is `mov edi, [pass]` at the
  bottom of the pass loop, not a remembered cursor. A new block is appended at
  the end and linked *backwards* (`next[new] = old`), so the list stays in
  insertion order.

Two things in the sort are easy to get wrong, and both were:

* only the **ends** are sorted. The starts stay where the search left them,
  because the finished list is read as `start[0], end[0], end[1], end[2] ...` —
  the ends *are* the boundaries. Sorting the whole records would scramble the
  pairing.
* the backwards walk steps by whole records, `gap * 0xA18` bytes in the
  original, so the pointers must move `gap * BLOCK_STRIDE` **dwords**. Stepping
  `gap` dwords looks right and leaves the array looking *almost* sorted — which
  is how it survived review the first time.

The tests check the shape rather than exact cuts, since the count depends
entirely on the threshold: a solid block stays whole; a quartered block gives
512 one-byte blocks at threshold 0, two blocks at 40, one at 200 and above, with
the ends ascending in every case; `/b2`, `/b4` and `/b1` give their block counts;
`/b5000` is clamped to 1024; and the sort itself is checked on a reversed and a
stride-7 scrambled set of ends.

### sub_407D80 and friends, the per byte filter callback

`FilterByte(byte)` is what the filter trials run the image through. It is
handed one image byte and

* at the start of a row, emits the **filter type** through the sink first —
  that is PNG's per-row filter byte — and sets `CurFilter` from `TrialFilter`.
  Filter 3 (average) needs the row above, and there is none on the first row,
  so the original substitutes filter 1 there. The substitution is sticky: the
  switch reads `CurFilter`, so the whole row is filtered as sub;
* stores the **raw** byte into the work buffer, one dword per byte;
* emits `raw - predictor` through the sink;
* remembers the raw byte as the left neighbour, advances the position, and at
  the end of a row wraps it and counts the row — except under the histogram
  sink, which is driven row by row by its own loop and must not be counted.

The sink is a function pointer (`dword_6D6668`) and there are two of them:

| sink | what it does |
|---|---|
| `loc_406510` `CountByte()` | `FilterHistogram[b]++`, `FilterByteCount++` — measure only |
| `sub_406530` `CompressByte()` | Adler-32, then the LZ coder `sub_4065A0` — compress |

That is the whole trick behind the trials: the same walk over the image can
either count or compress, once per filter type.

The buffer holds **two rows**, one dword per byte, and `LinBufUsed`'s low bit
says which of the two slots the row being written owns, so the store always
lands in the slot the filter is *not* reading. The second slot starts a dword
past the end of the first to keep it aligned. In dword indices into the data
area:

```
LinBufUsed odd  : words[pos] = byte,       the byte above is words[stride+pos+1]
LinBufUsed even : words[stride+pos+1] = byte,   the byte above is words[pos]
```

At the first byte of an even row the predictor's left-of-above byte is
`words[-1]`, i.e. the dword *in front* of the data area — the original reads it
too, and the tests pin what comes out.

The four "filter trials" in the driver are this **one function for 8, 16, 24
and 32 bit images**, differing only in how many bytes of the sample it filters:

| original | ours | bytes a call |
|---|---|---|
| `sub_407D80` | `FilterByte()` | 1 |
| `sub_407B00` | `FilterPixel16()` | 2 |
| `sub_4077C0` | `FilterPixel24()` | 3 |
| `sub_4071D0` | `FilterPixel32()` | 4 |

Each byte is filtered against the *same* byte of its neighbours, so a 24 bit
sample's sub filter uses the sample to the left's third byte for its own third
byte — and the 24 bit version simply never looks at the argument's top byte.
`FilterSample(pixel, nbytes)` is the shared body. Only the byte count differs
between the four; the row-start, store and row-end code is identical.

`sub_406530` (`CompressByte()`) is now reconstructed in `src/lz.c`: it is the
Adler-32 update followed by `LzCode(&byte, 1)` — one byte at a time, and the
coder is handed a pointer to the *argument slot* rather than the value, which is
what `lea ecx, [esp+0Ch+arg_0]; push 1; push ecx; call sub_4065A0` does.

### sub_407090, closing a row

`AdvanceRow()` is what the LZ coder calls when it has filled a block. It closes
the row record — the per row Huffman machinery — and opens the next one.

Closing means: count the block in the record, emit the code the row needs,
**unlock** the samples, rebuild the code lengths, and total the damage.

* the unlock is `samples[i] = samples[i] == 0x80000000 ? 1 :
  samples[i] & 0x7FFFFFFF`. Only bit 31 is the lock; the encoder sets it on
  samples it wants kept in the next code, and a locked sample goes back to a
  count of one rather than to zero.
* the code lengths are built into a 320 entry scratch mirror (`0x412168`) —
  286 entries from the literal/length samples, then 30 more at offset **288**
  for the distances, which is where the distance samples already are
  (`ROW_DISTFREQ`). The original's `dword_412668` is nothing but the loop's end
  sentinel, one past the last entry.
* the copy back runs over all 320 entries, so the four neither build covers
  keep whatever the mirror held — faithful, and harmless.
* the running cost (`0x18CBEEC`) is
  `Σ (new length − old length) × frequency`, i.e. how much worse this row's code
  got. That number is what the filter trials compare.

The tail is straight-line: `RowIndex++`, and if there is a row left,
`BuildDistanceSlots()`, one bit saying whether this is the last row, a two bit
`2`, then `FlushRow()` for the new record. Both calls were already
reconstructed, which is what made this one cheap.

It also pinned the record layout: with `RowRecord[0]` at `0x42E968` the
frequency array is field `+0x010`, the length array `+0x510`, the distance
samples `+0x490` (they sit at sample 288, just past the 286 the literal build
covers) and the block counter `+0x410` — **which is sample 256**, the first
length code, so every finished block nudges that symbol's frequency. `ROWOF()`
also moved from `optimize.c` into `pngout.h` so the tests can build records.

### sub_4065A0, the LZ coder

Reconstructed in `src/lz.c` as `LzCode(data, len)`, the last big piece of the
optimiser. `len == 0` means "flush", which is how `loc_4075B0` closes a block.
Its state:

| what | where | role |
|---|---|---|
| window | `dword_6F6B80` | the filtered image, indexed by a 16 bit position; `dword_706B80` saves its first dword so it can slide |
| write position | `dword_6D4968` | where the next byte goes |
| scan position | `dword_6F6B78` | how far the matcher has looked |
| hash | `((dword >> 12) - (WORD)dword) & 0xFFF` | 12 bits of the 4 bytes at a position |
| hash heads | `word_412EC0[4096]` | the newest position with that hash |
| hash chain | `word_41CEC8[32768]` | the previous one, per position |
| run lengths | `word_185BED8[32768]`, `dword_411D58` | `byte_42E55C` is the last byte seen; the counter saturates at `0x7FFF` |
| step table | `word_806B82[]`, `word_806D86[]` | per position and length, the distance to step to next |
| match start | `dword_6D6670[32768]`, `dword_6B4968[32768]` | where a position's step list begins, and how far it is still valid |
| best cost | `dword_412678[512]`, `dword_412668` | indexed by `position & 0x1FF`; `dword_412668` is the furthest position with a cost |
| match list | `dword_706B88[0x40000]`, `dword_180BA84` | length in the low half, distance in the high half |

The shape: it walks the window from `LzScan` to `writepos - 260`, and at every
position *tries* a match, keeping the cheapest encoding of each prefix in
`MatchCost[position & 0x1FF]` and the symbol that achieves it in
`MatchList[position & 0x3FFFF]`. A position nothing beats falls back to a
literal, which also **locks** the row record's sample for that byte value
(`|= 0x80000000`) — the bit `AdvanceRow()` clears. So the coder both fills the
row record and decides what the row's next code must keep.

Two strategies, kept apart: `/s0` (Xtreme) walks the *distance* in the inner
loop with the `SlotOrder[]` chain and the `BestCost[]` early-out;
`/s1` and `/s2` (`LzSimple()`) walk the *length*. Strategies 3 and 4 skip
matching entirely — `cmp ecx, 3; jge loc_406CED`.

**The one that took longest to get right is a register lifetime.** `ebx` holds
two different things and the reconstruction had them the wrong way round:

* the *trigger* for the emit block is `cmp ebx, edi; jnz loc_40706E`, against
  the copy of `dword_412668` taken at `0x406630`;
* but `0x406C02`, which closes the position's step-table slot, ends with
  `mov ebx, dword_412668` at `0x406CC4` — a **fresh read of the global**, which
  the search has just been raising to the furthest position its matches reach.
  That is the value the literal path's clamp
  (`cmp edi, ebx; jle` / `mov dword_412668, ebx`) is measured against.

Taking the copy from the top of the iteration instead makes that clamp fire
against a stale number and pull `MatchLimit` back down to `pos + 1`, throwing
away every match the position found. The symptom was subtle: the stream decoded
to the right *length* but one byte wrong at the end, and only the strategies
that actually match were affected. With the clamp fixed the emit block's own
reads of the global (0x406E69, and 0x406FEE for the loop bound) agree with the
trigger and the batches tile exactly.

The other thing that only shows up in a working coder is the **slide-down
loop** at `loc_406E97`. The last entry in the match list can reach back before
the row began, so its tail is copied down until it does not — and that is what
makes the emit walk one index per symbol rather than one per *byte*. A match of
length L at index i is one symbol and the next entry is i+1.

### The chain the emit walks

`MatchLimit` is the furthest position with a cost, and the emit runs only once
`pos == MatchLimit`: everything up to there is settled. The loop at 0x406ED4
walks `i` from the slid-down start to `dword_412668`, one index per symbol,
emitting a literal (length 1, the byte in the high half) or a length + distance
+ extra bits. The tests decode that back with a tagged code shape and check both
the byte count and the content.

### loc_4075B0, closing a block

`FinishBlock()` is the other half of what a full block needs — the half that
puts bytes on the wire. `FilterByte()` jumps here when the compressing sink has
taken as many bytes as the block allows, and the LZ coder asks for it when it is
flushed. The order is not arbitrary:

1. flush the LZ coder, so the last match is emitted;
2. `AdvanceRow()`, which closes the row record and opens the next one;
3. pad the bit stream out to a byte boundary — `PutBits(0, (-BitCount) & 7)`;
4. the **Adler-32** of the unfiltered data, big endian, so
   `PutBits(Swap32(AdlerSum), 32)`; step 2 has to come first because it is the
   flush that leaves the Adler sum in `AdlerSum`;
5. the block's CRC: `PutBE(Scratch, ~BitCrc)` and four bytes out. That is what
   closes a deflate block;
6. the IDAT's length — `OutCount - ChunkStart - 12`, big endian — now that its
   contents are all written. The 12 is the 8 byte chunk header and the 4 byte
   CRC just added, and the length deliberately does not count itself;
7. whatever chunks belong after the image data, each emitted whole as
   `length + 12` bytes.

This also settled the chunk indices. The trailing loop runs
`[Image->+0x68, Image->+0x6C)` over `Image->+0x70`, so there are **four** chunk
indices, not three: `chunkbeg` (+0x60), `chunkmid` (+0x64), `chunktail` (+0x68)
and `chunkend` (+0x6C). The model had `chunkend` at +0x68 with +0x6C as
padding, which would have made step 7 emit the chunks that belong *before* the
image data. `WritePngHeader()`'s second run is now `[chunkmid, chunktail)` — the
chunks that go between PLTE and IDAT — and the rest follow the image.

Step 1 is `FlushLzCoder()` — `LzCode(0, 0)` — which is a real call now rather
than a hook, so the coder's last match is on the wire before the padding and the
Adler-32. The tests that call `FinishBlock()` directly hand it an empty window
(`LzWrite = LzScan = 0`), which is what a block that never filtered anything
looks like.

### sub_4086E0, the /s4 writer

`WriteStored()` is what `/s4` calls: the image out with **no compression at
all**, and therefore the only path that does not touch the deflate machinery.
That makes it the easiest way to check the PNG framing against something other
than itself, and it is checked against Python's `zlib`.

`dword_411D54` (`LinBufBytes`) is how many bytes one row packs into, per colour
type:

| colour type | `LinBufBytes` |
|---|---|
| 0, 3 (grey below 8 bits, palette) | `(bitdepth * width + 7) / 8 + 1` |
| 2 (RGB) | `3 * width + 1` |
| 4 (grey + alpha) | `2 * width + 1` |
| 6 (RGBA) | `4 * width + 1` |

The trailing **+1 is the row's filter type**, which is the thing that took the
longest here. /s4 does not filter, so it is always 0 — but it is still written,
and it still goes through the Adler-32, and it is *inside* `LinBufBytes`. That
last point is what makes the row length come out right: the original computes
the count as `(LinBufBytes * 0x10001) ^ 0xFFFF0000`, which is LEN and ~LEN as
two little endian 16 bit fields — the header of a zlib **stored** deflate block.

The compressed path uses the same global with **no** `+1`, because
`sub_405150` computes it as `(bitdepth * width + 7) >> 3` and `FilterSample()`
counts sample bytes, emitting the filter type separately on the call where
`BitBufPos` is still 0. So `LinBufBytes` means two different things in the two
paths - the *whole* stored row in one, the *number of calls* per row in the
other - which is the same in the compressed path for a different reason: there the
buffer holds **one dword per call**, not per byte, and a call is a pixel however
many bytes the colour type gives it. That is why the RGB and RGBA paths get
`width` here and not `3 * width` / `4 * width`.
So each row is

```
1 byte   BFINAL, set on the last row
4 bytes  LEN, ~LEN   =  LinBufBytes and its complement
1 byte   the filter type, always 0
N bytes  the samples
```

which is exactly a stored deflate block per row, and `zlib.decompress()` takes
the whole IDAT. Five things had to be right for that, and four of them were
wrong at first:

* **the two placeholder bytes.** The 24184 mark is `0x78 0x5E`, written as two
  *single* byte stores, not as one 16 bit store of `0x5E78` — the original does
  `mov byte ptr [esi+ecx], 78h` and then `mov byte ptr [esi+edx], 1` further
  on, with a whole row loop in between. Writing it as a dword put two zero bytes
  after the mark and shifted everything;
* **the filter byte.** It has to be written and added to the sum, which is what
  the `+1` in every `LinBufBytes` formula is for;
* **header and samples are one loop, not two passes.** The original's row loop
  writes the flag, the count and the pixels before moving on;
* **the length field is *reserved*, not written.** `mov ebx,
  dword_185BECC; add ebx, 4; mov dword_185BECC, ebx` skips over it so the tag
  lands after it, and the CRC therefore starts at `start + 4` — it covers the
  tag and the payload but not the length. That is what PNG wants, and what the
  deflate path does too (there `BitCrc` is seeded before `PutBits(TAG_IDAT_,
  32)`);
* **the Adler-32 is reseeded** (`mov [esp+2Ch+Number], 1` at 0x4087D5). Without
  it the sum starts from whatever the last deflate trial left in the global, and
  every structural check still passes — only `zlib` complains, with "incorrect
  data check".

The two byte orders meet here too, the same way they do in `WritePngHeader()`:
a pixel is one DWORD `0xFFBBGGRR`, so the samples go out **B, G, R, A** — bytes
2, 1, 0 and then 3, which is PLTE's order and the reverse of the internal one.

The palette path is the one that is *looked up* rather than copied: each pixel
goes through `sub_407FC0` (`FindColor()`), and below 8 bits the index is packed
two or four to a byte, counting down from the top of the byte. The partial byte
at the end of a row is flushed on the last pixel as well as at each group
boundary, which is what stops a width that is not a multiple of the group size
from losing it:

```
if ((n & mask) != mask && n != width - 1) goto next sample
```

### Nothing left inside Optimize

`sub_402AD0` is reconstructed: the colour type and bit depth decision, the closed
form size estimate, the `/s4` shortcut, the trial loop, the block splitting and
the timing. `tools\pngcheck.cmd` round trips all of it.

The one thing that is still worth a second opinion is the length table's aliasing
(above): the reconstruction builds `LenPairs[]` last and reads it by slot, which
is what the original's *intent* is, but the original's actual reads for codes 30
to 58 land in the distance table that overlaps it in memory. The two agree
everywhere except the last length code, so the only visible difference is in the
extra bits of a 258 byte match.

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
* colour type must satisfy `(1 << type) & 0x5D`, so 0, 2, 4 or 6
* compression and filter methods must both be 0, interlace must be below 2
* `dword_18DB268 = interlace * 7`
* PLTE: entry count is `len / 3`, read backwards three bytes at a time

### The PNG chunk walk, `0x409680` ... `0x409A3B`

The signature is checked as two DWORDs, `0x474E5089` and `0x0A1A0A0D`, and
`dword_18D4350` (the transparent RGB triple) is cleared first. Then one loop
walks the chunks. Each iteration caches the cursor twice, once pointing at the
`len,tag` header and once at the data, and `next = data + len + 4` skips the
CRC. Only four tags are recognised; **everything else, CRC and all, is skipped
without a CRC check**, so PNGOUT accepts corrupt ancillary chunks.

Tags are matched as the little-endian DWORD at `data - 4`: IHDR `0x52444849`,
PLTE `0x504C5445`, bKGD `0x624B4744`, tRNS `0x534E5274`, IDAT `0x54414449`.

`dword_190A420` is a single 256 entry table at `0x190A420..0x190A81F` that
serves as *both* the greyscale ramp and the palette, with `dword_190A820` right
after it holding the entry count. Three things fill it:

* **IHDR**, for colour types 0 and 4 only, builds the ramp
  `dword_190A420[i] = 0xFF000000 + i * 0x010101 * (255 / maxval)`, and sets
  `dword_190A820 = 1 << depth`.
* **PLTE** sets `dword_190A820 = len / 3` and writes
  `dword_190A420[i] = (bswap32(data + 3*i) >> 8) | 0xFF000000`, backwards.
* **IDAT** setup, for bit depths below 8, expands whichever of the two is
  present out to a full 256 entries, so the per-pixel reader can index by a raw
  byte: depth 1 does `pal[j] = pal[j & 1]` from 2, depth 2 does `pal[k] =
  pal[k & 3]` from 4, depth 4 does `pal[m] = pal[m & 15]` from 16.

**tRNS** never allocates a second table. For colour type 0 with depth ≤ 8 it
sets `HIBYTE(dword_190A420[data[1]]) = 0`, and for colour type 3 it walks
`i` from `min(len, palcount) - 1` down to 0 doing

```c
dword_190A420[i] = (dword_190A420[i] & 0x00FFFFFF) | (data[i] << 24);
```

IDA names the store `dword_190A424[--i]`, which looks like a second array, but
`0x190A424 + 4 * (i - 1) == 0x190A420 + 4 * i`, so the `dec` and the +4 base
offset cancel and the update is in place on `dword_190A420[i]`. The same four
bytes are reused as a scalar by `sub_40D560` (the BMP decoder), which is why IDA
hinted two conflicting types. Colour type 2 with depth 8 instead stores the
single triple `dword_18D4350`, which `sub_40A8D0` compares against per pixel.

**bKGD** is stored separately, in `dword_190A3FC`, and split out to the three
channels `dword_411070`, `dword_411074`, `dword_411078`. Colour types 0 and 4
scale the 16 bit value through the same ramp formula; 2 and 6 use the byte
formula at depth 8 and `(sample / 257)` at depth 16; type 3 just looks the index
up in the table. Colour types other than 0, 2, 3, 4, 6 leave it alone.

### IDAT setup at `0x409A3B`

Everything up to here is pure chunk parsing. Reaching IDAT is the only way out
of the loop, so the first IDAT wins and later ones are handled by the pump. The
setup is:

```c
dword_18DB740 = data + len - 4;   /* the tail; the offset counts back to data */
dword_18DEE44 = 32 - 8 * len;     /* so the first read lands exactly on data */
dword_18D8F58 = 0;                /* not reading from a FILE * */
dword_18DB478 = 0;                /* no chunk staged yet */
```

then the seven remaining arguments are copied out, and a colour type switch fills
the per-sample channel ring the unfilter and pixel readers use:

| colour type | effect |
|---|---|
| 0, 3 | `dword_18DB85C[0] = 0`, a one slot ring |
| 2 | `dword_18DB860 = 2`, so the ring is 1, 2, 0 |
| 4 | `dword_18DB860 = 0`, so the ring is 1, 0 |
| 6 | `dword_18DB860 = 2` and additionally `dword_18DB864 = 3`, `dword_18DB868 = 0`, so the ring is 1, 2, 3, 0 |

`dword_18DB85C[0] = 1` for every type that has an alpha slot. The ring is
explained under **The unfilter** below.

The argument plumbing, for whoever writes the C: `arg_8` is the destination
base, `arg_C` the pitch, `arg_10` and `arg_14` reach the row-edge clamps,
`arg_18` an x origin and `arg_1C` a row or plane index.

### Samples per pixel, and the row byte count

The constant `0x04021301`, shifted by `4 * colourtype` and masked with `0xF`, is
the sample count per pixel: **{0:1, 2:3, 3:1, 4:2, 6:4}**. Palettised PNG is one
"sample" per pixel, namely the index. So `dword_18DB26C = width * samples`, then
converted to bytes by bit depth, `(x + 7) >> 3`, `(x + 3) >> 2`, `(x + 1) >> 1`
and identity for 1, 2, 4 and 8.

`sub_40A630` recomputes the same figure per Adam7 pass, which is where the
sub-8-bit expansion above matters.

### The bit reader and the input pump

`dword_18DEE44` is a signed bit offset and `dword_18DB740` a byte pointer. A read
of *n* bits is always

```c
eax = dword_18DEE44;                       /* the offset */
v   = *(unsigned *)(dword_18DB740 + (eax >> 3));
v >>= (eax & 7);                           /* arithmetic: the offset can be negative */
v &= mask[n];
dword_18DEE44 = eax + n;
if (dword_18DEE44 >= 0) { sub_40A340(); eax = dword_18DEE44; }
```

so the offset counts **up towards zero** from a large negative value, and the
pointer is anchored at the *end* of the region rather than the start. The
negative shift `eax >> 3` walks the bytes backwards, which is why the anchor is
the tail.

IDAT setup exploits that:

```c
dword_18DB740 = data + len - 4;    /* the last 4 bytes of the data */
dword_18DEE44 = 32 - 8 * len;      /* so (data + len - 4) + ((32-8len) >> 3) == data */
```

The pump fires as soon as an offset after a read would be non-negative, and has
three cases:

* **`dword_18D8F58` set**, reading from a `FILE *`: refill the window at
  `unk_18FA1C4` with `fread`, at most `0xFFFC` bytes, and subtract `0x7FFE0`
  (that is `8 * 65532`) from the offset.
* **`dword_18DB478` set**, a chunk already staged: just move the anchor to the
  next chunk header and add `32 - 8 * dword_18DEDE8`.
* otherwise, stage the *next* chunk: read its length from `[anchor + 8]`, set
  `dword_18DB478 = anchor + 8 + len + 12` (header plus data plus CRC plus
  header), copy `[anchor + 0]` into `dword_18D8F5C` and, when the next tag at
  `[anchor + 0xC]` really is IDAT, `[anchor + 0x10]` into `dword_18D8F60`; point
  `dword_18DB740` at `dword_18D8F60` and subtract 32.

The staging trick is neat and worth spelling out: `dword_18D8F5C` holds a copy
of the 4 bytes the old anchor pointed at and `dword_18D8F60` the next chunk's
first 4 data bytes, and those two live adjacently in `.bss`. Swapping the anchor
onto them and rebasing the offset keeps reads running across the boundary without
a copy.

**Two quirks here are in the original and must be kept.** The staged length
`dword_18DEDE8` is read once and never refreshed, so every IDAT after the first
is walked using the *first* one's length; and the tag is only checked when
staging, so continuation chunks are taken on trust. Well-formed files with
uniform IDAT sizes are unaffected.

### The inflate body, `0x409B40` ... `0x40A31E`

It is a `puff.c` descendant, and the structure is completely explicit:

```
bfinal = read 1;  btype = read 2;
```

* **btype 0, stored.** Discard bits to the byte boundary, read `len` and
  `nlen`, and bail out unless `len == ~nlen`. A zero length ends the image. The
  bytes go into a **16 KB circular window** at `byte_18E1098`, handed to
  `sub_40A8D0` in 16 KB chunks.
* **btype 1, fixed.** The code lengths are written out with five `rep stosd`
  runs, 144 entries of 8, 112 of 9, 24 of 7 and 8 of 8 at `dword_18DEE78`, then
  32 distances of 5 at `unk_18DF2F8`, with `arg_C = 32`. The four literal runs
  are contiguous, which is a free consistency check on the layout:
  `0x18DEE78 + 144*4 == 0x18DF0B8`, `+112*4 == 0x18DF278`, `+24*4 == 0x18DF2D8`.
* **btype 2, dynamic.** `HLIT = read 5 + 257`, `HDIST = read 5 + 1`,
  `HCLEN = read 4 + 4`, then `HCLEN` code lengths of 3 bits each indexed through
  the permutation table `dword_411080[]`, the remainder of that 19 entry table
  zeroed, and the lengths read with `puff`'s 16/17/18 repeat codes. A
  `dword_190A400` counter ticks once per dynamic block.
* **btype 3** is rejected, as it should be.

`dword_18DEE78` is a single flat array: the `HLIT` literal/length lengths at
`[0 .. HLIT-1]` and the `HDIST` distance lengths at `[HLIT .. HLIT+HDIST-1]`.

Each of the two tables is then compiled twice, once slowly and once fast:

```c
sub_40A580(offs,            count, bitcounts, work);
sub_40A4A0(bitcounts, work, symbols,   codes, maxbits);
```

`sub_40A580` is puff's `construct()`, and `sub_40A4A0` builds a direct lookup
table. The hot loop peeks 9 bits (6 for distances) into `codes[]`; a non-zero
entry means "this many extra bits to skip and the symbol is in `symbols[]`",
while a zero means the code is longer than the peek, so it falls back to
`sub_40A440`, puff's `decode()` bit at a time.

Lengths and distances come from the tables `sub_40AF90` built, one 8 byte
`{extra, base}` pair per entry and indexed by the raw symbol:

* length: `dword_18DBA98[sym].extra` / `dword_18DBA9C[sym].base`, symbol 257 up
* distance: `dword_18DC1B0[dsym].extra` / `dword_18DC1B4[dsym].base`

The extra bits are masked with `dword_4100D8[extra]`, the shared bit table.

The copy is byte at a time through the circular window, `src = write - dist`,
which is what makes overlapping matches expand correctly:

```c
src = write - dist;  end = write + len;
do { w[write & 0x7FFF] = w[src++ & 0x7FFF]; } while (++write < end);
```

At the end of the stream the tail is flushed, and if the last partial block
crossed the 16 KB boundary it is flushed in two pieces. The wrap test is
`((start ^ write) & 0xFF00) & 0x8000`, i.e. bit 8 of the XOR: `start` is always
a multiple of `0x4000` so it contributes nothing to bit 8, leaving "has the
write index passed 0x10000".

**Returning 0 clears `dword_190A820` when the colour type is 4**, the only sign
that grey+alpha has no palette.

### The unfilter, `sub_40A8D0`

It is handed 16 KB at a time and keeps three byte rows alive across calls:

| global | role |
|---|---|
| `dword_18FA1C0 + i` | the `b` neighbour, the row above, walked **downwards** from `dword_18DB26C` so offset 0 goes unused |
| `dword_18CBF68 + ch` | the `a` neighbour, left |
| `dword_18CBF54 + ch` | the `c` neighbour, up-left |

`dword_18DB85C[]` is a little ring of channel indices and `dword_18DC38C` is the
slot currently in use. IDAT setup fills the ring, and the first slot is
`dword_18DB85C[0]`:

| colour type | ring |
|---|---|
| 0, 3 | `0` |
| 2 | 1, 2, 0 |
| 4 | 1, 0 |
| 6 | 1, 2, 3, 0 |

So slots 1, 2 and 3 are R, G and B, and slot 0 is the alpha slot, which the
types without alpha simply leave unused. The five filters are the PNG five, but
two details are PNGOUT's own:

* `if (filter == dword_4110CC) filter = 5`, so a filter byte equal to
  `dword_4110CC` is redirected to case 5. That global lives in `.data` and
  `sub_40AF90` also writes 4 to it, so in practice **filter 4, the Paeth byte,
  is decoded as PNGOUT's own predictor.**
* case 4 calls `sub_40AF40`; case 5 has the same thing inlined against an
  absolute-value table at `0x18D6A40`, reached by folding the row offset into
  the address, so the three lookups land at `T[c-b]`, `T[2c-a-b]` and `T[c-a]`.

Those two copies of the predictor are not the same code. `sub_40AF40` returns
`a` on `d2 <= d3 && d2 <= d1`; the inline returns `b` when `d2 == d3 && d1 >=
d2`, having first swapped `c` for `a`. They disagree on 511 of the 261 121
`(a-c, b-c)` pairs — but in every one of those `a == b`, so **the value returned
is identical over the whole byte domain**, which brute force confirms. The
branches differ, the answers do not. `src/predict.c` matches `sub_40AF40`
instruction for instruction and needs no change.

A row ends when `dword_18D9FA4`, the bytes left in it, reaches zero, which then
advances `dword_18DA23C` by `dword_18DA234`, clears the left and up-left slots,
resets the counter to `dword_18DB26C`, and when the pass is done drops
`dword_18DB268` and calls `sub_40A630` again. Only once a row is complete does
the tail of the function expand it into pixels.

### The pixel expansion, and what colour type 4 really is

The expansion walks `v26` down from `dword_18DB270` to `dword_18DB274` writing
`0xFFBBGGRR` at `dword_18CBF00` and striding by `dword_18D9F90`. Type 2 folds
`dword_18D4350` in on the way, clearing the alpha byte of any pixel matching the
tRNS triple. Type 6 writes four bytes. Types 0 and 3 index `dword_190A420` with
the packed sample, once per pixel or per bit, with the shift out of `v26 & 7`,
`& 6` or `& 4` for depths 1, 2 and 4.

**Type 4 answers the question the round trip left open.** It reads two bytes per
pixel, one below the other in the reversed row:

```c
grey  = dword_190A420[ up[v26] ] & 0x00FFFFFF;   /* R = G = B */
out   = grey | bswap32( up[v26 - 1] );            /* the byte lands in byte 3 */
```

`v26` runs down from twice the pixel count while the row's bytes sit at offsets
1..2*pixels, so `up[v26]` is the row's **first** byte and `up[v26-1]` the second:
PNG's order, grey then alpha, is preserved on the way in, and the internal pixel
comes out as `0xAARRGGBB` with R = G = B = the ramp entry.

Which of the two bytes the *encoder* puts in which slot is a separate question,
and it is settled: **PNGOUT loses the alpha when it re-encodes grey+alpha as
colour type 4.** `sub_407B00` is the only two-byte filter, case 0 through case
4 all take bytes 0 and 1 of the internal DWORD low byte first
(`mov ecx, ebx` / `and ecx, 0FFh`, then `mov dl, bh`), which is what
`FilterSample(pixel, 2)` does. Under the layout above those two bytes are B and
G, both the grey, so byte 3 is never emitted.

This is visible end to end. A 30x30 type 4 input with more than 256 distinct
greys and a varying alpha comes back from `pngout.exe` as colour type 4 with no
`tRNS`, and its samples are the grey repeated:

```
$ python tools\decodetest.py --roundtrip   # ct4-many
ct4-many   896/900 pixels differ, first x=0 y=0 got (0, 0, 0, 0) want (0, 0, 0, 32)
```

while the *decode* of the same input is pixel exact:

```
$ python tools\decodetest.py                # ct4-many
ct4-many   ok  (30x30 ct4 d8)
```

So the loss is on the emit side, and the same input with few enough colours
round trips exactly, because `SuggestColType()` sends flags 1 to a palette
(`case 1: case 2: case 3: return 3;`) and only flags 1|4 to type 4
(`case 5: return 4;`):

```
ct4-few    ok  (20x20 ct4 d8)    # decode
ct4-few    ok  (20x20 ct4 d8)    # round trip, via a palette
```

`FilterSample()` is therefore right as transcribed; the encoder is faithful and
type 4 output from PNGOUT is simply lossy.

### The Huffman helpers, and how they were checked

`sub_40A580`, `sub_40A4A0` and `sub_40A440` were transcribed into a throwaway
model and driven against every IDAT in `build/`, comparing with `zlib`. That
settled three things the listing alone does not, and caught two mistakes in my
own reading:

* `sub_40A580` is puff's `construct()`. It counts `h[0..30]` from the lengths,
  prefix sums into `offs[0..31]`, then assigns `symbols[offs[len]++] = i`. The
  assignment **bumps `offs[len]` in place**, so what the consumers see is
  `offs[k] == sum(h[0..k])`, the *inclusive* cumulative array. Both consumers
  read that array starting at its first entry; reading it unshifted is what
  made `sub_40A440` look like a different function.
* `sub_40A4A0` builds the direct table keyed by the **bit-reversed** code. The
  inner loop `shr edx,1 / adc eax,eax`, run `maxbits` times, takes bit 0 into
  CF first and appends it as the new low bit, so it is a true reversal, not the
  "it just masks" reading it looks like. Slot counts come from `MASK[maxbits-L]`
  with `maxbits - L` counted down, giving `2^(maxbits-L)` slots a symbol, which
  is what fills the high (wildcard) bits. **0 mismatches** against canonical
  deflate codes for the fixed table and for every literal (maxbits 9) and
  distance (maxbits 6) table in the corpus. Symbols whose codes are longer than
  `maxbits` correctly get no entry and fall through to `sub_40A440`.
* `sub_40A440` is puff's `decode()` exactly. `esi` is reloaded as the data
  pointer every iteration, so it holds only the current bit, and the recurrence
  is `v = (bit + offs[len] - offs[len+1]) + 2*v` with `v < 0` as the stop test.
  The return index `offs[len] + v` then equals `index + (code - first)`. This
  was confirmed symbol-for-symbol against `puff` over 40 consecutive symbols.

Two things the fast path does that are easy to get wrong: it **peeks without
consuming** and then advances only by `codes[peek]`, and it only does that when
`codes[peek]` is non-zero.

**What is verified.** The inflate model is verified end to end:

* every one of the 31 IDAT streams in `build/` decodes byte for byte against
  `zlib`, and 16 further PNGs generated at `zlib` levels 0, 1, 6 and 9 crossed
  with the default, `Z_FILTERED`, `Z_RLE` and `Z_FIXED` strategies decode
  exactly as well;
* that covers stored, fixed and dynamic blocks, codes longer than the 9 bit peek
  so the `sub_40A440` fallback runs, and multi-bit length and distance fields.

**The extra bits are read least significant bit first**, which is what the
listing says: `sar edi, cl; and edi, mask` with `cl = offset & 7` and the offset
counting up takes the low bit of the byte first, and the value is assembled the
same naive way the Huffman path assembles a code. RFC 1951 stores these fields
most significant first, so this is worth stating plainly rather than
second-guessing: it is what the reconstruction does, and it is what decodes real
streams. The Huffman path is unaffected by the same fact because
`sub_40A4A0` reverses the canonical codes when it builds its table.

One trap in the distance table, which cost most of the debugging here and is
worth writing down: the first **four** distance codes have **no** extra bits.
PNGOUT's `sub_40AF90` builds them by bumping the extra-bit counter on odd indices
from 2 onwards, which gives `0,0,0,0,1,1,2,2,3,3,...`. The tempting
`extra = i / 2` is wrong at indices 2 and 3, and reading one bit too many there
desynchronises the stream with no local symptom.

The pieces underneath are pinned individually as well:

* the code-length parse was done twice over, once by matching canonical codes
  explicitly and once with `puff`, producing identical lengths and the same
  ending bit;
* the direct table was checked against canonical deflate codes for every
  literal and distance alphabet in the corpus, 0 mismatches;
* driven against `puff` on a real file, the model agrees symbol for symbol and
  bit for bit for 4000 consecutive symbols.

**The unfilter's shape, and a trap in reading it.** `sub_40A8D0` is one loop, and
Hex-Rays renders its exit in a way that reads backwards:

```c
    if (dword_18D9FA4 > 0)              return;   /* row not finished        */
    if (dword_18DA23C < dword_18DA254)  expand_row();  /* 0x40AB9D jnb skips it */
    dword_18DA23C += dword_18DA234;    /* ... but the advance ALWAYS runs */
```

`jnb loc_40AE33` at `0x40AB9D` skips *only the expansion* when the row index has
reached the height; `loc_40AE33` is the advance, and the expansion falls straight
into it. So every completed row is expanded and then the next row is started,
and the function returns only when the input runs out or a row is left
half-built. Reading the `jnb` as "skip the advance" - which the decompilation's
`break` strongly invites - yields a decoder that expands one row per call and
then does nothing, because `dword_18D9FA4` is left at zero. That is exactly the
contradiction that stopped the first attempt at `src/decode.c`.

The row index `dword_18DA23C` starts at `ystart + arg_1C` and steps by
`dword_18DA234`; the height it is compared against, `dword_18DA254`, is
`arg_14`, and the call site at `0x401E5F` shows `arg_14` is the fourth output of
`GetImageSize`, i.e. the height. `dword_18D9FA4`, the bytes left in the current
row, carries across calls, which is how a row can straddle a 16 KB block.

The expansion side is self-consistent and worth keeping: `dword_18DB270` and
`dword_18DB274` come out as `samples * width - 2` and `-2` for colour type 2 with
no interlace, which walks exactly `width` pixels.

`src/decode.c` now implements the PNG path: chunk walk, bit reader and pump, the
three Huffman helpers, the inflate body, the unfilter and the row expansion, for
colour types 0, 2, 3, 4 and 6 at depths 1, 2, 4 and 8. Interlaced input is
refused rather than guessed at.

**How far it is verified, precisely.** `tools\decodetest.py` drives
`tools\dumpdec.exe` - which links the same sources and calls the real
`DecodeImage()` - over 120 generated PNGs whose pixels are known by
construction, and compares the raw buffer the decoder hands to `Optimize()`
against what went in. All 120 match. Between them the cases cover every colour
type and bit depth the reconstruction accepts, every filter byte on every row,
stored, fixed and dynamic deflate blocks, all four zlib strategies, deflate
streams split across two, three and five IDAT chunks, sub-byte greyscale and
palettes, an RGB `tRNS`, `bKGD`, other ancillary chunks, images from one pixel
to 36000, and rows that straddle the 16 KB window hand-over.
`python tools\decodetest.py --roundtrip` then runs 21 of the same images
through `pngout.exe` itself and compares the file it writes.

The inflate half was also verified byte for byte against `zlib` on all 31
corpus streams and 16 generated ones, by dumping the window and diffing it
against Python's `zlib.decompress` output.

**Where it stands.** Every one of those cases matches. What follows is what was
wrong on the way, because each item is a trap the listing sets and the shape of
the code does not.

### The unfilter's addressing

The staging row is **mirrored and anchored at 0x18FA1C0**: row byte *k* lives at
`0x18FA1C0 + RowBytes - k`, so index 0 is unused and the row runs downwards
from `RowBytes` to 1 (`dec eax` *then* `mov [eax+18FA1C1h], bl` at 0x40A990,
which is `0x18FA1C0 + RowBytes - k` once the +1 and the pre-decrement cancel).
That fixes the three neighbours unambiguously:

| PNG's name | address | who holds it |
|---|---|---|
| `a`, the pixel-aligned left | `0x18FA1C0 + RowBytes - k + samples` | `RowA[ch]`, written as the byte is produced |
| `b`, the row above | `0x18FA1C0 + RowBytes - k` | the slot itself - Up, Average and Paeth all read and write the same address in place |
| `c`, up and to the left | written by the previous *pixel* | `RowC[ch]`, written as `b` was read |

`RowA[ch]` and `RowC[ch]` are indexed by `dword_18DC38C`, which walks
`dword_18DB85C[]` once per *byte*. That is exactly PNG's `bpp`: the value in
`RowA[ch]` was last written `samples` bytes ago, so it is the pixel-aligned left,
not the adjacent byte. The ring starts at 0 (`0x40A777`), and for colour type 2
it is 1, 2, 0 - a three cycle - which lines up with `bpp = 3`. The starting
value does not matter for the value either way; what matters is that the ring is
a cycle of length `samples`.

### The channel ring and the Paeth byte

The ring is set at IDAT setup (`0x409A96`), not at IHDR: `dword_18DB85C[0] = 1`
for any type with an alpha slot, `dword_18DB860 = 2` for type 2, `dword_18DB864 =
3` and `dword_18DB868 = 0` for type 6, and the whole array zero for types 0 and
3 - so it is a one slot ring there.

`dword_4110CC` starts at `0xFFFFFFFE` and is set to 4 by the format-4 decoder, so
**filter byte 4 is decoded as PNGOUT's own predictor, not PNG's Paeth**. Both
branches 4 and 5 are that predictor, and they are the same function: `sub_40AF40`
measures `|a + b - 2c|`, `|b - c|` and `|a - c|`, and the copy inlined at
`loc_40AAE0` reaches the same three numbers through `T[c-a]`, `T[2c-a-b]` and
`T[c-b]` off one indexed base - `lea edx, [18DAA60h + (c-a-b)*4]` with the
symbols added back at `[edx+ebx*4]`, `[edx+ecx*4]` and `[edx+eax*4]`. So filter
bytes 4 and 5 reconstruct identically, and a foreign PNG row that uses real
Paeth comes out wrong - which is PNGOUT's behaviour, and the test corpus has to
apply PNGOUT's predictor for byte 4 or it measures the difference instead of the
decoder.

**Two hypotheses that were ruled out**, so do not re-try them: the destination is
not misaddressed (`dword_18CBF00` really is a running pointer stepped by the
pitch, and it is stepped only when the row was expanded), and the channel
rotation is not the problem (a ring that enumerates a full cycle per pixel
changes nothing, because any starting value gives the same distance).

**The row buffer is not cleared between rows.** `ResetPass()` zeroes `RowA`,
`RowC` and `RowLeft` and nothing else - the `rep stosd` that clears `RowBytes + 1`
bytes of the row buffer is at `0x40A72B`, which runs once a pass. Clearing it per
row makes every row behave like the first one, and Up, Average and Paeth all
reconstruct against an empty row above: exactly the "flat or halved row"
symptom.

### Defects found by testing pixels, all fixed

* **The up-neighbour was read one byte too high.** With the row mirrored, Up,
  Average and Paeth all read and write `0x18FA1C0 + RowLeft` - the *same*
  address, in place. Reading `Up[RowLeft + 1]` instead takes the byte to the
  left of the one being written, so Up rows come out as the sum of two
  horizontally adjacent bytes. `Up` is now `RowUp` and the expansion indexes it
  with the original's own offsets.
* **The row record recorded the resolved filter, not the byte.** `Block[]`
  keeps what the file carried: `dword_18E909C` is set first, then `dword_4110CC`
  redirects 4 to 5 (`0x40A91B`, `0x40AED7`).
* **The packed cases stepped `v` by `8 / depth`.** `v` counts *bits*, so the
  step is `depth`: `dec ebx`, `sub ebx, 2`, `sub ebx, 4` at `0x40ABEF`,
  `0x40AC34` and `0x40AC7A`. Stepping by `8 / depth` ran the loop off the end of
  the pixel buffer.
* **The 256 entry widening ran at IHDR, where a palette is not known yet.**
  `0x409AF1` does it at IDAT setup, after the PLTE and the tRNS have both been
  read; at IHDR the table is still empty for colour type 3, so the widening
  spreads zeros over every index above the real entries.
* **`PackedSample()` invented a mapping the original does not have.** The three
  cases read one byte and shift it, with no mask at all - the mask is the
  pre-widened palette: `Up[v >> 3] >> (v & (8 - depth))` indexes a 256 entry
  table that is already periodic, so an unmasked byte is a legal index.
* **Colour type 3 at depth 8 skipped the table.** `loc_40ACC5` is the byte loop
  and it does go through `dword_190A420`, like the packed cases.
* **`BuildDecodeTables()` never advanced the canonical code.** It filled
  `MaskTable[maxbits - len] + 1` slots per symbol starting from `code = 0` every
  time, which happens to look right for a table where one length holds
  everything - the fixed table - and is wrong for anything else. `esi` in
  `sub_40A4A0` is the running canonical code and steps on by
  `2^(maxbits - len)` per symbol. This is what broke every dynamic block with
  more than one code length, which is most of them.
* **The pump read the anchor in bits where the listing uses bytes.** `[esi+8]`,
  `[esi+0Ch]` and `[esi+10h]` at `0x40A35D` are byte offsets; `WordAt()` takes a
  *bit* offset and divides by eight, so `+8` landed one byte along the data, the
  staged length came out as noise, and the next anchor pointed into the heap.
  A `DwordAt()` for bytes is the fix.
* **`NextHdr` was one chunk too far along.** `lea ecx, [esi+eax+0Ch]` has no
  `+8`: the anchor for the chunk after next is its *last* four data bytes, the
  same role this chunk's anchor has, which is what lets the backwards walk
  continue across the boundary. Adding the 8 puts the anchor inside the
  following chunk's data and every byte after the first IDAT is wrong.
* **The window hand-over was only checked where a literal is stored.** The
  check at `0x40A09C` is at the top of the *symbol* loop, and the match copy at
  `0x40A268` writes straight into the window without passing it. A
  compressible stream is mostly matches, so the boundary is missed entirely and
  the tail flush then reads tens of thousands of bytes past a 32 KB array.
* **The hand-over unfliltered "write index minus start" instead of 0x4000.**
  `push 4000h` at `0x40A0B4` and `0x409D09`. The index overshoots the boundary
  by whatever the last match's length was, and those bytes belong to the *next*
  hand-over; unflitering them twice corrupts the row they are in and every row
  after it. The same run also reset `WinWrite` to `start + 0x4000`, which threw
  the overshoot away.
* **The tail flush's first length was `0x4000 - (start & 0x7FFF)`.** The
  original is `(-start) & 0x7FFF` at `0x40A2CF`, and the two agree only while
  `start` is 0x4000; at 0x8000 the original's first call gets length zero.
* **`StartFilterByte()`'s Paeth redirect changed what the row record holds**
  (above), and `CurChannel` is set once a pass at `0x40A777`, not per row.

**Five `main()` and optimiser defects, all fixed.**

* `SETTINGS s;` was memset **twice**, the second time after the three
  "automatic" defaults had been written, so `s.coltype`, `s.filter` and
  `s.bitdepth` were all zero: a run with no `/c` went to greyscale rather than
  deciding for itself.
* The write path cleared `delta` on success and then printed it, so the `Chg`
  line always said "-<original> bytes, 0% of original". The result belongs in its
  own variable, `v58` in the original.
* Naming an output file that already ends in `.png` appended a *second*
  extension. `0x4019FD` is `len < 4 || (stricmp(tail, ".PNG") && len + 4 <
  MAX_PATH)` - the reconstruction had both the comparison and the room test
  inverted.
* `main()` returned the optimised size rather than the status. `delta` is what
  the `Chg` line reports; the exit status is `v58` - 0 written, 1 the write
  failed, 2 nothing smaller to write, 3 the optimiser bailed - and the write
  result has to go there.
* **`RowProgress()` computed a 32x32 -> 64 multiply in 32 bits.** `a * b >> 32`
  overflows the product first, so every run printed a percentage in the
  hundreds of millions. `unsigned __int64` spells out what `sub_403AF4` does.
* **`ScanImage()` left the flag unset for the common transparent case.** At
  `0x402D5C` the search for a see-through pixel sets bit 1 when it finds *any*
  pixel that is not opaque and is not the darkest one; the reconstruction only
  left the loop, so an image whose darkest pixel is the transparent background
  and whose other pixels are half transparent was written as RGB with the alpha
  silently dropped. `minsample >= 0x01000000` is also an **unsigned** compare at
  `0x402D09` (`jnb`), which matters because a non-opaque pixel is negative as a
  signed int.

Also worth recording: `dword_18DBA98` and `unk_18DC2A0` are the same array seen
through two symbols. `0x18DC2A0 - 0x18DBA98 == 0x808 == 257 * 8`, and the inflate
body indexes it by the raw symbol, so `dword_18DBA98[sym]` is entry `sym - 257`
of the length table `sub_40AF90` builds. That table is the standard one for
symbols 257 to 284, and the loop stops there - **symbol 285, the 258 byte code,
is written explicitly just after it** as `dword_18DC380 = 0` and
`dword_18DC384 = 258`, so the table has 29 entries, not 28.

### The Adam7 tables in `sub_40A630`

`dword_18DB268` is set from IHDR byte 12 at `0x409772` by `imul eax, 7`, so it
starts at 0 or 7. `sub_40A630` then walks it **down**, stopping at the first pass
whose tile is non-empty, so interlaced images skip their empty trailing passes.
The tables are nibble constants shifted by `4 * pass`:

| constant | shift | per-pass value, pass 7 down to pass 0 |
|---|---|---|
| `0x04020100` | `xstart = (c >> 4p) & 0xF` | 0 0 0 4 0 2 0 1 0 |
| `loc_402010` (the *address*, not a constant) | `ystart = (addr >> 4p) & 0xF` | 0 0 0 1 0 2 0 1 0 |
| `0x33221100` | `1 << log2step` → `dword_18D9F90` | 8 8 8 4 4 2 2 1 |
| `0x33322110` | `1 << log2step` → `dword_18DA234` | 8 8 8 8 4 4 2 1 |

That is, for pass 7 to pass 0:

| pass | xstart | ystart | xstep | ystep |
|---|---|---|---|---|
| 7 | 0 | 0 | 8 | 8 |
| 6 | 0 | 1 | 8 | 8 |
| 5 | 4 | 0 | 4 | 8 |
| 4 | 0 | 2 | 4 | 4 |
| 3 | 2 | 0 | 2 | 4 |
| 2 | 0 | 4 | 2 | 2 |
| 1 | 0 | 1 | 1 | 2 |
| 0 | 0 | 0 | 1 | 1 |

The X start is an immediate constant while the Y start is a shift of the
instruction's own operand address, which is why only the first shows up as a
literal in a hex dump.

`ebx` is the width and `edi` the height (`dword_18DA238 = be32(ihdr)` at
`0x409702`, `dword_18DA25C = be32(ihdr + 4)` at `0x40971B`), which makes the two
emptiness tests `cmp xstart, width` and `cmp ystart, height` at `0x40A66D` and
`0x40A672` correct, and the pass width
`(xstep - xstart + width - 1) >> log2xstep` at `0x40A6C9` the right count for
the tile's columns.

**The table is not a partition of the image, so PNGOUT cannot reconstruct a
specification Adam7 file.** Compare column 0 of a 16x16 image: pass 7 covers row
0, pass 6 row 1, pass 4 rows 2, 6, 10 and 14, pass 2 rows 4, 6, 8, 10, 12 and
14, and pass 1 rows 1, 3, 5, 7, 9, 11, 13 and 15. Rows 6, 10 and 14 are written
twice and rows 8 and 12 only once, by pass 2; nothing at all is written for
rows 2 and 4 by any pass with a step that reaches them. The spec's grid, for
comparison, is `0 4 0 2 0 1 0` by `0 0 4 0 2 0 4` with steps `8 8 4 4 2 2 1`;
PNGOUT's is neither that nor its transpose.

Two things follow, and they are why the code refuses interlaced input instead of
guessing:

* There is no arrangement of the pass data in a valid PNG that this table
  consumes correctly, so a transcription could not be verified against anything -
  there is no reference binary in the tree, and a self-built "PNGOUT grid" PNG is
  not well-formed because the tiles overlap and leave holes.
* Pass 0 is the whole image at step 1, which the spec has no equivalent of, and
  `sub_40A630` is reached from the IDAT setup (`0x409BB5`) for **every** PNG
  including non-interlaced ones. That is not a bug: for interlace 0 the pass
  index is 0 and the table gives exactly the whole-image geometry, which is why
  `src/decode.c` inlines it.

What is settled: the walk order, the emptiness test, the pass width, and that the
table itself is the original's and not a misreading of it - the two start tables
are transposed relative to the spec, which is the shape of a hand-transcribed
grid, and pass 0's whole-image row is consistent with the walk being written for
`interlace * 7` rather than the spec's seven passes.

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

**Transcribed** as `DecodePal8()` in `src/decode.c`, about sixty instructions
from `0x40D150` to `0x40D243`, and `DecodeImage()`'s dispatch in `sub_409560`
is now the original's: format code 1 PNG, 2 `FF D8` JPEG, 3 GIF, 4 this one, 5
`'BM'`, 6 `0A 05 01 08`, 7 `'DDS '` with a 124 byte header, then a
self-consistency test for Targa. Of those, 1, 4 and 6 are implemented; the rest
report that the format has not been reconstructed.

The whole function is:

```c
byte_190A824 = 3;  byte_190A825 = 8;  dword_190A820 = 0x100;
if (width  == 0) return -1;
if (height == 0) return -1;
for (i = 0; i < 256; i++, p += 3)
    dword_190A420[i] = (((((p[0] - 0x40) << 8) + p[1]) << 8) + p[2]) << 2;
for (row = roworigin; row < height; row++)
    for (col = 0; col < width; col++)
        dest[row][col] = dword_190A420[*p++];
```

Two details worth writing down:

* The transform is done in a **signed 32 bit** register with no clamp, and the
  result's top byte is the pixel's alpha - there is no separate alpha to set. So
  the alpha of an entry is `(p[0] - 64) >> 6`, and a first channel below 64
  borrows: `(0, 0, 0)` comes out `0xFF000000`, opaque black, while `(64, 128,
  192)` comes out `0x003FC000` with a *transparent* alpha. Both are in the
  original and both decide whether the image needs alpha downstream.
* The pixel loop clips against the width and height it was passed rather than
  trusting the header (`cmp ecx, arg_10` / `cmp edx, arg_14` at `0x403200`,
  `0x403206`), and it is the *last* row and column that get clipped - the
  `jge` guards are on the row and column ends.

`tools\dumpdec.exe` decodes it and `tools\decodetest.py --pal8` /
`--pal8-roundtrip` check three cases each way, including a palette built around
the wrap above and a single-colour image. The expected values have to be
computed with the same 32 bit wrap (`pal8_expected()` in the script), or the
comparison is against Python's arbitrary-precision arithmetic and means nothing.

Reaching a working round trip turned up three real defects in the *encoder*
side, all in code that no previous test could reach, because until this format
existed nothing ever left `palflag` non-zero - see **Known defects**.

### `sub_40DAC0`, PCX (format code 6)

Transcribed as `DecodePcx()`, 703 bytes of listing. Recognised by the first four
bytes as one dword, `0x0801050A` = `0A 05 01 08` — version 5, RLE, 8 bits a pixel
— and the size comes from the window bounds at +4..+0B as `xmax - xmin + 1` by
`ymax - ymin + 1`, which `GetImageSize()` already computed.

Two cases share the RLE reader:

* **`nplanes` 0 or 1: palettised.** With `nplanes` 0 the 256 entry palette is
  read from the **end** of the file (`lea eax, [edi+eax-300h]`, where `eax` is
  the file size) and `dword_190A820` becomes 256. Its transform is *not* the 8bpp
  format's — there is no `<< 2` and the bias is 256, not 64:

  ```c
  pal[i] = ((p[0] - 256) << 16) + (p[1] << 8) + p[2];
  ```

  which is the identity on the RGB triple, because subtracting 256 borrows into
  the top byte and leaves it at `0xFF` for every `r <= 255`. The entry comes out
  as the dword whose bytes are B, G, R, FF going up — opaque, as it should be.
  Worth writing down because the `- 256` looks like a mistake and is not one.

* **`nplanes` 3: RGB**, one RLE stream per plane, and **the plane number selects
  which byte of the pixel gets the sample**. The loop starts at `nplanes - 1` = 2
  and counts down to −1, where it resets to 2 (`dec edi` / `jns` at `0x40DD4E`,
  `mov edi, 2` at `0x40DD5A`), so the streams arrive in PCX's own order R, G, B
  and land in bytes 2, 1, 0 of `0xAARRGGBB`. PCX has no alpha, so `0x40DBF0`
  fills byte 3 of every pixel in the clipped rectangle with `0xFF` first and
  `byte_190A824` becomes 2. The three planes of **one row** are adjacent, which
  is what this plane counter assumes.

The RLE reader is `0x40DC7A`: a byte below `0xC0` is a literal with a count of
one, a byte at or above it has `& 0x3F` repeats of the byte after it, and a
count of zero (`0xC0`) advances to the next scanline writing nothing.

`nplanes` is neither 1 nor 3 — including the `nplanes == 0` that just loaded the
palette — and `0x40DCFC` / `0x40DD75` send it straight to the epilogue, so a
0-plane or 2-plane file sets up a palette and decodes no pixels at all. That is
in the original.

The clipping is the odd part, and it is reproduced as measured. The store tests
the row against the height it was passed and the **byte** offset against four
times the width (`0x40DCBA`, `0x40DCC0`), and the scanline length comes from
`bytesPerLine` when that is longer than the row *count* (`0x40DC36`) — two
quantities in different units compared as though they were the same. The row loop
runs to `height + yorigin` in rows and lets the store clip.

`tools\decodetest.py --pcx` and `--pcx-roundtrip` check four cases each way,
including one whose `bytesPerLine` is wider than the image, which is what
exercises that comparison.

### `sub_40D250`, Targa (format code 8)

Transcribed as `DecodeTarga()`, 733 bytes of listing. There is no magic, so the
format is guessed from the header being self consistent — the test at `0x409627`
and at `0x40D25D` inside the decoder are the same eight conditions
`GetImageSize()` applies: at least 19 bytes, colour map type 0 or 1, image type
below 12 with one of bits 1, 2, 3, 11 or 15 of `1 << type` set, a pixel depth
that is a multiple of 8, non-zero and no more than 32, and a descriptor whose top
two bits are clear.

The depth picks the colour type (`0x40D32C`): 8 bits greyscale, 16 and 24 bits
RGB, 32 bits RGBA. The 8 bit case also fills `dword_190A420` with the greyscale
ramp, and the two halves of that loop sit 4 bytes apart in the listing for a
reason worth stating: the read is at `dword_190A41C`, which is `dword_190A420`
one dword lower, so it is an ordinary sequential build — `Pal[i] = Pal[i-1] +
0x010101` from an opaque black at entry 0 — and the alpha byte rides along as
`0xFF`. Read as `Pal[i] = Pal[i] + 0x010101` it installs a ramp whose alpha is
zero everywhere but entry 0, which shows up at once as a picture that claims to
be transparent.

The four pixel widths are four different things:

| bytes | conversion |
|---|---|
| 1 | `dword_190A420[byte]` — the ramp or the palette |
| 2 | A1R5G5B5, expanded as `v << 3` |
| 3 | `dword \| 0xFF000000` — B, G, R, opaque |
| 4 | the dword as it stands — B, G, R, A |

The 16 bit case masks *in place* (bits 10-14 R, 5-9 G, 0-4 B) and then computes
`((B + (G + R * 8 - 0x40000) * 8) << 3)`, which lands the three channels in
bytes 0, 1, 2 as B, G, R with `v << 3` each — so the five bit channels are
**truncated, not scaled**: 31 comes out `0xF8`, not `0xFF`. Bit 15, the alpha
bit, is ignored; the top byte is `0xFF` whatever it says, because it comes from
the borrow out of the `- 0x40000`.

A colour mapped file has its *index* turned into a pointer into the map first
(`0x40D45E`, `0x40D477`), and the source pointer still steps by the entry size so
the two walks stay in step. At one byte a pixel the entry fetched is itself just a
byte, and **that byte** is what indexes `dword_190A420` — so a colour mapped
8 bit Targa decodes through the greyscale ramp the header switch installed a few
instructions earlier, and the file's colour map is read but never used. In the
original.

The run length reader is a state machine in `edi`, seeded at `0x40D403` by
`sbb edi, edi` then `dec edi`: −1 for an RLE type, −2 for a raw one. Below `0x80`
it is a raw packet count, and `& 0x7F == 0x7F` means the count is in the *next*
byte instead — Targa 2.0's repeater packet. At or above `0x80` it is a run and no
pixel is read at all: the previous one is stored again. The decrement at
`0x40D4DD` is `test edi, edi` / `jl` / `dec`, so a raw packet of n stores **n + 1**
pixels — one too many, harmless because the store is clipped, and in the
original.

Both origin bits are handled: bit 5 set walks rows from the bottom and bit 4 set
walks columns from the right, the destination stepping by `-pitch` and `-1`
respectively. `tools\decodetest.py --targa` and `--targa-roundtrip` check twelve
cases, covering all four origin corners, all three depths, RLE and raw, and long
runs so the run packets are actually used.

### `sub_40D560`, BMP (format code 5)

Transcribed as `DecodeBmp()`, 1340 bytes of listing. Two header shapes, and
which one it is says where everything is:

| | `biSize` | width | height | bits | compression | palette entry |
|---|---|---|---|---|---|---|
| `BITMAPCOREHEADER` | 12 | word `+0x12` | word `+0x14` | word `+0x18` | none | 3 bytes |
| `BITMAPINFOHEADER` | 40 | dword `+0x12` | dword `+0x16` | word `+0x1C` | dword `+0x1E` | 4 bytes |

`0x40D5F1` refuses a compression field that is neither 0 nor 3, so a 16 bit
image arrives as masked RGB rather than as plain RGB555.

**The palette and the masks sit after the 14 byte file header, not at
`fileimg + biSize`.** Both walks start from `lea eax, [esi+ebp+0Eh]` — `esi` is
`biSize` and `ebp` is `fileimg`, so it is `fileimg + 14 + biSize`. Reading it as
`fileimg + biSize` shifts a palettised image's whole table by three and a half
entries and nothing else notices, because every index still finds *an* entry.

The palette entries are stored `| 0xFF000000`, which forces the alpha opaque —
a 3 byte BGR entry has no fourth byte, and the fourth is read from whatever
follows it in the file. In the original, and for a 3 byte entry at the very end
of the buffer that reads past it. Two entries and a two bit image get black and
white installed first at `0x40D60F`, and the walk then overwrites them if the
file carries any.

The channel masks are the fiddliest part, and it is worth stating plainly what
the six globals hold. `sub_40D560` keeps three **bit counts** and three
**masks** in consecutive globals, and the conversion at `0x40D73F` is what turns
counts into masks:

```
shift = 24 - count - shift        r        mask = (-1 << (24 - count)) & 0xFF0000
shift = 16 - count - shift        g        mask = (-1 << (16 - count)) & 0x00FF00
shift =  8 - count - shift        b        mask = (-1 << ( 8 - count)) & 0x0000FF
```

`BI_BITFIELDS` overwrites both halves from the three mask dwords after the
header, with the bit count read by the shift-and-count loop at `0x40D710`. A 16
bit image starts at count 5 / shift 10, 5, 0 and a 32 bit one at count 8 /
shift 16, 8, 0 — so **16 bit BMP is RGB565 read without any scaling**: each
channel is its 5 bits `v << 3`, R and B top out at `0xF8` and G, five bits at
position 6, at `0x78`, and the alpha byte comes from the borrow out of the
`- 0x1000000`. Same truncation as the Targa A1R5G5B5 and for the same reason.

Rows are padded to four bytes — `((width * bpp + 31) >> 3) & ~3` at `0x40D7CA`. A
positive height is bottom up and the original handles that by stepping the
*source* pointer backwards from the last row (`0x40D7EC`) while the destination
steps forwards; a negative height is the top-down flag and flips it
(`0x40D7E4`). Either way the destination comes out the right way up.

**A top-down BMP cannot be read at all, and that is not in this decoder.**
`sub_409260` returns the height field raw — `mov ecx, [esi+16h]` / `mov [edx],
ecx`, with no negation — so a top-down file gives a negative height, and
`main()` then computes `4 * width * height` as a `size_t` and hands a huge value
to `malloc`, which fails: *"Out of memory"*. The top-down branch above is
faithful to the listing and unreachable through `main()`.

`tools\decodetest.py --bmp` and `--bmp-roundtrip` check six cases, both header
shapes and 8, 16, 24 and 32 bits.

### `sub_40CA70`, GIF (format code 3)

Transcribed and verified with twelve cases in each direction - the twelfth (`g-bit7`) pins the transposed-flag bug by construction: bit 7 set without bit 6, with 6 bytes after the descriptor for the signed skip to eat.  It decodes correctly *only* if those bytes are skipped (repointing the palette at them and clobbering `Pal[0,1]`, which the expectation models); a correct reader would parse them as LZW.  Passing proves the bug is reproduced, not fixed.

**The interlace and local-colour-table flags are transposed.** Two places in the
function get them the wrong way round, and both are visible in the decompilation
as well as the listing:

* `test cl, cl` / `jns` at `0x40CB66` is a *signed* test, so **bit 7** guards the
  block at `0x40CB6A` that sizes a table, repoints the palette at
  `0x40CB75` and steps the pointer over `3 * size` bytes. Bit 7 is the
  **interlace** flag.
* `test cl, 40h` at `0x40CB3D` sets `var_48 = 8` and `var_55 = 1`, and 8 is the
  first interlace pass's *stride*. So **bit 6** - the **local colour table**
  flag - is what starts the four-pass walk.

As written, a GIF carrying a local colour table (bit 6 set, bit 7 clear) is
decoded as interlaced and its palette is *not* skipped, so the LZW stream is read
from the palette bytes; and an interlaced one skips palette bytes that are not
there. Neither is recoverable from the file, and a local colour table is
extremely common. That is the original's behaviour and the transcription
reproduces it.

The palette pointer is the **global** colour table. `0x40CAAE` computes
`fileimg + 13` and `0x40CAC7` saves it into `var_24` before skipping `3 * size`
bytes; only the bit 7 arm overwrites it. So with bit 7 clear the palette is the
global table even when the image has a local one, and if the file has *no* global
table either then `var_24` is never written and the 256 entries are read from
whatever was on the stack.

The palette transform is the PCX trick again. `0x40CB98`-`0x40CBBE` loads `p[0]`
into `ecx`, subtracts `0x100`, shifts up twice and adds `p[1]` then `p[2`:

```
Pal[i] = ((p[0] - 0x100) << 8 + p[1]) << 8 + p[2]
```

which is the identity on R,G,B and leaves the top byte at `0xFF` by the borrow,
so a GIF ends up with the same internal `0xFFBBGGRR` byte order as PNG and BMP.
The entry counter is stored directly (`mov dword_190A420[ebp*4], ecx` at
`0x40CBC0`, with the source advancing by three), so the indices are `0..size-1`,
and `cmp ebp, 100h` is what stops the `rep stosd` at `0x40CBE6` that fills the
rest of the ramp with opaque black. A Graphic Control Extension's transparent
index has its alpha byte cleared at `0x40CBF9`.

The LZW runs over `dword_18D0030` (768 bytes of input), `dword_18D0330` (4100
code slots) and `byte_18D8F87`/`byte_18D8F88` (a slot's code and its length),
with `byte_18DD9D8` as the output stack. Codes arrive LSB first; the reader
shifts the tail down to the bottom of the buffer and appends the next sub-block
when it gets within three bytes of the end (`v48 = v59 - v47` at `0x40CF15`). A
code expands by walking `slot[]` - each slot's *tail* code - back to a root and
then pushing that root, and the emit loop at `0x40D025` walks the length *down*,
so the string comes out in order without being reversed first.

Two things about the code table are not what a straightforward reading suggests,
and both were needed to make the tests pass:

* **A code equal to the next free slot** repeats the *previous* string's own head,
  so the expansion has to start from `prev`, not from `next - 1`. `next - 1` is
  the last slot built, whose string is a different one.
* **The size bump needs `+1` on both sides.** The encoder's table runs one entry
  ahead of the decoder's, so the encoder steps up to `bits + 1` when the code it
  is about to assign is `2**bits - 1` while the decoder steps up after it has
  built slot `2**bits - 2`. Without the `+1` the first eight codes are right and
  every code after them is read at the wrong width.

The interlace passes are GIF's own after all - the switch at `0x40D068` keys off
the **stride** alone, 8, 8, 4 then 2, and each new pass starts at a fixed offset
from the row the image *began* at (`arg_1C`), so the four passes are 0, 4, 2 and 1.
Nothing guards the switch except the stride, which is why a stride of 1 only
reaches it on the last row and a stride of 2 ends the image. `var_55` only
alternates the two stride-8 passes.

Three things in the transcription are worth knowing about, because each produced
a plausible-looking decode rather than a crash:

* The clamp at `0x40CBAA` reads `(p[0] - 0x100) << 8 + p[1]) << 8 + p[2]`, and
  written without the outer parentheses C's `+` binds tighter than `<<`, so it
  becomes a shift by `8 + p[2]` - every palette entry a different colour, all of
  them valid. `/W3` catches it as C4554.
* `far` is a reserved word in MSVC's 16 bit memory models, so it cannot be the
  name of the right-hand clamp.
* Reproducing the interlace as "four passes if a flag is set" gets the row order
  wrong in a way that only shows on an image taller than four rows: the flag and
  the stride are different things, and only the stride is tested.

The frame placement at `0x40CCAE` computes four clamped bounds and runs four
`rep stosd` passes to lay down the background. The clamps look as if they mix the
image's left with its width, and they do - `v77` is
`min(xorigin + left, width)`, the image's *left* edge in destination coordinates,
not its right edge. It is still correct, because the fill covers `[colfirst, v77)`
and `[v78, faredge)` and so paints the gap left of the image and the gap right of
it without touching the image's own columns. `v78 = v28 + v18 + v57` is the right
edge. There is a fast path at `0x40CC1D` for the image at 0,0 exactly filling the
screen that skips the fill entirely, so the cases include a screen larger than
the image to exercise the other path.

### `sub_40B180`, JPEG (format code 2)

3491 listing lines, 36 666 characters of decompilation - about twice GIF. The
front end is worked through below so the transcription pass starts from facts.
The entropy body (`case 218`, 19 000 of those characters) is not transcribed.

**The marker dispatch is a two-step table.** `0x40B245` does `sub eax, 0C0h` and
`0x40B24A` `cmp eax, 1Dh`, so the range is `0xC0`..`0xDD` and anything outside
goes to `def_40B25B`; inside, `byte_40C560` maps the offset to a handler index and
`jpt_40B25B` dispatches on that. Seven markers are handled:

| marker | arm | what it does |
|---|---|---|
| `0xC0` `0xC1` `0xC2` | `0x40B262` | SOF0, SOF1 **and SOF2** |
| `0xC4` | `0x40B3C8` | DHT |
| `0xD9` | - | EOI |
| `0xDA` | `0x40B4..` | SOS and the entropy-coded body |
| `0xDB` | - | DQT |
| `0xDD` | - | DRI |

Everything else in the range - `0xC3`, `0xC5`..`0xC8`, `0xC9`..`0xCF`, `0xCC`,
`0xDC` - falls to the default, which is `v10 += v15`: skip the segment. So there
is **no** arithmetic coding (`0xC9`..`0xCB`) and **no** lossless (`0xC3`), and
`0xC8` (JPG extensions) is skipped rather than parsed.

**`0xC2` is progressive JPEG and shares the arm with `0xC0` and `0xC1` - and
that is correct, not a bug.** I wrote the opposite in the commit that introduced
this section. SOF0, SOF1 and SOF2 have *identical* layouts; only the marker
differs, so there is nothing for the arm to distinguish, and the work that
progressive needs is done later, in SOS. See the buffering branch below: it keys
off exactly the three parameters a progressive scan varies.

**The SOF parse is correct, but `inc ebx` at `0x40B262` is what makes it so.**
The marker loop leaves `ebx` on the byte *after* the two length bytes, which is
the precision byte, so the SOF arm increments once and then reads the height as
a word at `+0`, the width as a word at `+2` and the component count as a byte at
`+4`, both words through `Swap16`. The decompiler renders that increment as
`v16 = v10 + 1` with `v16 = v10 + 1` then `*(_WORD *)v16` for the height, which
reads as though the fields were all one byte early; they are not, and the extra
`+5` lands on the first component's id. Per component, three bytes:

* the component id, into `dword_18DC1A0[i]`
* the sampling byte, high nibble horizontal into `dword_18DEE68[i]`, low nibble
  vertical into `dword_18DA244[i]`
* the quantisation table selector, into `dword_18FA0A8[i]`

Each sampling factor is then mapped through an eight entry table at
`dword_410158` by a linear scan - `0x40B350`: `cmp esi, dword_410158[ecx*4]`,
`jz`, `inc ecx`, `cmp ecx, 8`, `jl` - and the *index found* is stored, in
`dword_18DE9D8[i]` horizontally and `dword_18DEE30[i]` vertically, not the value.
The maxima of the two are tracked across components (`v190`/`v188`,
`v191`/`v189`), and once the marker loop has ended each component's factors are
scaled: `dword_18CBF58[i] = h`, `dword_18DEE68[i] = 8 * h`,
`dword_18DA244[i] = 8 * v`, and the MCU edges `v201[i] = v188 - idx + 3`,
`v201[i + 4] = v189 - idx + 3`.

**A defect in the DHT index.** `0x40B3D4` computes the table's index as

```
index = spec < 0x10 ? spec : spec - 0x0C
```

where `spec` is the `Tc << 4 | Th` byte. DC tables (`Tc = 0`) get slots 0..15 and
AC tables (`Tc = 1`) get slots **4..7**, so AC0 aliases DC4 and AC1 aliases DC5.
It should be `- 0x10`. In practice it is harmless, because a JPEG has at most
four components so DC4..DC7 are never defined - the aliased slots are always
dead. A file that did define DC4 would be silently corrupted. In the original,
and reproduced.

The DHT body puts the sixteen count bytes into a twenty byte slot
(`unk_18CFF81 + 20 * index`, copied as four dwords), then walks lengths 1..16
building three things per length: the cumulative symbol table
(`dword_18D9FB4`), the start index for that length (`dword_18DB4C4`, the running
total minus the cumulative), and the symbols themselves, `memcpy`'d to
`byte_18E0898`. It also expands a ten bit fast table, `dword_18EA0A8` holding the
code length and `dword_18F20A8` the symbol, with the shift count counting down
from 9 - the usual look-ahead table.

**The DQT builds fixed-point tables, not standard ones.** The precision is
`spec >> 4`, and 16 bit entries are read big-endian. Each value is shifted left
19 (`v47 << 19`) and then put through *two* further scalings, chosen by two fields
of `*dword_18DF780` - three bits and five bits - indexing the table `off_4110D8`,
each a 64-bit multiply shifted back down by 24. So the quantised coefficients go
straight into whatever representation PNGOUT's own IDCT wants, and the standard
`q * x` integer scaling is nowhere to be seen.

`0xDD` is one swapped word, the restart interval.

**`sub_40C580` is a one-time init, guarded by `dword_190A1D4`** (`0x40B192`, and
`0x40B1A2` sets it). It is the part that says PNGOUT has a decoder of its own:

* the **zigzag order** `dword_18DF780[64]`, built by walking the seven
  anti-diagonals and alternating direction - `for i = 0, 2, ... 14` with
  `v3 = i + 49` stepping down by 7 and `v5 = i + 1` stepping up by 7 - and the
  **inverse zigzag** `dword_18DB758[64]` filled backwards (`for j = 63..0`).
* `byte_18EDF1[64] = 1 << (dword_18DF780[k] >> 3)`, which is where the DQT's two
  field reads come from.
* `dword_18DF888[128 + m] = m` and `[896 + n] = n`, a level-to-magnitude pair of
  ranges, and the two fixed-point conversions `dword_18E90A0[i] = v << 8` and
  `dword_18DC4D8[i] = (v - 256) << 16`.
* the IDCT's cosine basis, `dword_18CBF70/74` and `dword_18D4358/5C` filled from
  four arithmetic progressions stepping by `1470104`, `1858077`, `-360857` and
  `-748830`, two entries at a time.
* `memset32(&unk_18DFA88, 255, 0x180)`, `memset(&unk_18E0088, 0, 0x600)` and
  `memset(&unk_18D8D58, 0, 0x200)`.

So the IDCT is an integer one with its own basis tables, not a scaled AAN. The
`.bss` this decoder alone needs runs from `unk_18CFF81` to `unk_18FA1BC`.

**The corpus now exists.** `tools/jpeglib.py` writes baseline JPEG - greyscale,
4:4:4, 4:2:2 and 4:2:0, restart intervals and 16 bit quantisation tables - and
`python tools/decodetest.py --jpeg` runs 33 cases over it. Useful things about it:

* `sub_40B180` reads whatever Huffman and quantisation tables the file carries,
  so the corpus does **not** have to use the exact tables from the spec's annex
  K; any legal ones work and `jpeglib` builds its own from a code-length list.
  A slip in a table there cannot make a test pass that should fail, because the
  decoder under test never consults them - they only have to be self-consistent.
  The counts *are* checked against the values, since a mismatch there produces a
  file that is not legal.
* It also carries a separate, deliberately naive reader written from the spec,
  used only to check the writer. That reader is **not** a substitute for a third
  party decoder: if the writer and the reader share a misreading of the standard
  they will agree with each other and both be wrong.
* The pictures are smooth on purpose. A source with a sharp edge or a modulo
  wrap in it loses a large *legitimate* amount - ringing at a step edge reaches
  255 on a near-Nyquist pattern - so a bound measured against one says nothing.
  A flat picture comes back within 1 level, a smooth gradient within 33, and
  those bounds are what the cases assert; `checker` is kept as a smoke case with
  no bound, only "it decodes and it is the right size".

Four bugs in the writer, all found by the round trip and all of the kind that
produce a plausible file rather than a failure:

* `1/sqrt(2)` was folded into both the cosine table and the per-axis scale, so
  every coefficient was half what it should be and the round trip came back at
  a quarter. A flat block of 100 has a DC of 800, not 400.
* the quantisation table is stored in natural order but indexed by zigzag
  position, so both the coefficient and its divisor go through `ZIGZAG[k]`.
* the row count of the block grid was taken from a leftover loop variable, which
  is right for 4:4:4, 4:2:2 and greyscale and wrong only for 4:2:0 - the first
  layout with a vertical sampling factor of 2.
* the YCbCr coefficients were a wrong set; the ones that are actually the
  inverse of the reader's are `19595/38470/7471`, `-11056/-21712/32768` and
  `32768/-27440/-5328`.

The padding matters too, and was worth a change: filling it with black drags the
last row and column towards it, because a partial edge block then mixes real
samples with level -128. Repeating the edge - which is what a real encoder does -
brings a flat picture at an odd width back exactly.

So the corpus is ready and the entropy body can be transcribed against it. What
is still missing is a **progressive** file to pin the `0xC2` behaviour down; that
needs a second scan encoder, which `jpeglib` does not have.

#### Dequantise plus IDCT verified by composition (`dbl[zz]`, flat-DC check)

Integer coefficients go through `coef[t] * JpegQuant[qsel][ZIGZAG[t]]` into
natural row-major order, then `JpegIdct(dbl, 0xFF)`, with the IDCT blocks
dumped under `JPEGDEBUG`. Verified three ways that compose: coefficients
bit-exact, quant tables exact, IDCT correct as a transform - and one end to
end anchor: a flat block with DC=63 comes out as flat `528482304`, which is
exactly `63 * 0x800000` (`fixed_quant[0][0]`), proving the dequant mapping,
the zigzag placement and the IDCT gain combine correctly. AC mapping is
code-reviewed on top of that (same loop, same tables); pixels still await the
four colour tables, which are the only unverified link left.

#### Restart intervals verified bit-exact (`JpegRestart`, RSTn cycling)

The resync runs between MCUs when the interval countdown hits zero: drop
buffered bits, expect `FF Dn` cycling `D0..D7` from `D0`, advance past it,
reset all four DC predictors and reload the countdown. Verified 0/18 blocks
on a restart=2 file against the independent decode. The refill stops *before*
a marker (`FF` followed by nonzero) instead of consuming it as PNGOUT's
`++v102` unconditionally would appear to - because consuming `RSTn` as data
desynchronises everything past it, and the resync expects the marker still in
the stream. A mid-MCU marker sets a flag rather than decoding garbage.

#### The sequential entropy path is verified to coefficients (`JpegDecodeSeq`)

The bit reader, Huffman (10-bit fast with `sub_40C700` fallback), table
extend, DC prediction, AC runs with EOB and the MCU walk are now C, decoding
sequential scans without restart to integer quantized coefficients - and those
coefficients are **bit-exact** against an independent Python decode: 6/6 blocks
on single-component, 18/18 on three-component interleaved. Combined with the
already-verified IDCT, the pipeline is correct by composition up to the colour
conversion; only that last step needs the four tables. Still returning -1, so
production is unaffected; the coefficient dumps live under `JPEGDEBUG` as the
verification hook.

Two things it caught, both alignment rather than logic:

* The SOS `q` pointer starts on the **length bytes**, not on `ns` - unlike the
  segments above, whose length was already consumed. Reading `ns` from the
  length gives `ss=8, se=1, ahal=1` out of a baseline header and decodes
  garbage from the wrong offset; skipping two first makes `ss=0, se=63`.
* The first verification attempt compared against a Python reference that was
  itself misaligned by the same two bytes - the C's `-4` was right and the
  script's `0` was wrong. Fixed the script, not the C; 0 mismatches after.

#### SOS selectors are parsed; the entropy body is next

The SOS case now records each scan component's DC/AC table selectors against
its id (defaulting to 0), so the front end is complete through the scan
header: markers, SOF, DQT, DHT, DRI and SOS selectors. Still returning -1 -
the bit reader, coefficient loop and MCU walk that consume the scan are the
next piece, with the bit-reader/Huffman/extend helpers already drafted
(`JpegBit`, `JpegHuffSym`, `JpegExtend`) awaiting the block decoder that
calls them.

#### Global store search: nothing writes those neighbourhoods, anywhere

Seventy-six distinct store destinations in `0x18CC`-`0x18DF`, and **none** in
`0x18CC000-0x18CEFFF` or `0x18D6000-0x18D7FFF` - the two tables plus generous
neighbours on both sides. Every hit is another known table (Huffman, DQT, SOF,
SOS setup, IDCT basis, zigzag, magnitudes). So no instruction in the binary
stores to those addresses directly, under any symbol name. Combined with no
base loads, no `rep` fills, clean bills for every JPEG function, and a
complete function map, static analysis is exhausted: the fill uses a computed
pointer with no literal (needs execution trace, debugger, or binary - none
available), or IDA's bases are off by more than 8KB (no evidence for or
against). The tables stay zero-initialised `.data` as far as the listing
shows, yet colour JPEGs demonstrably decode in the original - so the fill
exists and is invisible statically. That is the definitive blocker statement;
further greps will not move it.

#### Index advance is per-pixel for 1:1, paired for 2:1 (`sar edi,1`)

The listing at `0x408FF0` shows the index computation directly, correcting
the decompiler's rendering: `mov ecx,[esi+ecx*4]` / `mov eax,[esi+ecx*4+100h]`
where `ecx = edi>>1` (`sar ecx,1`), then `sar ...,19` / `and ...,0FFFFFFFEh`,
then `dword_18CDF70[ecx*4]` / `dword_18D635C[eax*4]` etc. So both indices come
from one base (`esi`) 256 bytes apart, and the `>>1` pairs them: pixels
`(0,1)` share an index, `(2,3)` share, and so on. That is horizontal
subsampling (two luma pixels per chroma sample), i.e. the `h=2` path - for
`h=1` (4:4:4, greyscale) there is no pairing and indices advance per pixel.
This validates the per-pixel assumption behind the red/grey solves (all
`h=1` files), and means the slope fit must pair indices for `4:2:2`/`4:2:0`
but not for `4:4:4`. The `+100h` is 64 dwords (256 bytes) ahead, confirming
the decompiler's `v15[64]` was right about the offset but wrong to imply two
bases - it is one base, two offsets.

#### Colour slopes blocked on plane mapping; B~747M contradiction proves result != Cb

Red flat (`Y=-218103808`, want `(254,1,0)`) forces `B ~ +747M` via
`(Y+B)>>22 = 126` (for `R=254` from `T1[126]`), but `Cb` index `-358` reads
below-base (assumed zero). Contradiction - so `result` does not come from `Cb`
for red, and by extension the `v15`/`v4` plane assignment (which memory backs
`v6` vs `result` for single- vs multi-component, direct vs buffered) must be
mapped from the listing before any table solve. Nine plane-mapping hypotheses
(`Y/Cb/Cr` x `Y/Cb/Cr` for `v6`/`result`) all fail under analytic IDCT values;
exact C IDCT values (`Y=-218103808, Cb=-187170816, Cr=534773760` for red flat,
from verified dumps) confirm the indices (`Y->-416, Cb->-358, Cr->1020`) but
not which plane feeds which index. That single mapping read - trace `v4 =
a3+2560` and `v15` advances plus `v15[64]` through one `h=1` 8-pixel run in
the listing, noting which plane each lands in for the direct path - unblocks
everything: indices become known, tables solve to ranges (2^22 granularity,
functionally exact), slopes fit across primaries, `sub_408D80` transcribes
with hardcoded tables, pixels verify vs ImageMagick. All prior steps (front
end, entropy, IDCT, dequant, equation form, unsigned wrap, grey biases) are
committed and green; this mapping is the only missing link.

#### Red solved to ranges; green contradicts chroma-plane indexing

Red flat (`Y=-218103808`, want `(254,1,0)`) with `v6=result=Cr` index 1020
(both from `Cr` plane, flat so equal): `A1020 in [-318767104,-314572801]`,
`B1020 in [746586112,750780415]`, `(C+D) in [-314572800,-310378497]` -
midpoints give `(0,1,254,255)` as bytes `(B,G,R,A)`, i.e. exact red. So table
values at index 1020 are large biases (not slopes from zero), and `C` vs `D`
split needs `v6 != result` (gradient) data. Functionally exact; original
values unknowable to 2^22 granularity from pixels, which is fine since any
value in range gives identical output.

But green flat breaks chroma-plane indexing: its `Cb/Cr` IDCT (`-40`, `-50`
quantized) give indices `-680`/`-850`, both below-base (zeros), yet green
output IS green (not grey). So `v6`/`result` cannot come from `Cb`/`Cr` planes
there - at least one must come from `Y` (index 176, in-table) or another
in-table source. Hence `v15`/`v15[64]` plane assignment varies (by position,
component being output, or buffer layout) and must be traced per-pixel via
layout simulation, not assumed per-file. That mapping read is now the single
concrete step before table values solve generally; red and grey solve
specifically because their flats make `v6==result` (single index, sums only).

#### Fitted table biases at chroma index 0: `-2^23, -2^23, -2^22, -2^22`, worst 2

Brute-forced `(A0,B0,C0,D0)` over flat greys `L=0..255` through the replicated
equation with unsigned 32-bit wrap and logical `>>22` (which is what makes
negative sums land in `896..1023` instead of faulting - no OOB ever, since any
32-bit `>>22` is `0..1023`). Best is `A0=B0=-8388608 (-2^23)`,
`C0=D0=-4194304 (-2^22)`, worst 2 levels across nine greys with zero tables
elsewhere; all-powers-of-two, and the all-`-2^23` alternative is strictly
worse (worst 4). The residual 2 is systematic (-2 uniform, endpoints exact),
consistent with one quant step in `Y` (analytic DC vs true decoded DC) rather
than wrong biases - to be settled by feeding exact `Y` from C IDCT dumps
instead of analytic `Y`. Endpoints `L=0` and `L=255` are exact under the fit,
middles within 2.

So the four entries grey touches are (provisionally) those four integers.
Colour needs slopes (indices vary with chroma), which is the same fit with
colour flats probing distinct indices. Neither is committed as code yet -
these are fitted values awaiting the `sub_408D80` transcription that consumes
them, at which point C pixels vs ImageMagick decides exactness definitively.

#### Zero tables work for black and mid-grey, fail for white - and one constant cannot fix it

Flat greys through the replicated equation with zero address tables: `L=0`
gives `(0,0,0,255)` exact, `L=128` gives `(128,128,128,255)` exact, but
`L=255` gives `(0,0,0,255)` instead of white - the index lands at 128, in the
dead zone (`T0[128..895]` are zero; only `0..127` and `896..1023` are
populated). So the tables are nonzero, and specifically they must lift high-Y
into `896..1023`.

But no single constant can do that for all levels: `Y(L)` spans roughly
`-2^29..+2^29`, while the two populated regions total 256 indices (`128` plus
`128`) against a `2^30` span - a constant shift mapping that range onto two
disjoint regions must cross the dead `128..895` in between. Hence the table
index itself must vary with the pixel even for grey (i.e. the "chroma" index
tracks `Y`, because single-component reads chroma from somewhere
determinate, likely the Y plane itself). Solving the tables therefore needs
the index computation from `sub_408D80` mapped first - which planes `v15`
and `v4` point to for single-component - not just the equation. That mapping
is the next read, and it is small.

#### The colour equation replicates - zero tables give structured grey

Replicated `T0[(Y+A)>>22] + T1[(Y+B)>>22] + T2[(Y+C+D)>>22]` in Python with
the verified `T0/T1/T2` and zero address tables, fed flat mid-grey
(`Y = 128<<19`): out comes `(144, 144, 144, 255)`. Correct shape - grey plus
opaque alpha, from three table lookups summing to BGRA - off by 16 levels.
So the equation form is right and the four tables are small corrections, not
zeroes; flat greys at several levels give equations for `A0/B0/C0/D0`
(the tables at chroma index 0, the only entries grey touches), which is how
those four get determined without guessing. Colour needs the full tables;
grey needs just those four.

#### `sub_40C790` is transcribed and verified (`JpegIdct`)

The AAN integer IDCT is now C, variable for variable with the decompilation,
and checked as a transform rather than against pixels: linearity is exact
(`IDCT(a+b) == IDCT(a)+IDCT(b)` to the level), DC gain is 1 (flat in, flat
out), and a single AC coefficient comes out as the right cosine wave. It is
not yet called - the entropy body that feeds it is still missing - so this
is verified arithmetic awaiting its caller, not a decoder.

Two transcription notes, both the kind that produce a wrong transform rather
than a crash:

* The column loop's bounds are **byte addresses as unsigned**, not pointers.
  `v46`/`v47` are `(a1+64)` and `- 96` as integers (addr+256, addr+160), and
  the loop compares `(unsigned)v19 < v47` over addresses. Typing them as
  `int*` ends the loop after a single column; the rows come out right and
  everything below row 0 stays zero, which looks almost plausible.
* The products are multiply-high: `(unsigned __int64)(K * diff) >> 32` with
  `K` signed 64-bit, so a negative product contributes its high half. VC6
  has no `long long`; `__int64` is the spelling.

#### The front end is transcribed and verified (`DecodeJpeg`, markers through DRI)

The marker walk, SOF, DQT, DHT and DRI are now C, wired to the dispatch but
returning -1 until the entropy body lands - so production still refuses JPEG
cleanly, and `--jpeg` accepts both refusal messages meanwhile. Verified by
dumping the parsed tables out of the binary and comparing against
independently computed values: both fixed-point quant tables exact over all 64
entries, all four Huffman tables with correct counts, fast-table fills (1022,
1023, 1019, 1019 - the incomplete codes, as they should be) and symbols in
order, dimensions/components/sampling/restart correct across grey, 444, 422,
420, restart intervals and 16-bit tables.

Two things it caught, both worth recording because each produced a parse that
looked plausible rather than an error:

* Markers outside `0xC0..0xDD` must be **skipped, not rejected**. The first
  version refused APP0 outright; the original's `ja def_40B25B` sends them to
  the default arm, which skips by the segment length. (TEM and RSTn have no
  length and are continued past.)
* `JpegHuffs` needs **eight slots, not four**. DC tables take 0..3 but AC
  tables map via `spec - 12` to 4..7, so four slots overflow into whatever
  global follows - which presented as a garbage restart interval, not a crash.

And one real bug in the test generator, found because the C expects zigzag
order as the spec requires: `jpeglib` wrote DQT entries in natural row-major
order, misplacing every value past the second. The C put them in the wrong
band; fixing the writer to `q[ZIGZAG[k]]` (and the reader, which had
double-zigzagged to match the wrong writer) makes both tables exact. The
writer and its reader had agreed with each other and both been wrong - the
same failure mode the module docstring warns about, caught here by a third
implementation rather than by ImageMagick.

Still missing: the sampling-factor index map (`dword_410158`, no definition
in the reference), the entropy body, and the colour tables. The front end
stores the raw sampling factors and stops there.

#### The entropy body, and two helpers that make it tractable

`sub_40B180` calls only four things: `sub_409540` (Swap16), `sub_40C580` (the
one-time init above), and `sub_40C700` and `sub_40C790`. Those last two are the
whole of the decode inner loop, and both are recognisable.

**`sub_40C790` is the IDCT, and it is the AAN scaled integer transform.** It takes
a pointer to 64 dwords and a shift count, and works in place: eight iterations
handling a row each, guarded by `(shift & 1) != 0` with `shift >>= 1` after
each, then eight iterations over the columns. The four multiply constants are
recognisable once they are divided by 2^30:

| literal | / 2^30 | |
|---|---|---|
| `1518500224` | 1.41421356 | sqrt(2) |
| `1984016192` | 1.84775907 | 2 cos(pi/16) |
| `1402911296` | 1.30656296 | |
| `1162209792` | 1.08239220 | the row pass's c5 |

Those are the Arai/Agui-Nakajima scale factors to eight digits, so this is the
standard AAN butterfly rather than something of Ken Silverman's own devising.
The caller supplies the shift and does the final scaling and level shift. This is
worth knowing because it is a table-free transform: there is nothing here to
mis-transcribe beyond the arithmetic, and the constants can simply be written as
the literal 64-bit products they are.

**`sub_40C700` is one Huffman symbol**, taken a bit at a time:

```c
v5 = dword_410154[a2];              /* the byte, a2 the bit index */
result = (a3 & v5) != 0;            /* the next bit, a3 the mask */
v7 = 1;                             /* the code length so far */
while (result >= *((_DWORD *)&unk_18D9FB0 + 20 * a1 + v7)) {
    v5 >>= 1;
    result *= 2;
    if ((v5 & a3) != 0) ++result;   /* shift the code in */
    if (++v7 > 16) { *a5 = 16; *a4 = 0; return result; }
}
*a5 = v7;
v8 = byte_18E0898[256 * a1 + dword_18DB4C0[20 * a1 + v7] + result];
*a4 = v8;
```

`a1` is the table, `a2` the bit position, `a3` the bit mask; `*a4` comes back as
the symbol and `*a5` as its length. The per-length table it compares against is
the one DHT built - `unk_18D9FB0` and `dword_18D9FB4` are the same array under
two names, four bytes apart, the same thing that happened with the CRC table.
The 20-byte stride is the DHT slot size, and the symbol index is
`base[length] + code` with `code` the accumulated value, so the stored per-length
value has the two folded together.

`dword_410154` is the bit stream, `byte_190A825` the bit position (set to 8 in
the prologue, so MSB-first out of the byte), and `byte_190A824` set to 2 when the
scan has more than one component, i.e. when the data is interleaved.

#### The SOS arm, and what it says about progressive

`case 218` refuses outright if the width or height is zero, reads the scan's
component count into `unk_18FA1BC` and each component's id plus its DC and AC
table selectors, and then reads `Ss`, `Se` and `Ah`/`Al`. Those three are where
the progressive question is answered, and it is answered *here*, not in the SOF
arm:

```c
if (!restart_interval && (Ss || Se != 63 || (AhAl >> 4) || (AhAl & 0xF))) {
    /* buffer the whole image */
    ...  v58 = sum over components of (blocks_across * blocks_down);
    v9 = malloc(v58 << 7);          /* 128 bytes a block: 8x8 16 bit samples */
    memset(v9, 0, v58 << 7);
    v201[i + 32] = (int)v9 + 128 * running;
}
```

So a scan that is *not* a full sequential one, and has no restart interval to
resynchronise on, decodes into an interleaved block buffer first. That is
precisely the progressive case, so progressive input is at least partly
supported after all - which is why the shared SOF arm is not a defect. With a
restart interval the decoder streams instead, and with a plain sequential scan
neither branch is taken.

The buffer's geometry is worth recording because it is where the MCU sizes come
from: `v60 = (width + 8 * hmax - 1) >> (hmax_idx + 3)` and likewise for the
height, i.e. the MCU count shifted by the sampling index plus three (three being
half a block, so the edges are rounded up to a half-block). Each scan component
is then matched against the SOF components by id, and given its block cursor,
its sampling factors and its quantisation table selector, with the MCU edges
`v188 - h_idx + 3` and `vmax - v_idx + 3`.

#### The inner decode loop

The bit stream is a 32-bit register `v83` with `v81` counting *down* the bits
still in it, refilled whenever a read would not have enough:

```c
v103 = ((unsigned int)(15 - v81) >> 3) + 1;      /* for a Huffman symbol */
v81 += 8 * v103;
do {
    v104 = *v102++;
    if (v104 == 0xFF) ++v102;                     /* the byte stuffing */
    v83 = v104 + (v83 << 8);
} while (--v103);
```

The coefficient path refills the same way with `((v94 - v81 - 1) >> 3) + 1`,
i.e. "however many bytes it takes to have `v94` bits", so both paths share one
refill. `v81` counting down is why every read is written as `v81 -= length`.

**Huffman symbols go through a ten bit look-ahead first.** Ten bits are peeked
without consuming them:

```c
v105 = (v83 >> (v81 - 10)) & 0x3FF;
if (v105 >= v200)
    sub_40C700(dword_18CFF70[v166] + 4, v81, v83, &symbol, &length);  /* walk */
else {
    symbol = dword_18F60A8[2 * v105];           /* the fast table */
    v81 -= dword_18EE0A8[2 * v105];            /* and its length */
}
```

`v200` is how many of the table's 1024 slots DHT filled, so a peek below it hits
and anything else falls back to the bit-at-a-time walk. The tables are two dwords
per peeked value: `dword_18EE0A8` and `dword_18F60A8` are exactly `0x400` bytes
past the `dword_18EA0A8` and `dword_18F20A8` DHT writes, so there are two halves
of the same array and the inner loop uses the second. (The decompiler's
`*(int *)((char *)dword_18F60A8 + (_DWORD)&v194[v106])` is just
`dword_18F60A8[2 * v105]` with a stack slot standing in for the array base.)

**The DC difference is sign-extended through a pair of tables** rather than by a
shift, which is worth noting because it is not how the spec words it:

```c
v98 = dword_4100D8[v94];                        /* the mask, by size   */
v99 = v98 & (v83 >> v81);
if (v99 <= *(_DWORD *)&aOut8dBytesStor[4 * v94 + 20])
    v99 -= v98;
dword_18D9F94[v166] += v99;                     /* the predictor       */
```

The predictor is one dword per component, accumulated rather than assigned.
`aOut8dBytesStor` is another `.rdata` table of thresholds indexed by size.

**Progressive is handled properly, not incidentally.** The coefficient store
shifts by `Al` (`*(v82) = LOWORD(dword_18D9F94[v166]) << v182`, where `v182` is
`Al`), the magnitude bits are skipped when `Ah` is set - `if (v180) goto
LABEL_198` before the bit read - and a DC-only refinement scan works because
`v101 = max(Ss, 1)` clamps the spectral start so the AC loop never runs. The zero
runs are accumulated through `dword_18D158[v108]`, which is the **EOB run**
extension: a run of 15 zeroes followed by size zero adds to a counter instead of
emitting more EOBs, and `if (v170) { if (v180) goto LABEL_198; --v170; }` drains
it. That is a real progressive implementation, which settles the question the
shared SOF arm raised - `sub_40B180` is a full baseline-plus-progressive decoder,
and the three SOF markers sharing one arm is simply because they share a layout.

#### The output path, and `sub_408D80`

`sub_40B790` is called once per 8x8 block, `v177` stepping 256 bytes a time
(64 dwords), with the row bound `8 * dword_18CBF58[v166]` - eight rows per block,
times the horizontal sampling factor. There are two call sites, which is the
sequential path IDCTing straight out and the buffered path IDCTing out of the
progressive buffer afterwards.

The pixels themselves are not written by `sub_40B180`. It ends its loop with

```c
off_4110F8(v142, v141, &unk_18D8358);
v142 += dword_18DEE68[0];
...
v141 += dword_18DA244[0];
```

an **indirect call**, and `off_4110F8` is initialised in `.data` to
`&sub_408D80`. So the colour converter is a separate function reached through a
global pointer, called once per output position with the first component's
sampling factors as the step.

`sub_408D80` does the upsampling by **bit shift, not a filter** - no fancy
triangle interpolation anywhere:

```c
v4 = a3 + 4 * ((v9 >> dword_18DEE40)
               + 8 * ((v42 >> dword_18DC388) + 8 * dword_18DB898));
```

`dword_18DEE40` and `dword_18DC388` are the sampling *indices* the SOS arm
recorded for the first component, so a chroma sample is picked by shifting the
output coordinate right by them. Point sampling, then.

**And the YCbCr conversion contains no multiplies at all.** The coefficients are
folded into the addresses of four tables and the result is three lookups added
together:

```c
pixel = dword_18DF888[(v16 + dword_18CDF74[v6]) >> 22]
      + dword_18DC4D8[(v16 + dword_18D6358[result]) >> 22]
      + dword_18E90A0[(v16 + dword_18CDF70[v6] + dword_18D635C[result]) >> 22];
```

with `v6 = (*v15 >> 19) & 0xFFFFFFFE` and `result = (v15[64] >> 19) & 0xFFFFFFFE`
the two chroma planes - the `>> 19` because `sub_40C790` leaves its output scaled
by 2^19. `dword_18CDF70`/`dword_18CDF74` and `dword_18D635C`/`dword_18D6358` are
the Y-plus-k-times-chroma sums precomputed for the even and odd sample positions,
and the three result tables are the ones `sub_40C580` built: `dword_18E90A0[i] =
v << 8` and `dword_18DC4D8[i] = (v - 256) << 16`. So G is the sum of two table
lookups and R and B one each. `if (dword_18CBF58 == 1)` is **not** a greyscale path, whatever that sentence
once said. `dword_18CBF58[i]` is the horizontal sampling factor, so `== 1`
means no horizontal subsampling - and that branch reads all four address
tables exactly like the `== 2` branch does. There is no output path that
avoids them, greyscale included. So a "greyscale first" slice cannot side-step
the unknown tables, and the transcription has to carry all four as parameters
from the start.

Between all of that the whole decoder is accounted for: marker walk, SOF, DHT,
DQT, DRI, the buffering branch, the bit reader, the ten bit look-ahead, the
extend, the progressive refinement with EOB runs, the AAN transform in
`sub_40C790`, the point upsampling and the table-driven colour conversion in
`sub_408D80`. Nothing in the JPEG path is unread, so the transcription pass has
everything it needs - and the pieces that were hardest to pin down turned out to
be standard, which is the best possible outcome for a transcription.

#### The one thing still missing: who fills the colour tables

`sub_408D80` reads four tables - `dword_18CDF70`, `dword_18CDF74`,
`dword_18D635C`, `dword_18D6358` - and nothing in `reference\text.lst` ever
writes any of them. That is worth stating carefully, because it is the one thing
standing between here and the transcription.

What was checked:

* All four are `.bss` (`_globals.c` lists them uninitialised, `// weak` with no
  initialiser), so they are zero on entry.
* Searching the listing for those four names gives **twelve hits and all twelve
  are reads**, all inside `sub_408D80` at `0x408F1A`..`0x40918E`.
* There are **no direct writes** anywhere: no `mov dword_18CDF70, ...` and no
  `[dword_18CDF70 + ...]` store.
* The listing is not missing the writer. It runs continuously from `0x401000` to
  `0x40E600` with only two gaps, `0x40E456..0x40E566` (272 bytes, a library stub
  past the last function at `0x40E584`) and `0x40E5B8..0x40E600`. Nothing of
  consequence is absent.

So the fill has to go through a **register-held pointer**, which is why no address
literal appears for it. Two further hints that IDA's naming here is unreliable,
and the reason I am not going to reconstruct these by guesswork:

* The listing indexes them as `dword_18D635C[eax*4]` and `dword_18CDF70[edx*4]`
  while `_globals.c` types the same symbols as `int ...[]` with no scale. The two
  cannot both be right, so these are splits of one array IDA could not type - the
  same situation as `byte_40C560`/`jpt_40B25B` and the CRC table, where one
  object had two names.
* `off_4110D8`, the eight-entry scaling table DQT indexes, is not an array either
  but `&unk_16A09E6`, a pointer into the `.rdata` blob.

The consequence for the work: the IDCT, the bit reader, the Huffman tables, the
progressive refinement, the marker walk, the upsampling and the *shape* of the
colour conversion are all known, and every one of them is standard. What is not
known is the contents of four tables that hold `Y + k*Cr` and `Y + k*Cb` folded
into addresses. Transcribing `sub_408D80` without them would mean inventing the
colour conversion, which is the one thing this reconstruction is not allowed to do
- so the next pass traces the pointer in the listing instead.

#### Where the reference material runs out

Chasing those four tables turned up something more useful than the tables: two
concrete gaps in `reference/`, both small.

**`reference/rdata.lst` holds only the string constants.** It starts at
`0x40F0F0` with `aForAnEasyToUse` and runs on through the usage text - it has no
`411074`, no `4110CC`, no `411080`, nothing numeric at all. The numeric `.rdata`
lives in `_globals.c` instead, which is why `dword_411074 = 128`,
`dword_411078 = 128`, `dword_4110CC = -2` and

```
int dword_411080[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
```

are all known from there. Nineteen dwords is not a guess: `0x4110CC - 0x411080`
is `0x4C`, exactly 19, so IDA read the array up to the next symbol it recognised.

**The eight-entry table DQT indexes is found, in `reference/data.lst`.**
`0x4110D8` + 8 * 4 is `0x4110F8`, precisely where `off_4110F8 = &sub_408D80`
sits - so `0x4110D8..0x4110F7` is eight dwords and then the function pointer.
`reference/rdata.lst` has none of it (strings only, from `0x40F0F0` on), but
`reference/data.lst` lists all eight as dwords IDA misread as offsets. Divided
by 2^24, which is what the DQT's `>> 24` does to them:

| dword | / 2^24 |
|---|---|
| `0x16A09E6` | 1.41421354 |
| `0x163150B` | 1.38703984 |
| `0x14E7AE9` | 1.30656296 |
| `0x12D062F` | 1.17587560 |
| `0x1000000` | 1.00000000 |
| `0x0C9234E` | 0.78569496 |
| `0x08A8BD4` | 0.54119611 |
| `0x046A157` | 0.27589935 |

1.41421354 is sqrt(2) to eight digits with 1.0 sitting exactly where the DC
band belongs, so these are the per-band DCT scalings and `off_4110D8` is
`int[8]`, not a pointer - `_globals.c`'s `&unk_16A09E6` is IDA forming an
address out of the first dword, which is in neither `.text` nor `.rdata` and
cannot be right. One gap closed, from the reference material, nothing invented.

**The four tables are two arrays, not four.** The listing's
`dword_18CDF70[ecx*4]` is `dword_18CDF70[ecx]` - IDA renders `arr[eax*4]` when it
believes the element is a byte - so `dword_18CDF70[k]` and `dword_18CDF74[k]` are
consecutive dwords at `0x18CDF70 + 8k`, and likewise `0x18D6358` and
`0x18D635C`. That is one array of interleaved pairs, and one more, which is the
same shape `sub_40C580` builds for the IDCT basis (`dword_18CBF70`/`74`,
`dword_18D4358`/`5C`). Two arrays, read by `sub_408D80`, written nowhere in the
listing.

One trap to know about: **`reference/bss_refs.txt` mislabels these.** It files
all of them under `sub_408690` - `PutByte()` in the current tree - because
`0x409018` and `0x409028` fall inside a span IDA still labels with the previous
function, while the decompilation clearly puts them in `sub_408D80`. Trust the
addresses in that file; do not trust its function names without checking.

So finishing JPEG needs one thing, and it is not analysis any more:
whoever writes the two interleaved arrays at `0x18CDF70` and `0x18D6358`. It is
not in the listing, not in the dispatch (`sub_409560` never touches those
addresses), and not in `reference/data.lst` either - so it fills through a
register-held pointer somewhere, and tracing that pointer is the remaining job.
Everything else about the decoder is known, standard, and has its values.

#### `sub_40C580` replicates exactly - three of the six colour tables are known-good

The one-time init was reimplemented in Python from its decompilation and every
table it builds checks out, which matters because three of them feed the colour
conversion directly:

* the zigzag `dword_18DF780[64]` comes out **identical to Annex K** - all 64
  entries, built by the same seven-anti-diagonal walk;
* the inverse `dword_18DB758` is consistent both ways, and `byte_18EDF1[k] =
  1 << (zigzag[k] >> 3)` follows;
* `dword_18DF888` maps `0..127 -> 128..255` and `896..1023 -> 0..127`, and the
  two fixed-point conversions `dword_18E90A0[i] = v << 8` and
  `dword_18DC4D8[i] = (v - 256) << 16` replicate cleanly;
* the IDCT basis fills 2048 entries from the four progressions as written.

So `T0 = dword_18DF888`, `T1 = dword_18DC4D8`, `T2 = dword_18E90A0` are known
quantities. The colour equation `T0[(Y+A[cr])>>22] + T1[(Y+B[cb])>>22] +
T2[(Y+C[cr]+D[cb])>>22]` therefore has only the four address tables unknown,
and those must be `k * chroma` for small integer `k`. That is a fittable
problem: transcribe the decoder with those four as parameters, solve for them
against ImageMagick on the corpus, and either recover the standard
coefficients (confirming them) or discover PNGOUT's own (still correct for
PNGOUT). The form is known from the code structure; only the constants are
fitted, and a failed fit would say the form itself is wrong.

#### All forty-seven stores in `sub_40B180` are accounted for - and it is not there either

Forty-seven distinct store targets in `0x40B180..0x40C542`, and the only
register-indirect ones are three known operations, not a fill:

* `mov [ecx] / [ecx+4] / [ecx+8] / [ecx+0Ch]` at `0x40B403..0x40B414` - the DHT
  sixteen-byte count copy (`*v27 = *v26; v27[1..3]`), straight-line, four
  dwords. The sole pair-store pattern in the function.
* `mov [edx+ecx*4], eax` at `0x40BFB3` - the decoded DC coefficient into the
  block, `ecx` being the zigzag index from `dword_18DF780[edi*4]`.
* `mov [ebx+edi*2], ax` / `mov [ebx+edi*2], dx` - sixteen-bit coefficient
  stores (note `ax`/`dx`, not `eax`), the progressive/block buffers.

Everything else is a named `.data` table from the SOF/DHT/DQT/SOS setup or a
stack local. So the colour-table fill is not in `sub_40B180` by any store
form - direct, `rep`, pair, or indexed. Combined with clean bills for
`sub_40C580`, `sub_408D80` and the dispatch, that exhausts every function
that touches JPEG state.

What remains is one of two things, and both need more than the listing
alone: a fill through a pointer held in another variable (so neither the
address nor a `lea` of it ever appears - find it by tracing a `.data`
pointer value, not an address literal), or IDA mis-segmenting the symbols
(the `.data: ... dd ?` could still be wrong about *which* object lives at
those bytes). The `pngout.exe.lst` DATA XREFs confirm only `sub_408D80`
reads them, so whichever it is, it has no other reader to cross-check
against.

#### The fill is not a `rep` either - narrowing to computed stores

Seventy-two `rep` operations in the binary, and every one in JPEG range is a
small, attributable operation rather than a table fill:

* `0x40B1ED rep stosd` - the prologue clearing eight dwords (`ecx = 8`).
* `0x40B4A9 rep movsd` / `0x40B4B0 rep movsb` - a segment copy (`esi = ebx`,
  count from `ebp`, DHT/SOS setup data).
* `0x40B517` / `0x40B530 rep stosd` - small fills feeding
  `dword_18DEE48[ecx*4]`, the Huffman count (DHT setup).
* `0x40B818 rep stosd` / `0x40B81F rep stosb`, `0x40BD23 rep stosd` - the same
  class of small setup fills.
* `0x40C634`, `0x40C642`, `0x40C6EE rep stosd` - the three `memset`s in
  `sub_40C580`'s decompilation.
* The GIF `rep`s (`0x40CBEB`, `0x40CD41`, `0x40CD88`, `0x40CDA3`, `0x40CDE0`,
  `0x40CECD`, `0x40CED4`, `0x40CF47`, `0x40CF52`) - palette fill and LZW
  refill, all known.

So the two interleaved arrays are not `rep movsd`'d from `.rdata` and not
`rep stosd`'d. Combined with the earlier findings - no direct store, no base
load, not in `sub_40C580`, not in `sub_408D80`, not in the dispatch - the fill
must be a **loop with computed register stores** (`mov [reg], ...` /
`mov [reg+4], ...` in pairs), sitting in `sub_40B180`'s setup code. That is a
much smaller search than "anywhere": read that function's DHT/SOS setup loops
for pair stores, rather than grepping the binary again.

#### Those tables are `.data`, not `.bss` - and the cheap hypotheses are out

`pngout.exe.lst` settles the segment question directly:

```
.data:018CDF70 dword_18CDF70   dd ?
.data:018CDF74 dword_18CDF74   dd ?
.data:018D6358 dword_18D6358   dd ?
.data:018D635C dword_18D635C   dd ?
```

So they are `.data`, zero-initialised (`dd ?`), read only by `sub_408D80` (the
DATA XREFs are all `sub_408D80+...`), and never written by any instruction that
names them. The `.bss` framing in the notes above is wrong; the addresses and
the zero-initialisation were right, the segment was not.

Three more eliminations, all against the listing rather than the
decompilation:

* `sub_408D80`'s stores are fully accounted for - twenty distinct targets,
  all stack locals or the register-held pixel pointers - so Hex-Rays did not
  drop a table-building block there. That function is completely read.
* There is no `imul reg, <large decimal>` in twenty-two million lines of
  listing. The colour coefficients are not decimal immediates - so the
  builder either uses hex immediates, three-operand forms, or no multiply at
  all (addition loops, or a copy from `.rdata`).
* The function map accounts for every byte with no hidden builder, and no
  instruction loads those bases into a register.

What is left is a full data-flow trace: find the `rep movsd`/`rep stosd` or
arithmetic loop whose destination is computed rather than literal, most likely
by working backwards from a `.rdata` source or forwards from an init flag. It
is real reverse-engineering work, not another grep, and it is still the only
thing between here and the JPEG transcription - but everything it is *not* is
now written down, which is what makes it a scoped job instead of a hunt.

#### Narrowing the `.bss` writer: three clean bills and what is left

The two interleaved arrays the colour conversion reads were chased through
every cheap place they could hide, and all came back clean. Recording this so
it is not done again:

* **`sub_40C580` is not the writer.** Its listing range (`0x40C580` to
  `sub_40C700`) holds exactly ten distinct store targets -
  `byte_18DEDF1[eax]`, `dword_18CBF70[eax]`, `dword_18CBF74[eax]`,
  `dword_18D4358[eax]`, `dword_18D435C[eax]`, `dword_18DB758[ecx*4]`,
  `dword_18DC4D8[eax]`, `dword_18DF780[edi*4]`, `dword_18DF888[eax*4]`,
  `dword_18E90A0[eax]` - and every one is in its decompilation (zigzag,
  inverse zigzag, the four IDCT basis arrays, the magnitudes, the two result
  tables). Nothing else is stored there.
* **`sub_408D80` is not the writer either.** Twenty distinct store targets in
  `0x408D80..0x409251`, and all are stack locals or the register-held pixel
  pointers (`[ebx]`, `[ebx-4]`, `[edi]`). No `.bss` store at all.
* **The dispatch is clean.** `sub_409560`'s decompilation never mentions
  `18CDF`, `18D63`, `18E90A0`, `18DC4D8` or `18DF888`.
* **No instruction loads those bases.** Fifty-one instructions in the whole
  listing load a `0x18xxxxxx` address into a register, and exactly three touch
  the `0x18C8`/`0x18D6` neighbourhood - `lea ebx, dword_18CBF68` (unfilter),
  `lea ecx, unk_18CFF81` (DHT table base, known), `push offset dword_18CBF08`
  (dispatch). None targets `0x18CDF70` or `0x18D6358`.
* **The full listing agrees.** `pngout.exe.lst` (924 MB, the complete IDA
  output) has six hits for those two addresses in its first two million
  lines, and all six are the known reads at `0x408F1A`..`0x409166`.
* **The function map is complete.** `reference/fn_index.txt` accounts for
  every byte from `0x401000` to `0x40E429` with no gaps large enough to hide
  a table builder; `sub_408D80` itself spans `0x408D80..0x409251`, which is
  why `bss_refs.txt` filing those reads under `sub_408690` is a stale label.

So the fill goes through a pointer computed by arithmetic from another base,
or IDA put these symbols on the wrong segment. Either way it needs a full
data-flow trace through the listing, not another grep - which is a dedicated
session with `pngout.exe.lst`, and is now the only thing standing between here
and the JPEG transcription.

### `sub_40DD80`, DDS (format code 7)

Transcribed as `DecodeDds()`, 1158 listing lines, and verified pixel-exact
with eight cases in each direction - DXT1/2/3/4/5 and uncompressed32.  DXT2
(implicit punch-through alpha, no alpha block) and DXT4 (explicit alpha with
premultiplied colour) exercise the even-code un-premultiply path (`v101[a] =
`0xFF0000 / a`) that DXT1/3/5 never touch; C-vs-expected is exact on both.
ImageMagick cannot read DXT2/DXT4 at all (`ImageTypeNotSupported`), so those
two lack third-party pixels and rely on exact transcription fidelity plus
structural sanity instead - stated, not hidden.

The front end is as analysed: `ddspf.flags` at +80, DDPF_RGB (0x40) taking an
uncompressed A8R8G8B32 surface with the four masks exactly, DDPF_FOURCC (0x4)
reading `'D' 'X' 'T' c` with `c - '0'` in 1..5, and `and eax, 1` at `0x40DEFE`
sending the odd codes down the straight path.

**The even codes store premultiplied colour and the decoder divides it back
out.** `0x40DF0E` builds a 256-entry table with `v101[a] = 0xFF0000 / a`
(`v101[0] = 0x10000`, to avoid the divide by zero), and each channel goes out
as `(channel * v101[alpha]) >> 16` with saturation - i.e. channel * 255 / alpha.
So DXT2 and DXT4 are un-premultiplied to straight RGB here, and the odd codes
skip it. The even codes also never punch through: the `c0 <= c1` test requires
`v63 == 1` as well.

The colour block is a DXT1 block for every code. The endpoints expand with
R = `(c >> 8) & 0xF8`, G = `(c >> 3) & 0xFC`, B = `(c & 31) << 3` - G is `<< 2`,
not the usual `<< 3 | >> 3 - and the four-colour ramp is `(21845 * x) >> 16`,
i.e. /3, while DXT1 with `c0 <= c1` takes the average and a transparent fourth.
The alpha is per code: DXT1 from a four-entry table, DXT2/3 an explicit nibble
times 17 (low nibble first), DXT4/5 an 8-entry ramp with `(13107 * x) >> 16`
(/5) for the six-value case and `(9362 * x) >> 16` (/7) for the eight-value
one - and those two constants are the right way round, the six-value branch
being `a1 >= a0`, which is worth checking because the decompiler's branch order
invites reading them swapped. The 3-bit indices are read as unaligned fields
out of the six bytes, the same way the original's dword-at-an-offset does.

The source row stride is `2 * width` for DXT1 and `4 * width` otherwise, which
is `(width/4) * blocksize` only for a multiple of 4 - otherwise the rows drift,
and the edge blocks are still visited with per-pixel clipping. In the original,
and reproduced; the cases use multiples of 4 so the stride is exact.

`tools/decodetest.py --dds` and `--dds-roundtrip` check six cases - DXT1,
DXT3, DXT5 and uncompressed32, in two sizes - with the expected pixels computed
by an independent block decoder in the harness, so the C is checked against the
formulas rather than against itself.

### Still to do inside DecodeImage

The chunk walk, the bit reader and its pump, the three Huffman helpers, the
inflate body, the unfilter and the row expansion are all transcribed and
verified pixel for pixel. What is left:

* `sub_40A630`, the Adam7 path. Interlaced input is refused rather than guessed
  at, because the pass table it reads is not a partition of the image and so
  could not be checked against anything - see **The Adam7 tables** above.
* The other two decoders — JPEG and DDS — and re-declaring the 22 MB of
  `.bss` they use. Seven of the eight input formats are transcribed now: PNG, the
  8bpp palettised one, GIF, PCX, Targa, BMP and DDS. The one left, measured:

  | original | listing lines | bytes | note |
  |---|---|---|---|
  | `0x40B180` JPEG | 3 491 | 5 058 | front end read, above; entropy body blocked on two reference gaps |

  The corpus exists - `tools/jpeglib.py`, 33 cases, plus a progressive file -
  and the IDCT, the Huffman reader, the progressive refinement and the colour
  conversion are all read. What is missing is eight dwords at `0x4110D8` and
  the writer of two `.bss` arrays; see the JPEG section.

## Verification so far

Three independent checks, all runnable.

### tools\decodetest.py - decoder verification

`tools\dumpdec.cmd` builds `build\dumpdec.exe`, a probe that links the same
sources, calls the real `DecodeImage()` and writes the pixel buffer out raw.
`tools\decodetest.py` generates PNGs whose pixels are known by construction,
decodes them through that probe and compares the buffer against what went in.

```
tools\dumpdec.cmd                        build the probe
python tools\decodetest.py               all 122 cases
python tools\decodetest.py 37,41         two of them
python tools\decodetest.py --roundtrip   21 of them through pngout.exe
python tools\decodetest.py --pal8        the 8bpp format, 3 cases
python tools\decodetest.py --pal8-roundtrip   the same three through pngout.exe
python tools\decodetest.py --pcx         three plane PCX, 4 cases
python tools\decodetest.py --pcx-roundtrip   the same four through pngout.exe
python tools\decodetest.py --targa      Targa, 12 cases
python tools\decodetest.py --targa-roundtrip  the same twelve through pngout.exe
python tools\decodetest.py --bmp        BMP, 6 cases
python tools\decodetest.py --bmp-roundtrip   the same six through pngout.exe
python tools\decodetest.py --gif        GIF, 12 cases
python tools\decodetest.py --gif-roundtrip   the same twelve through pngout.exe
python tools\decodetest.py --jpeg       JPEG, 33 cases (see below)
python tools\decodetest.py --jpeg-im    the same 33 through ImageMagick,
                                    plus a progressive file it generates
python tools\decodetest.py --dds        DDS, 8 cases
python tools\decodetest.py --dds-roundtrip   the same eight through pngout.exe
```

`--jpeg` is the odd one out: `sub_40B180` is not transcribed, so there are no
pixels to compare. What it checks is that `tools/jpeglib.py` writes a baseline
JPEG which reads back within a measured bound, and that `dumpdec.exe` and
`pngout.exe` both refuse it cleanly while it is missing. It pins the corpus
rather than the decoder.

`--jpeg-im` is the same 33 read back through ImageMagick 7's `convert` instead
of through `jpeglib`'s own reader, which removes the one honest caveat on the
corpus: that the writer and its reader could share a misreading and agree with
each other. ImageMagick is an independent decoder and does not share it. It
needs `C:\ImageMagick\convert.exe` and skips gracefully without it, and it
also brings the progressive file, generated by ImageMagick itself from a PPM -
SOF2, ten scans - which `jpeglib` cannot write. That file is sized and refused
cleanly, and is the corpus side of the `0xC2 shares the SOF0 arm` finding for
when `sub_40B180` lands.

Two genuine encoder bugs came out of pointing a third-party decoder at the
corpus, and both are the kind the self-check could never have caught because
the writer and its own reader made the same wrong assumption:

* The restart markers have to **count**: RST0, RST1, ... RST7 and round again,
  one per interval. Emitting RST0 every time made libjpeg say "found marker
  0xd0 instead of RST1" and resynchronise at the wrong place. `jpeglib`'s own
  reader accepted any D0..D7, so it never noticed.
* The frame header's sample precision is always 8 - baseline JPEG has no
  other - and libjpeg rejects 16 outright. It is **not** the quantisation
  table's precision byte, which was what the `precision` argument was driving.

One parsing note for anyone touching `im_pixels()`: ImageMagick's `txt:`
output writes `black` and `white` (and other names) for exact primaries
instead of `srgb(...)`/`gray(...)`, so matching on those misses every pixel
that lands exactly. Parse the leading Q16 `(r,g,b)` or `(v)` and take the top
byte.

The GIF cases need an LZW *encoder* in the test harness rather than a row of
literals, so that the code table, the size bump and the "repeat the previous
string's head" case are actually exercised; `gif_lzw()` is a real encoder and
`gif_walk()` models the row order `sub_40CA70` walks so the interlace cases can
be checked independently of the decoder.

The probe has to put stdout in binary mode (`_setmode(1, O_BINARY)`, as `main()`
does for the "-" case): left in text mode the CRT turns every 0x0A sample into
CR LF, which shifts the rest of the dump by a byte - and since one of the values
in a smooth gradient usually *is* 0x0A, that looks exactly like a decoding fault.
Worth knowing before spending an afternoon on it.

The 122 cases cover all four colour types at depth 8 with three picture shapes,
every filter on every row for each of them, filters rotating row by row, depths
1, 2 and 4 with all six filters, `tRNS` on types 0 and 2, and the two alpha
shapes that matter for the internal layout: `alpha_ramp` gives one see-through
row in the middle of an opaque picture, and `grey_few` is greyscale plus alpha
with few enough colours that `SuggestColType()` answers with a palette, which is
the only way a type 4 image survives a round trip (see **The pixel expansion**).

### tools\pngcheck.cmd - round-trip verification

`tools\pngcheck.c` runs `Optimize()` over a set of images and writes each result
plus the source pixels; `tools\pngcheck.py` then parses the PNG, checks every
chunk CRC, hands the IDAT payloads to zlib, undoes the per row filters and
compares every sample against the source. The unit test can only check the output
*structurally*; this checks it inflates and carries the right pixels, which is
the failure mode a structural check cannot see. `tools\lztrace.py` goes one level
deeper and walks the deflate block symbol by symbol, which is what you want when
the stream is short rather than merely wrong.

```
tools\pngcheck.cmd            build the driver, run it, verify
tools\pngcheck.cmd run        skip the build
tools\pngcheck.cmd build      build the driver and stop
```

The driver takes a case selector, which is what makes a failure bisectable -
a case number on its own, or one plus a comma separated list of others:

```
build\pngcheck.exe 7          just case 7
build\pngcheck.exe 7 5,9      cases 7, 5 and 9, in case order
```

30 cases, and every one of them round trips: all five `/s` strategies, `/r`, a
ramp, noise and a flat field, forced colour types 0, 2, 3 and 6, an image that
needs alpha and one that needs it because the same grey is both opaque and
see-through, each of the five fixed filters, `/n2` and `/n8`, `/b64` and `/b8`,
odd widths and a one row image. That is every colour type `FilterSample()` can
feed except 4, every filter, every strategy, all three `SeedBlockTables()`
presets and both block splitting modes.

### Known defects

`tools\pngcheck.cmd` found eleven. All are fixed; the round trip is clean over
all 30 cases. The last four were found together and are worth reading together,
because they are all the same mistake - a field of the row record addressed by
the wrong offset - in four different places.

* **Fixed - `FinishBlock()` wrote the IDAT length in the wrong place.**
  `EmitBytes()` appends at `OutCount`, so the four bytes went out at the *end* of
  the stream instead of being patched back into the chunk header at `ChunkStart`.
  Every compressed PNG carried a nonsense length - in practice `0x89504E47`, the
  file's own signature, which is what `Scratch` held when the chunk was opened.
* **Fixed - no deflate block was ever closed.** `FilterSample()` ended its row
  with a call through `BlockFullHook`, a function pointer that nothing ever
  assigned, so `FinishBlock()` never ran: no BFINAL, no Adler-32, no block CRC
  and no length patch. The original does a direct `jmp loc_4075B0` at the end of
  `sub_407D80` / `sub_4077C0` / `sub_407B00` / `sub_4071D0`, and it is a real call
  now.
* **Fixed - the PLTE, tRNS and bKGD CRCs were wrong.** `WritePngHeader()` seeded
  the running CRC from `ChunkCrc()`, which returns the *finished* (complemented)
  CRC, and then kept folding - a double complement. It also declared `crc` as a
  signed `int`, so `crc >> 8` was an arithmetic shift once the running value went
  negative. `ChunkCrc()` was always right because it keeps its own accumulator
  unsigned, which is why IHDR's CRC passed and only the chunks whose CRC is
  continued after the tag were wrong.
* **Fixed - the zlib header went out byte for byte reversed.** The two bytes are
  0x78 then 0x5E - `CMF` = 8 with a 32K window, and `FLG` chosen so
  `0x78 * 256 + 0x5E` is 31 * 994 exactly. But `PNGOUT_MARK` is 0x5E78, so
  `>> 8` is 0x5E, and writing that first put 0x5E where the compression-method
  byte belongs: `CMF = 0x5E` claims compression method 14, which does not exist,
  and every compressed PNG was rejected with *"incorrect header check"*. Read back
  the other way the same two bytes are the little endian DWORD 0x5E78 = 24184, so
  this is also the value `main()` recognises a file of ours by.
* **Fixed - `ROW_LIMIT` did not point at anything.** It was `(0x1018 / 4)`, which
  is a dword *past* the end of the row record, so `cap = min(writePos, rowend)`
  compared against whatever happens to be there and `end <= cap` never held. No
  match was ever accepted and every row was closed as soon as it was opened - a
  whole image came out as 368 literals where 5 symbols would do. See the row
  record notes above.
* **Fixed - `SeedBlockTables()` wrote the code lengths 16 bytes too high.**
  `samples + ROW_SYMBOLS` is field `+0x520`, not `+0x510`, so every literal's
  length stayed zero, every match cost more than a literal, and the compressor
  emitted nothing but literals even once `ROW_LIMIT` was right. The three presets
  all write *one* contiguous run of 320 dwords at `+0x510`; the original reaches it
  through three different addresses and they happen to be one array.
* **Fixed - `InitBlockTables()` never cleared `MatchLimit`.** The reconstruction
  had `LensScratch[ROW_SAMPLE_MAX - 1] = 0` standing in for `dword_412668 = 0`,
  which is the wrong storage. It only worked because the first image in a process
  found `.bss` zeroed; from the second image on, a stale `MatchLimit` from the
  previous run sat past the end of the new one, `pos != furthest` held for every
  position, and the block was closed with nothing in it.
* **Fixed - `FilterSample()` emitted the bytes of a sample in ascending order.**
  They go out 2, 1, 0 for 24 bit and 2, 1, 0, 3 for 32 bit, which is PNG's R, G, B
  order because an internal pixel is one DWORD with B, G, R, A from the bottom
  up; only the 1 and 2 byte cases walk upwards. Every RGB image came out with its
  red and blue channels exchanged - and nothing noticed, because the stream still
  inflates and the length is still right.
* **Fixed - `FilterPredict()` measured its first distance from the wrong sample.**
  `sub_40AF40` builds `|a + b - 2c|`, not `|a + c - 2b|`, so it is the sample
  *diagonally above and to the left* that the three candidates are compared
  against, not the one above. On most triples the two disagree.
* **Fixed - the end-of-block code was a variable nothing wrote.** `dword_6F6A78`
  is not its own variable: it is `dword_6F6678 + 256`, i.e. `LitCodes[256]`, and
  `AdvanceRow()` writes it with the row's own length for symbol 256 to close
  every block. Modelled as a separate `int` it kept whatever `.data` held, which
  happened to be a legal 7 bit code for the seeded tables, so the common cases
  closed by accident - and `/s3`, whose seed gives symbol 256 a 9 bit code, did
  not close at all: the stream came out one symbol long and zlib reported
  *"incomplete or truncated stream"*.

* **Fixed - `PlanBlocks()` read the wrong buffer.** All four `call sub_408390`
  sites - `0x408071`, `0x408142`, `0x40822A` and `0x40827F` - take their base out
  of `dword_18CBEF4`, the same global `sub_408690` (`PutByte`) appends to, so
  `BestSplit()` walks the filtered bytes the collecting pass laid down. The
  reconstruction used `dword_18CBEE4` (`LinBufData`) instead, which is a
  different allocation: only `8 * LinBufBytes + 8` bytes, against a `SplitEnd` of
  `height * (3 * width + 1)`. For a 24x24 RGB picture that is 1752 bytes read out
  of 592. The block boundaries came from whatever the heap happened to hold, which
  is why they changed from run to run, and on a noisy image the walk ran off the
  end of the heap and `pngout.exe` died with an access violation in `BestSplit()`
  - `tools\decodetest.py --roundtrip` reported it as *"e-noise-ct2 no output"*.
  `dword_18CBEE4` is the right base in `FilterSample()` (`0x4063F6`), where it is
  read; the two are not interchangeable.

* **Fixed - the palette was seeded from an address inside itself.**
  `SETTINGS+24` (`idx 6`) is a **pointer**: `sub_402AD0` reads it at
  `mov esi, [ebp+18h]` and uses it as the source of a `rep movsd` into
  `dword_180B578` at `0x402EFA`, so `main()`'s `v84[6] = dword_190A420` is the
  array decaying to an address. Hex-Rays' "int `dword_190A420[]`" guess makes it
  read as a value, and the reconstruction had `s.unk6 = dword_190A420[0]` - the
  first ramp entry. Along with the missing `rep movsd` (below) every lookup in
  the colour hash missed, `FindColor()` returned −1, and −1 shifted into a 4 bit
  palette field is `0xF`: the 8bpp format came out with index 15 in a twelve
  entry palette, which is not a legal PNG. `unk6` is now `palette`, an
  `int *`.
* **Fixed - the `rep movsd` that copies the input palette in was missing.**
  `0x402ED4`-`0x402F02` clamps `Image->palflag` to 0..256 — the clamp is spelled
  out with `test eax, eax` / `setle cl` / `dec ecx` / `and ecx, eax` and the
  `cmp eax, 100h` above it — and copies that many dwords from the input ramp over
  `dword_180B578`. It runs *after* `ScanImage()` has built the palette from the
  pixels and *after* the colour type has been decided, which is how a palette
  the file arrived with replaces the one `ScanImage()` would have built. With
  `palflag` 0 — what `main()` leaves it for a PNG palette, since
  `s.coltype == 3 && !s.keeppal` clears it — the count is zero and
  `ScanImage()`'s palette stands, which is why every PNG test passed without it.
* **Fixed - `CompactKeptPalette()` did not exist, and what replaced it
  mirrored the palette.** It walked down from the last entry while `keep` counted
  up, so with every entry still used — `keep != i` is true at every step, because
  the two counters move in opposite directions — each step copied `pal[i]` to
  `pal[keep]` *in place*: a twelve entry palette came out as its last six entries
  mirrored over its first six, with two entries duplicated and six missing. Every
  lookup then missed. `loc_40321A` is not this at all: it is the backwards
  search for the bKGD colour (`xor edx, ecx` / `test edx, offset unk_FFFFFF` /
  `dec eax`), and there is no compaction pass in the original — nothing is ever
  removed. The palette only ever grows, through the append arm of the pixel scan
  at `0x40311B` (`PaletteUse[palcount] = 1`, `dword_180B578[palcount] = pixel`,
  `AddColor(pixel, palcount)`, `palcount++`), which is now part of
  `CountPalette()`.

  All three were dead code until now. `CompactKeptPalette()` was reachable only
  when `palflag != 0`, and for every input PNGOUT could read before the 8bpp
  format that meant a PNG palette with `/kp` — and `main()` clears `palflag` for
  exactly that case. `/kp` and `/ks` both pass before and after, because they
  never got there.

Two more things the round trip turned up that are not defects but are worth
knowing, because a test image can trip over them:

* **A greyscale image can only have one see-through colour.** Colour type 0 and 3
  both reach the samples through `FindColor()`, which compares the whole DWORD -
  alpha included. For colour type 0 the "palette" is the ramp `0xFF000000 +
  k * step`, and `loc_403019` splices `MinSample` in at its own index, so exactly
  one entry can carry a transparent alpha. An image with several *different*
  fully transparent greys therefore looks up misses, and `FindColor()` returns -1,
  which reaches the filter as the byte 0xFF. The same cap applies to colour type
  3: an image with more than 256 distinct pixels cannot be named by a PLTE.
  `tools\pngcheck.c` keeps its test pictures inside both limits, and says so.
* **A palette's tRNS is a prefix.** One alpha per entry from index 0 up to the
  last entry that is not opaque; the rest are left out, which PNG reads as opaque.
  Entries can repeat the same RGB and differ only in that alpha, which is exactly
  what happens to a picture whose greys are both opaque and see-through, so
  `tools\pngcheck.py` matches palette entries on `(RGB, alpha)` rather than RGB
  alone.

`tools\lztrace.py` is what pinned the last two down: it walks the deflate block
symbol by symbol, which is the only way to tell "the block never ended" from
"the block ended in the wrong place" - zlib only says the stream is short. It is
validated by pointing it at a PNG built by Python's `zlib`, which it decodes
cleanly and whose symbol count it reports exactly. (Note that `zlib` writes
`78 da` for its own streams where PNGOUT writes `78 5e` - both are valid headers,
and which one you get is just the compression level.)

Colour type 4 is the one type that does not survive the round trip, and now
that the question is settled it is settled *as the original's behaviour*, not as
a gap in the reconstruction. `sub_407B00`, the 2 byte filter, emits bytes 0 and
1 of the internal DWORD and under that layout both of those are the grey, so
PNGOUT drops the alpha whenever it re-encodes grey+alpha as type 4 - confirmed
by running it: a 30x30 type 4 input with a varying alpha comes back with the grey
in both bytes of every sample and no `tRNS`. `FilterSample()` reproduces that
byte for byte. Every other colour type round trips: 0, 2, 3 and 6 are all in
`tools\pngcheck.c`.

### tools\selftest.cmd

240 checks over the recovered code: `Swap16`, `GetImageSize` for all
eight format codes, the `/k` list handling in `ScanChunks`, `FilterPredict()`,
`CpuFeatures()`, the optimiser primitives (CRC table, Huffman heap, colour
hash, `PutBits()`/`EmitBytes()`, `RankValues()`, `FlushRow()`, `WritePngHeader()`, `EstimateEntropy()`, `BestSplit()`, `PlanBlocks()` with its block-end sort, `FilterByte()` with `Adler32Update()`, `AdvanceRow()`, `LzCode()`, and `WriteStored()`), and `Optimize()` end to end.
They caught twenty real defects, all now fixed, plus the eleven that
`tools\pngcheck.cmd` found (see **Known defects** above):

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
* **`BuildLenCodeToSlot()` was scribbling over unrelated globals.** It fills
  the table *backwards* from its end - `mov esi, offset unk_180BA7F; sub esi,
  eax` with `eax = 258`, then `mov [esi+eax], cl` - so 255 bytes *below* the
  table are overwritten with 58 as well. In the original that is the tail of
  the palette area and nothing else; here it landed on whatever the linker put
  next, and the entropy histogram plus the colour hash counters came back as
  `0x3A3A3A3A`. It is now found by the tests reading a global before writing
  it, and the array carries its own 255 bytes of slack in front. This one ran
  at the start of every deflate block, so it would have corrupted the optimiser
  mid-run.
* **The block-end shell sort stepped by dwords instead of records.** The
  original's backwards walk is `sub esi, 2584 * gap` bytes, i.e. `gap` whole
  records; the transcription used the same index arithmetic but moved the
  pointers by `gap` dwords, so every swap landed in the *start* and *next* fields
  of neighbouring records and left the ends nearly - but not quite - sorted. The
  tests now sort a reversed and a stride-7 scrambled set of ends directly.
* **The LZ coder read `MatchLimit` from the wrong place.** It took it from the
  top of the loop iteration instead of from `0x406CC4`, where the original
  re-reads the global after the search has been raising it. The literal path's
  clamp then measured against a stale number and pulled `MatchLimit` back down
  to `pos + 1`, discarding every match that position had just found. The stream
  still decoded to the right length and was one byte wrong at the end, and only
  the three strategies that actually match were affected.
* **The step table's long-range hop was 1026 entries instead of 258.** IDA's
  `word_806D86` minus `word_806B82` is 0x204 bytes, which is 258 shorts, not
  the 1026 the note claimed. Xtreme (`/s0`) is the only strategy that uses it.
* **`FinishBlock()`'s LZ flush was a hook.** With the coder reconstructed it is
  a real call, `FlushLzCoder()`, because step 1 has to run before step 4 — the
  flush is what leaves the Adler-32 sum in `AdlerSum`.
* **`WriteStored()` did not reseed the Adler-32.** The original does
  `mov [esp+2Ch+Number], 1` at 0x4087D5; leaving the global alone means
  starting from whatever the last deflate trial finished with, and the sum is
  checked by every decoder so the PNG is unreadable. Found by handing the
  output to Python's `zlib`, which rejected it with "incorrect data check"
  while every structural check passed.
* **The greyscale test was a mask, not the original's shifted halves.**
  `(v & 0xFFFF00FF) == ((v & 0xFF00) << 8)` is zero only for a pixel whose
  *blue* byte is zero, so it called every ordinary grey a colour and sent
  greyscale images to a palette. The original's `(v & 0xFFFF) << 8` against
  `v & 0xFFFF00` is the real `R == G == B` test.
* **`MinSample` was compared signed.** It starts at `0xFFFFFFFF` and the loop's
  `jnb` is an unsigned compare, so a transparent black (`0x00000000`) has to
  win it. Signed, 0 is greater than -1 and the transparent background was never
  noticed - which is the very pixel that decides whether the image needs alpha.
* **`InitColorHash()` was called again when building the palette.** The original
  (`loc_40327E`) only calls `FindColor()` and counts; the palette and the hash
  were already built by the classification pass. Re-seeding made every pixel
  look new and appended a second copy of the palette.
* **`dword_180A568` was declared as two variables.** `CountByte()` counts into
  it and `EstimateEntropy()` decrements it to get the largest count to
  tabulate; split in two, the decrement ran on a counter nothing incremented
  and sized the table from `-1`, so every filter score came back "failed".
* **`LinBufData` was never assigned.** The unit tests set it by hand, so the
  filter tests passed while the real path wrote through whatever the previous
  block left there and every trial loop crashed.
* **`/f` with no number never got its default.** `loc_402ED4` turns a negative
  `s->filter` into 5 ("mixed", pick per row) or 0 for a palette; left at -1 the
  filter switch in `FilterSample()` matched no case and emitted no data at all.

The filter byte order and the predictor's own distance (both of which
`tools\pngcheck.cmd` caught) are covered above; the unit test now pins them too,
against explicit byte values rather than against itself:

* `FilterPredict()` is checked for all three of its outcomes on triples chosen so
  that each one wins, and `FilterSample()` for the emitted byte order of all four
  widths, because the order is the one thing a round trip cannot see - a stream
  with its red and blue exchanged still inflates to the right number of bytes.

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

`dumpbin /imports build\pngout.exe` compared against the `extrn` list in
`reference/idata.lst` is the objective measure of how much of the original is
actually reachable:

* **All 55 of the original's imports are present, with no extras.** This is the
  acceptance test for `Optimize()`: the last four to appear were
  `QueryPerformanceCounter`, `QueryPerformanceFrequency`, `rand` and `srand`,
  precisely the ones used only by it - the "Took %.3f sec." timing and the `/r`
  randomised initial tables. (IDA spells the ten `start` helpers `__imp_*` where
  `dumpbin` says `*`, and prefixes CRT names with a cdecl underscore; both are
  the same imports once the decoration is stripped.)
* **USER32 and GDI32 match exactly**, in the same order: `OpenClipboard`,
  `GetClipboardData`, `CloseClipboard` / `DeleteDC`, `GetObjectA`,
  `CreateCompatibleDC`, `GetDIBits`.
* **MSVCRT matches exactly**: all 23 runtime functions plus the ten `start`
  helpers (`__getmainargs`, `__p___commode`, `__p__fmode`, `__p___initenv`,
  `__set_app_type`, `__imp___initterm`, `__setusermatherr`, `_adjust_fdiv`,
  `__imp___except_handler3`, `__imp__controlfp`).
* **KERNEL32 matches exactly**, all seven: `CreateFileA`,
  `QueryPerformanceCounter`, `QueryPerformanceFrequency`, `GetFileTime`,
  `CloseHandle`, `SetFileTime`, `GetTickCount`.

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
src/chunk.c       PNG chunk walk, format sniffer, Swap16/Swap32
src/clipboard.c   ReadClipboard
src/globals.c     .data
src/optimize.c    CRC table, Huffman heap, colour hash, bit/byte output,
                  deflate tables, per row slots, the row writer, the PNG header,
                  the /b block splitter, the /s4 stored writer, and the two
                  no-op defaults (NothingChanged, PutByte)
src/lz.c          the LZ coder (sub_4065A0) and its byte sink (sub_406530)
src/trials.c      Optimize() (sub_402AD0): the colour type / bit depth decision,
                  the size estimate, the /s4 shortcut and the trial loop
src/decode.c      DecodeImage(): the PNG chunk walk, the bit reader and its
                  pump, the three Huffman helpers, the inflate body, the
                  unfilter and the row expansion
src/predict.c     FilterPredict
src/cpuid.c       CpuFeatures
src/stubs.c       the one unrecovered entry point, plus the math error handler
tools/            extraction scripts, build.cmd, run.cmd, selftest.cmd, regen.cmd,
                  pngcheck (the encoder's round trip), dumpdec + decodetest
                  (the decoder's)
reference/        extracted listing/decompilation/strings (generated, gitignored)
