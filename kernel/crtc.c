/**
 * Novus OS - CRTC / scanout controller abstraction (Mini-DWM display base).
 *
 * See crtc.h. The module is deliberately a thin, allocation-free layer over
 * NovusDisplay: outputs come from nd_outputs() at init, presents blit through
 * nd_crtc_blit(), and VSync comes from nd_wait_vsync(). Full-screen exclusive
 * leases and the cursor are bookkeeping the compositor queries through the
 * syscall ABI added in the next step.
 */

#include "kernel.h"
#include "crtc.h"
#include "vidmem.h"
#include "nd.h"
#include "mm.h"

static crtc_t  g_crtc[CRTC_MAX_OUTPUTS];
static UINT32  g_count;
static UINT32  g_mode = CRTC_MODE_CLONE;

void crtc_init(void) {
  const nd_output_t *outs;
  UINT32 count = 0u;
  UINT32 i;

  memset(g_crtc, 0, sizeof(g_crtc));
  g_count = 0u;
  outs = nd_outputs(&count);
  for (i = 0; i < count && i < CRTC_MAX_OUTPUTS; i++) {
    crtc_t *c = &g_crtc[g_count];
    c->index     = g_count;
    c->available = outs[i].available;
    c->primary   = outs[i].primary;
    c->enabled   = outs[i].available;
    c->width     = outs[i].width;
    c->height    = outs[i].height;
    g_count++;
  }
  kputs("[CRTC] outputs=");
  kputs_hex(g_count);
  kputs(" mode=clone\n");
}

UINT32 crtc_count(void) { return g_count; }

crtc_t *crtc_get(UINT32 index) {
  return (index < g_count) ? &g_crtc[index] : 0;
}

UINT32 crtc_mode(void) { return g_mode; }

void crtc_set_mode(UINT32 mode) {
  g_mode = (mode == CRTC_MODE_EXTEND) ? CRTC_MODE_EXTEND : CRTC_MODE_CLONE;
}

int crtc_wait_vsync(UINT32 index) {
  if (index >= g_count || !g_crtc[index].enabled) {
    return 0;
  }
  return nd_wait_vsync();
}

static int blit_object(UINT32 index, vidmem_handle_t handle, INT32 x, INT32 y,
                       UINT32 w, UINT32 height) {
  vidmem_obj_t *o = vidmem_get(handle);
  UINT32        stride;

  if (o == 0) {
    return -1;
  }
  if (x < 0 || y < 0) {
    return -1;
  }
  if ((UINT32)x >= g_crtc[index].width || (UINT32)y >= g_crtc[index].height) {
    return 0;
  }
  if (w > g_crtc[index].width - (UINT32)x) {
    w = g_crtc[index].width - (UINT32)x;
  }
  if (height > g_crtc[index].height - (UINT32)y) {
    height = g_crtc[index].height - (UINT32)y;
  }
  /* The object's bytes are a packed ARGB bitmap; its stride in pixels is the
   * width requested at allocation time (the compositor renders row-major w). */
  stride = (o->size / 4u) ? (o->size / 4u) : w;
  if (stride < w) {
    stride = w;
  }
  nd_crtc_blit(index, (const UINT32 *)phys_to_virt(o->phys), stride,
               x, y, w, height);
  return 0;
}

int crtc_present(UINT32 index, UINT32 handle, INT32 x, INT32 y,
                 UINT32 w, UINT32 h, UINT64 owner) {
  UINT32 i;
  int rc;
  crtc_t *c;

  if (index >= g_count || handle == 0u) {
    return -1;
  }
  c = &g_crtc[index];

  /* Exclusive lease: only the owner may drive this output. The game presents
   * straight to its leased CRTC - the compositor is excluded below - which is
   * the low-latency path that bypasses the compositor entirely. */
  if (c->exclusive_owner != 0u) {
    if (c->exclusive_owner != owner) {
      return -1;
    }
    (void)crtc_wait_vsync(index);
    return blit_object(index, handle, x, y, w, h);
  }

  /* A compositor (owner 0) present must not touch a leased output even in
   * clone mode; the owner is rendering there. */
  (void)crtc_wait_vsync(index);

  if (g_mode == CRTC_MODE_CLONE) {
    for (i = 0; i < g_count; i++) {
      crtc_t *d = &g_crtc[i];
      if (!d->enabled || d->exclusive_owner != 0u) {
        continue;
      }
      rc = blit_object(i, handle, x, y, w, h);
      if (rc != 0) {
        return rc;
      }
    }
    return 0;
  }

  /* Extend: only the addressed output receives the frame. */
  if (!c->enabled) {
    return -1;
  }
  return blit_object(index, handle, x, y, w, h);
}

int crtc_set_exclusive(UINT32 index, UINT32 handle, UINT64 owner) {
  crtc_t *c;

  if (index >= g_count) {
    return -1;
  }
  c = &g_crtc[index];
  if (handle == 0u) {
    /* Release: only the owner may give the output back. */
    if (c->exclusive_owner != 0u && c->exclusive_owner != owner) {
      return -1;
    }
    c->exclusive_owner  = 0u;
    c->exclusive_handle = 0u;
    kputs("[CRTC] output ");
    kputs_hex(index);
    kputs(" exclusive released\n");
    return 0;
  }
  if (c->exclusive_owner != 0u && c->exclusive_owner != owner) {
    return -1;
  }
  if (vidmem_get(handle) == 0) {
    return -1;
  }
  c->exclusive_owner  = owner;
  c->exclusive_handle = handle;
  kputs("[CRTC] output ");
  kputs_hex(index);
  kputs(" exclusive acquired by ");
  kputs_hex(owner);
  kputs("\n");
  return 0;
}

void crtc_set_cursor(UINT32 index, INT32 x, INT32 y, int visible) {
  crtc_t *c;

  if (index >= g_count) {
    return;
  }
  c = &g_crtc[index];
  c->cursor_x = x;
  c->cursor_y = y;
  c->cursor_visible = (UINT8)(visible ? 1u : 0u);
  /* The hardware cursor unit does not exist yet on this GOP framebuffer, so
   * the primary output uses NovusDisplay's software overlay. Mirrors follow
   * on the next present. A real cursor plane plugs in here without changing
   * the syscall ABI above. */
  if (index == 0u) {
    if (visible) {
      nd_cursor_set(x, y);
    } else {
      nd_cursor_hide();
    }
  }
}

void crtc_owner_exit(UINT64 owner) {
  UINT32 i;

  for (i = 0; i < g_count; i++) {
    if (g_crtc[i].exclusive_owner == owner) {
      g_crtc[i].exclusive_owner  = 0u;
      g_crtc[i].exclusive_handle = 0u;
      g_crtc[i].cursor_visible   = 0u;
    }
  }
}

int crtc_selftest(void) {
  int ok = 1;

  if (g_count == 0u) {
    kputs("[CRTC] selftest SKIP: no outputs\n");
    return 0;
  }
  if (crtc_get(0) == 0 || !crtc_get(0)->primary) {
    ok = 0;
    kputs("[CRTC] selftest FAIL: primary output missing\n");
  }
  if (crtc_present(0xffffu, 1u, 0, 0, 1u, 1u, 0u) == 0) {
    ok = 0;
    kputs("[CRTC] selftest FAIL: bad index accepted\n");
  }
  if (crtc_set_exclusive(0, 0xffffu, 42u) == 0) {
    ok = 0;
    kputs("[CRTC] selftest FAIL: stale handle leased\n");
  }
  if (crtc_set_exclusive(0xffffu, 1u, 42u) == 0) {
    ok = 0;
    kputs("[CRTC] selftest FAIL: bad index leased\n");
  }
  kputs(ok ? "[CRTC] selftest PASS\n" : "[CRTC] selftest FAIL\n");
  return ok ? 0 : -1;
}
