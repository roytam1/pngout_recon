/*
 *  dumpsteps.c  -   dump the first 32K step-table entries once the cursor passes them
 *
 *  The step table is append-only per block (the cursor only moves forward until
 *  it wraps at 0x7FFF00), so entries [0..32767] are written by the first few
 *  thousand positions and never touched again until a wrap.  If two binaries
 *  write different values there, their searches parted company almost
 *  immediately; if they match, the seed is later.  That bisects the hunt.
 *
 *    dumpsteps <exe> [args...]
 *
 *  StepTable base defaults to the original's 0x806B82; WATCH_STEP overrides it
 *  for the reconstruction (tools\linkmap.cmd).  The cursor (to confirm it has
 *  moved past the window) defaults to 0x180BA84 / WATCH_COUNT.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <tlhelp32.h>

typedef HANDLE (__stdcall *OpenThreadFn)(DWORD, BOOL, DWORD);
static OpenThreadFn pOpenThread;

static unsigned long EnvAddr(const char *name, unsigned long dflt)
{
    char *s = getenv(name);

    return s ? strtoul(s, NULL, 0) : dflt;
}

static void Freeze(DWORD pid, int stop)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;

    if (!pOpenThread) {
        pOpenThread = (OpenThreadFn)GetProcAddress(GetModuleHandle("kernel32"),
                                                   "OpenThread");
        if (!pOpenThread)
            return;
    }
    if (snap == INVALID_HANDLE_VALUE)
        return;
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) {
                HANDLE th = pOpenThread(THREAD_SUSPEND_RESUME, FALSE,
                                        te.th32ThreadID);

                if (th) {
                    if (stop)
                        SuspendThread(th);
                    else
                        ResumeThread(th);
                    CloseHandle(th);
                }
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

int main(int argc, char **argv)
{
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    char cmd[1024];
    int argi, i;
    unsigned long a_step, a_count, a_scan;
    DWORD count = 0, scan = 0, lastscan = 0, lastcount = 0;
    int resets = 0, sweeps = 1;
    static unsigned short window[8192];   /* one 16 KB read chunk; DUMPOUT streams */
    long dumpoff, dumplen, margin, wanttrial, wantsweep;
    long triglist[16];
    int trign = 0, trigi = 0;
    long scantrig;
    long dbgpoll = 0;
    DWORD t0tick = GetTickCount();
    unsigned long a_ms, a_ma, a_rl, a_hh, a_ds2;
    unsigned long a_slots, a_order, a_best, a_cost, a_img;
    char tabsout[300];
    char auxout[300];
    char outbase[256];
    char *outname;
    char *e;

    if (argc < 2) {
        fprintf(stderr, "usage: dumpsteps <exe> [args...]\n");
        return 2;
    }
    a_step  = EnvAddr("WATCH_STEP",  0x806B82UL);
    a_count = EnvAddr("WATCH_COUNT", 0x180BA84UL);
    a_scan  = EnvAddr("WATCH_SCAN",  0x6F6B78UL);
    e = getenv("DUMPOFF");
    dumpoff = e ? atol(e) : 0;
    e = getenv("DUMPLEN");
    dumplen = e ? atol(e) : 8192;
    if (dumplen > 0x800000 + 260)      /* the whole table, for full diffs */
        dumplen = 0x800000 + 260;
    /* Settle margin: the alias mechanism rewinds the cursor (MatchCount goes
     * *down* to PrevMark), so freezing the instant the cursor passes the
     * window can catch it mid-rewind with the window under rewrite.  Waiting
     * until it is `margin' entries past makes the window deep history. */
    e = getenv("DUMPMARGIN");
    margin = e ? atol(e) : 16384;
    /* Trial/sweep selection (both 0/1-based to match DUMPTRIAL/DUMPSWEEP on
     * the recon side): trials restart the scan (big backward jump), sweeps
     * restart the cursor (big drop with the scan still climbing; alias
     * rewinds are reach-scale, never 1M).  A sweep takes seconds-to-minutes
     * while polls run every millisecond, so no boundary slips through. */
    e = getenv("DUMPTRIAL");
    wanttrial = e ? atol(e) : 0;
    e = getenv("DUMPSWEEP");
    wantsweep = e ? atol(e) : 1;
    /* DUMPCNT: explicit cursor trigger(s), comma separated, for windows too
     * big to be "settled" (a whole-table dump fires near the cap, not past
     * off+len).  One run then yields a series of images to bisect. */
    e = getenv("DUMPCNT");
    if (e) {
        char *p = e;
        while (*p && trign < 16) {
            triglist[trign++] = atol(p);
            while (*p && *p != ',')
                p++;
            if (*p == ',')
                p++;
        }
    }
    /* DUMPOUT: write the window as raw little-endian u16 instead of text on
     * stderr, so a 16 MB table dump is practical. */
    outname = getenv("DUMPOUT");
    if (outname) {
        strncpy(outbase, outname, sizeof(outbase) - 1);
        outbase[sizeof(outbase) - 1] = 0;
    } else {
        outbase[0] = 0;
    }
    /* DUMPSCAN + DUMPAUX: freeze once the scan passes a position and write the
     * five work arrays in the recon's lzdump_aux layout, so the search state
     * can be compared position by position instead of only the step table. */
    e = getenv("DUMPSCAN");
    scantrig = e ? atol(e) : -1;
    auxout[0] = 0;
    if (getenv("DUMPAUX")) {
        strncpy(auxout, getenv("DUMPAUX"), sizeof(auxout) - 1);
        auxout[sizeof(auxout) - 1] = 0;
    }
    a_ms  = EnvAddr("WATCH_MS",  0x6D6670UL);   /* MatchStart  [32768]s32 */
    a_ma  = EnvAddr("WATCH_MA",  0x6B4968UL);   /* MatchArea   [32768]s32 */
    a_rl  = EnvAddr("WATCH_RL",  0x185BED8UL);  /* RunLen      [65536]u16 */
    a_hh  = EnvAddr("WATCH_HH",  0x412EC0UL);   /* HashHead    [ 4096]u16 */
    a_ds2 = EnvAddr("WATCH_DSL", 0x41CEC8UL);   /* DistSlot    [32768]u16 */
    /* DUMPTABS: the three search-heuristic tables, which are rebuilt per
     * block/trial and so differ between trials: DistSlots[64], SlotOrder[32]
     * and BestCost[256].  BestCost is never reset, so it is the one piece of
     * search state that has never been compared between the binaries. */
    tabsout[0] = 0;
    if (getenv("DUMPTABS"))
        strncpy(tabsout, getenv("DUMPTABS"), sizeof(tabsout) - 1);
    a_slots = EnvAddr("WATCH_SLOTS", 0x6D5578UL);   /* DistSlots [64]s32 */
    a_order = EnvAddr("WATCH_ORDER", 0x185BA90UL);  /* SlotOrder[32]u8  */
    a_best  = EnvAddr("WATCH_BEST",  0x6D5170UL);   /* BestCost [256]s32 */
    a_cost  = EnvAddr("WATCH_COST",  0x412678UL);   /* MatchCost[512]s32 */
    /* Image record: strategy is at +0x4C, and the first 0x60 bytes
     * are dumped so the whole per-trial header can be diffed. */
    a_img   = EnvAddr("WATCH_IMG",   0x42E960UL);
    if (tabsout[0] && !outname)
        dumplen = 0;                      /* tables only: no step text dump */

    cmd[0] = 0;
    if (strchr(argv[1], ' ') || strchr(argv[1], '\t'))
        sprintf(cmd, "\"%s\"", argv[1]);
    else
        strcpy(cmd, argv[1]);
    for (argi = 2; argi < argc; argi++) {
        strcat(cmd, " ");
        if (strchr(argv[argi], ' ') || strchr(argv[argi], '\t'))
            sprintf(cmd + strlen(cmd), "\"%s\"", argv[argi]);
        else
            strcat(cmd, argv[argi]);
    }

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    {
        HANDLE nul = CreateFile("NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, OPEN_EXISTING, 0, NULL);

        SetHandleInformation(GetStdHandle(STD_ERROR_HANDLE),
                             HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = nul;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    }
    if (!CreateProcess(argv[1], cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
        return 1;
    }

    /* Poll fast (no freeze) until the window is settled; freezing every cycle
     * costs ~20 ms a round (Toolhelp snapshot) while the cursor crosses an 8K
     * window in under a millisecond, so a freezing poll always misses. Once
     * the cursor is past, freeze once and dump - the window is then stable
     * (written, and far from any wrap). */
    /* No settle delay: early windows (first 40K entries) pass in milliseconds
     * on dense inputs, so any sleep misses them.  Reading is harmless during
     * CRT init (only *freezing* there crashes), and the break needs scan > 0
     * *and* count past the window, which loader garbage cannot satisfy. */
    for (;;) {
        DWORD code = 0;
        int hit;

        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
            GetExitCodeProcess(pi.hProcess, &code);
            fprintf(stderr, "[t] child exited code=%lu at scan %lu count %lu\n",
                    code, scan, count);
            break;
        }
        if (!ReadProcessMemory(pi.hProcess, (void *)(ULONG_PTR)a_scan,
                               &scan, sizeof(scan), NULL) ||
            !ReadProcessMemory(pi.hProcess, (void *)(ULONG_PTR)a_count,
                               &count, sizeof(count), NULL)) {
            continue;
        }
        /* Trial/sweep tracking without trusting any pointer hop: the scan
         * restarts every trial and the cursor restarts every sweep, so a big
         * backward jump in either is a boundary - and a whole sweep takes
         * seconds while polls run every millisecond, so no boundary can slip
         * through unseen. */
        if (scan + 1000000UL < (unsigned long)lastscan) {
            resets++;
            sweeps = 1;
        } else if (count + 1000000UL < (unsigned long)lastcount) {
            sweeps++;
        }
        lastscan = scan;
        lastcount = count;
        if (resets != wanttrial || (scantrig < 0 && sweeps != wantsweep))
            continue;
        if (dbgpoll++ % 20000 == 0)
            fprintf(stderr, "[t] poll t=%lu scan=%lu count=%lu resets=%d sweeps=%d "
                    "trig=%ld/%ld\n", GetTickCount() - t0tick, scan, count, resets,
                    sweeps, scantrig, trign);
        hit = (scantrig >= 0
               ? ((long)scan > scantrig)
               : (trign > 0
                  ? ((unsigned long)count > (unsigned long)triglist[trigi])
                  : ((unsigned long)count >
                     (unsigned long)(dumpoff + dumplen + margin)))) &&
              (unsigned long)count < 0x7FFF00UL && scan > 0;
        if (!hit)
            continue;
        if (trign > 0)
            trigi++;
        {
            DWORD stopscan = scan, stopcount = count;

        Freeze(pi.dwProcessId, 1);
        /* No re-verify: aligned DWORD/count reads never tear, and the count
         * can legitimately rewind below the threshold during the suspend
         * round (alias cuts) while the deep window stays put.  The stop
         * point is informational only. */
        ReadProcessMemory(pi.hProcess, (void *)(ULONG_PTR)a_scan,
                          &stopscan, sizeof(stopscan), NULL);
        ReadProcessMemory(pi.hProcess, (void *)(ULONG_PTR)a_count,
                          &stopcount, sizeof(stopcount), NULL);
        fprintf(stderr, "[t] frozen scan=%lu stop=%lu count=%lu stopcount=%lu\n",
                scan, stopscan, count, stopcount);

        /* The window was past (cursor beyond it with margin) at freeze time,
         * so dump it unconditionally: re-checking the rewound count here
         * would false-fail on alias cuts. */
        {
            /* Read in 16 KB chunks with retries: a single big
             * ReadProcessMemory fails sporadically, and while frozen the
             * child waits, so retries are free.
             *
             * Note the +3: entry k lives at StepTable[k + 3]
             * (LzStepWrite(k) = StepTable[k + 3]; word_806B82/86/88 are one
             * array with a 3-entry preamble), so entry A's address is
             * a_step + 2*(A + 3). */
            long got = 0, tries = 0;
            int ok = 1;
            FILE *bf = NULL;
            char thisout[300];

            thisout[0] = 0;
            if (outname) {
                if (trign > 1)
                    sprintf(thisout, "%s.%d", outbase, trigi);
                else
                    strcpy(thisout, outbase);
                bf = fopen(thisout, "wb");
                if (!bf) {
                    fprintf(stderr, "[t] cannot open %s\n", thisout);
                    ok = 0;
                }
            }
            while (ok && got < dumplen) {
                long want = dumplen - got;

                if (want > 8192)
                    want = 8192;
                if (!ReadProcessMemory(pi.hProcess,
                               (void *)(ULONG_PTR)(a_step + 2UL * (dumpoff + got + 3)),
                               window, (SIZE_T)(2UL * want), NULL)) {
                    if (++tries > 5) {
                        ok = 0;
                        break;
                    }
                    Sleep(50);
                    continue;
                }
                if (bf && fwrite(window, 2, (size_t)want, bf) != (size_t)want) {
                    fprintf(stderr, "[t] short write at %ld\n", got);
                    ok = 0;
                    break;
                }
                got += want;
                tries = 0;
            }
            if (bf)
                fclose(bf);
            if (ok) {
                if (bf) {
                    fprintf(stderr, "[t] scan=%lu count=%lu trial=%d sweep=%d "
                            "step%ld..%ld -> %s (%ld entries)\n",
                            scan, count, resets, sweeps, dumpoff,
                            dumpoff + dumplen - 1, thisout, dumplen);
                } else {
                    fprintf(stderr, "[t] scan=%lu count=%lu trial=%d sweep=%d step%ld..%ld:",
                            scan, count, resets, sweeps, dumpoff, dumpoff + dumplen - 1);
                    for (got = 0; got < dumplen; got += 8192) {
                        long want = dumplen - got;
                        if (want > 8192)
                            want = 8192;
                        if (!ReadProcessMemory(pi.hProcess,
                                   (void *)(ULONG_PTR)(a_step + 2UL * (dumpoff + got + 3)),
                                   window, (SIZE_T)(2UL * want), NULL)) {
                            fprintf(stderr, " readfail@%ld", got);
                            break;
                        }
                        for (i = 0; i < want; i++)
                            fprintf(stderr, " %u", window[i]);
                    }
                    fprintf(stderr, "\n");
                }
            } else {
                fprintf(stderr, "[t] read failed at scan %lu\n", scan);
            }
        }
        /* the heuristic tables: DistSlots[64]s32, SlotOrder[32]u8, BestCost[256]s32 */
        if (tabsout[0]) {
            static unsigned long tbuf[256];
            FILE *tf = fopen(tabsout, "wb");

            if (tf) {
                if (ReadProcessMemory(pi.hProcess, (void *)(ULONG_PTR)a_slots,
                                      tbuf, 64 * 4, NULL))
                    fwrite(tbuf, 4, 64, tf);
                if (ReadProcessMemory(pi.hProcess, (void *)(ULONG_PTR)a_order,
                                      tbuf, 32, NULL))
                    fwrite(tbuf, 1, 32, tf);
                if (ReadProcessMemory(pi.hProcess, (void *)(ULONG_PTR)a_best,
                                      tbuf, 256 * 4, NULL))
                    fwrite(tbuf, 4, 256, tf);
                if (ReadProcessMemory(pi.hProcess, (void *)(ULONG_PTR)a_cost,
                                      tbuf, 256 * 4, NULL))
                    fwrite(tbuf, 4, 256, tf);
                if (ReadProcessMemory(pi.hProcess, (void *)(ULONG_PTR)a_img,
                                      tbuf, 4, NULL)) {
                    unsigned long ip = tbuf[0];

                    fprintf(stderr, "[t] image=%08lX\n", ip);
                    if (ReadProcessMemory(pi.hProcess,
                                          (void *)(ULONG_PTR)ip, tbuf, 0x60, NULL))
                        fwrite(tbuf, 4, 0x18, tf);
                }
                fclose(tf);
                fprintf(stderr, "[t] tabs -> %s (scan %lu count %lu)\n",
                        tabsout, scan, count);
            }
        }
        /* the five work arrays, in the recon's lzdump_aux layout */
        if (auxout[0]) {
            static unsigned long abuf[32768];
            FILE *af = fopen(auxout, "wb");
            struct { unsigned long a; long n; } parts[5];
            int pi2;

            parts[0].a = a_ms;  parts[0].n = 32768;   /* s32 */
            parts[1].a = a_ma;  parts[1].n = 32768;   /* s32 */
            parts[2].a = a_rl;  parts[2].n = 65536;   /* u16 -> 2 per word */
            parts[3].a = a_hh;  parts[3].n = 4096;    /* u16 */
            parts[4].a = a_ds2; parts[4].n = 32768;   /* u16 */
            if (af) {
                for (pi2 = 0; pi2 < 5; pi2++) {
                    long n = parts[pi2].n, done = 0;
                    int is32 = (pi2 < 2);

                    while (done < n) {
                        long want = n - done;

                        if (want > 8192)
                            want = 8192;
                        if (!ReadProcessMemory(pi.hProcess,
                                   (void *)(ULONG_PTR)(parts[pi2].a +
                                        (is32 ? 4UL * (ULONG)done : 2UL * (ULONG)done)),
                                   abuf, (SIZE_T)((is32 ? 4UL : 2UL) * (ULONG)want),
                                   NULL)) {
                            fprintf(stderr, "[t] aux read fail part %d at %ld\n",
                                    pi2, done);
                            done = n;
                            memset(abuf, 0, (size_t)((is32 ? 4UL : 2UL) * (ULONG)want));
                            break;
                        }
                        fwrite(abuf, is32 ? 4 : 2, (size_t)want, af);
                        done += want;
                    }
                }
                fclose(af);
                fprintf(stderr, "[t] aux -> %s (scan %lu count %lu)\n", auxout,
                        scan, count);
            }
        }
        Freeze(pi.dwProcessId, 0);
        /* More triggers left: the child is deterministic, so freezing it
         * between images does not change what it writes. */
        if (trigi < trign)
            continue;
        TerminateProcess(pi.hProcess, 0);
        break;
        }
    }
    CloseHandle(pi.hThread);    CloseHandle(pi.hProcess);
    return 0;
}
