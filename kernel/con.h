/**
 * Novus OS - kernel text console (M11).
 *
 * A minimal framebuffer terminal used when the system boots without the window
 * manager ("no desktop" boot option). It draws text straight to the display
 * surface with the same NovusDisplay primitives the splash and desktop use, and
 * reads the keyboard from the shared input queue (PS/2 or USB HID).
 *
 * The console is deliberately WM-free: no windows, no compositor, no desktop.
 * nsh reaches it through SYS_CONSOLE, the same syscall the serial path used, so
 * the shell engine is unchanged.
 */
#ifndef NOVUS_CON_H
#define NOVUS_CON_H

#include "nd.h"

/* Bind the console to a display surface and clear the screen. Pass NULL when no
 * framebuffer is available; the console then degrades to a serial-only log. */
void con_init(nd_surface_t *s);

/* True once a display surface is bound. */
int con_ready(void);

/* Render a buffer of UTF-8 text bytes to the console (handles \\n, \\r, \\b). */
void con_out(const UINT8 *buf, UINT32 len);

/* Drain pending keyboard bytes into `buf`, non-blocking. Returns the count. */
UINT32 con_in_read(UINT8 *buf, UINT32 len);

#endif /* NOVUS_CON_H */
