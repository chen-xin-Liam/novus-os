/**
 * Novus OS - Mini-DWM display syscall implementations.
 *
 * Each handler validates every user pointer through user_copy() and every
 * handle/owner relationship through the vidmem/crtc modules. The task pid is
 * the capability: an app can only free, map or grant objects it owns (or was
 * explicitly granted), and only lease a CRTC for itself.
 */

#include "kernel.h"
#include "idt.h"
#include "sched.h"
#include "vm.h"
#include "vidmem.h"
#include "crtc.h"
#include "dwmsys.h"

#define DWM_MAX_LAYERS 32u

static int get_caller(task_t **out) {
  task_t *t = sched_current();

  if (t == 0) {
    return -1;
  }
  *out = t;
  return 0;
}

/* SYS_VMEM_ALLOC(size, flags) -> handle */
void dwm_sys_vmem_alloc(trap_frame_t *f) {
  task_t *t;
  UINT32  size = (UINT32)f->rdi;
  UINT32  flags = (UINT32)f->rsi;
  UINT32  allowed = VIDMEM_FLAG_WC | VIDMEM_FLAG_SCANOUT;

  if (get_caller(&t) != 0 || size == 0u || (flags & ~allowed) != 0u) {
    f->rax = 0u;
    return;
  }
  f->rax = vidmem_alloc(t->pid, size, flags);
}

/* SYS_VMEM_FREE(handle) -> 0 / -1 */
void dwm_sys_vmem_free(trap_frame_t *f) {
  task_t *t;

  if (get_caller(&t) != 0) {
    f->rax = (UINT64)(INT64)-1;
    return;
  }
  f->rax = (UINT64)(INT64)vidmem_release((vidmem_handle_t)f->rdi, t->pid);
}

/* SYS_VMEM_MAP(handle, va, prot) -> 0 / -1 */
void dwm_sys_vmem_map(trap_frame_t *f) {
  task_t *t;
  UINT64  va = f->rsi;
  UINT32  prot = (UINT32)f->rdx;
  UINT32  allowed = VM_PROT_WRITE | VM_PROT_USER | VM_PROT_EXEC;

  if (get_caller(&t) != 0 || (va & 0xFFFULL) != 0u ||
      (prot & ~allowed) != 0u) {
    f->rax = (UINT64)(INT64)-1;
    return;
  }
  f->rax = (UINT64)(INT64)vidmem_map_user((vidmem_handle_t)f->rdi,
                                           &t->space, va, prot, t->pid);
}

/* SYS_VMEM_GRANT(handle, pid) -> 0 / -1 */
void dwm_sys_vmem_grant(trap_frame_t *f) {
  task_t *t;

  if (get_caller(&t) != 0) {
    f->rax = (UINT64)(INT64)-1;
    return;
  }
  f->rax = (UINT64)(INT64)vidmem_grant((vidmem_handle_t)f->rdi, f->rsi,
                                        t->pid);
}

/* SYS_SURFACE_PRESENT(crtc, layers_va, count) -> 0 / -1 */
void dwm_sys_surface_present(trap_frame_t *f) {
  task_t *t;
  UINT32  crtc = (UINT32)f->rdi;
  UINT32  count = (UINT32)f->rdx;
  UINT64  va = f->rsi;
  UINT32  i;

  if (get_caller(&t) != 0 || crtc >= crtc_count() ||
      count == 0u || count > DWM_MAX_LAYERS) {
    f->rax = (UINT64)(INT64)-1;
    return;
  }
  for (i = 0; i < count; i++) {
    dwm_layer_t layer;

    memset(&layer, 0, sizeof(layer));
    if (user_copy(t->cr3, va + (UINT64)i * sizeof(layer), &layer,
                  sizeof(layer), 0) != 0) {
      f->rax = (UINT64)(INT64)-1;
      return;
    }
    /* Only handles the caller may use are accepted, so a forged handle in a
     * Present cannot scan out another process's pixels. */
    if (!vidmem_can_access(layer.handle, t->pid) ||
        crtc_present(crtc, layer.handle, layer.stride, layer.x, layer.y,
                     layer.w, layer.h, t->pid) != 0) {
      f->rax = (UINT64)(INT64)-1;
      return;
    }
  }
  f->rax = 0u;
}

/* SYS_WAIT_VSYNC(crtc) -> 1 / 0 / -1 */
void dwm_sys_wait_vsync(trap_frame_t *f) {
  UINT32 crtc = (UINT32)f->rdi;

  if (crtc >= crtc_count()) {
    f->rax = (UINT64)(INT64)-1;
    return;
  }
  f->rax = (UINT64)(INT64)crtc_wait_vsync(crtc);
}

/* SYS_SET_EXCLUSIVE(crtc, handle) -> 0 / -1 */
void dwm_sys_set_exclusive(trap_frame_t *f) {
  task_t *t;
  UINT32  crtc = (UINT32)f->rdi;
  UINT32  handle = (UINT32)f->rsi;

  if (get_caller(&t) != 0 || crtc >= crtc_count()) {
    f->rax = (UINT64)(INT64)-1;
    return;
  }
  if (handle != 0u && !vidmem_can_access(handle, t->pid)) {
    f->rax = (UINT64)(INT64)-1;
    return;
  }
  f->rax = (UINT64)(INT64)crtc_set_exclusive(crtc, handle, t->pid);
}

/* SYS_SET_CURSOR(crtc, x, y, visible) -> 0 / -1 */
void dwm_sys_set_cursor(trap_frame_t *f) {
  UINT32 crtc = (UINT32)f->rdi;

  if (crtc >= crtc_count()) {
    f->rax = (UINT64)(INT64)-1;
    return;
  }
  crtc_set_cursor(crtc, (INT32)(UINT64)f->rsi, (INT32)(UINT64)f->rdx,
                  (int)f->r10);
  f->rax = 0u;
}

/* SYS_SET_MODE(mode) -> 0 / -1 */
void dwm_sys_set_mode(trap_frame_t *f) {
  UINT32 mode = (UINT32)f->rdi;

  if (mode != CRTC_MODE_CLONE && mode != CRTC_MODE_EXTEND) {
    f->rax = (UINT64)(INT64)-1;
    return;
  }
  crtc_set_mode(mode);
  f->rax = 0u;
}

/* SYS_DWM_QUERY(buf, cap) -> bytes written / -1 */
void dwm_sys_query(trap_frame_t *f) {
  task_t      *t;
  dwm_query_t  q;
  UINT32       cap = (UINT32)f->rsi;
  UINT32       n;
  UINT32       i;

  if (get_caller(&t) != 0) {
    f->rax = (UINT64)(INT64)-1;
    return;
  }
  memset(&q, 0, sizeof(q));
  q.mode = crtc_mode();
  q.count = crtc_count();
  if (q.count > DWM_QUERY_MAX_OUTPUTS) {
    q.count = DWM_QUERY_MAX_OUTPUTS;
  }
  for (i = 0; i < q.count; i++) {
    crtc_t *c = crtc_get(i);

    q.out[i].width     = c->width;
    q.out[i].height    = c->height;
    q.out[i].primary   = c->primary;
    q.out[i].available = c->available;
  }
  n = sizeof(q);
  if (cap < n) {
    n = cap;
  }
  if (user_copy(t->cr3, f->rdi, &q, n, 1) != 0) {
    f->rax = (UINT64)(INT64)-1;
    return;
  }
  f->rax = n;
}
