/**
 * Novus OS - kernel log ring (M11).
 *
 * kputs() has always mirrored to VGA and COM1, and the boot stage has always
 * printed through the firmware console. Neither copy was available to the
 * graphical boot splash, which showed only a status line. klog keeps one
 * chronological byte ring across both stages: the boot stage leaves its lines
 * in the shared-memory handoff block, klog_init() seeds the kernel ring from
 * it, and every later kputs() appends to the same ring. The splash paints the
 * newest lines into a panel over its backdrop.
 *
 * Writers run with interrupts disabled (kputs serialises that way), readers
 * are the splash's stage updates. No allocation, no locks of its own.
 */
#ifndef NOVUS_KLOG_H
#define NOVUS_KLOG_H

#include "kernel.h"

/* 32 KiB keeps several thousand lines; old bytes are overwritten in order. */
#define KLOG_CAP 32768u

/* Seed the ring from the boot stage's shared-memory log. Call once, right
 * after shared_mem_kernel_init() and before the first kputs(). */
void klog_init(void);

/* Append a NUL-terminated string. Safe before klog_init(): the ring lives in
 * zeroed BSS. Must be called with interrupts disabled, like kputs(). */
void klog_puts(const char *s);

/* Bumped on every append; readers use it to decide when to repaint. Before
 * klog_init() (i.e. in the UEFI boot stage) this tracks the shared-memory
 * ring instead, so the splash can watch for new firmware lines. */
UINT32 klog_version(void);

/* Total bytes written, capped at KLOG_CAP. */
UINT32 klog_size(void);

/* Copy the most recent bytes into dst, chronological and NUL-terminated.
 * Returns the number of bytes copied (excluding the terminator). */
UINT32 klog_snapshot(char *dst, UINT32 cap);

#endif /* NOVUS_KLOG_H */
