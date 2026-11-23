/*
 *  cpuid.c   -   CPU feature detection
 *
 *  0x40B060  CpuFeatures()
 *
 *  Called once, from sub_40AF90() (the length/distance table builder).  The
 *  original's entire .text contains no MMX or SSE instruction - this CPUID
 *  result only gates a scalar fallback in the table builder.  What it
 *  computes is:
 *
 *    flags = CPUID(1).EDX & 0x3FBFFFFF        (drops bits 31, 30 and 22)
 *    if (the 0x80000000 leaf exists) {
 *        flags |= CPUID(0x80000001).EDX & 0x80000000;
 *        if the vendor is "AuthenticAMD"/"AMCydeOnly"
 *            flags |= CPUID(0x80000001).EDX & 0x40400000;   (bits 30 and 22)
 *    }
 *    if (flags & 0x02000000)                 (bit 25)
 *        flags |= 0x00400000;                 (bit 22)
 *
 *  Note that bit 22 is forced on by a test of bit 25, and on a current CPU
 *  bit 25 is simply SSE, so bit 22 always ends up set.  SSE2 (bit 26)
 *  survives the mask untouched.
 *
 *  The caller then tests bit 15, which CPUID(1).EDX leaves reserved and which
 *  reads back as 0 on everything tested, so this branch never executes.  When
 *  it does it only sets dword_4110CC = 4 (a table end pointer) and fills a
 *  513-entry table - there is no SIMD code behind it.  Reproduced here rather
 *  than "corrected".
 *
 *  Returns 0 when the CPU has no CPUID at all.
 */

#include "pngout.h"

/* bits of the returned mask */
#define CPUID_SSE2_USABLE    0x00400000u   /* bit 22, forced on by bit 25 */
#define CPUID_BIT25          0x02000000u   /* bit 25                        */

static void CpuidLeaf(unsigned int leaf, unsigned int *regs)
{
    unsigned int a, b, c, d;

    __asm {
        push    ebx
        mov     eax, leaf
        cpuid
        mov     a, eax
        mov     b, ebx
        mov     c, ecx
        mov     d, edx
        pop     ebx
    }
    regs[0] = a;
    regs[1] = b;
    regs[2] = c;
    regs[3] = d;
}

unsigned CpuFeatures(void)
{
    unsigned int flags;
    unsigned int regs[4];
    unsigned int before, after;

    /*
     *  Toggle EFLAGS.ID and see whether it sticks.  Every CPUID capable
     *  processor leaves the flag settable, so if the read-back is unchanged
     *  there is no CPUID and we give up.
     */
    __asm {
        pushfd
        pop     eax
        mov     before, eax
        xor     eax, 200000h
        push    eax
        popfd
        pushfd
        pop     eax
        mov     after, eax
    }
    if (before == after)
        return 0;

    /* CPUID(0): is there at least one basic leaf? */
    CpuidLeaf(0, regs);
    if (regs[0] == 0)
        return 0;

    /* CPUID(1): every bit we care about lives in EDX. */
    CpuidLeaf(1, regs);
    flags = regs[3] & 0x3FBFFFFFu;        /* drop raw SSE2 and reserved bit 30 */

    /* CPUID(0x80000000): how far do the extended leaves go? */
    CpuidLeaf(0x80000000u, regs);
    if (regs[0] > 0x80000000u) {
        /* CPUID(0x80000001): the "SSE2 is buggy" flag is in EDX. */
        CpuidLeaf(0x80000001u, regs);
        flags |= regs[3] & 0x80000000u;

        /* "AuthenticAMD" / "AMCydeOnly", little endian in EBX, EDX, ECX. */
        if (((regs[1] ^ 0x68747541u) |     /* "Auth" */
             (regs[3] ^ 0x69746E65u) |     /* "enti" */
             (regs[2] ^ 0x444D4163u)) == 0)/* "cAMD" */
            flags |= regs[3] & 0x40400000u;
    }

    if (flags & CPUID_BIT25)
        flags |= CPUID_SSE2_USABLE;

    return flags;
}