/**
 * Novus OS - kernel stop screen (blue screen of death).
 *
 * The stop screen owns the glass directly: it does not go through the
 * scheduler, the IPC window manager or a client display list. Entry points
 * are the ring-0 fault path, the kernel panic landing pad and a hidden key
 * sequence. Once entered the function never returns; the only exit is a
 * reset requested with Ctrl+Alt+R.
 */

#ifndef NOVUS_BSOD_H
#define NOVUS_BSOD_H

#include "uefi.h"
#include "idt.h"

/* Windows-style stop codes. DEBUG.md is the human-readable counterpart. */
#define BSOD_BC_KMODE_EXCEPTION 0x0000001Eu
#define BSOD_BC_PAGE_FAULT      0x00000050u
#define BSOD_BC_IRQL            0x0000000Au
#define BSOD_BC_WHEA            0x00000124u
#define BSOD_BC_BAD_POOL        0x000000C2u
#define BSOD_BC_MANUAL          0x000000E3u

/* Set-1 scancodes the hook understands. USB reports are mapped onto these. */
#define BSOD_SCAN_R        0x13u
#define BSOD_SCAN_TAB      0x0Fu
#define BSOD_SCAN_CTRL     0x1Du
#define BSOD_SCAN_SHIFT_L  0x2Au
#define BSOD_SCAN_SHIFT_R  0x36u
#define BSOD_SCAN_ALT      0x38u

/* Modifier bits, kept in sync with ND_MOD_* without pulling in nd_input.h. */
#define BSOD_MOD_SHIFT 0x01u
#define BSOD_MOD_CTRL  0x02u
#define BSOD_MOD_ALT   0x04u

/* Ring-0 exception: captures the trap frame and never returns. */
void bsod_fault(const trap_frame_t *f)
    __attribute__((noreturn));

/* Explicit stop code (manual trigger, panic hook, etc.): never returns. */
void bsod_manual(UINT32 code) __attribute__((noreturn));

/* Raw PS/2 set-1 scancode, with the break bit already removed. */
void bsod_key_hook(UINT8 scancode, int down);

/* USB boot-protocol path: same scancode space, modifiers supplied by caller. */
void bsod_key_hook_mods(UINT8 scancode, int down, UINT8 mods);

/* Periodic housekeeping: drives the hidden-sequence timeout. */
void bsod_hotkey_tick(void);

/* 1 once the stop screen owns the glass. */
int bsod_active(void);

#endif /* NOVUS_BSOD_H */
