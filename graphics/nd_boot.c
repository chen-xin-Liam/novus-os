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
#include "klog.h"
#include "boot_logo.h"   /* baked fallback mark */
#include "nd_oemlogo.h"  /* the firmware's own logo, when captured */

#define SPLASH_TOP    0x0A0F17u
#define SPLASH_BOTTOM 0x131C2Bu
#define SPLASH_TEXT   0xE8EEF8u
#define SPLASH_DIM    0x9AA6BAu

#define SPLASH_STATUS_H  44  /* the one status line's band */
#define SPLASH_LABEL_MAX 56

/* The boot log rides on the splash as a translucent panel between the logo
 * and the status line. It is drawn from the same ring the kernel keeps, so
 * the firmware stage's lines are already on screen when splash first paints. */
#define SPLASH_LOG_MAX_W    900
#define SPLASH_LOG_MAX_H    320
#define SPLASH_LOG_LINE_H   16
#define SPLASH_LOG_MAX_LINES 20
#define SPLASH_LOG_SNAP     4096
#define SPLASH_LOG_PAD      10

/* The progress rail sits between the mark and the status line. It is a
 * thin capsule rather than a filled rectangle band: at 6 px it reads as a
 * line of light under the logo, which is what the boot screen wants, and
 * it leaves the two thirds of the panel around it dark. */
#define SPLASH_BAR_H     6
#define SPLASH_BAR_MIN_W 220
#define SPLASH_BAR_MAX_W 520
#define SPLASH_BAR_TRACK 0x1E2A3Cu
#define SPLASH_BAR_FILL  0x5B9BFFu

/* .data, not .bss: the boot stage paints the splash before ExitBootServices
 * and kernel_entry zeroes .bss on its way into kmain, which would otherwise
 * make the kernel believe there is no splash and clear the screen. The
 * framebuffer keeps the pixels; this keeps the state that goes with them. */
static struct {
  UINT8        active;
  UINT8        percent;  /* bar fill, monotonic across both halves of boot */
  UINT8        has_mark; /* 1 = a mark (vendor or baked) is on the glass */
  char         label[SPLASH_LABEL_MAX];
  nd_rect_t    mark;
  nd_rect_t    logo;    /* where the vendor logo sits, when there is one */
  nd_rect_t    bar;     /* the progress rail */
  nd_rect_t    status;
  nd_rect_t    log;     /* boot-log panel, between logo and status */
  UINT32       log_seen; /* klog version the panel was last painted with */
  nd_surface_t scratch; /* used only before nd_init() */
} g_splash __attribute__((section(".data")));

/* Allocated in .bss: kernel_entry clears it on the way in, and the first
 * kernel-stage nd_boot_stage() repaints the panel from the inherited ring. */
static char  g_log_snap[SPLASH_LOG_SNAP];
static char *g_log_lines[SPLASH_LOG_MAX_LINES];

static void splash_status(nd_surface_t *s);

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

static void splash_log_layout(nd_surface_t *s);

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
    g_splash.bar = g_splash.mark;
    g_splash.status = nd_rect(0, 0, s->w, 0);
    splash_log_layout(s);
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
  {
    /* The rail is sized from the panel, not from the logo: a wide panel
     * gets a line long enough to read the last few percent on, a small one
     * keeps a margin on both sides. It sits 18 px above the status band so
     * the label and the bar read as one block. */
    INT32 bar_w = (s->w * 32) / 100;
    INT32 bar_y = status_y - SPLASH_BAR_H - 18;

    if (bar_w > SPLASH_BAR_MAX_W) {
      bar_w = SPLASH_BAR_MAX_W;
    }
    if (bar_w < SPLASH_BAR_MIN_W) {
      bar_w = SPLASH_BAR_MIN_W;
    }
    if (bar_w > s->w - 96) {
      bar_w = s->w - 96;
    }
    if (bar_y < logo_top + lh + 16) {
      bar_y = logo_top + lh + 16;
    }
    if (bar_w < 48 || bar_y + SPLASH_BAR_H > s->h) {
      g_splash.bar = nd_rect(0, 0, 0, 0);
    } else {
      g_splash.bar = nd_rect((s->w - bar_w) / 2, bar_y, bar_w, SPLASH_BAR_H);
    }
  }
  splash_log_layout(s);
}

/* The log panel is an overlay in the literal sense: the logo and the status
 * line keep their rectangles, and the log fills the gap between them. It is
 * bottom-aligned so the newest line sits just above the status band, the way
 * a console scrolls. */
static void splash_log_layout(nd_surface_t *s) {
  INT32 logo_bottom = (g_splash.logo.h > 0)
                          ? g_splash.logo.y + g_splash.logo.h
                          : (s->h * 20) / 100;
  INT32 top = logo_bottom + 16;
  INT32 bottom = (g_splash.status.h > 0) ? g_splash.status.y - 12 : s->h - 24;
  INT32 h = bottom - top;
  INT32 w = s->w - 96;

  if (h > SPLASH_LOG_MAX_H) {
    h = SPLASH_LOG_MAX_H;
  }
  if (w > SPLASH_LOG_MAX_W) {
    w = SPLASH_LOG_MAX_W;
  }
  if (h < SPLASH_LOG_LINE_H * 2 || w < 240) {
    g_splash.log = nd_rect(0, 0, 0, 0);
    return;
  }
  g_splash.log = nd_rect((s->w - w) / 2, bottom - h, w, h);
}

/* Copy the baked logo. At 1:1 with no fade nd_blit handles clipping and the
 * framebuffer's channel order; otherwise sample it nearest-neighbour and
 * blend, so the animation can scale and fade the same picture without a
 * scratch buffer (there is no heap when the timeline runs). */
static void splash_logo_at(nd_surface_t *s, nd_rect_t r, UINT8 alpha) {
  nd_rect_t    clip;
  nd_surface_t img;

  if (r.w <= 0 || r.h <= 0 || alpha == 0u) {
    return;
  }
  /* Remembered for the hand-over: nd_boot_logo_rect() answers from this,
   * so the desktop transition lifts whichever mark the splash actually
   * drew - the firmware's capture or the baked one - and not just when a
   * vendor logo happened to be on the scanout. */
  g_splash.has_mark = 1;
  /* The firmware's own mark wins when the boot stage caught one: the point
   * of showing it is that the screen looks like this machine's boot screen
   * and not like an advertisement for an operating system. */
  if (nd_oem_logo_available()) {
    nd_oem_logo_draw(s, r, alpha);
    return;
  }
  if (r.w == BOOT_LOGO_W && r.h == BOOT_LOGO_H && alpha == 255u) {
    img.px = (UINT32 *)(UINTN)g_boot_logo;
    img.w = BOOT_LOGO_W;
    img.h = BOOT_LOGO_H;
    img.stride = BOOT_LOGO_W;
    img.clip = nd_rect(0, 0, img.w, img.h);
    img.owned = 0;
    img.fb = 0;
    img.bgr = 0;
    img.pad = 0;
    nd_blit(s, r.x, r.y, &img, img.clip);
    return;
  }
  clip = nd_rect_intersect(r, s->clip);
  if (nd_rect_empty(clip)) {
    return;
  }
  {
    INT32 y;

    for (y = clip.y; y < clip.y + clip.h; y++) {
      INT32 sy = ((y - r.y) * BOOT_LOGO_H) / r.h;
      INT32 x;

      if (sy < 0) {
        sy = 0;
      } else if (sy >= BOOT_LOGO_H) {
        sy = BOOT_LOGO_H - 1;
      }
      for (x = clip.x; x < clip.x + clip.w; x++) {
        INT32  sx = ((x - r.x) * BOOT_LOGO_W) / r.w;
        UINT32 sp;
        UINT32 dp;
        UINT8  a;

        if (sx < 0) {
          sx = 0;
        } else if (sx >= BOOT_LOGO_W) {
          sx = BOOT_LOGO_W - 1;
        }
        sp = g_boot_logo[(UINT32)sy * BOOT_LOGO_W + (UINT32)sx];
        a = (UINT8)(sp >> 24);
        if (a == 0u) {
          continue;
        }
        if (alpha != 255u) {
          a = (UINT8)(((UINT32)a * (UINT32)alpha) / 255u);
          if (a == 0u) {
            continue;
          }
        }
        if (a == 255u) {
          nd_pixel(s, x, y, sp);
          continue;
        }
        dp = nd_get_pixel(s, x, y);
        nd_pixel(s, x, y,
                 ND_RGB((ND_RED(sp) * a + ND_RED(dp) * (255u - a)) / 255u,
                        (ND_GREEN(sp) * a + ND_GREEN(dp) * (255u - a)) / 255u,
                        (ND_BLUE(sp) * a + ND_BLUE(dp) * (255u - a)) / 255u));
      }
    }
  }
}

/* The mark at rest, exactly as the desktop transition expects to find it. */
static void splash_logo(nd_surface_t *s) {
  splash_logo_at(s, g_splash.mark, 255);
}

/* The rectangle the mark occupies at `scale` percent of its resting size.
 * Scaling happens about the centre, so the mark grows into place instead of
 * sliding up from a corner. */
static nd_rect_t splash_logo_scaled(UINT32 scale) {
  nd_rect_t r = g_splash.logo;
  INT32     w;
  INT32     h;

  if (r.w <= 0 || r.h <= 0 || scale == 0u) {
    return nd_rect(r.x + r.w / 2, r.y + r.h / 2, 0, 0);
  }
  if (scale > 100u) {
    scale = 100u;
  }
  w = (INT32)(((UINT32)r.w * scale) / 100u);
  h = (INT32)(((UINT32)r.h * scale) / 100u);
  if (w < 1) {
    w = 1;
  }
  if (h < 1) {
    h = 1;
  }
  return nd_rect(r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, w, h);
}

/* The rail, filled to the last whole percent recorded. Drawn with
 * nd_round_rect so both ends are round: a butted 6 px bar reads as a
 * loading widget from 1995. */
static void splash_bar(nd_surface_t *s) {
  nd_rect_t r = g_splash.bar;
  INT32     fill;

  if (r.w <= 0 || r.h <= 0) {
    return;
  }
  nd_round_rect(s, r, r.h / 2, SPLASH_BAR_TRACK);
  fill = (INT32)(((UINT64)r.w * (UINT64)g_splash.percent) / 100u);
  if (fill <= 0) {
    return;
  }
  if (fill >= r.w) {
    fill = r.w;
  }
  nd_round_rect(s, nd_rect(r.x, r.y, fill, r.h),
                (fill < r.h) ? fill / 2 : r.h / 2, SPLASH_BAR_FILL);
}

/* Everything the animation repaints: the mark's whole travel range (the
 * resting rectangle, which every scale fits inside), the rail and the
 * status line. Repainting only this strip is what keeps a fade from
 * rebuilding the backdrop of the entire panel on every frame. */
static nd_rect_t splash_marks_band(nd_surface_t *s) {
  nd_rect_t band = nd_rect(0, 0, 0, 0);

  if (g_splash.logo.w > 0) {
    band = nd_rect_union(band, nd_rect(g_splash.logo.x - 4,
                                       g_splash.logo.y - 4,
                                       g_splash.logo.w + 8,
                                       g_splash.logo.h + 8));
  }
  if (g_splash.bar.w > 0) {
    band = nd_rect_union(band, nd_rect(g_splash.bar.x - 4,
                                       g_splash.bar.y - 6,
                                       g_splash.bar.w + 8,
                                       g_splash.bar.h + 12));
  }
  if (g_splash.status.w > 0) {
    band = nd_rect_union(band, nd_rect(g_splash.status.x,
                                       g_splash.status.y - 4,
                                       g_splash.status.w,
                                       g_splash.status.h + 8));
  }
  return nd_rect_intersect(band, nd_rect(0, 0, s->w, s->h));
}

/* Paint the splash backdrop and lay the empty frame out: rail at zero, no
 * mark, no label. This is the state the timeline fades in from, so the
 * first authored frame never has a full-opacity splash to fall back from. */
static int splash_open(nd_surface_t *s) {
  g_splash.active = 1;
  g_splash.percent = 0;
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
  return 1;
}

/* Start the boot animation: the screen is ours, the frame is empty and
 * every later call to nd_boot_anim_frame() paints on top of it. Same
 * contract as nd_boot_splash() otherwise, so the boot stage can pick
 * either one and only branch on the return value. */
int nd_boot_anim_begin(void) {
  nd_surface_t *s;

  if (g_splash.active) {
    return 1;
  }
  s = splash_surface();
  if (s == 0) {
    return 0;
  }
  if (!splash_open(s)) {
    return 0;
  }
  splash_bar(s);
  return 1;
}

void nd_boot_anim_frame(UINT32 logo_scale, UINT8 logo_alpha, UINT32 bar,
                        const char *label) {
  nd_surface_t *s;
  UINT32        i;

  if (!g_splash.active) {
    return;
  }
  s = splash_surface();
  if (s == 0) {
    return;
  }
  if (bar > 100u) {
    bar = 100u;
  }
  /* The bar never goes backwards: a stage that reports a smaller number
   * than the previous one is a caller bug, and stepping back would read as
   * the boot restarting. */
  if (bar > (UINT32)g_splash.percent) {
    g_splash.percent = (UINT8)bar;
  }
  if (label != 0 && label[0] != '\0') {
    for (i = 0; i + 1u < SPLASH_LABEL_MAX && label[i] != '\0'; i++) {
      g_splash.label[i] = label[i];
    }
    g_splash.label[i] = '\0';
  }
  nd_surface_reset_clip(s);
  splash_backdrop(s, splash_marks_band(s));
  splash_logo_at(s, splash_logo_scaled(logo_scale), logo_alpha);
  splash_bar(s);
  splash_status(s);
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

/* One line of the ring becomes one row of the panel. Lines are split in
 * place in the static snapshot; only the last few are kept, so a long boot
 * still shows the part that matters without a scrolling control. */
static UINT32 splash_log_lines(void) {
  UINT32 n = klog_snapshot(g_log_snap, (UINT32)sizeof(g_log_snap));
  UINT32 count = 0u;
  UINT32 i;
  UINT32 start = 0u;

  for (i = 0u; i < n; i++) {
    if (g_log_snap[i] == '\n') {
      g_log_snap[i] = '\0';
      if (count < SPLASH_LOG_MAX_LINES) {
        g_log_lines[count++] = &g_log_snap[start];
      } else {
        UINT32 k;
        for (k = 0u; k + 1u < SPLASH_LOG_MAX_LINES; k++) {
          g_log_lines[k] = g_log_lines[k + 1u];
        }
        g_log_lines[SPLASH_LOG_MAX_LINES - 1u] = &g_log_snap[start];
      }
      start = i + 1u;
    }
  }
  if (start < n && g_log_snap[start] != '\0') {
    if (count < SPLASH_LOG_MAX_LINES) {
      g_log_lines[count++] = &g_log_snap[start];
    } else {
      UINT32 k;
      for (k = 0u; k + 1u < SPLASH_LOG_MAX_LINES; k++) {
        g_log_lines[k] = g_log_lines[k + 1u];
      }
      g_log_lines[SPLASH_LOG_MAX_LINES - 1u] = &g_log_snap[start];
    }
  }
  return count;
}

/* Repaint just the log rectangle. The backdrop is rebuilt before the panel
 * and its text, so no old glyph survives underneath; nothing outside the
 * rectangle is touched, which is what keeps the splash from flickering. */
static void splash_log_paint(nd_surface_t *s) {
  nd_rect_t r = g_splash.log;
  UINT32    count;
  UINT32    visible;
  UINT32    i;

  if (r.w <= 0 || r.h <= 0) {
    return;
  }
  g_splash.log_seen = klog_version();
  count = splash_log_lines();
  visible = (UINT32)((r.h - 2 * SPLASH_LOG_PAD) / SPLASH_LOG_LINE_H);
  if (visible > count) {
    visible = count;
  }
  if (visible > SPLASH_LOG_MAX_LINES) {
    visible = SPLASH_LOG_MAX_LINES;
  }

  splash_backdrop(s, r);
  nd_blend_rect(s, r, ND_RGB(0x05, 0x0A, 0x12), 176u);
  nd_frame(s, r, 1, ND_RGB(0x2C, 0x40, 0x58));
  for (i = 0u; i < visible; i++) {
    const char *line = g_log_lines[count - visible + i];
    INT32       y = r.y + r.h - SPLASH_LOG_PAD -
                    (INT32)((visible - i) * SPLASH_LOG_LINE_H);

    nd_text_limit(s, r.x + SPLASH_LOG_PAD, y, line,
                  (UINT32)(r.w - 2 * SPLASH_LOG_PAD), SPLASH_DIM, 1u);
  }
}

void nd_boot_splash_end(void) { g_splash.active = 0; }

/* Where the splash put the machine's logo. The desktop transition lifts
 * the same pixels from the same place, so the handover reads as one
 * continuous movement rather than as two screens that happen to share a
 * colour. Answers for both marks: the firmware's capture, and the baked
 * one on machines that had no vendor logo to capture. */
int nd_boot_logo_rect(nd_rect_t *out) {
  if (out == 0 || !g_splash.has_mark || g_splash.mark.w <= 0) {
    return 0;
  }
  *out = g_splash.mark;
  return 1;
}

/* Draw the splash's own mark into any surface, at any rectangle, with a
 * fade. The desktop transition needs exactly this: the mark the user has
 * been looking at, lifted off the closed splash and dissolved over the
 * finished desktop. Same picture and same sampler as the splash uses, so
 * the first transition frame matches the last splash frame pixel for pixel
 * whatever the machine booted with. */
void nd_boot_logo_draw(nd_surface_t *s, nd_rect_t r, UINT8 alpha) {
  if (s == 0) {
    return;
  }
  splash_logo_at(s, r, alpha);
}

/* The closing move of the boot animation: dim the boot screen to black
 * while the mark stays put, so the desktop transition opens from a frame it
 * can reproduce exactly instead of cutting the bar and the stage line away
 * in one step. Runs in the kernel, right before nd_boot_transition(). */
void nd_boot_anim_close(void) {
  nd_surface_t *s = splash_surface();
  const INT32   steps = 5;
  INT32         i;

  if (!g_splash.active || s == 0) {
    return;
  }
  nd_surface_reset_clip(s);
  for (i = 1; i <= steps; i++) {
    UINT8 veil = (UINT8)((255 * i) / steps);

    nd_blend_rect(s, nd_rect(0, 0, s->w, s->h), ND_RGB(0, 0, 0), veil);
    /* Over the dimmed backdrop, opaque: the screen the transition starts
     * from is black with the machine's mark on it. */
    if (g_splash.has_mark) {
      splash_logo_at(s, g_splash.mark, 255);
    }
    /* One line per step so the hand-over is on the record with the same
     * detail as the UEFI frames: a smoke run can then check that the splash
     * dimmed in steps instead of cutting to black. */
    kputs("[BA] close step=");
    kputs_hex((UINT64)i);
    kputs(" veil=");
    kputs_hex((UINT64)veil);
    kputs(" mark=opaque\n");
    nd_wait_ms(24);
  }
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
  if (!splash_open(s)) {
    return 0;
  }
  splash_logo(s);
  /* The firmware stage's lines are already in the handoff ring, so the very
   * first splash frame carries the whole boot log collected so far. */
  splash_log_paint(s);
  splash_bar(s);
  return 1;
}

void nd_boot_stage(UINT32 percent, const char *text) {
  nd_surface_t *s;
  nd_rect_t     dirty;
  int           changed = 0;
  UINT32        i;

  if (!g_splash.active) {
    return;
  }
  /* The percentage drives the rail and nothing else: the label below it is
   * the whole of the progress report, and a per-mille counter next to it
   * would be a number to watch instead of a boot to wait out. */
  if (percent > 100u) {
    percent = 100u;
  }
  if (percent > (UINT32)g_splash.percent) {
    g_splash.percent = (UINT8)percent;
    changed = 1;
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
  if (!changed && klog_version() == g_splash.log_seen) {
    return;
  }
  s = splash_surface();
  if (s == 0) {
    return;
  }
  /* The kernel reopens the surface on the real screen after nd_init(); the
   * layout is cheap, so recompute it rather than trusting rectangles that
   * were measured against the boot stage's view of the panel. */
  splash_layout(s);
  nd_surface_reset_clip(s);
  /* Only the rail, the status band and the log panel are touched; the lockup
   * above them is already correct on the glass, and rebuilding it on every
   * stage change would be the flicker this screen exists to avoid. */
  dirty = nd_rect_union(g_splash.bar, g_splash.status);
  dirty = nd_rect(dirty.x, dirty.y - 8, dirty.w, dirty.h + 16);
  splash_backdrop(s, dirty);
  if (changed) {
    splash_bar(s);
    splash_status(s);
  }
  if (klog_version() != g_splash.log_seen) {
    splash_log_paint(s);
  }
}
