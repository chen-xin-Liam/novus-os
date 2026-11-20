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
/* A well-formed 32-bit image. The loader understands only PE32+ today; the
 * compatibility executor a PE32 needs is a separate subsystem, so this is its
 * own code instead of a generic format error. */
#define PE_E32      -7

/* What an image is, independent of whether this layer can run it. The probe
 * reads the DOS/NT headers only, so it answers for PE32, PE32+, x86, x86-64
 * and ARM64 alike - which is what a "will this run here" report needs. */
typedef struct {
  UINT16 machine;      /* 0x14C i386, 0x8664 x86-64, 0xAA64 arm64 */
  UINT16 magic;        /* 0x10B PE32, 0x20B PE32+ */
  UINT16 subsystem;    /* 2 GUI, 3 console */
  UINT16 os_major;     /* optional header OS version */
  UINT16 os_minor;
  UINT16 sub_major;    /* subsystem version: the Windows generation it targets */
  UINT16 sub_minor;
  UINT8  is_dll;
  UINT8  bits;         /* 32 or 64 */
  UINT32 entry_rva;
  UINT64 image_base;
} pe_probe_t;

/* Fill `out` from the image. Returns 0, or PE_ENOTPE / PE_EFORMAT. */
int pe_probe(const UINT8 *img, UINT32 size, pe_probe_t *out);

/* One line naming the image and the Windows generation its subsystem version
 * targets ("PE32+ x86-64 console EXE, subsystem 6.1 (Windows 7)"). Returns
 * 0, or PE_ENOTPE. A 16-bit DOS image gets "DOS MZ executable (16-bit)".
 */
int pe_describe(const UINT8 *img, UINT32 size, char *out, UINT32 cap);

/* Validate `img` and start it as a Win32 compatibility process. Returns 0
 * on success, or one of the PE_E* codes. */
int pe_load(const char *name, const UINT8 *img, UINT32 size);

/* Feed two malformed images through the validator. Returns 0 when both are
 * refused. */
int pe_selftest(void);

#endif /* NOVUS_PE_H */
