/**
 * Novus OS - PE32+ (Windows x64) image loader, prototype (M9).
 *
 * The loader takes an in-memory .exe, validates it, maps its sections at the
 * fixed compatibility base, applies base relocations, fills the import
 * address table with Win32 thunks (kernel/win32.c) and starts it as a
 * TASK_ABI_WIN32 process.
 *
 * Only PE32+ for x86-64 is understood, and only the import-by-name subset
 * the compatibility layer implements. Anything else is refused before a
 * task is created, so a malformed image costs nothing but a log line.
 */

#ifndef NOVUS_PE_H
#define NOVUS_PE_H

#include "uefi.h"

#define PE_ENOTPE   -1
#define PE_EMACHINE -2
#define PE_EFORMAT  -3
#define PE_ENOMEM   -4
#define PE_EIMPORT  -5
#define PE_ERELOC   -6

/* Validate `img` and start it as a Win32 compatibility process. Returns 0
 * on success, or one of the PE_E* codes. */
int pe_load(const char *name, const UINT8 *img, UINT32 size);

/* Feed two malformed images through the validator. Returns 0 when both are
 * refused. */
int pe_selftest(void);

#endif /* NOVUS_PE_H */
