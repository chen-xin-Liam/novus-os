/**
 * Novus OS - boot splash.
 *
 * The boot path has two halves: the UEFI stage, which can still read disks,
 * and the kernel stage, which takes over after ExitBootServices. Both show
 * the same screen so the user sees one continuous boot instead of a wall of
 * firmware text followed by a black screen.
 *
 * The composition is deliberately spare: the boot logo from
 * assets/splash/boot.png sits in the upper third, and the single line that
 * matters - the step the boot is on right now - sits centred in the lower
 * fifth. One line, no percentage, no progress bar: a boot screen is
 * something to glance at, and a bar that walks 4, 88, 90, 91 percent in three
 * tenths of a second reads as a stutter rather than as progress.
 *
 * The logo is baked into a plain ARGB array by tools/splash/mkbootlogo.py
 * and copied with nd_blit(); there is no PNG decoder or heap this early in
 * boot. Everything else uses the ordinary renderer (nd_fill_rect/nd_text),
 * so there is no separate "boot drawing" path to keep in sync. The only
 * special case is the surface: before nd_init() there is no heap and possibly
 * no high mapping, so the splash binds a static surface straight to the
 * framebuffer the boot stage captured, as long as it sits in the low 4 GiB.
 *
 * Updates repaint the status line only. Repainting the whole screen on every
 * step would be the very flicker this milestone is about.
 */

#include "kernel.h"
#include "nd.h"
#include "boot_logo.h"

#define SPLASH_TOP    0x0A0F17u
#define SPLASH_BOTTOM 0x131C2Bu
#define SPLASH_TEXT   0xE8EEF8u
#define SPLASH_DIM    0x9AA6BAu

#define SPLASH_STATUS_H  44  /* the one status line's band */
#define SPLASH_LABEL_MAX 56

/* .data, not .bss: the boot stage paints the splash before ExitBootServices
 * and kernel_entry zeroes .bss on its way into kmain, which would otherwise
 * make the kernel believe there is no splash and clear the screen. The
 * framebuffer keeps the pixels; this keeps the state that goes with them. */
static struct {
  UINT8        active;
  char         label[SPLASH_LABEL_MAX];
  nd_rect_t    mark;
  nd_rect_t    status;
  nd_surface_t scratch; /* used only before nd_init() */
} g_splash __attribute__((section(".data")));

/* The surface to draw on: the real screen once the kernel has bound it,
 * otherwise a temporary view of the captured framebuffer. Exported so the
 * boot menu can draw on the same surface the splash uses. */
nd_surface_t *splash_surface(void) {
  nd_surface_t       *s = nd_screen();
  const nd_display_t *d;

  if (s != 0) {
    return s;
  }
  if (g_splash.scratch.px != 0) {
    return &g_splash.scratch;
  }
  d = nd_display();
  if (d == 0 || !d->available || d->fb_phys == 0 ||
      d->fb_phys >= 0x100000000ULL || d->width == 0 || d->height == 0) {
    /* The splash draws before the paging layer exists, so it can only use a
     * framebuffer the boot identity map already covers. On a discrete card
     * the firmware framebuffer lives inside the card's VRAM aperture, which
     * is above 4 GiB: the splash is skipped there and the first thing drawn
     * is the desktop, through the aperture mapping set up later. Say why
     * instead of going quiet. */
    if (d != 0 && d->available && d->fb_phys >= 0x100000000ULL) {
      kputs("[ND] boot splash skipped: framebuffer at ");
      kputs_hex(d->fb_phys);
      kputs(" is above the 4 GiB identity map\n");
    }
    return 0;
  }
  g_splash.scratch.px = (UINT32 *)(UINTN)d->fb_phys;
  g_splash.scratch.w = (INT32)d->width;
  g_splash.scratch.h = (INT32)d->height;
  g_splash.scratch.stride = d->pitch;
  g_splash.scratch.fb = 1;
  g_splash.scratch.bgr = d->bgr;
  g_splash.scratch.clip = nd_rect(0, 0, g_splash.scratch.w,
                                  g_splash.scratch.h);
  return &g_splash.scratch;
}

/* Vertical gradient, recomputed for whichever band has to be repainted.
 * Row by row because the renderer has no gradient-with-clip form and the
 * splash only ever repaints a small band. */
static void splash_backdrop(nd_surface_t *s, nd_rect_t r) {
  INT32 y;

  r = nd_rect_intersect(r, nd_rect(0, 0, s->w, s->h));
  if (nd_rect_empty(r)) {
    return;
  }
  for (y = r.y; y < r.y + r.h; y++) {
    UINT32     t = (s->h > 1) ? ((UINT32)y * 255u) / (UINT32)(s->h - 1) : 0u;
    UINT32     inv = 255u - t;
    nd_color_t c = ND_RGB(
        (ND_RED(SPLASH_TOP) * inv + ND_RED(SPLASH_BOTTOM) * t) / 255u,
        (ND_GREEN(SPLASH_TOP) * inv + ND_GREEN(SPLASH_BOTTOM) * t) / 255u,
        (ND_BLUE(SPLASH_TOP) * inv + ND_BLUE(SPLASH_BOTTOM) * t) / 255u);

    nd_fill_rect(s, nd_rect(r.x, y, r.w, 1), c);
  }
}

/* Upper third: the boot logo, centred and scaled down only when the panel
 * cannot fit it. Lower fifth: the status band. Both are centred horizontally,
 * so a 768- and a 1080-line panel put them in the same places relative to
 * their own height instead of hugging one edge. */
static void splash_layout(nd_surface_t *s) {
  INT32 lw = BOOT_LOGO_W;
  INT32 lh = BOOT_LOGO_H;
  INT32 max_w = s->w - 64;
  INT32 max_h = (s->h * 45) / 100;
  INT32 logo_top;
  INT32 status_y;

  if (max_w < 32 || max_h < 32) {
    g_splash.mark = nd_rect(0, 0, 0, 0);
    g_splash.status = nd_rect(0, 0, s->w, 0);
    return;
  }
  if (lw > max_w) {
    lh = (lh * max_w + lw / 2) / lw;
    lw = max_w;
  }
  if (lh > max_h) {
    lw = (lw * max_h + lh / 2) / lh;
    lh = max_h;
  }
  logo_top = (s->h * 26) / 100 - lh / 2;
  if (logo_top < 24) {
    logo_top = 24;
  }
  status_y = s->h - (s->h / 5) - SPLASH_STATUS_H / 2;
  if (status_y < logo_top + lh + 32) {
    status_y = logo_top + lh + 32;
  }
  g_splash.mark = nd_rect((s->w - lw) / 2, logo_top, lw, lh);
  g_splash.status = nd_rect(0, status_y, s->w, SPLASH_STATUS_H);
}

/* Copy the baked logo. At 1:1 nd_blit handles clipping and the framebuffer's
 * channel order; on a small panel, sample it nearest-neighbour so no scaling
 * table or scratch buffer is needed before the heap exists. */
static void splash_logo(nd_surface_t *s) {
  nd_rect_t  r = g_splash.mark;
  nd_surface_t img;

  if (r.w <= 0 || r.h <= 0) {
    return;
  }
  img.px = (UINT32 *)(UINTN)g_boot_logo;
  img.w = BOOT_LOGO_W;
  img.h = BOOT_LOGO_H;
  img.stride = BOOT_LOGO_W;
  img.clip = nd_rect(0, 0, img.w, img.h);
  img.owned = 0;
  img.fb = 0;
  img.bgr = 0;
  img.pad = 0;

  if (r.w == BOOT_LOGO_W && r.h == BOOT_LOGO_H) {
    nd_blit(s, r.x, r.y, &img, img.clip);
    return;
  }
  {
    nd_rect_t clip = nd_rect_intersect(r, s->clip);
    INT32     x, y;

    for (y = clip.y; y < clip.y + clip.h; y++) {
      INT32 sy = ((y - r.y) * BOOT_LOGO_H) / r.h;
      for (x = clip.x; x < clip.x + clip.w; x++) {
        INT32 sx = ((x - r.x) * BOOT_LOGO_W) / r.w;

        s->px[(UINT32)y * s->stride + (UINT32)x] =
            nd_hw_pixel(s, g_boot_logo[(UINT32)sy * BOOT_LOGO_W + (UINT32)sx]);
      }
    }
  }
}

/* The one line: whatever the boot is doing right now, centred, no
 * percentage. Sized up on a tall panel and shrunk back to 1x if the label is
 * too long to fit - a clipped step name is worse than a small one. */
static void splash_status(nd_surface_t *s) {
  const char *buf = g_splash.label;
  UINT32      scale = (s->h >= 700) ? 2u : 1u;
  UINT32      w;
  INT32       h;

  if (buf[0] == '\0') {
    return;
  }
  w = nd_text_width(buf, scale);
  while (scale > 1u && (INT32)w > s->w - 96) {
    scale--;
    w = nd_text_width(buf, scale);
  }
  h = (INT32)nd_text_height(buf, scale);
  nd_text(s, (s->w - (INT32)w) / 2,
          g_splash.status.y + (SPLASH_STATUS_H - h) / 2, buf, SPLASH_TEXT,
          scale);
}

void nd_boot_splash_end(void) { g_splash.active = 0; }

int nd_boot_splash_active(void) { return g_splash.active != 0; }

int nd_boot_splash(void) {
  nd_surface_t *s;

  if (g_splash.active) {
    return 1;
  }
  s = splash_surface();
  if (s == 0) {
    return 0;
  }
  g_splash.active = 1;
  g_splash.label[0] = '\0';
  splash_layout(s);
  nd_surface_reset_clip(s);
  splash_backdrop(s, nd_rect(0, 0, s->w, s->h));
  splash_logo(s);
  return 1;
}

void nd_boot_stage(UINT32 percent, const char *text) {
  nd_surface_t *s;
  nd_rect_t     dirty;
  int           changed = 0;
  UINT32        i;

  /* The percentage still arrives from the callers that compute it - the boot
   * stage uses it to time each load step - but it is not drawn: the bar went
   * away with the redesign, and this line is the whole of the progress
   * report. */
  (void)percent;

  if (!g_splash.active) {
    return;
  }
  if (text != 0) {
    for (i = 0; i + 1u < SPLASH_LABEL_MAX && text[i] != '\0'; i++) {
      if (g_splash.label[i] != text[i]) {
        break;
      }
    }
    if (text[i] != g_splash.label[i]) {
      for (i = 0; i + 1u < SPLASH_LABEL_MAX && text[i] != '\0'; i++) {
        g_splash.label[i] = text[i];
      }
      g_splash.label[i] = '\0';
      changed = 1;
    }
  }
  if (!changed) {
    return;
  }
  s = splash_surface();
  if (s == 0) {
    return;
  }
  splash_layout(s);
  nd_surface_reset_clip(s);
  /* Only the status band is touched; the lockup above it is already correct
   * on the glass. */
  dirty = g_splash.status;
  dirty = nd_rect(dirty.x, dirty.y - 4, dirty.w, dirty.h + 8);
  splash_backdrop(s, dirty);
  splash_status(s);
}
