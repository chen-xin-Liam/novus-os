/**
 * Novus OS - Novus-WM window manager (M6, README part 10).
 *
 * The WM is the compositor and the IPC server of the desktop: it owns the
 * window list, z-order, focus and decorations, routes pointer and keyboard
 * input to the process that owns the window under the cursor, and answers
 * the protocol in nwm_proto.h on the well-known port "nwm".
 *
 * It runs inside the kernel next to the display stack (like a compositor
 * thread) rather than as a separate process: M6 keeps the driver, the
 * compositor and the server in one place so the IPC protocol - which is
 * what this milestone is about - is the only boundary a client sees.
 */

#ifndef NOVUS_NWM_H
#define NOVUS_NWM_H

#include "uefi.h"

/* Create the WM port, run the protocol self test and log the banner.
 * Call after nd_demo_init() has built the shell scene. */
void nwm_init(void);

/* Timer tick: input routing, IPC server pump, composite. */
void nwm_tick(void);

/* Protocol self test: drives the request handlers with synthetic messages
 * and checks window state, z-order, focus and error paths. */
int nwm_selftest(void);

/* Build a ring-3 client (a hand-assembled NUC program) that talks to the
 * WM over IPC: create a window, then walk it across the desktop and
 * repaint when the WM sends it input. Returns 0 when the task is running. */
int nwm_spawn_client(void);

/* Desktop accent (theme colour) - read by the settings panel. */
UINT32 nwm_accent_get(void);
void   nwm_accent_set(UINT32 rgb);

/* Desktop wallpaper (0..2): the shell's mica gradient pair. */
UINT32 nwm_wallpaper_get(void);
void   nwm_wallpaper_set(UINT32 idx);

/* Windows currently held, for the sysinfo stats reply. */
UINT32 nwm_window_count(void);

#endif /* NOVUS_NWM_H */
