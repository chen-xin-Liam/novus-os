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
void kputs_dec(UINT64 v);
void kputs_hex(UINT64 v);
/* Coarse boot timeline; mirrored by kputs to VGA, framebuffer and COM1. */
void novus_timeline(const char *stage);
/* Hand the firmware stage's TSC calibration to novus_timeline() so the first
 * call there does not re-probe (see the note on the seed in kmain.c). The
 * kernel stage calls the reset on entry to restart the numbering. */
void novus_timeline_seed(UINT64 start_tsc, UINT64 hz);
void novus_timeline_reset(void);

/* Busy-wait |us| microseconds against the ACPI PM timer. Drivers that have
 * to let a device settle (a USB port reset, a controller reset) use this
 * instead of counting I/O reads: an ISA read is only "about a microsecond"
 * on real silicon, and on an emulated machine one read can cost hundreds of
 * microseconds, which turns a 60 ms reset settle into half a minute. */
void cpu_delay_us(UINT32 us);

/* Monotonic microseconds from the TSC and the rate novus_timeline() already
 * calibrated (CPUID 0x15/0x16, else the PM timer the ACPI FADT names). A
 * protocol that is specified in milliseconds needs a unit of time, and the
 * TSC is the one clock a driver can rely on without probing a port itself:
 * this laptop answers nothing at the literal 0x608 the USB driver used to
 * assume, even though the FADT names a working port. Returns 0 when the boot
 * could not calibrate the rate. */
UINT64 cpu_uptime_us(void);

/* Kernel version, as NKM module headers state it (spec 3.2: a module is
 * refused when its min_kernel_ver is higher than this). Major in the
 * high 32 bits, minor in the low 32 bits: 1.0. */
#define NOVUS_KERNEL_VERSION 0x0000000100000000ULL

/* Freestanding memory helpers; the compiler may emit calls to these. */
#if CONFIG_BIOS_BOOT
#define NOVUS_MEMORY_ABI __attribute__((sysv_abi))
#else
#define NOVUS_MEMORY_ABI
#endif
void *NOVUS_MEMORY_ABI memcpy(void *dst, const void *src, UINT64 n);
void *NOVUS_MEMORY_ABI memset(void *dst, int c, UINT64 n);
int NOVUS_MEMORY_ABI memcmp(const void *a, const void *b, UINT64 n);

/* Copy between user memory and a kernel buffer through a task's page
 * tables (trap.c). Returns 0, or -1 as soon as a page is not mapped.
 * `to_user` selects the direction. The Win32 layer uses it to read API
 * arguments and write results. */
int user_copy(UINT64 cr3, UINT64 va, void *buf, UINT64 len, int to_user);

#endif /* NOVUS_KERNEL_H */
