/**
 * Novus OS - Mini-DWM display syscall ABI (shared kernel / user contract).
 *
 * Syscall numbers live in the 28-39 free range of trap.c (1-27, 40=win32,
 * 60=exit are taken). Only handles and small descriptors cross this boundary:
 * pixels stay in vidmem objects mapped into both address spaces, so a Present
 * never copies a framebuffer through a syscall argument buffer.
 *
 *  28 vmem_alloc(size, flags)          -> handle (0 = error)
 *  29 vmem_free(handle)                -> 0 / -1   (drops caller's refs)
 *  30 vmem_map(handle, va, prot)       -> 0 / -1   (maps caller's space)
 *  31 vmem_grant(handle, pid)          -> 0 / -1   (owner only)
 *  32 surface_present(crtc, layers, n) -> 0 / -1
 *  33 wait_vsync(crtc)                 -> 1 synced / 0 no source / -1 bad
 *  34 set_exclusive(crtc, handle)      -> 0 / -1
 *  35 set_cursor(crtc, x, y, visible)  -> 0 / -1
 *  36 dwm_query(buf, cap)              -> bytes written / -1
 *  37 set_mode(mode)                   -> 0 / -1
 *
 * A Win32-ABI task is confined to SYS_WIN32 by the dispatcher, so it cannot
 * reach any of these.
 */

#ifndef NOVUS_DWMSYS_H
#define NOVUS_DWMSYS_H

#include "uefi.h"
#include "idt.h"

void dwm_sys_vmem_alloc(trap_frame_t *f);
void dwm_sys_vmem_free(trap_frame_t *f);
void dwm_sys_vmem_map(trap_frame_t *f);
void dwm_sys_vmem_grant(trap_frame_t *f);
void dwm_sys_surface_present(trap_frame_t *f);
void dwm_sys_wait_vsync(trap_frame_t *f);
void dwm_sys_set_exclusive(trap_frame_t *f);
void dwm_sys_set_cursor(trap_frame_t *f);
void dwm_sys_set_mode(trap_frame_t *f);
void dwm_sys_query(trap_frame_t *f);

#define SYS_VMEM_ALLOC       28u
#define SYS_VMEM_FREE        29u
#define SYS_VMEM_MAP         30u
#define SYS_VMEM_GRANT       31u
#define SYS_SURFACE_PRESENT  32u
#define SYS_WAIT_VSYNC       33u
#define SYS_SET_EXCLUSIVE    34u
#define SYS_SET_CURSOR       35u
#define SYS_DWM_QUERY        36u
#define SYS_SET_MODE         37u

#define DWM_LAYER_FLIP       0u  /* layer pixels come from a shared object */
#define DWM_LAYER_BLT        1u  /* layer was copied into the object by flt */

/* One composited layer. The compositor usually submits a single layer: the
 * fully composed frame. Multiple layers are blitted in array order. */
typedef struct {
  UINT32 handle;
  UINT32 flags;
  INT32  x;
  INT32  y;
  UINT32 w;
  UINT32 h;
} dwm_layer_t; /* 24 bytes */

#define DWM_QUERY_MAX_OUTPUTS 4u

typedef struct {
  UINT32 mode;                       /* CRTC_MODE_CLONE / EXTEND */
  UINT32 count;                      /* number of valid entries below */
  struct {
    UINT32 width;
    UINT32 height;
    UINT8  primary;
    UINT8  available;
    UINT8  pad[2];
  } out[DWM_QUERY_MAX_OUTPUTS];
} dwm_query_t;

#endif /* NOVUS_DWMSYS_H */
