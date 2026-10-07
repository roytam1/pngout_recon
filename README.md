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
and only the first 0x930 of them are touched, so `src/pngout.h` defines the
field offsets and `src/globals.c` has the lot as **one** `RowRecord[]` array
rather than one array per field. That is not cosmetic: modelled separately the
reconstruction needed two full records - 21MB of `.bss` - and the fields *do*
overlap on purpose (`sub_407090` reads the previous row's samples as
`samples[i] - 320`, which is inside the record in front of it).

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

### Still open

* `sub_405A60` (`BuildDistanceSlots()`) fills `dword_6D5578[]` with 30 slots
  from the length half and 29 from the distance half, 4096 meaning "no match";
  builds `byte_185BA90[]` when the strategy is Xtreme; and finishes with the two
  `sub_405800` calls. Note that both halves are arrays of *code lengths* - what
  is added is the number of extra bits - so it is a comparison key for the
  match search, not a length or a distance.
* The CRC table's single writer sits 4 bytes above all 18 readers
  (`0x185BABC` vs `0x185BAB8`). Modelled at the readers' address.

### sub_408000, the /b block splitter

`PlanBlocks(start, end)` turns one byte range into the list of deflate blocks
the compressor will emit, calling `BestSplit()` on each block to decide whether
cutting it pays. It reads the range out of `dword_18CBEE4` (the filtered image
buffer, whose base pointer is the freed `dword_18CBEE0` one dword below the
data) and keeps its list in the second 0xA18-strided record array at
`0x42F364`: `split`, `start`, `end`, `next`, and a `double` score, five fields
of the 0x18 bytes the function ever touches. `NumBlockList` is `dword_6D5570`.

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

### Still to do inside Optimize

`sub_402AD0` itself (5 983 listing lines), the min-sum filter
`sub_4086E0`, the timing `sub_4063F0`'s caller, and the `.bss` globals they
all use.

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

428 checks over the recovered code: `Swap16`, `GetImageSize` for all
eight format codes, the `/k` list handling in `ScanChunks`, `FilterPredict()`,
`CpuFeatures()`, and the optimiser primitives (CRC table, Huffman heap, colour
hash, `PutBits()`/`EmitBytes()`, `RankValues()`, `FlushRow()`, `WritePngHeader()`, `EstimateEntropy()`, `BestSplit()`, `PlanBlocks()` with its block-end sort, `FilterByte()` with `Adler32Update()`, `AdvanceRow()`, and `LzCode()`).
They caught thirteen real defects, all now fixed:

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

* **51 of the original's 55 imports are present.** The only name this build
  has that the original does not is `time`, which every stock VC6 `/MD` program
  picks up from the CRT as soon as it uses `atof` or `printf("%f")` - a three
  line program with just those two already imports it, so it says nothing about
  the reconstruction. (IDA spells the ten `start` helpers `__imp_*` where
  `dumpbin` says `*`; they are the same imports.)
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
src/chunk.c       PNG chunk walk, format sniffer, Swap16/Swap32
src/clipboard.c   ReadClipboard
src/globals.c     .data
src/optimize.c    CRC table, Huffman heap, colour hash, bit/byte output,
                  deflate tables, per row slots, the row writer, the PNG header,
                  the /b block splitter
src/predict.c     FilterPredict
src/cpuid.c       CpuFeatures
src/stubs.c       the two unrecovered entry points, plus the math error handler
tools/            extraction scripts, build.cmd, run.cmd, selftest.cmd, regen.cmd
reference/        extracted listing/decompilation/strings (generated, gitignored)
```