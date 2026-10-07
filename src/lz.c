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
#include "pngout.h"

/* ---- the geometry, straight out of the immediates in the listing ---- */
#define LZ_MAXMATCH 258              /* `cmp esi, 102h`                     */
#define LZ_COSTMASK 0x1FF            /* dword_412678[], 512 entries         */
#define LZ_LISTMASK 0x3FFFF          /* dword_706B88[], 0x40000 entries     */
#define LZ_STEPMASK 0x7FFF           /* every 32K array is indexed like that */
#define LZ_MAXDIST  0x8000
#define LZ_STEPCAP  0x7FFF00         /* `cmp eax, offset unk_7FFF00`        */
#define LZ_SCANBACK 0x40000          /* `(pos - rowstart + 260) >= 0x40000` */
#define LZ_SKIPBACK 260              /* `lea ecx, [esi-104h]`              */
#define LZ_STARTHACK  30              /* `cmp byte_185BA90[dl], 1Eh`        */

#define LZ_STEPHOPS  258             /* word_806D86 - word_806B82 = 0x204 bytes */

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
static void LzSimple(int pos, int savedCost, int cap, int maxlen, int dist)
{
    int len, end, next, dslot, lslot, cost, q;

    for (len = 3, end = pos + 3; ; ) {
        LzStepWrite(MatchCount) = (unsigned short)dist;
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
            dist += StepTable[MatchStart[q] + len];
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

    seen = 0;
    rank = 0;
    saved = dist;
    here = 0;
    target = 0;
    next = 0;

    for (len = 3; ; ) {
        /*
         *  loc_406920: a distance is only *recorded* the first time this
         *  length is reached, so the step table has one entry per (position,
         *  length) rather than one per attempt.
         */
        here = DistCodeOf(dist);
        target = DistSlots[here];

        if (len - 3 >= seen) {
            LzStepWrite(MatchCount) = (unsigned short)dist;
            MatchCount++;
            seen++;
            BestCost[len - 3] = target;
        } else if (target < BestCost[len - 3]) {
            BestCost[len - 3] = target;
        }

        /* loc_406989: what a match of this length would cost */
        end = pos + len;
        cost = savedCost + DistSlots[LenCodeOf(len)] + target;
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
                dist += StepTable[MatchStart[q] + LZ_STEPHOPS];
                if (dist <= LZ_MAXDIST)
                    continue;
            }
            return;
        }

        rank = SlotOrder[here];
        next = LzByte(end);
        saved = dist;

        for (;;) {
            /* loc_406A22: can the match grow by one more byte here? */
            if (LzByte(pos - dist + len) == next) {
                len++;
                break;
            }

            /* loc_406A53: step the distance outwards */
            if (len == maxlen)
                dist += RunLen[(pos - dist) & 0xFFFF];

            q = (pos - dist) & LZ_STEPMASK;
            if (len - 3 < MatchReach[q]) {
                dist += StepTable[MatchStart[q] + len];
                if (dist <= LZ_MAXDIST) {
                    /* loc_406A8D: is there a cheaper slot to land on? */
                    if (rank > DistCodeOf(dist))
                        continue;
                    s = rank;
                    t = DistCodeOf(dist);
                    while (SlotOrder[s] <= t)
                        s = SlotOrder[s];
                    target = DistSlots[s];
                    goto trim;
                }
            }

            /* loc_406B7D: nothing cheaper in range; keep the rank we had */
            if (rank >= LZ_STARTHACK)
                return;
            target = DistSlots[rank];

        trim:
            /*
             *  loc_406AC9: walk the length down while a *shorter* match has
             *  already been paid this little.  The two loops step by three and
             *  by one, which are the widths the length code table uses, and
             *  they read BestCost at len-6 and len-4 - BestCost is indexed by
             *  len-3 everywhere else.
             */
            while (len > 5 && BestCost[len - 6] > target)
                len -= 3;
            while (len > 3 && BestCost[len - 4] > target)
                len--;

            /* loc_406B05: and try that shorter length against the rank */
            dist = saved;
            for (;;) {
                if (len == maxlen) {
                    dist += RunLen[(pos - dist) & 0xFFFF];
                    if (dist > LZ_MAXDIST)
                        return;
                    if (DistCodeOf(dist) >= rank) {
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
                if (len - 3 >= MatchReach[q])
                    return;
                dist += StepTable[MatchStart[q] + len];
                if (dist > LZ_MAXDIST)
                    return;
                if (DistCodeOf(dist) < rank)
                    continue;
                break;
            }
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  sub_4065A0                                                         */
/* ------------------------------------------------------------------ */

void (*LzTraceHook)(int, int, int, int, int, int);

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
    int reach, left, k, same;
    int *samples, *lens, *lencnt, *distfreq, *lenbits, *distbits;

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

    while (LzScan < limit) {
        pos = LzScan;
        runEnd = RunEnd;

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
        for (;;) {
            if (((LzDword(pos - dist) ^ (unsigned)word) & 0xFFFFFF) == 0)
                break;
            prev = (int)(unsigned short)DistSlot[prev];
            if (dist >= (((pos - prev - 1) & LZ_STEPMASK) + 1)) {
                dist = -1;             /* the chain ran out; flag it below */
                break;
            }
            dist = ((pos - prev - 1) & LZ_STEPMASK) + 1;
        }

        if (dist > 0 && pos >= dist) {
            /* loc_40677C: a real candidate */
            int maxlen = RunEnd - pos;

            if (Image->strategy == 0) {
                LzXtreme(pos, savedCost, cap, maxlen, dist);
            } else {
                LzSimple(pos, savedCost, cap, maxlen, dist);
            }
        }

        /* ---- 0x406C02: close this position's slot ---- */
        reach = MatchCount - MatchStart[start];
        MatchReach[start] = reach;

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
                if (StepTable[MatchStart[start] + 3] !=
                    StepTable[PrevMark + 3 - reach])
                    same = 0;
            }
            if (same) {
                left = reach & 0xFFFFFFFE;
                for (k = 0; k < left; k += 2) {
                    if (LzStepPairAt(MatchCount - left + k) !=
                        LzStepPairAt(PrevMark - left + k)) {
                        same = 0;
                        break;
                    }
                }
            }
            if (same) {
                MatchCount = PrevMark;
                MatchStart[start] = PrevMark - reach;
            }
        }

        PrevMark = MatchCount;
        PrevReach = MatchReach[start];
        DistSlot[start] = (int)(unsigned short)HashHead[hash];
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
        if (Image->strategy != 2 &&
            (Image->strategy == 0 || pos + LZ_MAXMATCH > furthest) &&
            pos - unk_42D7C8 + LZ_SKIPBACK >= LZ_SCANBACK) {
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
                DistSlot[q] = (int)(unsigned short)HashHead[h];
                HashHead[h] = q;
                pos++;
                LzScan = pos;
            }
        }

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

            /* loc_406ED4: emit them */
            for (; i <= MatchLimit; i++) {
                int mlen = MatchLenAt(i);

                if (LzTraceHook)
                    LzTraceHook(i, mlen, MatchValAt(i), furthest, MatchLimit,
                                unk_42D7C8);

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

    return 0;
}
