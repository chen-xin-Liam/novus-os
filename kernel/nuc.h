/**
 * Novus OS - NUC executable loader (M4).
 *
 * Implements docs/NUC 可执行二进制格式规范 V1.0.md sections 9-10: check
 * the header, map every segment at the address the compiler chose, apply
 * relocations, build a user stack, and hand the entry point to the
 * scheduler as a ring-3 task.
 *
 * The image arrives in reserved pages published by the boot stage: the
 * kernel cannot read the ESP itself after ExitBootServices. NUC has no
 * signature (unlike NKM) - it is a user program, confined by its page
 * table rather than by a key.
 */

#ifndef NOVUS_NUC_H
#define NOVUS_NUC_H

#include "uefi.h"

/* Load `img` as a NUC program named `name` and make it runnable. Returns 0
 * on success, or one of the negative NOVUS_E* codes from the spec. */
int nuc_load(const void *img, UINT64 size, const char *name);

/* Load a program that loops on the yield syscall, for the scheduler to
 * fall back on when every real task has exited. */
int nuc_create_idle(void);

/* A user task that never yields (`jmp $`). It exists to prove the timer
 * preempts: nothing in its code gives up the CPU, so if it is ever seen
 * being switched away from, the tick did it. */
int nuc_create_spin(void);

/* Start a task whose entry point is a page mapped without EXEC. It must
 * die on #PF with the instruction-fetch bit set; if it ever prints its
 * exit code as 0 the NX bit is not doing anything. */
int nuc_nx_probe(void);

/* Build a minimal .nuc in kernel memory - one text segment, `mov eax,142;
 * ret` - and load it. Exercises header checks, segment mapping, the user
 * stack, the trampoline and the exit syscall without needing a file on
 * the medium, so M4 is verifiable on any image. */
int nuc_selftest(void);

#endif /* NOVUS_NUC_H */
