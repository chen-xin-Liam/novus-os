/**
 * Novus OS - interrupt descriptor table (M4).
 *
 * M4 needs three kinds of vector:
 *
 *   0..31   CPU exceptions. Without handlers, the first #PF in a buggy
 *           user program is a triple fault and the whole machine resets -
 *           the README's "a crashing process must not crash the kernel".
 *   32      the LAPIC timer, which is what makes scheduling preemptive.
 *   0x100   not an IDT vector at all: the syscall entry stub builds the
 *           same frame with this number so one dispatcher handles both.
 */

#ifndef NOVUS_IDT_H
#define NOVUS_IDT_H

#include "uefi.h"

#define IDT_TIMER_VECTOR  32u
#define IDT_SYSCALL_MARK  0x100u /* frame-only marker, never an IDT entry */

/* One trap frame, laid out by trap.S. The order is the push order of the
 * stub reversed; do not reorder without changing the assembly. */
typedef struct {
  UINT64 r15, r14, r13, r12, r11, r10, r9, r8;
  UINT64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
  UINT64 vector;  /* 0..31 exception, 32 timer, 0x100 syscall */
  UINT64 errcode; /* CPU error code, or 0 for vectors that have none */
  UINT64 rip, cs, rflags, rsp, ss;
} trap_frame_t;

/* Install the table. Interrupts stay disabled until sched_start(). */
void idt_init(void);

#endif /* NOVUS_IDT_H */
