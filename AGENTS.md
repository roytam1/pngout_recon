# AGENTS.md — pngout-reconstruct

A clean-room reconstruction of `pngout.exe` (32-bit, VC6-era) in C, built with
Visual C++ 6.0. The goal is behavior-compatible output; the open defect is
speed (see "The speed problem" below).

## Layout

- `src/` — the reconstruction. `pngout.c` (main), `chunk.c`, `decode.c`
  (input decoders), `optimize.c` (trials, Huffman, rows, blocks), `lz.c`
  (`sub_4065A0`, the LZ coder + `LzXtreme`/`LzSimple`), `trials.c`,
  `globals.c` (all `.bss`/`.data` with original addresses in comments),
  `pngout.h` (shared macros/globals), plus small `cpuid`/`predict`/
  `clipboard`/`stubs` units.
- `reference/` — IDA outputs for the 32-bit target: `text.lst` (disassembly),
  `decomp/sub_*.c` (Hex-Rays), `fn/sub_*.lst` (per-function listings),
  `bss_refs.txt` (global cross-references), `data.lst`/`rdata.lst`.
- `pngout-x64-linux*` — a later (20150319) x64-Linux build with IDA Hex-Rays
  (`.c`, 445 KB) and listing (`.lst`, 1.2 GB). Same LZ algorithm as the
  32-bit target (same 16 MB step table, same structures, only the base offset
  differs by 3 entries), much more readable than x86 asm. Use it as a second
  oracle when the 32-bit listing is ambiguous. It is FAST, like the original.
- `tools/` — build and measurement scripts (see "Tools").
- `build/` — VC6 build output (`build\pngout.exe` is the release binary).
  Variant builds go in `build\<name>\` via `tools\buildvar.cmd`.
- `150650258_p0.png` — the large test photo (2296x3606 RGBA). `pngout /b0`:
  original 144s, recon 858s.

## Build, test, verify

```cmd
tools\build.cmd              release build -> build\pngout.exe (VC6, /O2)
tools\buildvar.cmd <name> [/D...]   variant build -> build\<name>\pngout.exe
tools\build.cmd clean
tools\selftest.cmd           unit tests (must say PASSED, 0 failures)
tools\pngcheck.cmd build     30-case round-trip suite (must be clean)
tools\buildgcc.cmd           MinGW-GCC 64-bit build (needs C:\msys64\mingw64\bin
                             in PATH; sources copied to local disk first because
                             MinGW cannot write to the UNC share)
```

`cmd.exe` cannot `chdir` into the UNC share; the `.cmd` scripts `pushd`-map a
drive. PowerShell output from them is mojibake-heavy; pipe through
`Select-String -Pattern "ok|FAILED|error"` and check `$LASTEXITCODE`.
`pngout` rewrites its input **in place** — always run it on copies, never on
the original test files. Case generators/comparators live in `tools/`:
`finddiff.py` (find small diverging inputs), `makecase.py`, `lzdiff.py`
(symbol-stream diff), `lztrace.py` (IDAT symbol dump).

Variant-build defines (all off by default; measurement only, except the skip
fix which is unconditional and verified):
`/DLZSTATS` (pos/chain/xtreme/matchcount per trial), `/DLZFOOT`
(step-table lag/chain/reach histograms + step-vs-restore split + rank
histogram), `/DLZTRACE` + `LZLO`/`LZHI` env (per-position search trace),
`/DDUMPSTATE` + `DUMPPOS`/`DUMPNTH` env (frozen search inputs for one
position: `lzstate_<pos>.txt` + `lzdump_{step,win,aux}_<pos>.bin` for the
offline oracle; zero-cost when off),
`/DHEAPSTEP` (VirtualAlloc the step table), `/DLZ_STEPCAP=<n>` (shrink the
table). `tools\linkmap.cmd` writes `build\pngout.map` (reconstruction `.bss`
addresses for the watch tools).

## The speed problem (FIXED - root cause below)

Measured with `/b0` on `150650258_p0.png` (8.28 M positions, 4 trials):
original **144 s** (9 s, then ~45 s per trial), reconstruction **858 s**
(28 s, then ~225 s per trial). Decode is 0.1 s and filter selection 0.7 s per
trial, so **96% is `LzCode`**, and inside it ~93% is `LzXtreme`'s step loop.
The gap was trial-dependent: trial 0 was only ~3x, trials 1+ were ~5x.

### Root cause and fix

`src/globals.c` declared four of the LZ block variables twice, under both the
live name and IDA's `unk_` name for the same address:

| address | shadow (reset by `InitBlockTables`) | live (read by `LzCode`) |
|---|---|---|
| 0x41CEC0 | `unk_41CEC0` | `RunEnd` |
| 0x411D58 | `unk_411D58` | `RunCount` |
| 0x185BEC8 | `unk_185BEC8` | `PrevReach` |
| 0x185BEB8 | `unk_185BEB8` | `PrevMark` |

The linker gave these four pairs *different* `.bss` addresses, so the per-block
reset at `src/optimize.c:740-748` cleared the shadows while the coder kept
reading the live copies. Trial 0 hid the bug (both start zeroed); from trial 1
on `RunEnd` held trial 0's end-of-image value (~7.56 M), so `maxlen = RunEnd -
pos` was millions instead of 261, the `len == maxlen` step-loop exits never
fired, and the record rate jumped from ~9 to ~59 per position. The saved
`lzstate_89547.txt` (trial 1) shows `runend=7564800 maxlen=7475253 count=5287216`.

Fix: the four `unk_*` names are now macros aliasing the live variables in
`src/pngout.h`, and the duplicate definitions are gone from `globals.c`.
`InitBlockTables()`' existing reset now clears what `LzCode` actually reads.
Measured after the fix, clean side-by-side on `150650258_p0.png` `/b0` with
`/O2`: original **150.4 s**, reconstruction **156.6 s** (ratio **1.04**);
before the fix the recon took 858 s.

A second, independent win came from compiler flags: `/G6` (schedule for
Pentium Pro and later) is **17% faster** than `/O2` alone on
`1790_still.png` (41.3 s -> 34.5 s), and a clean paired run on the big photo
gave 148.9 s -> 123.9 s (20%), both with byte-identical output. `/Ox` alone
does not help (42.1 s); `/Ox /G6` is only marginally better than `/O2 /G6`.
Forcing the two search routines inline (a `/DLZINLINE` experiment) was
*2.7% slower* and was removed. `build.cmd` and `buildvar.cmd` now use `/G6`.

Output stays within the known ~140-byte difference (1,598,717 vs the
original's 1,598,860). All 30 round-trip cases and the unit tests still pass.

### RULED OUT - do not re-derive these

* **The work is identical in trial 0** (where all the whole-table comparisons
  below were made). Same record rate (9.35 per position, ~77 M records per
  trial), same mean `MatchArea` reach (256 - every searched position writes the
  whole length range), same alias rate (96.8% vs 96.0%), same skipped fraction,
  output size within 138 bytes (the recon is 138 bytes *smaller*). Do not extend
  these claims to trial 1+: the stale-`RunEnd` bug inflated the recon's record
  rate there to ~59/position until the alias fix above.
* **The step table is bit-identical in trial 0**: the whole 16.8 MB table was
  dumped and diffed at six cursor checkpoints through sweeps 1 and 2
  (`tools\dumpsteps.exe` `DUMPOUT`/`DUMPCNT`, `tools\stepfull.py`) with zero
  real differences. In trial 1 the tables are equivalent after a cursor shift
  (delta distributions: mean 3412.7 vs 3411.7, same modal values).
* **`DistSlots`, `SlotOrder` and `BestCost` are identical in trial 1** (read from
  the original at 0x6D5578 / 0x185BA90 / 0x6D5170 via `DUMPTABS`).
* **`MatchArea` is identical** at trial-0 position 269031.
* **The code matches the listing instruction for instruction**: the inner step
  loop is 11 instructions with 3 loads and 2 branches in both binaries, and the
  skip decision (0x406D68 / 0x406D84 / 0x406D9E), both trim loops, the record
  path, the `BestCost` stores and the alias rewind all match.
* **Locality is not the cause**: the recon's step reads are *hotter* (max lag
  661 KB) than the original's (50% of its `MatchStart` entries are more than 2 MB
  behind the cursor).
* Not the strategies, not the block count (one block per trial in both), not
  decode, not filter selection, not the Huffman tie-breaks.
* **The old output-shape numbers are void**: the "first divergence at symbol 29 /
  1,817,826 bytes" comparison was `/b0` against a *different* configuration.
  With matching switches the recon is 1,598,722 bytes vs 1,598,860, and the
  first divergent decision is at decompressed offset 269,030 (original
  `len=11 dist=1`, recon `len=258 dist=1`).

### Where the time actually goes (EIP profile, `tools\sampler.exe`)

Both binaries show the same 75% ntdll sampling artifact - ignore it. The rest,
on `1790_still.png` (original 36.6 s, recon 128.9 s):

| region | original | recon |
|---|---|---|
| hash-chain walk (`0x406758` / `0x40b4f8`) | **12.4%** | 2.6% |
| `LzXtreme` step loop (`0x40bd9f`/`0x40bdb7`/`0x40bd56`/`0x40bda6`) | ~1.4% | **~17%** |

In absolute terms the chain walks cost about the same (4.5 s vs 3.4 s), but the
recon spends **~22 s in the step loop against the original's ~0.5 s - roughly 40x
more step-loop iterations** out of identical tables, reaches and heuristic
tables. That loop is the entire defect.

### Next step

Build the offline oracle: an interpreter of the listing's `LzXtreme` driven by
the frozen `lzdump_aux_*.bin` / `lzdump_win_*.bin` inputs, counting step
iterations. If a faithful-to-the-listing oracle takes far fewer iterations than
`src/lz.c` on identical inputs, the defect is in the C translation of that loop;
if it matches, the remaining difference is per-iteration cost rather than trip
count.

## Debugging the search

- External sampling (no rebuild): `tools\watchmem.c` (cursor/positions rate),
  `tools\sampler.c` (EIP profile; both binaries show 75% in one ntdll routine
  — artifact, compare the rest), `tools\cycles.c` (cycles vs wall),
  `tools\snapstate.c`/`dumpreach.c`/`dumptabs.c`/`statelog.c`/`dumpsteps.c`
  (read globals out of the live 32-bit process; needs real `.bss` addresses
  from `build\pngout.map` via `WATCH_*` env). `ReadProcessMemory` sampling at
  >~30/s measurably slows the child; keep it ≤20/s with sleeps.
- NEVER capture a sampler's long numeric output with PowerShell `2>`/`>`:
  PS rewraps lines at 80 columns *mid-number* (`256` becomes `2`+`56` on two
  lines), silently shifting every downstream index. Always capture with
  `cmd /c "prog args 2> file"` (raw bytes), and sanity-check value counts
  (e.g. 4096 reach values, not 4165) before diffing.
- Rebuild-required: `/DLZTRACE` + `LZLO`/`LZHI` for a position-range trace;
  compare against the original's *inferred* decisions from `lzdiff.py`.
- GDB (MSYS2 mingw64) can read a live process but software breakpoints do not
  fire on the 32-bit target; no ASan/UBSan runtimes ship with that GCC (use
  `-Wall -Wextra` instead — `lz.c` is clean).
- MinGW cannot write to the UNC share: copy sources to local disk before any
  GCC build. The GCC build needs `-fcommon` (VC6 merges tentative definitions;
  GCC ≥10 does not) and has known MSVC-isms fixed only in the local copy
  (`static CurFilter`, `i64` suffixes, `__asm` in `cpuid.c` stubbed to 0).
  It runs ~18 bytes off the VC6 binary (64-bit pointer truncation in
  non-LZ paths) — fine for speed comparison, not for fidelity.

## Conventions

- Match the listing exactly; comment every block with its address (`0x406xxx`
  / `loc_xxxxxx`). Keep the `/* ---- 0x... ---- */` section banners.
- Never add comments as scratch thought; keep them concise and load-bearing
  (register lifetimes, off-by-one quirks that were verified).
- Behavior-changing defines must be off by default; measurement defines must
  cost zero when off (verify the hot loop disassembly).
- `pngout` overwrites inputs in place; tests must copy first. Never commit
  secrets, never force-push. Build with `tools\build.cmd`, not `mk.bat`
  (stale).
