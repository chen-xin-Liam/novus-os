/**
 * Novus OS - Win32 compatibility layer, prototype (M9).
 *
 * The layer has two halves and no libc:
 *
 *   - kernel/pe.c maps a PE32+ image and fills its IAT with the thunks
 *     described here. Each thunk carries one API id in R10 and enters the
 *     single SYS_WIN32 system call.
 *   - kernel/win32.c answers that system call: console output, ticks,
 *     sleep and the simulated registry.
 *
 * Windows x64 passes the first argument in RCX, but `syscall` overwrites
 * RCX with the return address, so the thunk moves RCX to RDI before it
 * traps. RDI is callee-saved in that ABI, so the thunk parks it in the
 * caller's shadow space first and puts it back after the trap.
 */

#ifndef NOVUS_WIN32_H
#define NOVUS_WIN32_H

#include "uefi.h"
#include "idt.h"

/* README part 8: the numbered syscall table. 40 is the compatibility gate;
 * no other call is reachable from a TASK_ABI_WIN32 process. */
#define SYS_WIN32 40u

/* Fixed addresses inside the compatibility process. They are clear of the
 * NUC image area (0x400000) and of each other:
 *
 *   0x50000000  PE image (relocated from its preferred base)
 *   0x60000000  one page of API thunks
 *   0x70000000  command line / image name
 *   0x90000000  return trampoline: ExitProcess(RAX)
 *   0xA0000000  user stack top (64 KiB below it)
 */
#define WIN32_IMAGE_BASE  0x0000000050000000ULL
#define WIN32_THUNK_VA    0x0000000060000000ULL
#define WIN32_ARGV_VA     0x0000000070000000ULL
#define WIN32_TRAMPOLINE  0x0000000090000000ULL
#define WIN32_STACK_TOP   0x00000000A0000000ULL
#define WIN32_STACK_MIN   0x0000000000010000ULL

/* Parked Sleep() waits here; nothing ever sends to it, so only the tick
 * deadline can wake the task. */
#define WIN32_SLEEP_PORT  0xFFFFFFFFFFFFFFFFULL

/* Thunk stride, see win32_thunk_bytes(). */
#define WIN32_THUNK_LEN 32u

/* ExitProcess .. RegCloseKey. The id is the thunk's R10 value. */
#define WIN32_API_COUNT 15u

void win32_init(void);

/* SYS_WIN32 entry. Called from trap.c with the frame the syscall came in
 * on; arguments are read from the Win64 registers and the user stack. */
void win32_syscall(trap_frame_t *f);

/* Drop the registry handles a process left open when it exits. */
void win32_owner_exit(UINT64 pid);

/* Map "<dll>!<func>" onto an API id, or -1 when the pair is not part of the
 * supported subset. DLL names are compared case-insensitively. */
int win32_api_lookup(const char *dll, const char *func);

/* Emit the machine code for one API id (exactly WIN32_THUNK_LEN bytes). */
void win32_thunk_bytes(UINT32 api_id, UINT8 out[WIN32_THUNK_LEN]);

#endif /* NOVUS_WIN32_H */
