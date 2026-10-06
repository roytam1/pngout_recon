/*
 *  defs.h
 *
 *  Compatibility layer that lets the Hex-Rays output in pngout.exe.c compile
 *  unmodified under Visual C++ 6.0.
 *
 *  IDA renders types as _BYTE/_WORD/_DWORD and hides the size of opaque blobs
 *  behind _UNKNOWN.  It also uses its own spelling for a few CRT routines.
 */

#ifndef DEFS_H
#define DEFS_H

typedef unsigned char  _BYTE;
typedef unsigned short _WORD;
typedef unsigned int   _DWORD;

/* Opaque blob: IDA could not determine a size. Declared as a 1-byte object so
 * that "extern _UNKNOWN x;" and "&x" both work; never dereference it. */
typedef unsigned char _UNKNOWN;

typedef void (__cdecl *_PVFV)(void);

/* IDA's name for the CRT memcpy. */
#define qmemcpy  memcpy

/* IDA spells the VC intrinsic like a function. */
#ifndef _byteswap_ulong
#  define _byteswap_ulong(x)  _byteswap_ulong((unsigned long)(x))
#endif

/* VC6 has no __noreturn. */
#ifndef __noreturn
#  define __noreturn
#endif

/* VC6 does not know the C99 restrict keyword under this name. */
#ifndef restrict
#  define restrict
#endif

#endif /* DEFS_H */