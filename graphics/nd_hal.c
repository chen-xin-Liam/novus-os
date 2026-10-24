/**
 * Novus OS - NovusDisplay hardware layer (M5).
 *
 * README 11.1 asks for one interface over the display hardware. Here the
 * hardware is a UEFI linear framebuffer: the boot stage reads the current
 * Graphics Output Protocol mode before ExitBootServices (the only time the
 * protocol can be called) and leaves the numbers in `g_disp`; the kernel
 * stage binds a surface straight to that physical range through the low
 * 4 GiB identity map. Nothing is copied per frame.
 *
 * If the firmware offered no usable linear framebuffer the layer reports
 * "none" and the VGA text console (vga.c) stays the only output - the
 * fallback the spec asks for. Boot messages always go there as well, so a
 * display failure never hides the reason.
 */

#include "kernel.h"
#include "mm.h"
#include "nd.h"
#include <stddef.h>

#define ND_FB_MAX_WIDTH  8192u
#define ND_FB_MAX_HEIGHT 4320u
#define ND_LOW_4G       0x100000000ULL

#define ND_CURSOR_W 12
#define ND_CURSOR_H 20

/* .data, not .bss: kernel_entry zeroes .bss on the way into kmain, which
 * would erase exactly the mode the boot stage captured a moment earlier.
 * The section attribute keeps the linker from moving a zero-initialised
 * object back into the zeroed range. */
static nd_display_t g_disp __attribute__((section(".data")));
static nd_surface_t g_screen;
static int          g_screen_ready;

static INT32  g_cx;
static INT32  g_cy;
static int    g_cursor_on;
static UINT32 g_save[ND_CURSOR_W * ND_CURSOR_H];

/* The pointer shape: X = black outline, W = white body, . = transparent. */
static const char *const g_cursor_shape[ND_CURSOR_H] = {
    "X...........",
    "XX..........",
    "XWX.........",
    "XWWX........",
    "XWWWX.......",
    "XWWWWX......",
    "XWWWWWX.....",
    "XWWWWWWX....",
    "XWWWWWWWX...",
    "XWWWWWWWWX..",
    "XWWWWWWWWWX.",
    "XWWWWWWXXXXX",
    "XWWWXWWX....",
    "XWWX.XWWX...",
    "XWX..XWWX...",
    "XX....XWWX..",
    "X......XWWX.",
    "........XWWX",
    ".........XWX",
    "..........X.",
};

/* ---- boot stage ------------------------------------------------------- */

/* Every graphics output the firmware offers. The boot stage fills this
 * in and the kernel stage reads it after ExitBootServices, so it lives in
 * .data: the kernel's entry point clears .bss, not .data. */
static nd_output_t g_outputs[ND_MAX_OUTPUTS] __attribute__((section(".data")));
static UINT32 g_output_count __attribute__((section(".data")));

/* Can the renderer draw into this pixel format directly? */
static int mode_drawable(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info,
                         UINT32 *bgr_out) {
  if (info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
    *bgr_out = 1u;
  } else if (info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) {
    *bgr_out = 0u;
  } else if (info->PixelFormat == PixelBitMask) {
    /* Most GOPs expose a 32-bit XRGB/XBGR bitmask whose component order we
     * use to pick BGR vs RGB so the common cases stay colour-correct. Odd
     * masks are still drawn as 32bpp rather than dropping the whole output. */
    if (info->PixelInformation.RedMask == 0x000000FFu &&
        info->PixelInformation.BlueMask == 0x00FF0000u) {
      *bgr_out = 0u;
    } else {
      *bgr_out = 1u;
    }
  } else {
    return 0;
  }
  return 1;
}

/* What does the display ask for?
 *
 * The EDID block names the preferred timing and whether the link is
 * digital (DP, eDP, HDMI) or analog (VGA). A display without EDID keeps
 * the mode the firmware picked. */
static void edid_preferred(EFI_SYSTEM_TABLE *st, EFI_HANDLE handle,
                           UINT32 *w, UINT32 *h, UINT8 *digital) {
  EFI_GUID                  guid = EFI_EDID_ACTIVE_PROTOCOL_GUID;
  EFI_EDID_ACTIVE_PROTOCOL *edid = NULL;
  UINT8                    *d;

  *w = 0u;
  *h = 0u;
  *digital = 0u;
  if (EFI_ERROR(st->BootServices->HandleProtocol(handle, &guid,
                                                (VOID **)&edid)) ||
      edid == 0 || edid->Data == 0) {
    return;
  }
  d = edid->Data;
  if (d[0] != 0x00u || d[1] != 0xFFu || d[18] == 0x00u) {
    return; /* not an EDID block, or no detailed timing descriptor */
  }
  /* Byte 20, bit 7: 1 = digital input, 0 = analog. */
  *digital = (d[20] & 0x80u) ? 1u : 0u;
  /* The first detailed timing descriptor starts at byte 54. */
  *w = (UINT32)d[56] | ((UINT32)(d[58] & 0xF0u) << 4);
  *h = (UINT32)d[59] | ((UINT32)(d[61] & 0xF0u) << 4);
}

/* Switch the output to the mode the display prefers, when the controller
 * offers one. The firmware's own mode is kept when it does not. */
static void gop_apply_preferred(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, UINT32 pw,
                                UINT32 ph) {
  UINT32 m;

  if (pw == 0u || ph == 0u || gop->QueryMode == 0) {
    return;
  }
  for (m = 0; m < gop->Mode->MaxMode; m++) {
    UINTN                                size = 0;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
    UINT32                               bgr = 0;

    if (EFI_ERROR(gop->QueryMode(gop, m, &size, &info)) || info == 0) {
      continue;
    }
    if (info->HorizontalResolution != pw || info->VerticalResolution != ph) {
      continue;
    }
    if (!mode_drawable(info, &bgr)) {
      continue;
    }
    if (EFI_ERROR(gop->SetMode(gop, m))) {
      continue;
    }
    return;
  }
}

int nd_capture_boot(void *system_table) {
  EFI_SYSTEM_TABLE *st = (EFI_SYSTEM_TABLE *)system_table;
  EFI_GUID          gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
  EFI_HANDLE       *handles = NULL;
  UINTN             n = 0;
  UINTN             i;
  UINT32            count = 0;

  memset(&g_disp, 0, sizeof(g_disp));
  memset(g_outputs, 0, sizeof(g_outputs));
  g_output_count = 0;
  g_disp.backend = "none";
  if (st == 0 || st->BootServices == 0) {
    return 0;
  }

  /* Every graphics output the firmware offers, not just the first one. */
  if (!EFI_ERROR(st->BootServices->LocateHandleBuffer(AllHandles, &gop_guid,
                                                      NULL, &n, &handles)) &&
      handles != 0) {
    for (i = 0; i < n && count < ND_MAX_OUTPUTS; i++) {
      EFI_GRAPHICS_OUTPUT_PROTOCOL         *gop = NULL;
      EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
      nd_output_t *o = &g_outputs[count];
      UINT32       pw = 0;
      UINT32       ph = 0;
      UINT8        digital = 0;
      UINT32       bgr = 0;
      UINT64       size;

      if (EFI_ERROR(st->BootServices->HandleProtocol(handles[i], &gop_guid,
                                                    (VOID **)&gop)) ||
          gop == 0 || gop->Mode == 0 || gop->Mode->Info == 0) {
        continue;
      }
      edid_preferred(st, handles[i], &pw, &ph, &digital);
      gop_apply_preferred(gop, pw, ph);

      info = gop->Mode->Info;
      if (!mode_drawable(info, &bgr)) {
        continue;
      }
      if (info->HorizontalResolution == 0u || info->VerticalResolution == 0u ||
          info->HorizontalResolution > ND_FB_MAX_WIDTH ||
          info->VerticalResolution > ND_FB_MAX_HEIGHT) {
        continue;
      }
      /* A framebuffer above 4 GiB is no longer a reason to drop the output:
       * the kernel maps it on demand through mm_map_phys. Accept it as long
       * as the base is non-zero; store our own byte size for the mapping. */
      size = (UINT64)info->PixelsPerScanLine * (UINT64)info->VerticalResolution *
             4u;
      if (gop->Mode->FrameBufferBase == 0) {
        continue;
      }

      o->available = 1;
      o->bgr = (UINT8)bgr;
      o->digital = digital;
      o->primary = (count == 0u) ? 1u : 0u;
      o->width = info->HorizontalResolution;
      o->height = info->VerticalResolution;
      o->pitch = info->PixelsPerScanLine;
      o->pref_width = pw;
      o->pref_height = ph;
      o->fb_phys = gop->Mode->FrameBufferBase;
      o->fb_size = (UINT32)size;
      count++;
    }
  }
  g_output_count = count;
  if (count == 0u) {
    return 0;
  }

  /* The first output carries the desktop; the others mirror it. */
  {
    nd_output_t *p = &g_outputs[0];

    g_disp.available = 1;
    g_disp.bgr = p->bgr;
    g_disp.width = p->width;
    g_disp.height = p->height;
    g_disp.pitch = p->pitch;
    g_disp.bpp = 32;
    g_disp.fb_phys = p->fb_phys;
    g_disp.fb_size = p->fb_size;
    g_disp.backend = (count > 1u) ? "GOP framebuffer, mirrored"
                                  : "GOP framebuffer";
  }
  return 1;
}

/* ---- kernel stage ----------------------------------------------------- */

/* The other outputs: they show the same picture as the primary one. */
static nd_surface_t g_mirror[ND_MAX_OUTPUTS - 1u];
static UINT32       g_mirror_count;

/* What the settings page shows: one entry per framebuffer, because
 * firmware often lists a second head that points at the very same memory
 * and calling that a display would be a lie. */
static nd_output_t g_reported[ND_MAX_OUTPUTS];
static UINT32      g_reported_count;

int nd_init(void) {
  UINT32 i;

  g_screen_ready = 0;
  memset(&g_screen, 0, sizeof(g_screen));
  memset(g_mirror, 0, sizeof(g_mirror));
  g_mirror_count = 0;
  g_cursor_on = 0;

  if (!g_disp.available) {
    kputs("[ND] no linear framebuffer captured, VGA text stays active\n");
    return 0;
  }
  /* The surface uses pixel-word addressing, while GOP pitch may exceed the
   * visible width; retaining PixelsPerScanLine is necessary to land each
   * scanline at its actual hardware offset. */
  /* The primary surface may sit above 4 GiB; map it through the high virtual
   * region when it does so the renderer can address it directly. */
  {
    void *fb = (g_disp.fb_phys < 0x100000000ULL)
                   ? (void *)(UINTN)g_disp.fb_phys
                   : mm_map_phys(g_disp.fb_phys, g_disp.fb_size, MM_MAP_UC);

    if (fb == NULL) {
      kputs("[ND] primary framebuffer unmapable above 4 GiB\n");
      g_disp.available = 0;
      return 0;
    }
    g_screen.px = (UINT32 *)fb;
  }
  g_screen.w = (INT32)g_disp.width;
  g_screen.h = (INT32)g_disp.height;
  g_screen.stride = g_disp.pitch;
  g_screen.fb = 1;
  g_screen.bgr = g_disp.bgr;
  g_screen.clip = nd_rect(0, 0, g_screen.w, g_screen.h);

  /* Every further output mirrors the primary one, and the reported list
   * keeps one entry per framebuffer: a second head over the same memory is
   * not a display, it is a duplicate the firmware felt like listing. */
  g_reported_count = 0;
  for (i = 0; i < g_output_count && i < ND_MAX_OUTPUTS; i++) {
    UINT32 k;
    int    duplicate = 0;

    for (k = 0; k < g_reported_count; k++) {
      if (g_reported[k].fb_phys == g_outputs[i].fb_phys) {
        duplicate = 1;
        break;
      }
    }
    if (duplicate) {
      continue;
    }
    g_reported[g_reported_count++] = g_outputs[i];
  }
  for (i = 1; i < g_reported_count && i < ND_MAX_OUTPUTS; i++) {
    nd_surface_t *m = &g_mirror[g_mirror_count];
    void         *fb;

    fb = (g_reported[i].fb_phys < 0x100000000ULL)
             ? (void *)(UINTN)g_reported[i].fb_phys
             : mm_map_phys(g_reported[i].fb_phys, g_reported[i].fb_size,
                           MM_MAP_UC);
    if (fb == NULL) {
      /* A mirror whose framebuffer cannot be mapped would fault on the first
       * blit; skip it rather than take the whole display down. */
      kputs("[ND] mirror framebuffer unmapable, skipped\n");
      continue;
    }
    m->px = (UINT32 *)fb;
    m->w = (INT32)g_reported[i].width;
    m->h = (INT32)g_reported[i].height;
    m->stride = g_reported[i].pitch;
    m->fb = 1;
    m->bgr = g_reported[i].bgr;
    m->clip = nd_rect(0, 0, m->w, m->h);
    g_mirror_count++;
  }

  g_screen_ready = 1;
  g_cx = g_screen.w / 2;
  g_cy = g_screen.h / 2;

  kputs("[ND] NovusDisplay: ");
  kputs(g_disp.backend);
  kputs(" ");
  kputs_hex(g_disp.width);
  kputs("x");
  kputs_hex(g_disp.height);
  kputs(", pitch ");
  kputs_hex(g_disp.pitch);
  kputs(", base ");
  kputs_hex(g_disp.fb_phys);
  kputs(g_disp.bgr ? ", BGR\n" : ", RGB\n");

  /* One line per output: what it is, how big, and what it would prefer. */
  for (i = 0; i < g_reported_count; i++) {
    kputs("[ND] output ");
    kputs_hex((UINT64)i);
    kputs(g_reported[i].primary ? " primary" : " mirror");
    kputs(" ");
    kputs_hex(g_reported[i].width);
    kputs("x");
    kputs_hex(g_reported[i].height);
    kputs(g_reported[i].digital ? " digital" : " analog");
    if (g_reported[i].pref_width != 0u) {
      kputs(", preferred ");
      kputs_hex(g_reported[i].pref_width);
      kputs("x");
      kputs_hex(g_reported[i].pref_height);
    }
    kputs("\n");
  }

  nd_clear(&g_screen, ND_RGB(0x10, 0x14, 0x1C));
  return 1;
}

const nd_display_t *nd_display(void) { return &g_disp; }

const nd_output_t *nd_outputs(UINT32 *count) {
  if (count != 0) {
    *count = g_reported_count;
  }
  return g_reported;
}

nd_surface_t *nd_screen(void) { return g_screen_ready ? &g_screen : 0; }

/* Red and blue trade places when the two framebuffers disagree about
 * the byte order. */
static UINT32 mirror_swap(UINT32 px) {
  return (px & 0xFF00FF00u) | ((px & 0x000000FFu) << 16) |
         ((px & 0x00FF0000u) >> 16);
}

/* Present a rectangle of the primary output on every other output. When a
 * mirror matches the primary it is a straight 1:1 clone; otherwise the picture
 * is scaled to fit (preserving aspect ratio and centred, with letterbox
 * margins) so no output loses the desktop regardless of its resolution. The
 * scaling is nearest-neighbour, which is what the boot-stage framebuffer path
 * already relied on for the 1:1 case. */
void nd_mirror_rect(nd_rect_t r) {
  nd_surface_t *src = nd_screen();
  UINT32        i;

  if (src == 0 || g_mirror_count == 0u) {
    return;
  }
  for (i = 0; i < g_mirror_count; i++) {
    nd_surface_t *dst = &g_mirror[i];
    INT32         x0, x1, y0, y1, x, y;

    if (dst->w == src->w && dst->h == src->h) {
      x0 = (r.x > 0) ? r.x : 0;
      y0 = (r.y > 0) ? r.y : 0;
      x1 = r.x + r.w;
      y1 = r.y + r.h;
      if (x1 > dst->w) x1 = dst->w;
      if (y1 > dst->h) y1 = dst->h;
      if (x1 > src->w) x1 = src->w;
      if (y1 > src->h) y1 = src->h;
      for (y = y0; y < y1; y++) {
        const UINT32 *srow = src->px + (UINT32)y * src->stride + (UINT32)x0;
        UINT32       *drow = dst->px + (UINT32)y * dst->stride + (UINT32)x0;
        for (x = x0; x < x1; x++) {
          UINT32 px = srow[x - x0];
          if (src->bgr != dst->bgr) {
            px = mirror_swap(px);
          }
          drow[x - x0] = px;
        }
      }
      continue;
    }
    {
      INT64  scale;
      INT32  off_x, off_y;
      int    full = (r.x <= 0 && r.y <= 0 && r.x + r.w >= src->w &&
                     r.y + r.h >= src->h);

      if ((INT64)dst->h * (INT64)src->w < (INT64)dst->w * (INT64)src->h) {
        scale = (INT64)dst->h * 1000 / (INT64)src->h;
      } else {
        scale = (INT64)dst->w * 1000 / (INT64)src->w;
      }
      off_x = (INT32)(((INT64)dst->w - (INT64)src->w * scale / 1000) / 2);
      off_y = (INT32)(((INT64)dst->h - (INT64)src->h * scale / 1000) / 2);
      /* On a full redraw, clear the letterbox margins so stale pixels do not
       * linger beside the scaled picture. */
      if (full) {
        UINT32 yy, xx;
        for (yy = 0; yy < (UINT32)dst->h; yy++) {
          UINT32 *row = dst->px + (UINT32)yy * dst->stride;
          for (xx = 0; xx < (UINT32)dst->w; xx++) {
            row[xx] = 0xFF10141Cu;
          }
        }
      }
      y0 = off_y + (INT32)((INT64)r.y * scale / 1000);
      y1 = off_y + (INT32)((INT64)(r.y + r.h) * scale / 1000);
      x0 = off_x + (INT32)((INT64)r.x * scale / 1000);
      x1 = off_x + (INT32)((INT64)(r.x + r.w) * scale / 1000);
      if (y0 < 0) y0 = 0;
      if (x0 < 0) x0 = 0;
      if (y1 > dst->h) y1 = dst->h;
      if (x1 > dst->w) x1 = dst->w;
      for (y = y0; y < y1; y++) {
        INT32         sy = (INT32)((INT64)(y - off_y) * 1000 / scale);
        const UINT32 *srow;
        UINT32       *drow;

        if (sy < 0 || sy >= src->h) {
          continue;
        }
        srow = src->px + (UINT32)sy * src->stride;
        drow = dst->px + (UINT32)y * dst->stride;
        for (x = x0; x < x1; x++) {
          INT32  sx = (INT32)((INT64)(x - off_x) * 1000 / scale);
          UINT32 px;

          if (sx < 0 || sx >= src->w) {
            continue;
          }
          px = srow[sx];
          if (src->bgr != dst->bgr) {
            px = mirror_swap(px);
          }
          drow[x] = px;
        }
      }
    }
  }
}

/* ---- software pointer ------------------------------------------------- */

void nd_cursor_hide(void) {
  INT32 row;

  if (!g_screen_ready || !g_cursor_on) {
    return;
  }
  for (row = 0; row < ND_CURSOR_H; row++) {
    UINT32 *dst = g_screen.px + (UINT32)(g_cy + row) * g_screen.stride +
                  (UINT32)g_cx;
    const UINT32 *src = g_save + row * ND_CURSOR_W;
    INT32 col;
    for (col = 0; col < ND_CURSOR_W; col++) {
      dst[col] = src[col];
    }
  }
  g_cursor_on = 0;
}

/* The pointer position is its top-left corner: draw/hide copy a fixed
 * 12-by-20 save-under rectangle, so the whole rectangle must remain inside
 * the framebuffer at every edge. */
void nd_cursor_set(INT32 x, INT32 y) {
  if (!g_screen_ready) {
    return;
  }
  nd_cursor_hide();
  if (x < 0) {
    x = 0;
  }
  if (y < 0) {
    y = 0;
  }
  if (x > g_screen.w - ND_CURSOR_W) {
    x = g_screen.w - ND_CURSOR_W;
  }
  if (y > g_screen.h - ND_CURSOR_H) {
    y = g_screen.h - ND_CURSOR_H;
  }
  g_cx = x;
  g_cy = y;
}

void nd_cursor_get(INT32 *x, INT32 *y) {
  if (x != 0) {
    *x = g_cx;
  }
  if (y != 0) {
    *y = g_cy;
  }
}

void nd_cursor_draw(void) {
  INT32 row;

  if (!g_screen_ready || g_cursor_on) {
    return;
  }
  /* Save-under first: the cursor is painted straight onto the screen, so
   * the pixels it covers have to survive until the next move. */
  for (row = 0; row < ND_CURSOR_H; row++) {
    UINT32 *src = g_screen.px + (UINT32)(g_cy + row) * g_screen.stride +
                  (UINT32)g_cx;
    UINT32 *dst = g_save + row * ND_CURSOR_W;
    INT32 col;
    for (col = 0; col < ND_CURSOR_W; col++) {
      dst[col] = src[col];
    }
  }
  for (row = 0; row < ND_CURSOR_H; row++) {
    const char *line = g_cursor_shape[row];
    INT32 col;
    for (col = 0; col < ND_CURSOR_W; col++) {
      if (line[col] == 'X') {
        nd_pixel(&g_screen, g_cx + col, g_cy + row, ND_RGB(0, 0, 0));
      } else if (line[col] == 'W') {
        nd_pixel(&g_screen, g_cx + col, g_cy + row,
                 ND_RGB(0xE8, 0xEC, 0xF2));
      }
    }
  }
  g_cursor_on = 1;
}
