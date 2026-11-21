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
 * The logo is whichever mark this machine actually has: the firmware's own
 * boot logo when the boot stage caught one, otherwise the baked ARGB array
 * from tools/splash/mkbootlogo.py. The bake exists because there is no PNG
 * decoder and no heap this early in boot; the capture exists because on a
 * laptop the screen should look like that laptop's boot screen. Everything
 * else uses the ordinary renderer (nd_fill_rect/nd_round_rect/nd_text), so
 * there is no separate "boot drawing" path to keep in sync. The only special
 * case is the surface: before nd_init() there is no heap and possibly no high
 * mapping, so the splash binds a static surface straight to the framebuffer
 * the boot stage captured, provided the boot stage's page tables cover it -
 * which nd_capture_boot() establishes by probing the live tables and adding
 * an uncached identity mapping when the firmware left one out.
 *
 * Updates repaint the status line only. Repainting the whole screen on every
 * step would be the very flicker this milestone is about.
 */

#include "kernel.h"
#include "nd.h"
#include "boot_logo.h"   /* baked fallback mark */
#include "nd_oemlogo.h"  /* the firmware's own logo, when captured */

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
  nd_rect_t    logo;    /* where the vendor logo sits, when there is one */
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
  /* What matters is whether the boot stage's page tables actually cover
   * this framebuffer, not where it sits: nd_capture_boot() already asked,
   * and built an uncached identity mapping when the firmware had none. */
  if (d == 0 || !d->available || d->fb_phys == 0 || !d->fb_boot_usable ||
      d->width == 0 || d->height == 0) {
    /* Say why instead of going quiet: a splash that does not appear is
     * otherwise indistinguishable from one that was never attempted. */
    if (d != 0 && d->available && d->fb_phys != 0 && !d->fb_boot_usable) {
      kputs("[ND] boot splash skipped: framebuffer at ");
      kputs_hex(d->fb_phys);
      kputs(" is not mapped by the boot stage\n");
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
  /* Two marks can land here and they have different shapes: the firmware's
   * capture arrives at panel resolution (so it is capped at 22% of the
   * height to keep the status line its room), while the baked fallback is
   * a fixed lockup that may take 45%. Everything after the caps is shared,
   * so both sit in the same place relative to their own panel height. */
  int   oem = nd_oem_logo_available();
  INT32 lw = oem ? nd_oem_logo_width() : BOOT_LOGO_W;
  INT32 lh = oem ? nd_oem_logo_height() : BOOT_LOGO_H;
  INT32 max_w = oem ? (s->w * 62) / 100 : s->w - 64;
  INT32 max_h = oem ? (s->h * 22) / 100 : (s->h * 45) / 100;
  INT32 logo_top;
  INT32 status_y;

  if (oem && max_w < 120) {
    max_w = s->w - 48;
  }
  if (max_w < 32 || max_h < 32 || lw <= 0 || lh <= 0) {
    g_splash.mark = nd_rect(0, 0, 0, 0);
    g_splash.logo = g_splash.mark;
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
  g_splash.logo = g_splash.mark;
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
  /* The firmware's own mark wins when the boot stage caught one: the point
   * of showing it is that the screen looks like this machine's boot screen
   * and not like an advertisement for an operating system. */
  if (nd_oem_logo_available()) {
    nd_oem_logo_draw(s, r, 255);
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

/* The rectangle the logo moves through during the intro: its first frame
 * sits `rise` pixels lower than its resting place, so the band has to
 * cover both and the backdrop under it has to be rebuilt every frame. */
static nd_rect_t splash_logo_band(INT32 rise) {
  nd_rect_t a = g_splash.logo;

  a = nd_rect(a.x - 8, a.y - 8, a.w + 16, a.h + 16 + rise);
  return a;
}

/* Opening move of the splash: the logo fades in as it lifts the last few
 * pixels into place. `delay_ms` is the boot stage's Stall; the kernel
 * passes NULL and the splash simply appears finished, because by then the
 * user has been looking at it for the whole kernel stage anyway. */
void nd_boot_splash_intro(void (*delay_ms)(UINT32)) {
  nd_surface_t *s = splash_surface();
  /* 16 steps at 24 ms: long enough for the eye to read a fade rather than
   * a flash, short enough that the boot stage still feels responsive. The
   * alpha follows an ease-out curve; the old 10 linear steps spent most of
   * their time at near-final opacity and read as a cut. */
  const INT32   frames = 16;
  const INT32   rise = 28;
  INT32         i;

  if (!g_splash.active || s == 0 || !nd_oem_logo_available() || delay_ms == 0) {
    return;
  }
  nd_surface_reset_clip(s);
  for (i = 0; i <= frames; i++) {
    UINT32    p = ((UINT32)i * 1000u) / (UINT32)frames;
    UINT32    q = 1000u - p;
    UINT32    eased = 1000u - (q * q) / 1000u;
    UINT8     fade = (UINT8)((255u * eased) / 1000u);
    INT32     dy = rise - (rise * i) / frames;

    splash_backdrop(s, splash_logo_band(rise));
    nd_oem_logo_draw(s, nd_rect(g_splash.logo.x, g_splash.logo.y + dy,
                                g_splash.logo.w, g_splash.logo.h),
                     (i == 0) ? 0u : fade);
    if (i == 0) {
      continue; /* the empty band is the start state, nothing to hold on */
    }
    delay_ms(24);
  }
  /* Last frame exactly on the resting rect, fully opaque. */
  splash_backdrop(s, splash_logo_band(rise));
  nd_oem_logo_draw(s, g_splash.logo, 255);
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

/* Where the splash put the machine's logo. The desktop transition lifts
 * the same pixels from the same place, so the handover reads as one
 * continuous movement rather than as two screens that happen to share a
 * colour. Returns 0 when there was no captured logo. */
int nd_boot_logo_rect(nd_rect_t *out) {
  if (out == 0 || !nd_oem_logo_available() || g_splash.logo.w <= 0) {
    return 0;
  }
  *out = g_splash.logo;
  return 1;
}


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
  /* The firmware drew its logo and its console through its own cacheable
   * mapping. Those dirty lines can land on the framebuffer after our first
   * fill and ghost through the backdrop - the vendor text sitting on top of
   * the splash is that writeback seen from the other side. Flush once, then
   * paint over a clean surface. */
  __asm__ volatile("wbinvd" ::: "memory");
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
