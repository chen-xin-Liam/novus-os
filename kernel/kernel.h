/**
 * Novus OS - kernel-stage public interface.
 */

#ifndef NOVUS_KERNEL_H
#define NOVUS_KERNEL_H

#include "uefi.h"

/* BSP kernel entry, called by the UEFI boot stage right after
 * ExitBootServices. The arguments describe the final EFI memory map
 * (buffer address, total size, descriptor size); parsing starts in M1,
 * the signature is kept stable so the boot stage does not change. */
void kmain(UINT64 memmap_addr, UINT64 memmap_size, UINT64 desc_size);

/* Assembly kernel entry (arch/x86_64/kernel/entry.S): installs the
 * 64 KiB BSP kernel stack, zeroes .bss, then calls kmain() with these
 * arguments unchanged. Never returns. */
void kernel_entry(UINT64 memmap_addr, UINT64 memmap_size, UINT64 desc_size);

/* Unified kernel-stage output: mirrors text to VGA and COM1. */
void kputs(const char *s);
void kputs_hex(UINT64 v);

/* Kernel version, as NKM module headers state it (spec 3.2: a module is
 * refused when its min_kernel_ver is higher than this). Major in the
 * high 32 bits, minor in the low 32 bits: 1.0. */
#define NOVUS_KERNEL_VERSION 0x0000000100000000ULL

/* Freestanding memory helpers; the compiler may emit calls to these. */
void *memcpy(void *dst, const void *src, UINT64 n);
void *memset(void *dst, int c, UINT64 n);
int memcmp(const void *a, const void *b, UINT64 n);

#endif /* NOVUS_KERNEL_H */
