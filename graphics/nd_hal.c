/**
 * Novus OS - NovusDisplay hardware layer (M5).
 *
 * README 11.1 asks for one interface over the display hardware. Here the
 * hardware is a UEFI linear framebuffer: the boot stage reads the current
 * Graphics Output Protocol mode before ExitBootServices (the only time the
 * protocol can be called) and leaves the numbers in `g_disp`; the kernel
 * stage binds a surface straight to that physical range through the low
 * 4 GiB identity map.
 *
 * Presentation works the way a compositor on Windows does: the scene is
 * drawn into an off-screen buffer (nd_frame) and the finished rectangle is
 * copied to the glass once (nd_present). Two properties fall out of that:
 *
 *   - no intermediate state is ever visible, so dragging a window or
 *     rebuilding the shell after a theme change does not flicker;
 *   - the copy is one row-wise blit that can be started inside the
 *     display's vertical blank, so it does not cross the scanout and
 *     tear. The blank comes from the VGA status port when the firmware
 *     drives a VGA-compatible CRTC; when it does not, the blit still runs,
 *     just without the phase reference.
 *
 * The software pointer is an overlay on top of that present, not a
 * save-under patch of the framebuffer. Save-under forces the caller to
 * hide the arrow before every draw and put it back afterwards - the arrow
 * blinks for as long as the paint takes - and its saved pixels go stale as
 * soon as anything repaints the area underneath.
 *
 * If the firmware offered no usable linear framebuffer the layer reports
 * "none" and the VGA text console (vga.c) stays the only output - the
 * fallback the spec asks for. Boot messages always go there as well, so a
 * display failure never hides the reason.
 */

#include "kernel.h"
#include "mm.h"
#include "cpu.h"
#include "nd.h"
#include <stddef.h>

#define ND_FB_MAX_WIDTH  8192u
#define ND_FB_MAX_HEIGHT 4320u
#define ND_LOW_4G       0x100000000ULL

#define ND_CURSOR_W 12
#define ND_CURSOR_H 20

/* VGA input status register 1: bit 3 is set while the CRTC is in vertical
 * retrace, i.e. while the display is not reading the framebuffer. */
#define ND_VGA_STATUS 0x3DAu
#define ND_VGA_VRETRACE 0x08u
/* How long a present may wait for the blank. This runs in the timer tick
 * with interrupts off, so a whole frame (~16 ms) would stall input and
 * serial; a fraction of one only shifts the copy into the blank when the
 * blank is about to start anyway. Tearing is not fully eliminated without
 * a hardware flip or a vblank interrupt - what this buys is that the
 * common large present (a dragged window, a theme switch) lands inside
 * the blank whenever its phase allows, and the row-wise blit that
 * follows is short enough to stay there. */
#define ND_BLANK_SPINS   800u
/* Presents smaller than this skip the wait. Kept above the cursor/icon size so
 * latency-sensitive pointer moves are not delayed, but low enough that the
 * common window drag repaint (a few hundred squared pixels) lands in the blank.
 * ~362x362 pixels. */
#define ND_BLANK_MIN_AREA 0x20000

/* .data, not .bss: kernel_entry zeroes .bss on the way into kmain, which
 * would erase exactly the mode the boot stage captured a moment earlier.
 * The section attribute keeps the linker from moving a zero-initialised
 * object back into the zeroed range. */
static nd_display_t g_disp __attribute__((section(".data")));
static nd_surface_t g_screen;
static int          g_screen_ready;

/* Compositor buffer. Allocated in nd_init() from the kernel heap; when that
 * fails the stack keeps working, one direct draw at a time, and says so. */
static nd_surface_t g_frame;
static int          g_frame_ok;

/* Vertical retrace probe: 0 not yet run, 1 usable, 2 not usable. */
static UINT8 g_retrace;

/* Map a framebuffer uncacheable and return a pointer to its first pixel.
 * mm_map_phys() accepts any physical address but aligns the mapping to a 2
 * MiB boundary, so the offset between fb_phys and that boundary is added
 * back: otherwise pixel (0,0) would land up to 2 MiB into the device. */
static void *nd_map_framebuffer(UINT64 phys, UINT64 size) {
  void *base;

  if (size == 0u) {
    size = 1u;
  }
  base = mm_map_phys(phys, size, MM_MAP_UC);
  if (base == 0) {
    return 0;
  }
  return (void *)((char *)base + (phys & 0x1FFFFFull));
}

static INT32     g_cx;
static INT32     g_cy;
static int       g_cursor_visible;
static nd_rect_t g_cursor_damage;

static void present_probe(void);

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
  g_frame_ok = 0;
  memset(&g_screen, 0, sizeof(g_screen));
  memset(&g_frame, 0, sizeof(g_frame));
  memset(g_mirror, 0, sizeof(g_mirror));
  g_mirror_count = 0;
  g_cursor_visible = 0;
  g_cursor_damage = nd_rect(0, 0, 0, 0);

  if (!g_disp.available) {
    kputs("[ND] no linear framebuffer captured, VGA text stays active\n");
    return 0;
  }
  /* The surface uses pixel-word addressing, while GOP pitch may exceed the
   * visible width; retaining PixelsPerScanLine is necessary to land each
   * scanline at its actual hardware offset.
   *
   * The framebuffer is device memory and must be accessed uncacheable: a
   * cacheable alias lets the CPU hold present writes in its cache while the
   * display reads the same pages uncached, so the screen shows stale or
   * torn pixels. QEMU (TCG) ignores PAT/PCD and looks fine either way, which
   * is exactly why this bug is invisible in the emulator and only shows on
   * real hardware. Map it through mm_map_phys() unconditionally - it works
   * for both low and high addresses - and flush any stale cache lines the
   * firmware/early splash left behind. */
  {
    void   *fb = nd_map_framebuffer(g_disp.fb_phys, g_disp.fb_size);

    if (fb == NULL) {
      kputs("[ND] primary framebuffer unmapable\n");
      g_disp.available = 0;
      return 0;
    }
    g_screen.px = (UINT32 *)fb;
    cpu_wbinvd(); /* drop WB data the boot splash wrote through the identity map */
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

    fb = nd_map_framebuffer(g_reported[i].fb_phys, g_reported[i].fb_size);
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

  /* One compositor buffer for the whole display stack. Everything below
   * this line - the shell, the WM, client windows - draws into it and
   * reaches the glass only through nd_present(). A failed allocation is
   * not fatal: the stack falls back to the old direct path. */
  g_frame_ok = (nd_surface_create(&g_frame, g_screen.w, g_screen.h) == 0);

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

  /* Probe the retrace source once. Firmware that drives a VGA-compatible
   * CRTC toggles the bit within a frame; one that does not leaves it
   * constant, which is exactly the answer we need to decide whether to
   * phase presents at all. */
  present_probe();

  kputs("[ND] compositor: ");
  kputs(g_frame_ok ? "off-screen buffer" : "direct to framebuffer");
  kputs(", vsync ");
  kputs(g_retrace == 1u ? "available\n" : "unavailable\n");

  if (nd_boot_splash_active()) {
    /* The boot progress bar is on the glass; leave it there until the
     * desktop has a finished first frame. */
    kputs("[ND] boot splash kept until the desktop presents\n");
  } else {
    nd_clear(nd_canvas(), ND_RGB(0x10, 0x14, 0x1C));
    nd_present(nd_rect(0, 0, g_screen.w, g_screen.h));
  }
  return 1;
}

const nd_display_t *nd_display(void) { return &g_disp; }

nd_surface_t *nd_canvas(void) { return g_frame_ok ? &g_frame : &g_screen; }

int nd_canvas_buffered(void) { return g_frame_ok; }

int nd_present_vsync(void) { return g_retrace == 1u; }

/* ---- presentation ----------------------------------------------------- */

/* Does this surface store red and blue swapped relative to 0xAARRGGBB?
 * Only a framebuffer can: off-screen surfaces always hold the canonical
 * word, which is what lets them be copied with memcpy. */
static int surface_swapped(const nd_surface_t *s) {
  return s->fb != 0 && s->bgr == 0;
}

static UINT32 swap_rb(UINT32 px) {
  return (px & 0xFF00FF00u) | ((px & 0x000000FFu) << 16) |
         ((px & 0x00FF0000u) >> 16);
}

/* Copy `r` between two surfaces that share a coordinate system, one whole
 * row at a time. The row copy is a memcpy whenever the byte orders agree,
 * which is every GOP mode that names BGR - the common case, and the
 * difference between a present that fits inside the blank and one that
 * does not. */
static void surface_copy(nd_surface_t *dst, const nd_surface_t *src,
                         nd_rect_t r) {
  INT32  y;
  UINT64 bytes = (UINT64)r.w * 4u;
  int    swap = (surface_swapped(dst) != surface_swapped(src));

  for (y = 0; y < r.h; y++) {
    const UINT32 *s = src->px + (UINT32)(r.y + y) * src->stride + (UINT32)r.x;
    UINT32       *d = dst->px + (UINT32)(r.y + y) * dst->stride + (UINT32)r.x;

    if (!swap) {
      memcpy(d, s, bytes);
    } else {
      INT32 x;
      for (x = 0; x < r.w; x++) {
        d[x] = swap_rb(s[x]);
      }
    }
  }
}

static void present_probe(void) {
  if (g_retrace != 0u) {
    return;
  }
  {
    UINT32 i;
    UINT8  seen = 0;

    for (i = 0; i < 20000u && seen != 3u; i++) {
      seen |= (UINT8)(1u << ((io_in8(ND_VGA_STATUS) & ND_VGA_VRETRACE) ? 1u
                                                                     : 0u));
    }
    g_retrace = (seen == 3u) ? 1u : 2u;
  }
}

/* Enter the blanking interval if it is about to start. Bounded on purpose:
 * the alternative - sleeping until the next frame - would stall everything
 * that runs off the same timer for up to 16 ms. */
static void present_wait_blank(nd_rect_t r) {
  UINT32 spins;

  if (g_retrace != 1u) {
    return;
  }
  if ((INT64)r.w * (INT64)r.h < ND_BLANK_MIN_AREA) {
    return;
  }
  if ((io_in8(ND_VGA_STATUS) & ND_VGA_VRETRACE) != 0u) {
    return;
  }
  for (spins = 0; spins < ND_BLANK_SPINS; spins++) {
    if ((io_in8(ND_VGA_STATUS) & ND_VGA_VRETRACE) != 0u) {
      return;
    }
    __asm__ volatile("pause");
  }
}

/* One pixel straight into a framebuffer, ignoring the scissor: the pointer
 * overlay has to be paintable on any output, and its coordinates were
 * clamped before it got here. */
static void fb_pixel(nd_surface_t *s, INT32 x, INT32 y, nd_color_t c) {
  if (x < 0 || y < 0 || x >= s->w || y >= s->h) {
    return;
  }
  s->px[(UINT32)y * s->stride + (UINT32)x] = nd_hw_pixel(s, c);
}

/* Where a rectangle of `src` lands on `dst`: 1:1 with no offset when the
 * two agree, otherwise scaled to fit (aspect preserved) and centred, with
 * letterbox margins. Shared by the mirror blit and the pointer overlay so
 * the arrow sits on the same pixel the primary shows. */
typedef struct {
  INT64 scale; /* 1000 = one destination pixel per source pixel */
  INT32 off_x;
  INT32 off_y;
} nd_map_t;

static nd_map_t map_of(const nd_surface_t *src, const nd_surface_t *dst) {
  nd_map_t m;

  m.scale = 1000;
  m.off_x = 0;
  m.off_y = 0;
  if (dst->w != src->w || dst->h != src->h) {
    if ((INT64)dst->h * (INT64)src->w < (INT64)dst->w * (INT64)src->h) {
      m.scale = (INT64)dst->h * 1000 / (INT64)src->h;
    } else {
      m.scale = (INT64)dst->w * 1000 / (INT64)src->w;
    }
    m.off_x = (INT32)(((INT64)dst->w - (INT64)src->w * m.scale / 1000) / 2);
    m.off_y = (INT32)(((INT64)dst->h - (INT64)src->h * m.scale / 1000) / 2);
  }
  return m;
}

static void cursor_paint(nd_surface_t *dst, INT32 x, INT32 y) {
  INT32 row;

  for (row = 0; row < ND_CURSOR_H; row++) {
    const char *line = g_cursor_shape[row];
    INT32       col;

    for (col = 0; col < ND_CURSOR_W; col++) {
      if (line[col] == 'X') {
        fb_pixel(dst, x + col, y + row, ND_RGB(0, 0, 0));
      } else if (line[col] == 'W') {
        fb_pixel(dst, x + col, y + row, ND_RGB(0xE8, 0xEC, 0xF2));
      }
    }
  }
}

/* The pointer rides on top of every present, on every output. Drawing it
 * here rather than at the end of an input handler is what removes the
 * blink: the arrow is part of the frame, not a patch that has to be
 * removed and reapplied around it. */
static void cursor_overlay(void) {
  UINT32 i;

  if (!g_cursor_visible) {
    return;
  }
  cursor_paint(&g_screen, g_cx, g_cy);
  for (i = 0; i < g_mirror_count; i++) {
    nd_surface_t *dst = &g_mirror[i];
    nd_map_t      m = map_of(&g_screen, dst);
    INT32         x = m.off_x + (INT32)((INT64)g_cx * m.scale / 1000);
    INT32         y = m.off_y + (INT32)((INT64)g_cy * m.scale / 1000);

    cursor_paint(dst, x, y);
  }
}

void nd_present(nd_rect_t r) {
  nd_rect_t area;

  if (!g_screen_ready || g_screen.px == 0) {
    return;
  }
  area = nd_rect_intersect(r, nd_rect(0, 0, g_screen.w, g_screen.h));
  if (nd_rect_empty(area)) {
    return;
  }
  present_wait_blank(area);
  if (g_frame_ok) {
    surface_copy(&g_screen, &g_frame, area);
  }
  /* The mirrors are refreshed from the composed buffer, not from the
   * primary: reading the primary back would mean reading uncached MMIO a
   * pixel at a time, and it would copy whatever the scanout happens to
   * hold halfway through the blit above. */
  nd_mirror_rect(area);
  cursor_overlay();
}

const nd_output_t *nd_outputs(UINT32 *count) {
  if (count != 0) {
    *count = g_reported_count;
  }
  return g_reported;
}

nd_surface_t *nd_screen(void) { return g_screen_ready ? &g_screen : 0; }

/* Present a rectangle of the composed frame on every other output. A
 * mirror that matches the primary is a straight row-wise clone (the same
 * memcpy path as the primary present); one that does not is scaled to fit
 * with the aspect ratio preserved, centred and letterboxed. The source is
 * always nd_canvas(): the primary's scanout is uncached memory and only
 * half-written while a present is running. */
void nd_mirror_rect(nd_rect_t r) {
  nd_surface_t *src = nd_canvas();
  UINT32        i;

  if (src == 0 || src->px == 0 || g_mirror_count == 0u) {
    return;
  }
  for (i = 0; i < g_mirror_count; i++) {
    nd_surface_t *dst = &g_mirror[i];
    nd_map_t      m = map_of(src, dst);
    INT32         x0, x1, y0, y1, x, y;

    if (m.scale == 1000) {
      x0 = (r.x > 0) ? r.x : 0;
      y0 = (r.y > 0) ? r.y : 0;
      x1 = r.x + r.w;
      y1 = r.y + r.h;
      if (x1 > dst->w) x1 = dst->w;
      if (y1 > dst->h) y1 = dst->h;
      if (x1 > src->w) x1 = src->w;
      if (y1 > src->h) y1 = src->h;
      surface_copy(dst, src, nd_rect(x0, y0, x1 - x0, y1 - y0));
      continue;
    }
    {
      int   full = (r.x <= 0 && r.y <= 0 && r.x + r.w >= src->w &&
                    r.y + r.h >= src->h);
      int   swap = (surface_swapped(src) != surface_swapped(dst));

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
      y0 = m.off_y + (INT32)((INT64)r.y * m.scale / 1000);
      y1 = m.off_y + (INT32)((INT64)(r.y + r.h) * m.scale / 1000);
      x0 = m.off_x + (INT32)((INT64)r.x * m.scale / 1000);
      x1 = m.off_x + (INT32)((INT64)(r.x + r.w) * m.scale / 1000);
      if (y0 < 0) y0 = 0;
      if (x0 < 0) x0 = 0;
      if (y1 > dst->h) y1 = dst->h;
      if (x1 > dst->w) x1 = dst->w;
      for (y = y0; y < y1; y++) {
        INT32         sy = (INT32)((INT64)(y - m.off_y) * 1000 / m.scale);
        const UINT32 *srow;
        UINT32       *drow;

        if (sy < 0 || sy >= src->h) {
          continue;
        }
        srow = src->px + (UINT32)sy * src->stride;
        drow = dst->px + (UINT32)y * dst->stride;
        for (x = x0; x < x1; x++) {
          INT32  sx = (INT32)((INT64)(x - m.off_x) * 1000 / m.scale);
          UINT32 px;

          if (sx < 0 || sx >= src->w) {
            continue;
          }
          px = srow[sx];
          drow[x] = swap ? swap_rb(px) : px;
        }
      }
    }
  }
}

/* ---- software pointer ------------------------------------------------- */

/* The arrow's rectangle at its current position; the whole of it has to
 * stay inside the framebuffer, which nd_cursor_set() guarantees. */
static nd_rect_t cursor_rect(void) {
  return nd_rect(g_cx, g_cy, ND_CURSOR_W, ND_CURSOR_H);
}

static void cursor_touch(nd_rect_t r) {
  g_cursor_damage = nd_rect_union(g_cursor_damage, r);
}

void nd_cursor_hide(void) {
  if (!g_screen_ready || !g_cursor_visible) {
    return;
  }
  /* Hiding is a composite request, not a pixel operation: the area under
   * the arrow has to be repainted from the real scene, which only the
   * compositor can do. */
  g_cursor_visible = 0;
  cursor_touch(cursor_rect());
}

void nd_cursor_set(INT32 x, INT32 y) {
  if (!g_screen_ready) {
    return;
  }
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
  if (x == g_cx && y == g_cy) {
    return;
  }
  cursor_touch(cursor_rect()); /* where it was: needs the scene back */
  g_cx = x;
  g_cy = y;
  cursor_touch(cursor_rect()); /* where it is now */
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
  if (!g_screen_ready || g_cursor_visible) {
    return;
  }
  /* No damage and no pixels: the arrow is composited on top of the next
   * present, so an appearing pointer costs nothing extra. */
  g_cursor_visible = 1;
}

int nd_cursor_take_damage(nd_rect_t *out) {
  nd_rect_t d = g_cursor_damage;

  if (nd_rect_empty(d)) {
    return 0;
  }
  g_cursor_damage = nd_rect(0, 0, 0, 0);
  if (out != 0) {
    *out = d;
  }
  return 1;
}
