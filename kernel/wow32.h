/**
 * Novus OS - WOW32: PE32 execution in the long mode compatibility submode.
 *
 * A WOW task uses a 32-bit CS (GDT_SEL_UCODE32), a 32-bit stack and the
 * stdcall argument convention. `int 0x80` is its only gate into the kernel;
 * EAX carries an index into the WOW API table and the arguments sit on the
 * user stack. The IDT gate is a 64-bit interrupt gate, so the trap frame is
 * still the kernel's normal 64-bit one and iretq returns to 32-bit code.
 */

#ifndef NOVUS_WOW32_H
#define NOVUS_WOW32_H

#include "kernel.h"
#include "idt.h"

/* Fixed addresses, all below 4 GiB so the compatibility stack and pointers
 * are representable in 32 bits. */
#define WOW32_THUNK_VA   0x60010000ULL
#define WOW32_TEB_VA     0x60020000ULL
#define WOW32_ARGV_VA    0x70010000ULL
#define WOW32_STACK_TOP  0x80000000ULL
#define WOW32_STACK_SIZE 0x00020000ULL
#define WOW32_THUNK_LEN  16u
#define WOW32_MAX_APIS   256u

/* Called by trap_dispatch for the int 0x80 marker. */
void wow32_syscall(trap_frame_t *f);

/* Fill `out` with a 32-bit stdcall thunk for API index `api`:
 *   mov eax, api ; int 0x80 ; ret (nargs*4)
 * Exactly WOW32_THUNK_LEN bytes are written. */
void wow32_thunk_bytes(UINT32 api, UINT32 nargs, UINT8 out[WOW32_THUNK_LEN]);

/* Map "<dll>!<func>" to an index in the WOW API table. Also reports the
 * stdcall byte count and the Win32 API id used by kernel/win32.c.
 * Returns -1 for an import we know nothing about. */
int wow32_api_lookup(const char *dll, const char *func, UINT32 *nargs,
                     UINT32 *win32_id, UINT8 *direct);

/* Address the IAT of a PE32 image must store for API index `api`. */
UINT64 wow32_api_entry(UINT32 api);

#endif /* NOVUS_WOW32_H */
