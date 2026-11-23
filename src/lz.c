/*
 *  lz.c   -   sub_4065A0, the LZ coder, and sub_406530, the byte sink
 *
 *  `LzCode(data, len)` appends `len` bytes to a 64K window of the filtered
 *  image and then lifts matches out of it.  `len == 0` means "flush", which is
 *  how FinishBlock() closes a block.
 *
 *  The shape of it, which is what makes the function readable:
 *
 *    - it walks the window one position at a time, from `LzScan` up to
 *      writepos - 260;
 *    - at every position it *tries* a match, keeping the cheapest one it has
 *      found so far in `MatchCost[position & 0x1FF]` and the winner in
 *      `MatchList[position & 0x3FFFF]` (length in the low half, distance in
 *      the high half, and length 1 for a literal);
 *    - a position with no match left falls back to a literal, which *also*
 *      locks the row record's sample for that byte value (`|= 0x80000000`) -
 *      that is the bit AdvanceRow() clears, so the coder is what decides which
 *      samples the row's next code must keep;
 *    - the match list is only *emitted* once the matcher has run past the
 *      furthest position that still has a cost, which is what the cost table
 *      and `MatchLimit` are for;
 *    - when the scan reaches the end of the row (the end of the /b block) it
 *      flushes the list, calls AdvanceRow() and opens the next row.
 *
 *  The two strategies are the interesting part and are kept apart below:
 *  `/s0` (Xtreme) does the lazy length/distance search with the slot order
 *  walk, everything else does the one in loc_4067D0.  Strategies 3 and 4 skip
 *  matching altogether - `cmp ecx, 3; jge loc_406CED`.
 *
 *  Two register-lifetime details are load-bearing and are easy to get wrong:
 *
 *    - **`furthest` is read after the search, not before.**  0x406C02 closes
 *      the position's step-table slot and its last act is
 *      `mov ebx, dword_412668` at 0x406CC4 - a *fresh* read of MatchLimit,
 *      which the search has just been raising to the furthest position its
 *      matches reach.  Taking the value from the top of the iteration instead
 *      makes the literal path's clamp (`cmp edi, ebx; jle`, then
 *      `mov dword_412668, ebx`) fire against a stale number and pull MatchLimit
 *      back down, which loses every match that position found.  The emit block
 *      reads the global again (0x406E69, and 0x406FEE for the loop bound),
 *      which is what finally matters: the trigger is the copy, the amount to
 *      emit is the global.
 *    - **The step table's long-range hop is 258 entries, not 1026.**
 *      `word_806D86 - word_806B82` is 0x204 bytes = 258 shorts.
 */

#include <stdio.h>
#ifdef DUMPSTATE
#include <stdlib.h>
#endif
#include "pngout.h"

/* ---- the geometry, straight out of the immediates in the listing ---- */
#define LZ_MAXMATCH 258              /* `cmp esi, 102h`                     */
#define LZ_COSTMASK 0x1FF            /* dword_412678[], 512 entries         */
#define LZ_LISTMASK 0x3FFFF          /* dword_706B88[], 0x40000 entries     */
#define LZ_STEPMASK 0x7FFF           /* every 32K array is indexed like that */
#define LZ_MAXDIST  0x8000
#ifndef LZ_STEPCAP
#define LZ_STEPCAP  0x7FFF00         /* `cmp eax, offset unk_7FFF00`        */
#endif
#define LZ_SCANBACK 0x40000          /* `(pos - rowstart + 260) >= 0x40000` */
#define LZ_SKIPBACK 260              /* `lea ecx, [esi-104h]`              */
#define LZ_STARTHACK  30              /* `cmp byte_185BA90[dl], 1Eh`        */

#define LZ_STEPHOPS  258             /* word_806D86 - word_806B82 = 0x204 bytes */

/*
 *  Every step-table access goes through these two.  They are the whole point
 *  of /DLZFOOT: the table is 16 MB and the coder's speed is the speed of these
 *  loads, so the footprint has to be measured rather than argued about.  With
 *  the define off both expand to the bare subscript.
 *
 *  FpRead is handed the lag rather than the index: the lag is the cursor minus
 *  the read's own index, which is what decides whether the load lands in L1, and
 *  it has to be taken inside LzXtreme because MatchStart[pos] is not known
 *  there.  FPBASE sets the reference once per position.
 */
#define LzStepRead(q, i) (FPREAD(MatchStart[(q)] + (i) - base), \
                          StepTable[MatchStart[(q)] + (i)])
#define LzStepPairRead(k) (FPREAD((k) + 3 - base), LzStepPairAt(k))

/*
 *  sub_406530: the sink FilterByte() points at when a trial really compresses.
 *  It is Adler-32 first and then the coder, in that order, one byte at a time -
 *  `lea ecx, [esp+0Ch+arg_0]; push 1; push ecx; call sub_4065A0`, so the
 *  coder is handed a pointer to the *argument slot* and a length of 1.
 */
int CompressByte(int byte)
{
    AdlerSum = (int)Adler32Update((unsigned int)AdlerSum, byte);
    LzCode((const unsigned char *)&byte, 1);
    return byte;
}

/* ------------------------------------------------------------------ */
/*  the match search, strategies 1 and 2: loc_4067D0                    */
/* ------------------------------------------------------------------ */

/*
 *  One pass over the lengths.  For each length it records the distance it is
 *  trying - that record is what the *step* table is, and other positions read
 *  it back to pick their own step - works out what the match would cost, and
 *  keeps it if it beats what is already there.  Then it either extends the
 *  match by a byte or steps the distance forward and tries again.
 */
#if defined ZTIME || defined PHASE || defined DUMPSTATE
unsigned __int64 ZxOuter, ZxStep, ZxRestore, ZxTrim, ZxRank;
unsigned __int64 ZxStepC;
unsigned long DbgSweeps, DbgXstep, DbgXouter;
/* log2 histogram of the step-loop trip count per search: bucket b counts
 * searches that took 2^b..2^(b+1)-1 steps. */
/* log2 histogram of the step-loop trip count per search (bucket b counts
 * searches that took 2^b..2^(b+1)-1 steps) - measurement only. */
unsigned long ZxHist[24];
unsigned long ZxCalls;
/* how often the restore-dist path fires (len==maxlen, slot>=rank) */
unsigned long ZxDistBase;
/* how many LzXtreme calls re-search a position that was already searched
 * (pos != furthest keeps the scan in place while MatchLimit runs ahead) */
unsigned long ZxResearch;
static int ZxLastPos = -1;
/* steps taken by the current LzXtreme call: reset on entry, so the caller
 * does not have to snapshot a running total */
unsigned long ZxCur;
/* only report long searches once LzScan is past this, so the early trials
 * (empty table, no steps at all) do not fill the log */
long LzMinPos = 0;
#define ZCOUNT(x) ((void)(x))
#else
#define ZCOUNT(x) ((void)0)
#endif

#ifdef WALKCAP
/* the current search's step-loop trip count, for the WALKCAP experiment.
 * Kept outside the ZTIME block so the experiment builds standalone. */
unsigned long ZxWalkSteps;
#endif

static void LzSimple(int pos, int savedCost, int cap, int maxlen, int dist)
{
    int len, end, next, dslot, lslot, cost, q;
    /* see LzXtreme: the cursor as it stood when this position opened its slot */
    int base = MatchStart[pos & LZ_STEPMASK];

    for (len = 3, end = pos + 3; ; ) {
        LzStepWrite(MatchCount) = (unsigned short)dist;
#ifdef LZTRACE
        if (RECLOGGING(MatchCount))
            fprintf(stderr, "[rec] entry=%d pos=%d len=%d dist=%d\n",
                    MatchCount, pos, len, dist);
#endif
        MatchCount++;

        dslot = DistCodeOf(dist);
        lslot = LenCodeOf(len);
        cost = savedCost + DistSlots[dslot] + DistSlots[lslot];

        if (cost < MatchCost[end & LZ_COSTMASK] && end <= cap) {
            if (end > MatchLimit)
                MatchLimit = end;
            SetMatch(end, len, dist);
            MatchCost[end & LZ_COSTMASK] = cost;
        }

        if (len == LZ_MAXMATCH)
            return;

        next = LzByte(end);
        if (LzByte(pos - dist + len) == next) {
            len++;
            end++;
            continue;
        }

        /* the match stops here: walk the distance outwards */
        for (;;) {
            if (len == maxlen)
                dist += RunLen[(pos - dist) & 0xFFFF];

            q = (pos - dist) & LZ_STEPMASK;
            if (len - 3 >= MatchReach[q])
                return;
            dist += LzStepRead(q, len);
            if (dist > LZ_MAXDIST)
                return;
            if (LzByte(pos - dist + len) == next) {
                len++;
                end++;
                break;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  the Xtreme search, strategy 0: loc_4068F5 ... loc_406BFE            */
/* ------------------------------------------------------------------ */

/*
 *  The same idea with three extra heuristics, and it walks the *distance* in
 *  the inner loop instead of the length:
 *
 *    1. `BestCost[len - 3]` remembers the cheapest distance slot any position
 *       has reached at this length, so a length that cannot beat what has
 *       already been paid is abandoned early;
 *    2. `SlotOrder[]` (byte_185BA90, built by BuildDistanceSlots) is a linked
 *       list of the distance slots in ascending cost, so instead of trying
 *       every distance the search walks down to the best slot reachable;
 *    3. when a shorter length is already at least as cheap the search drops
 *       the length rather than emit a match nobody wants.
 */
static void LzXtreme(int pos, int savedCost, int cap, int maxlen, int dist)
{
    int len, seen, rank, target, here, saved, cost, end, next, q, t, s;
    /* the cursor as it stood when this position opened its slot; every read
     * below is measured against it (see the FPREAD note in pngout.h) */
    int base = MatchStart[pos & LZ_STEPMASK];

#ifdef ZTIME
    ZxStepC++;
#endif
#if defined ZTIME || defined PHASE || defined DUMPSTATE
    ZxCur = 0;                    /* this call's step-loop trip count */
#endif
#ifdef WALKCAP
    ZxWalkSteps = 0;
#endif
    HIRESET();
    seen = 0;
    rank = 0;
    saved = dist;
    here = 0;
    target = 0;
    next = 0;

    for (len = 3; ; ) {
#if defined ZTIME || defined PHASE || defined DUMPSTATE
        ZxOuter++;
#endif
#ifdef LZSTATS
        LzStatXtreme++;
#endif
        /*
         *  loc_406920: a distance is only *recorded* the first time this
         *  length is reached, so the step table has one entry per (position,
         *  length) rather than one per attempt.
         */
        here = DistCodeOf(dist);
        target = DistSlots[here];
#ifdef LZTRACE
        if (LZTRACING(pos))
            fprintf(stderr, "[lx] pos=%d len=%d dist=%d here=%d target=%d "
                    "savedCost=%d maxlen=%d cap=%d\n",
                    pos, len, dist, here, target, savedCost, maxlen, cap);
#endif

        if (len - 3 >= seen) {
            LzStepWrite(MatchCount) = (unsigned short)dist;
#ifdef LZTRACE
            if (RECLOGGING(MatchCount))
                fprintf(stderr, "[rec] entry=%d pos=%d len=%d dist=%d\n",
                        MatchCount, pos, len, dist);
#endif
            MatchCount++;
            seen++;
            BestCost[len - 3] = target;
        } else if (target < BestCost[len - 3]) {
            BestCost[len - 3] = target;
        }

        /* loc_406989: what a match of this length would cost */
        end = pos + len;
        cost = savedCost + DistSlots[LenCodeOf(len)] + target;
#ifdef LZTRACE
        if (LZTRACING(pos))
            fprintf(stderr, "[lx]   cost len=%d dist=%d end=%d cost=%d "
                    "have=%d cap=%d %s\n",
                    len, dist, end, cost, MatchCost[end & LZ_COSTMASK], cap,
                    (cost < MatchCost[end & LZ_COSTMASK] && end <= cap) ?
                    "SET" : "no");
#endif
        if (cost < MatchCost[end & LZ_COSTMASK] && end <= cap) {
            if (end > MatchLimit)
                MatchLimit = end;
            SetMatch(end, len, dist);
            MatchCost[end & LZ_COSTMASK] = cost;
        }

        /* loc_4069F6 */
        if (len == LZ_MAXMATCH) {
            /* loc_406B9A: only worth one more try if the slot is any good */
            if (SlotOrder[DistCodeOf(dist)] >= LZ_STARTHACK)
                return;
            if (maxlen == LZ_MAXMATCH)
                dist += RunLen[(pos - dist) & 0xFFFF];
            q = (pos - dist) & LZ_STEPMASK;
            if (MatchReach[q] > 0xFF) {
                dist += LzStepRead(q, LZ_STEPHOPS);
                HICOUNT();
                if (dist <= LZ_MAXDIST)
                    continue;
            }
            return;
        }

        rank = SlotOrder[here];
        next = LzByte(end);
        saved = dist;
        FPRANK(rank);

        for (;;) {
            /* loc_406A22: can the match grow by one more byte here? */
            if (LzByte(pos - dist + len) == next) {
#ifdef LZTRACE
                if (LZTRACING(pos))
                    fprintf(stderr, "[lx]   len %d -> %d at dist %d\n",
                            len, len + 1, dist);
#endif
                len++;
                break;
            }

            /* loc_406A53: step the distance outwards */
            if (len == maxlen)
                dist += RunLen[(pos - dist) & 0xFFFF];

            q = (pos - dist) & LZ_STEPMASK;
            if (len - 3 < MatchReach[q]) {
#if defined ZTIME || defined PHASE || defined DUMPSTATE
                ZxStep++;
                ZxCur++;
#endif
#ifdef WALKCAP
                if ((int)++ZxWalkSteps >= WALKCAP)
                    return;
#endif
                dist += LzStepRead(q, len);
                FPSTEP();
                HICOUNT();
#ifdef LZTRACE
                if (LZTRACING(pos))
                    fprintf(stderr, "[lx]   step len=%d dist->%d rank=%d slot=%d\n",
                            len, dist, rank, DistCodeOf(dist));
#endif
                if (dist <= LZ_MAXDIST) {
                    /* loc_406A8D: is there a cheaper slot to land on? */
                    if (rank > DistCodeOf(dist))
                        continue;
                    s = rank;
                    t = DistCodeOf(dist);
                    while (SlotOrder[s] <= t) {
#ifdef ZTIME
                        ZxRank++;
#endif
                        s = SlotOrder[s];
                    }
                    target = DistSlots[s];
#ifdef LZTRACE
                    if (LZTRACING(pos))
                        fprintf(stderr, "[lx]   slotwalk s=%d t=%d target=%d\n",
                                s, t, target);
#endif
                    goto trim;
                }
            }

            /* loc_406B7D: nothing cheaper in range; keep the rank we had */
            if (rank >= LZ_STARTHACK) {
#ifdef LZTRACE
                if (LZTRACING(pos))
                    fprintf(stderr, "[lx]   exit: rank %d >= 30\n", rank);
#endif
                return;
            }
            target = DistSlots[rank];

        trim:
            /*
             *  loc_406AC9: walk the length down while a *shorter* match has
             *  already been paid this little.  The two loops step by three and
             *  by one, which are the widths the length code table uses, and
             *  they read BestCost at len-6 and len-4 - BestCost is indexed by
             *  len-3 everywhere else.
             */
            while (len > 5 && BestCost[len - 6] > target) {
#ifdef ZTIME
                ZxTrim++;
#endif
                len -= 3;
            }
            while (len > 3 && BestCost[len - 4] > target) {
#ifdef ZTIME
                ZxTrim++;
#endif
                len--;
            }

            /* loc_406B05: and try that shorter length against the rank */
            dist = saved;
#ifdef LZTRACE
            if (LZTRACING(pos))
                fprintf(stderr, "[lx]   trim %d -> rank %d target %d\n",
                        len, rank, target);
#endif
            for (;;) {
                if (len == maxlen) {
                    dist += RunLen[(pos - dist) & 0xFFFF];
                    if (dist > LZ_MAXDIST) {
#ifdef LZTRACE
                        if (LZTRACING(pos))
                            fprintf(stderr, "[lx]   exit: restore dist %d > 32768\n",
                                    dist);
#endif
                        return;
                    }
                    if (DistCodeOf(dist) >= rank) {
#if defined ZTIME || defined PHASE || defined DUMPSTATE
                        ZxDistBase++;
#endif
                        /*
                         *  loc_406B8E: give up on this distance and start
                         *  again from the base of the code `rank` names.  It
                         *  reads the *distance* table here, since `rank` came
                         *  out of SlotOrder[], which indexes the distance
                         *  slots.
                         */
                        dist = DistCodeBase(rank);
                        break;
                    }
                }
                q = (pos - dist) & LZ_STEPMASK;
                if (len - 3 >= MatchReach[q]) {
#ifdef LZTRACE
                    if (LZTRACING(pos))
                        fprintf(stderr, "[lx]   exit: restore reach %d at q\n",
                                MatchReach[q]);
#endif
                    return;
                }
#ifdef ZTIME
                ZxRestore++;
#endif
                dist += LzStepRead(q, len);
                FPRESTORE();
                HICOUNT();
                if (dist > LZ_MAXDIST) {
#ifdef LZTRACE
                    if (LZTRACING(pos))
                        fprintf(stderr, "[lx]   exit: restore step to %d\n",
                                dist);
#endif
                    return;
                }
                if (DistCodeOf(dist) < rank)
                    continue;
                break;
            }
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  /DZTIME: cycle attribution for the position loop                   */
/* ------------------------------------------------------------------ */
#ifdef ZTIME
unsigned __int64 ZtCall, ZtChain, ZtSearch, ZtSkip, ZtRest, ZtPos;
unsigned __int64 ZtXtre, ZtEmit, ZtLit;
static unsigned __int64 zt(void)
{
    unsigned lo, hi;

    __asm {
        rdtsc
        mov lo, eax
        mov hi, edx
    }
    return ((unsigned __int64)hi << 32) | lo;
}
#define ZT(x) x
#else
#define ZT(x) ((void)0)
#endif

/* ------------------------------------------------------------------ */
/*  sub_4065A0                                                         */
/* ------------------------------------------------------------------ */

void (*LzTraceHook)(int, int, int, int, int, int);

#ifdef DUMPSTATE
/* ---- DUMPSTATE: frozen search inputs for the offline oracle ---- */
void DumpLzState(int pos, int savedCost, int cap, int maxlen, int dist,
                 int atTop)
{
    static int hits = 0;
    static int dumppos = -2;
    static int dumpnth = 1;
    static int dumpcount = -2;
    static int countFired = 0;
    static int dumptrial = -2;
    static int dumpsweep = -2;
    static int sweeps = 1;
    static int lastcount = 0;
    static int lasttrial = -9999;
    static int dumplist[16];
    static int listn = 0;
    static int listi = 0;
    static int posat = 0;
    static long scantrig = -1;
    static int scanfired = 0;
    static int dumpptrial = -2;
    char txtname[64], stepname[64], winname[64], auxname[64];
    char *e, *n, *c;
    FILE *f;
    int i, q;
    int byPos, byCount;
    int trialnow;

    if (dumppos == -2) {
        e = getenv("DUMPPOS");
        n = getenv("DUMPNTH");
        c = getenv("DUMPCOUNT");
        dumppos = (e ? atoi(e) : -1);
        dumpnth = (n ? atoi(n) : 1);
        dumpcount = (c ? atoi(c) : -1);
        if (dumpnth < 1)
            dumpnth = 1;
        /* DUMPCOUNT takes a comma separated list, so one run yields a series
         * of whole-table images and the first differing one can be bisected
         * out of them.  A bare number behaves as a one-entry list. */
        if (c) {
            char *p = c;
            while (*p && listn < 16) {
                dumplist[listn++] = atoi(p);
                while (*p && *p != ',')
                    p++;
                if (*p == ',')
                    p++;
            }
        }
        e = getenv("DUMPTRIAL");
        dumptrial = (e ? atoi(e) : 0);
        e = getenv("DUMPSWEEP");
        dumpsweep = (e ? atoi(e) : 1);
        e = getenv("DUMPPOSAT");
        posat = (e ? atoi(e) : 0);
        e = getenv("DUMPSCAN");
        scantrig = (e ? atol(e) : -1);
        e = getenv("DUMPPOSTRIAL");
        dumpptrial = (e ? atoi(e) : -1);
    }
    /* Sweep tracking: the cursor wraps to 0 at LZ_STEPCAP.  A big
     * instantaneous drop with the scan still climbing is a wrap (alias
     * rewinds are reach-scale, never 1M); a trial reset restarts the sweep
     * count instead (Image->trial is authoritative, no pointer hops). */
    trialnow = (Image ? Image->trial : -999);
    if (trialnow != lasttrial) {
        lasttrial = trialnow;
        sweeps = 1;
    } else if (MatchCount + 1000000 < lastcount) {
        sweeps++;
    }
    lastcount = MatchCount;
    /* The two call sites split the triggers: the loop-top call (dist -1,
     * before the chain walk) only honours DUMPCOUNT, the post-chain call
     * only DUMPPOS.  Otherwise DUMPPOS dumps would carry no search inputs. */
    byPos = ((!atTop && pos == dumppos) ||
             (atTop && posat && pos == dumppos)) &&
            (dumpptrial < 0 || trialnow == dumpptrial);
    byCount = (atTop && listn > 0 && listi < listn && trialnow == dumptrial &&
               sweeps == dumpsweep && MatchCount >= dumplist[listi]);
    /* DUMPSCAN: the loop-top trigger by scan position, gated on the trial, so
     * a later trial's state can be frozen at a chosen point. */
    if (atTop && scantrig >= 0 && !scanfired && trialnow == dumptrial &&
        LzScan > scantrig) {
        byCount = 1;
        scanfired = 1;
    }
    if (!byPos && !byCount)
        return;
    if (atTop) {
        /* Phase-build cross-check: how much work one trial really did. */
        extern unsigned long DbgSweeps, DbgXstep, DbgXouter;

        DbgSweeps = (unsigned long)sweeps;
        DbgXstep = ZxStep;
        DbgXouter = ZxOuter;
    }
    if (byCount) {
        countFired = ++listi;
        fprintf(stderr, "[dmp] count=%d pos=%d trial=%d sweep=%d (trigger %d/%d)\n",
                MatchCount, pos, trialnow, sweeps, listi, listn);
    }
    if (byPos) {
        hits++;
        if (hits != dumpnth)
            return;
    }

    sprintf(txtname, "lzstate_%d.txt", pos);
    sprintf(stepname, "lzdump_step_%d.bin", pos);
    sprintf(winname, "lzdump_win_%d.bin", pos);
    sprintf(auxname, "lzdump_aux_%d.bin", pos);
    if (countFired > 1) {            /* DUMPCOUNT list: keep the files apart */
        txtname[strlen(txtname) - 4] = 0;
        sprintf(txtname + strlen(txtname), "_%d.txt", countFired);
        stepname[strlen(stepname) - 4] = 0;
        sprintf(stepname + strlen(stepname), "_%d.bin", countFired);
        winname[strlen(winname) - 4] = 0;
        sprintf(winname + strlen(winname), "_%d.bin", countFired);
        auxname[strlen(auxname) - 4] = 0;
        sprintf(auxname + strlen(auxname), "_%d.bin", countFired);
    }

    f = fopen(txtname, "w");
    if (f) {
        fprintf(f, "pos=%d savedCost=%d cap=%d maxlen=%d dist=%d\n",
                pos, savedCost, cap, maxlen, dist);
        fprintf(f, "scan=%d write=%d count=%d limit=%d strategy=%d row=%d\n",
                LzScan, LzWrite, MatchCount, MatchLimit,
                (Image ? Image->strategy : -999), RowIndex);
        fprintf(f, "trial=%d sweep=%d rowend=%d\n",
                trialnow, sweeps, ROW_LIMIT(RowIndex));
        fprintf(f, "prevmark=%d prevreach=%d runcount=%d runend=%d\n",
                PrevMark, PrevReach, RunCount, RunEnd);
        fprintf(f, "slots:");
        for (i = 0; i < 64; i++)
            fprintf(f, " %d", DistSlots[i]);
        fprintf(f, "\norder:");
        for (i = 0; i < 32; i++)
            fprintf(f, " %u", SlotOrder[i]);
        fprintf(f, "\nbest:");
        for (i = 0; i < 256; i++)
            fprintf(f, " %d", BestCost[i]);
        fprintf(f, "\ncost:");
        for (i = 0; i < 512; i++)
            fprintf(f, " %d", MatchCost[i]);
        fprintf(f, "\nnear: (q start reach)");
        for (i = -32; i <= 32; i++) {
            q = (pos + i) & LZ_STEPMASK;
            fprintf(f, " [%d %d %d]", (pos + i), MatchStart[q], MatchArea[q]);
        }
        fprintf(f, "\naux=%s win=%s step=%s\n", auxname, winname, stepname);
        fprintf(f, "aux-layout: MatchStart[32768]s32 MatchArea[32768]s32 "
                "RunLen[65536]u16 HashHead[4096]u16 DistSlot[32768]u16 "
                "DistCodeToSlot[32768]u8 LenCodeToSlot[259]u8\n");
        fclose(f);
    }

    /* DUMPAUXONLY: skip the 16 MB step image (and the window) when only the
     * work arrays are wanted - the step image dominates the dump cost. */
    if (getenv("DUMPAUXONLY") == NULL) {
        f = fopen(stepname, "wb");
        if (f) {
            fwrite(StepTable, sizeof(StepTable[0]), LZ_STEPCAP + 260, f);
            fclose(f);
        }
        f = fopen(winname, "wb");
        if (f) {
            fwrite(LzWindow, 1, sizeof(LzWindow), f);
            fclose(f);
        }
    }
    f = fopen(auxname, "wb");
    if (f) {
        extern unsigned char DistCodeToSlot[];
        extern unsigned char LenCodeToSlotStore[];
        fwrite(MatchStart, sizeof(MatchStart[0]), 32768, f);
        fwrite(MatchArea, sizeof(MatchArea[0]), 0x8000, f);
        fwrite(RunLen, sizeof(RunLen[0]), 65536, f);
        fwrite(HashHead, sizeof(HashHead[0]), 4096, f);
        fwrite(DistSlot, sizeof(DistSlot[0]), 32768, f);
        /* the static code tables, so the offline oracle can index them too */
        fwrite(DistCodeToSlot, 1, 32768, f);
        fwrite(LenCodeToSlotStore, 1, 259, f);
        fclose(f);
    }
    fprintf(stderr, "[dumpstate] pos=%d hit=%d wrote %s\n",
            pos, hits, txtname);
}
#endif

#ifdef LZSTATS
unsigned __int64 LzStatChain, LzStatXtreme, LzStatPos, LzStatEmit;
#endif

/*
 *  The two hooks `FinishBlock()` and `FilterByte()` go through.  In the
 *  original these are jumps, not calls: `FilterByte()` jumps into loc_4075B0
 *  (the row loop's block-full handler) and loc_4075B0's first step is
 *  `sub_4065A0(0, 0)`.  Both are direct now that the coder exists, so the
 *  hooks are installed once rather than left to the caller.
 */
void FlushLzCoder(void)
{
    LzCode((const unsigned char *)0, 0);
}

int LzCode(const unsigned char *data, int len)
{
    int writePos, limit, rowend, cap;
    int pos, savedCost, hash, furthest, runEnd, start, dist, prev, word;
    int reach, left, k, same, alias;
#ifdef ZTIME
    unsigned __int64 t0, t1, t2, t3, tprev;
#endif
#ifdef LZFOOT
    int chainSteps, base;
#endif
    int *samples, *lens, *lencnt, *distfreq, *lenbits, *distbits;
#ifdef ZTIME
    unsigned __int64 tcall0 = zt();
#endif
#if defined ZTIME || defined DUMPSTATE
    unsigned __int64 zbefore;
#endif

    /* ---- 0x4065A9: append to the window ---- */
    writePos = LzWrite;
    if (len != 0) {
        if (len > 0) {
            for (k = 0; k < len; k++)
                LzByte(writePos + k) = data[k];
        }
        writePos += len;
        LzWrite = writePos;
        limit = writePos - LZ_SKIPBACK;
        LzHead = LzDword(0);
    } else {
        limit = writePos;
    }

    /* how far this row may reach: its own end, and never past what is written */
    rowend = ROW_LIMIT(RowIndex);
    cap = (writePos < rowend) ? writePos : rowend;

    if (LzScan >= limit)
        return 0;

    ZT(tprev = zt());
    while (LzScan < limit) {
#ifdef ZTIME
        t0 = zt();
        ZtPos++;
        ZtRest += t0 - tprev;
#endif
        pos = LzScan;
        runEnd = RunEnd;
#ifdef DUMPSTATE
        /* DUMPCOUNT sync: dump the moment the cursor first reaches the
         * target, before this position writes anything (dist -1: the
         * chain has not been walked yet).  Zero-cost when off. */
        DumpLzState(pos, MatchCost[pos & LZ_COSTMASK],
                    cap, runEnd - pos, -1, 1);
#endif
#ifdef ZTIME
        if ((ZtPos & 0x3FFFF) == 0)
            fprintf(stderr,
                    "[zt] pos=%I64u xtre=%I64u | per-pos call=%.0f xtre=%.0f "
                    "chain=%.0f lit=%.0f emit=%.0f rest=%.0f search=%.0f "
                    "| inner per-call: outer=%.1f step=%.1f restore=%.1f "
                    "trim=%.1f rank=%.1f scan=%d count=%d | steps/search log2:",
                    ZtPos, ZtXtre,
                    (double)ZtCall / (double)ZtPos,
                    (double)ZtXtre / (double)ZtPos,
                    (double)ZtChain / (double)ZtPos,
                    (double)ZtLit / (double)ZtPos,
                    (double)ZtEmit / (double)ZtPos,
                    (double)ZtRest / (double)ZtPos,
                    (double)ZtSearch / (double)ZtPos,
                    (double)ZxOuter / (double)(ZxStepC ? ZxStepC : 1),
                    (double)ZxStep / (double)(ZxStepC ? ZxStepC : 1),
                    (double)ZxRestore / (double)(ZxStepC ? ZxStepC : 1),
                    (double)ZxTrim / (double)(ZxStepC ? ZxStepC : 1),
                    (double)ZxRank / (double)(ZxStepC ? ZxStepC : 1),
                    LzScan, MatchCount);
            {
                int hi2;

                for (hi2 = 0; hi2 < 24; hi2++)
                    if (ZxHist[hi2])
                        fprintf(stderr, " 2^%d=%lu(%.1f%%)", hi2, ZxHist[hi2],
                                100.0 * (double)ZxHist[hi2] /
                                (double)(ZxCalls ? ZxCalls : 1));
                fprintf(stderr, "\n");
            }
#endif
#ifdef LZSTATS
        LzStatPos++;
        /* Periodic so a long run can be sampled without waiting for the
         * end-of-trial line in trials.c. */
        if ((LzStatPos & 0xFFFFF) == 0)
            fprintf(stderr,
                    "[lz] pos=%I64u chain=%I64u xtreme=%I64u scan=%d count=%d "
                    "chain/pos=%.2f xtreme/pos=%.2f\n",
                    LzStatPos, LzStatChain, LzStatXtreme, LzScan, MatchCount,
                    (double)LzStatChain / (double)LzStatPos,
                    (double)LzStatXtreme / (double)LzStatPos);
#endif

        savedCost = MatchCost[pos & LZ_COSTMASK];
        MatchCost[pos & LZ_COSTMASK] = 0x7FFFFFFF;

        word = (int)LzDword(pos);
        hash = (((word >> 12) - (unsigned short)word) & 0xFFF);

        if (Image->strategy >= 3)
            goto literal;

        /* ---- 0x40668C: the run of identical bytes this position is in ---- */
        {
            int b = LzByte(pos);
            int n;

            if (b == unk_42E55C) {
                n = RunCount + 1;
                if (n > 0x7FFF)
                    n = 0x7FFF;
            } else {
                n = 0;
                unk_42E55C = (unsigned char)b;
            }
            RunCount = n;
            RunLen[pos & 0xFFFF] = (unsigned short)n;

            if (runEnd <= pos + 1) {
                runEnd = pos + 1;
                RunEnd = runEnd;
            }
            if (runEnd < writePos) {
                while (runEnd < writePos && LzByte(runEnd) == b)
                    runEnd++;
                RunEnd = runEnd;
            }
        }

        /* ---- 0x4066FB: open this position's slot in the step table ---- */
        if (MatchCount > LZ_STEPCAP) {
            MatchCount = 0;
        }
        MatchStart[pos & LZ_STEPMASK] = MatchCount;

        start = pos & LZ_STEPMASK;
        prev = (int)(unsigned short)HashHead[hash];
        dist = ((pos - prev - 1) & LZ_STEPMASK) + 1;

        /* ---- 0x406740: walk the hash chain while the 3 low bytes agree ---- */
#ifdef LZFOOT
        chainSteps = 0;
#endif
        for (;;) {
#ifdef LZSTATS
            LzStatChain++;
#endif
#ifdef LZFOOT
            chainSteps++;
#endif
            if (((LzDword(pos - dist) ^ (unsigned)word) & 0xFFFFFF) == 0)
                break;
            prev = (int)(unsigned short)DistSlot[prev];
            if (dist >= (((pos - prev - 1) & LZ_STEPMASK) + 1)) {
                dist = -1;             /* the chain ran out; flag it below */
                break;
            }
            dist = ((pos - prev - 1) & LZ_STEPMASK) + 1;
        }
#ifdef LZFOOT
        FpChain(chainSteps);
#endif
#ifdef ZTIME
        t1 = zt();
        ZtChain += t1 - t0;
#endif

        if (dist > 0 && pos >= dist) {
            /* loc_40677C: a real candidate */
            int maxlen = RunEnd - pos;

#ifdef DUMPSTATE
            DumpLzState(pos, savedCost, cap, maxlen, dist, 0);
#endif
#ifdef LZTRACE
            if (LZTRACING(pos))
                fprintf(stderr, "[lx] search pos=%d chainDist=%d maxlen=%d\n",
                        pos, dist, maxlen);
#endif
#ifdef ZTIME
            t2 = zt();
            zbefore = ZxStep;
#elif defined DUMPSTATE
            zbefore = ZxStep;
#endif
            if (Image->strategy == 0) {
#if defined ZTIME || defined PHASE || defined DUMPSTATE
                if (pos == ZxLastPos)
                    ZxResearch++;
                ZxLastPos = pos;
#endif
                LzXtreme(pos, savedCost, cap, maxlen, dist);
#if defined ZTIME || defined PHASE || defined DUMPSTATE
                {
                    unsigned long d0 = ZxCur;   /* before the bucket shift */
                    unsigned long d = d0;
                    int b = 0;

                    while ((d >>= 1) != 0 && b < 23)
                        b++;
                    ZxHist[b]++;
                    ZxCalls++;
#ifdef DUMPSTATE
                    if ((ZxCalls % 10000000UL) == 0)
                        fprintf(stderr, "[rs] calls=%lu research=%lu (%.1f%%) distbase=%lu\n",
                                ZxCalls, ZxResearch,
                                100.0 * (double)ZxResearch / (double)ZxCalls,
                                ZxDistBase);
#endif
#ifdef DUMPSTATE
                    /* LZMINSTEPS: report the positions of the long searches,
                     * which is where the step-loop time actually goes. */
                    {
                        static long minsteps = -2;

                        if (minsteps == -2) {
                            char *e = getenv("LZMINSTEPS");

                            minsteps = (e ? atol(e) : 0);
                        }
                        if (minsteps && (long)d0 >= minsteps) {
                            LzMinPos = LzScan;
                            fprintf(stderr, "[long] trial=%d pos=%d steps=%lu\n",
                                    (Image ? Image->trial : -1), pos, d0);
                        }
                    }
#endif
                }
#endif
#ifdef HIREACH
                if (HiReachReads > 2000 && pos > 500000) {
                    static int hr2count = 0;

                    if (hr2count < 2) {
                        fprintf(stderr, "[hr2] pos=%d reads=%d\n", pos,
                                HiReachReads);
                        hr2count++;
                    }
                }
#endif
            } else {
                LzSimple(pos, savedCost, cap, maxlen, dist);
            }
#ifdef ZTIME
            ZtXtre += zt() - t2;
#endif
#ifdef LZTRACE
            if (LZTRACING(pos))
                fprintf(stderr, "[lx] done pos=%d reach=%d\n",
                        pos, MatchCount - MatchStart[start]);
#endif
        }
#ifdef ZTIME
        t3 = zt();
#endif
        /* ---- 0x406C02: close this position's slot ---- */
        reach = MatchCount - MatchStart[start];
        MatchReach[start] = reach;

#ifdef LZFOOT
        base = MatchStart[start];
#endif
        if (reach <= PrevReach) {
            /*
             *  loc_406C29: the same list of distances as the position before?
             *  If so this position is an alias of it - the step table says the
             *  same steps will be taken - so its entries are thrown away and
             *  MatchStart moves back over them.  The comparison is done two
             *  shorts at a time, and only when the count is even, so an odd
             *  one gets one extra single compare at loc_406C33.
             */
            same = 1;
            if (reach & 1) {
                if (LzStepRead(start, 3) !=
                    StepTable[PrevMark + 3 - reach])
                    same = 0;
            }
            if (same) {
                left = reach & 0xFFFFFFFE;
                for (k = 0; k < left; k += 2) {
                    if (LzStepPairRead(MatchCount - left + k) !=
                        LzStepPairRead(PrevMark - left + k)) {
                        same = 0;
                        break;
                    }
                }
            }
            if (same) {
                MatchCount = PrevMark;
                MatchStart[start] = PrevMark - reach;
            }
            alias = same;
        } else {
            alias = 0;
        }
        FPREACH(reach, alias);
#ifdef HIREACH
        if (reach >= 5 && reach <= 12 && pos > 5000 && pos < 8000) {
            static int hrcount = 0;

            if (hrcount < 2) {
                fprintf(stderr, "[hr] pos=%d reach=%d\n", pos, reach);
                hrcount++;
            }
        }
#endif

#ifdef ZTIME
        t3 = zt();
#endif
        PrevMark = MatchCount;
        PrevReach = MatchReach[start];
        DistSlot[start] = HashHead[hash];
        HashHead[hash] = start;

    literal:
        /*
         *  0x406C02: the position's slot is closed - PrevMark and PrevReach
         *  are updated and the hash chains are linked - and the last thing
         *  it does is `mov ebx, dword_412668` at 0x406CC4.  That is where
         *  the local `furthest` comes from, and it is a *fresh* read of the
         *  global: the search routines above have been raising MatchLimit to
         *  the furthest position their matches reach, so by now it can be far
         *  ahead of what it was when this iteration started.  Taking the
         *  value from the top of the iteration instead makes the literal's
         *  clamp below fire against a stale number and pull MatchLimit back
         *  down, which loses every match the position found.
         */
        furthest = MatchLimit;

        /* ---- 0x406CED: no match here, so a literal ---- */
        {
            int b = LzByte(pos);

            samples = ROWOF(RowIndex, ROW_SAMPLES);
            lens = ROWOF(RowIndex, ROW_SYMBOLS);

            samples[b] |= 0x80000000;
            pos++;

            if (savedCost + lens[b] <= MatchCost[pos & LZ_COSTMASK]) {
                if (pos > furthest) {
                    furthest = pos;
                    MatchLimit = pos;
                }
                SetMatch(pos, 1, b);
                MatchCost[pos & LZ_COSTMASK] = savedCost + lens[b];
            }
            LzScan = pos;
        }

        /* ---- 0x406D68: keep scanning, or skip ahead ---- */
#ifdef ZTIME
        t2 = zt();
        ZtSearch += t2 - t1;
        if (t2 != t3)
            ZtLit += t2 - t3;
        t3 = zt();
#endif
        /*
         *  0x406D68 decides it in three steps, and the shape matters: strategy
         *  2 always takes the skip loop, strategy 0 skips the distance check
         *  and goes straight to the rowstart check, and strategy 1 takes the
         *  skip loop at once when the position is already 258 short of the
         *  furthest match - `jle loc_406D9E` - and only otherwise falls through
         *  to the rowstart check.  Reading the middle arm as "skip when past
         *  the furthest" inverts it: that is exactly when the original does
         *  *not* take the fast path, and the positions that should have been
         *  hash-linked get searched instead.
         */
        {
            int takeSkip = 0;

            if (Image->strategy == 2) {
                takeSkip = 1;
            } else if (Image->strategy == 0) {
                if (pos - unk_42D7C8 + LZ_SKIPBACK >= LZ_SCANBACK)
                    takeSkip = 1;
            } else {
                if (pos + LZ_MAXMATCH <= furthest)
                    takeSkip = 1;
                else if (pos - unk_42D7C8 + LZ_SKIPBACK >= LZ_SCANBACK)
                    takeSkip = 1;
            }

            if (takeSkip) {
                /*
                 *  0x406D9E: the match is as long as it can get from here on, so
                 *  everything up to `furthest` only has to go into the hash chains -
                 *  no match list, no costs.
                 */
                for (k = RunCount; pos < furthest; ) {
                    int h, p, q;
                    unsigned int w;

                    p = pos & 0xFFFF;
                    MatchCost[pos & LZ_COSTMASK] = 0x7FFFFFFF;
                    w = LzDword(pos);
                    h = (((int)(w >> 12) - (unsigned short)w) & 0xFFF);

                    if (Image->strategy < 3) {
                        int c = LzByte(pos);

                        if (c == unk_42E55C) {
                            k++;
                            if (k > 0x7FFF)
                                k = 0x7FFF;
                        } else {
                            k = 0;
                            unk_42E55C = (unsigned char)c;
                        }
                        RunCount = k;
                        RunLen[p] = (unsigned short)k;
                        MatchReach[pos & LZ_STEPMASK] = 0;
                        PrevReach = 0;
                    }

                    q = pos & LZ_STEPMASK;
                    DistSlot[q] = HashHead[h];
                    HashHead[h] = q;
                    pos++;
                    LzScan = pos;
                }
            }
        }

#ifdef ZTIME
        tprev = zt();
#endif
        if (pos != furthest)
            continue;

        /* ---- 0x406E69: everything up to `furthest` is settled ---- */
        /*
         *  `furthest` above is the copy of MatchLimit this iteration started
         *  with - 0x4065B4's `mov ebx, dword_412668` - and it is only the
         *  *trigger* for this block (`cmp ebx, edi; jnz loc_40706E`), which
         *  the literal path below updates.  The emit itself re-reads the
         *  global: 0x406E69's `mov esi, dword_412668`, and again at 0x406FEE
         *  for the loop bound.  The search routines raise MatchLimit to the
         *  furthest position a match reaches, so by the time the scan catches
         *  up the global is ahead of the copy the trigger used, and it is the
         *  global that says how much there is to emit.  Using the copy here
         *  emits the matches one batch too late, and the chain then reaches
         *  back past the row start.
         */
        {
            int i = MatchLimit;
            int from = MatchLimit - MatchLenAt(MatchLimit);

            /*
             *  loc_406E97: the last entry can reach back before the row began,
             *  so its tail is slid down until it no longer does.
             */
            if (from > unk_42D7C8) {
                unsigned int w = MatchList[2 * (from & LZ_LISTMASK)] |
                                 ((unsigned int)MatchList[2 * (from & LZ_LISTMASK) + 1] << 16);

                for (;;) {
                    i--;
                    MatchList[2 * (i & LZ_LISTMASK)] = (int)(w & 0xFFFF);
                    MatchList[2 * (i & LZ_LISTMASK) + 1] = (int)(w >> 16);
                    if (LzTraceHook)
                        LzTraceHook(-1, i, (int)(w & 0xFFFF), from,
                                    (int)(w >> 16), unk_42D7C8);
                    from -= MatchLenAt(from);
                    if (from <= unk_42D7C8)
                        break;
                    w = MatchList[2 * (from & LZ_LISTMASK)] |
                        ((unsigned int)MatchList[2 * (from & LZ_LISTMASK) + 1] << 16);
                }
            }

            samples = ROWOF(RowIndex, ROW_SAMPLES);
            lens = ROWOF(RowIndex, ROW_SYMBOLS);
            lencnt = ROWOF(RowIndex, ROW_LENCNT);
            distfreq = ROWOF(RowIndex, ROW_DISTFREQ);
            lenbits = ROWOF(RowIndex, ROW_LENBITS);
            distbits = ROWOF(RowIndex, ROW_DISTBITS);

#ifdef ZTIME
            t0 = zt();
#endif
            /* loc_406ED4: emit them */
            for (; i <= MatchLimit; i++) {
                int mlen = MatchLenAt(i);

                if (LzTraceHook)
                    LzTraceHook(i, mlen, MatchValAt(i), furthest, MatchLimit,
                                unk_42D7C8);
#ifdef EMITLOG
                {
                    /* one line per emitted symbol, so a byte offset from
                     * tools\lzdiff.py can be turned into a scan position */
                    extern int EmitMax, EmitCount;

                    if (EmitCount < EmitMax) {
                        EmitCount++;
                        fprintf(stderr, "[emit] n=%d i=%d len=%d val=%d limit=%d\n",
                                EmitCount, i, mlen, MatchValAt(i), MatchLimit);
                    }
                }
#endif

                /*
                 *  Each entry stands for the run of bytes that *ends* at its own
                 *  index, so the walk does not advance by the length: a match of
                 *  length L at index i is one symbol and the next entry is i+1,
                 *  which is why the shift loop above exists - it slides the tails
                 *  down so the entries line up one index per symbol.
                 */
                if (mlen == 1) {
                    int b = MatchValAt(i);

                    samples[b]++;
                    PutBits((unsigned int)LitCodes[b],
                            (unsigned int)lens[b]);
                } else {
                    int mdist = MatchValAt(i);
                    int lslot = LenCodeOf(mlen);       /* 30 .. 58 */
                    int dslot = DistCodeOf(mdist);     /*  0 .. 29 */

                    lencnt[lslot]++;
                    distfreq[dslot]++;

                    PutBits((unsigned int)LenCodeOut(lslot),
                            (unsigned int)lenbits[lslot]);
                    PutBits((unsigned int)(mlen - LenCodeBase(lslot - 30)),
                            (unsigned int)LenCodeExtra(lslot - 30));

                    PutBits((unsigned int)DistCodes[dslot],
                            (unsigned int)distbits[dslot]);
                    PutBits((unsigned int)(mdist - DistCodeBase(dslot)),
                            (unsigned int)DistCodeExtra(dslot));
                }
            }
        }

#ifdef ZTIME
        ZtEmit += zt() - t0;
#endif
        /* ---- 0x407000: close the row if it is full ---- */
        unk_42D7C8 = LzScan;
        if (LzScan >= ROW_LIMIT(RowIndex)) {
            AdvanceRow();
            memset32(MatchCost, 0x7FFFFFFF, 512);
            MatchCost[LzScan & LZ_COSTMASK] = 0x80000000;
            rowend = ROW_LIMIT(RowIndex);
            cap = (LzWrite < rowend) ? LzWrite : rowend;
        }
    }

#ifdef ZTIME
    ZtCall += zt() - tcall0;
#endif
    return 0;
}
