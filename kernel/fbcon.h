/**
 * Novus OS - kernel-stage text console on the firmware framebuffer.
 *
 * kputs() has always mirrored to the VGA text buffer at 0xB8000 and to
 * COM1, which is right for a machine with a legacy display and a serial
 * header and wrong for a laptop that has neither. On such a machine every
 * line after ExitBootServices went into memory nobody looked at: the boot
 * stage's UEFI text stayed on the glass and the kernel appeared to have
 * stopped, with no way to tell which stage it stopped in.
 *
 * This is the fallback for exactly that case. When the graphical splash
 * cannot run - the usual reason being a framebuffer above the 4 GiB the
 * boot stage can reach - the kernel draws its own log onto the framebuffer
 * instead, using the same 5x7 glyphs NovusDisplay uses. The desktop
 * repaints the whole screen when it starts, so the log never survives
 * into a running system.
 */

#ifndef NOVUS_FBCON_H
#define NOVUS_FBCON_H

#include "kernel.h"

/* Take over the captured framebuffer for text. Returns 1 when there is one
 * to take over, 0 when the display stays with VGA/serial. Safe to call
 * before the PMM, the heap or the page tables exist: it only touches the
 * firmware's own mapping. */
int fbcon_begin(void);

/* The console needs no pause around mm_init(): mm_init() keeps the
 * framebuffer identity-mapped, uncached, before it switches CR3, so the
 * raw physical pointer this console draws through stays valid and the log
 * keeps scrolling across the switch. */

/* Give the screen back: everything from here is the desktop's. */
void fbcon_end(void);

/* 1 while the console owns the screen. */
int fbcon_active(void);

/* The surface the console draws through, or NULL when it never started:
 * a raw pointer to the captured framebuffer that the kernel has already
 * proven writable by printing the boot log on it. The display stack uses
 * this instead of building a second mapping for the same scanout. */
void *fbcon_framebuffer(void);

/* Append one string; embedded '\n' starts a new line and the log scrolls
 * once it reaches the bottom of its box. A no-op when inactive. */
void fbcon_puts(const char *text);

#endif /* NOVUS_FBCON_H */
