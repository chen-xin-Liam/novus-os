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

/* Restore, raise and focus the topmost window belonging to a running app. */
int nwm_activate_owner(UINT64 owner);

/* Build a ring-3 client (a hand-assembled NUC program) that talks to the
 * WM over IPC: create a window, then walk it across the desktop and
 * repaint when the WM sends it input. Returns 0 when the task is running. */
int nwm_spawn_client(void);

/* Desktop accent (theme colour) - read by the settings panel. */
UINT32 nwm_accent_get(void);
void   nwm_accent_set(UINT32 rgb);

/* Desktop wallpaper (0..6): the embedded photo wallpapers; the theme keeps
 * a matching gradient pair as fallback. */
UINT32 nwm_wallpaper_get(void);
void   nwm_wallpaper_set(UINT32 idx);

/* Light / dark / auto (ND_THEME_*). One entry point so the settings panel
 * and the shell's quick settings cannot drift apart. */
UINT32 nwm_theme_mode(void);
void   nwm_theme_set_mode(UINT32 mode);

/* Windows currently held, for the sysinfo stats reply. */
UINT32 nwm_window_count(void);

/* ---- kernel-callable window API (M9 Win32 compatibility layer) --------
 *
 * A Win32 process is a ring-3 task, not an IPC client: it cannot speak the
 * protocol in nwm_proto.h, because its only syscall is the Win32 gate. So
 * the kernel drives the same request handlers on its behalf, with the
 * process id as the owner - which is what keeps every ownership check,
 * z-order rule and event route working unchanged.
 *
 * These are synchronous and single-threaded: they must be called from the
 * task that owns the window. `owner` is a task pid; `win` is the window id
 * the WM assigned (also what events carry back).
 */
int nwm_owner_create(UINT64 owner, const char *title, INT32 x, INT32 y,
                     INT32 w, INT32 h, UINT32 *win);
int nwm_owner_destroy(UINT64 owner, UINT32 win);
int nwm_owner_clear(UINT64 owner, UINT32 win);
int nwm_owner_shape(UINT64 owner, UINT32 win, UINT8 op, INT16 x, INT16 y,
                    INT16 w, INT16 h, UINT32 color);
int nwm_owner_text(UINT64 owner, UINT32 win, INT16 x, INT16 y, UINT8 scale,
                   const char *text, UINT32 len, UINT32 color);
int nwm_owner_flush(UINT64 owner, UINT32 win);
int nwm_owner_title(UINT64 owner, UINT32 win, const char *title);
/* Client-area size of a window, which is what a Win32 program sizes its
 * painting and its GetClientRect from. Returns 0 when the window is gone. */
int nwm_owner_client(UINT32 win, INT32 *w, INT32 *h);
/* The placed rectangle, frame included. A request that was clamped to stay
 * on screen comes back at the position the compositor really used, so the
 * caller can answer GetWindowRect with the truth rather than with what it
 * asked for. Returns 0 when the window is gone. */
int nwm_owner_rect(UINT32 win, INT32 *x, INT32 *y, INT32 *w, INT32 *h);
/* Frame geometry and keyboard ownership, for MoveWindow and SetFocus. The
 * WM clamps a moved or resized frame back onto the screen and answers with
 * the rectangle it really used, which nwm_owner_rect reports. */
int nwm_owner_move(UINT64 owner, UINT32 win, INT32 x, INT32 y);
int nwm_owner_resize(UINT64 owner, UINT32 win, INT32 w, INT32 h);
int nwm_owner_focus(UINT64 owner, UINT32 win);

#endif /* NOVUS_NWM_H */
