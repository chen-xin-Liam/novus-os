/**
 * Novus OS - PE32 (i386) loader, WOW32 executor (M13).
 *
 * A PE32 image is mapped at its preferred base (both bundled installers use
 * 0x400000), base relocations of type HIGHLOW are applied, and its 4-byte
 * IAT entries point at 32-bit stdcall thunks in kernel/wow32.c. The task is
 * created with TASK_ABI_WOW32 so iretq enters the compatibility submode.
 */

#ifndef NOVUS_PE32_H
#define NOVUS_PE32_H

#include "uefi.h"

#define PE32_ENOTPE   -1
#define PE32_EMACHINE -2
#define PE32_EFORMAT  -3
#define PE32_ENOMEM   -4
#define PE32_EIMPORT  -5
#define PE32_ERELOC   -6

/* Validate `img`, map it and start it as a 32-bit compatibility process. */
int pe32_load(const char *name, const UINT8 *img, UINT32 size);

/* A byte source. Readers return the number of bytes produced at `off` and
 * may short-read; the loader loops. This is what lets a 319 MiB installer be
 * mapped straight from the VFS without ever holding it in one buffer. */
typedef struct {
  void *ctx;
  int  (*read)(void *ctx, UINT64 off, void *buf, UINT32 len, UINT32 *done);
} pe32_src_t;

int pe32_load_src(const char *name, pe32_src_t *src, UINT32 size);

/* Resolve an absolute VFS path and load the PE32 image it names. */
int pe32_load_path(const char *name, const char *path);

#endif /* NOVUS_PE32_H */
