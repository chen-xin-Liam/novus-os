/**
 * Novus OS - model-specific register access (M4).
 *
 * Two instructions, but they are the only way to reach the registers that
 * decide how a CPU enters the kernel: EFER.SCE turns the syscall
 * instruction on, STAR/LSTAR/FMASK say where it lands and what it masks,
 * and GS_BASE/KERNEL_GS_BASE are what swapgs swaps. Keeping them in one
 * place keeps the MSR numbers next to their names.
 */

#ifndef NOVUS_MSR_H
#define NOVUS_MSR_H

#include "uefi.h"

/* EFER (0xC0000080): bit 0 SCE = syscall/sysret enable, bit 8 LME, bit 11
 * NXE. Only SCE is touched here; LME is already set by the UEFI firmware. */
#define MSR_EFER        0xC0000080u
#define EFER_SCE        (1ULL << 0)

/* STAR (0xC0000081): bits 47:32 = kernel CS/SS selector base,
 * bits 63:48 = user CS/SS selector base (for sysret). */
#define MSR_STAR        0xC0000081u

/* LSTAR (0xC0000082): the 64-bit syscall entry point. */
#define MSR_LSTAR       0xC0000082u

/* FMASK (0xC0000084): RFLAGS bits cleared on syscall entry. Clearing IF
 * matters: the entry runs on a stack that is not the caller's yet. */
#define MSR_FMASK       0xC0000084u

/* GS bases: swapgs exchanges them. KERNEL_GS_BASE holds the kernel's
 * per-CPU pointer while user code runs. */
#define MSR_GS_BASE        0xC0000101u
#define MSR_KERNEL_GS_BASE 0xC0000102u

static inline UINT64 msr_read(UINT32 msr) {
  UINT32 lo, hi;
  __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
  return ((UINT64)hi << 32) | lo;
}

static inline void msr_write(UINT32 msr, UINT64 value) {
  __asm__ volatile("wrmsr"
                   :
                   : "c"(msr), "a"((UINT32)(value & 0xFFFFFFFFu)),
                     "d"((UINT32)(value >> 32))
                   : "memory");
}

static inline void cpu_cli(void) { __asm__ volatile("cli" : : : "memory"); }
static inline void cpu_sti(void) { __asm__ volatile("sti" : : : "memory"); }
static inline void cpu_halt(void) { __asm__ volatile("hlt"); }

#endif /* NOVUS_MSR_H */
