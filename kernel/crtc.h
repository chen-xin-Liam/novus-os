/**
 * Novus OS - CRTC / scanout controller abstraction (Mini-DWM display base).
 *
 * One crtc_t per physical output reported by NovusDisplay (nd_outputs()).
 * The layer owns the Present queue, the VSync source, the hardware/software
 * cursor state and full-screen exclusive ownership. It sits between the
 * compositor's finished frame and the scanout: a present takes a vidmem
 * handle, waits for the vertical blank, and blits the object's canonical
 * ARGB pixels into the target output.
 *
 * Today the hardware is a GOP framebuffer (no programmable CRTC yet), so the
 * "flip" is a bounded row-wise copy through nd_crtc_blit() and the VSync is
 * the VGA 0x3DA probe with a timer fallback. A real display driver can later
 * replace those two paths without changing the API above them.
 */

#ifndef NOVUS_CRTC_H
#define NOVUS_CRTC_H

#include "uefi.h"

#define CRTC_MAX_OUTPUTS 4u

/* Display topology. CLONE sends one composed frame to every output; EXTEND
 * gives each output its own composition target. */
#define CRTC_MODE_CLONE  0u
#define CRTC_MODE_EXTEND 1u

typedef struct {
  UINT32 index;       /* 0 = primary */
  UINT8  available;
  UINT8  primary;
  UINT8  enabled;
  UINT8  pad;
  UINT32 width;
  UINT32 height;
  /* Full-screen exclusive lease: owner==0 means compositor owns the output. */
  UINT64 exclusive_owner;
  UINT32 exclusive_handle;
  /* Software cursor position (hardware cursor unit slots in here later). */
  INT32  cursor_x;
  INT32  cursor_y;
  UINT8  cursor_visible;
  UINT8  pad2[3];
} crtc_t;

void crtc_init(void);

UINT32    crtc_count(void);
crtc_t   *crtc_get(UINT32 index);
UINT32    crtc_mode(void);
void      crtc_set_mode(UINT32 mode);

/* Wait for the vertical blank on `index`. Returns 1 when synchronised. */
int crtc_wait_vsync(UINT32 index);

/* Scan out a canonical ARGB buffer (a vidmem object) at (x,y) for `owner`
 * (a task pid; 0 = the compositor). In CLONE mode the rectangle is blitted
 * to every enabled output the caller may drive.
 *
 * Exclusive mode: a leased output accepts presents ONLY from its owner (the
 * game drives that scanout directly, bypassing the compositor); the
 * compositor's presents to that output are refused. Returns 0 on success,
 * -1 for a bad index/handle or an ownership violation. */
int crtc_present(UINT32 index, UINT32 handle, UINT32 stride,
                 INT32 x, INT32 y, UINT32 w, UINT32 h, UINT64 owner);

/* Take/release a full-screen exclusive lease on `index`. handle==0 releases.
 * Only the current owner (or the compositor when unowned) may release. */
int crtc_set_exclusive(UINT32 index, UINT32 handle, UINT64 owner);

/* Position/hide the cursor on one output (software overlay fallback). */
void crtc_set_cursor(UINT32 index, INT32 x, INT32 y, int visible);

/* Task-exit hook: drop its exclusive leases and cursor references. */
void crtc_owner_exit(UINT64 owner);

/* Bounded self test. 0 on success. */
int crtc_selftest(void);

#endif /* NOVUS_CRTC_H */
