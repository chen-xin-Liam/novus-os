/**
 * Novus OS - GDT and TSS (M4).
 *
 * The UEFI firmware leaves a flat 64-bit GDT behind that only has ring 0
 * segments. syscall/sysret and iret to ring 3 both need ring 3 segments,
 * and any transition from ring 3 into the kernel needs a TSS: the CPU
 * loads RSP0 from it on the way in. So M4 installs its own table.
 *
 * Layout (fixed - the selectors are used in assembly):
 *
 *   0x00 null
 *   0x08 kernel code (64-bit, DPL 0)
 *   0x10 kernel data (DPL 0)
 *   0x18 user code   (64-bit, DPL 3)   -> ring 3 selector 0x1B
 *   0x20 user data   (DPL 3)           -> ring 3 selector 0x23
 *   0x28 TSS (16-byte descriptor)
 */

#ifndef NOVUS_GDT_H
#define NOVUS_GDT_H

#include "uefi.h"

#define GDT_SEL_KCODE 0x08u
#define GDT_SEL_KDATA 0x10u
#define GDT_SEL_UCODE 0x1Bu /* RPL 3 */
#define GDT_SEL_UDATA 0x23u /* RPL 3 */
#define GDT_SEL_TSS   0x28u

/* Install the GDT, reload the segment registers, load the task register.
 * Runs in ring 0 with interrupts off. */
void gdt_init(void);

/* Set the ring-0 stack the CPU switches to when an interrupt or exception
 * arrives from ring 3 - i.e. the kernel stack of the task about to run.
 * Must be called on every task switch. */
void tss_set_rsp0(UINT64 rsp0);

#endif /* NOVUS_GDT_H */
