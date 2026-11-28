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
#define SPLASH_LOG_LINE_H   18
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
/* B-52: the progress part sits under the indeterminate comet, so it is a
 * dimmer blue; the comet's tail is dimmer than its head. */
#define SPLASH_BAR_TRACK_FILL 0x2E4570u
#define SPLASH_BAR_SPIN_TAIL  0x3D6FB8u

/* .data, not .bss: the boot stage paints the splash before ExitBootServices
 * and kernel_entry zeroes .bss on its way into kmain, which would otherwise
 * make the kernel believe there is no splash and clear the screen. The
 * framebuffer keeps the pixels; this keeps the state that goes with them. */
static struct {
  UINT8        active;
  UINT8        percent;  /* bar fill, monotonic across both halves of boot */
  UINT8        has_mark; /* 1 = a mark (vendor or baked) is on the glass */
  UINT8        log_owned;/* 1 = the text console owns the log panel */
  char         label[SPLASH_LABEL_MAX];
  nd_rect_t    mark;
  nd_rect_t    logo;    /* where the vendor logo sits, when there is one */
  nd_rect_t    bar;     /* the progress rail */
  nd_rect_t    status;
  nd_rect_t    log;     /* boot-log panel, between logo and status */
  UINT32       log_seen; /* klog version the panel was last painted with */
  /* What is on the glass right now, so a repaint can skip what did not
   * change. The mark in particular is a still picture on a persistent
   * framebuffer: redrawing it because a log line arrived was the single
   * most expensive thing the splash did. */
  UINT32       logo_scale;   /* 0 = the mark is not on the glass */
  UINT8        logo_alpha;   /* its opacity */
  UINT32       bar_pct;      /* rail fill drawn, 0xFFFFFFFF = not drawn yet */
  char         status_seen[SPLASH_LABEL_MAX]; /* label already painted */
  nd_surface_t scratch; /* used only before nd_init() */
} g_splash __attribute__((section(".data")));

/* B-53: kernel-side spin frames painted, see nd_boot_spin_ticks(). */
static UINT32 g_spin_drawn;

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

/* Pure black backdrop. The boot screen's whole job is to look like the
 * machine's own power-on screen: a black field, the board's logo, and the
 * log along the bottom. Anything that shades the field - a gradient, a
 * vignette - reads as an OS advertisement instead of the board's firmware
 * screen, which is exactly what this is trying not to be. */
static void splash_backdrop(nd_surface_t *s, nd_rect_t r) {
  r = nd_rect_intersect(r, nd_rect(0, 0, s->w, s->h));
  if (nd_rect_empty(r)) {
    return;
  }
  /* A machine whose firmware splash is a full-screen picture holds no badge
   * to crop, and cropping it anyway would paste a squashed screenshot into
   * the mark's slot. Using that picture wholesale was tried and removed: it
   * carries whatever the firmware printed before us - POST text, its own
   * console - and re-scaling it puts a ghost of the firmware's screen behind
   * our rail instead of a boot screen. The vendor mark is used when it is
   * recognisable; otherwise the field is black and the splash draws its own
   * mark, which is a clean screen on every machine. */
  (void)nd_oem_frame_available();
  nd_fill_rect(s, r, ND_RGB(0, 0, 0));
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
  logo_top = (s->h * 34) / 100 - lh / 2;
  if (logo_top < 24) {
    logo_top = 24;
  }
  g_splash.mark = nd_rect((s->w - lw) / 2, logo_top, lw, lh);
  g_splash.logo = g_splash.mark;

  /* Bottom block, measured from the bottom edge up: the log panel is what
   * the user asked to have at the bottom, so it owns the edge, and the
   * status line with its rail sits directly above it. */
  splash_log_layout(s);
  {
    INT32 log_top = (g_splash.log.h > 0) ? g_splash.log.y : (s->h - 24);

    status_y = log_top - 10 - SPLASH_STATUS_H;
    if (status_y < logo_top + lh + 24) {
      status_y = logo_top + lh + 24;
    }
  }
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

/* The log panel is anchored to the bottom edge: the newest line sits just
 * above the bottom margin, the way a console scrolls, and the block grows
 * upwards into the space under the logo. The status line is placed after
 * this one, above the panel. */
static void splash_log_layout(nd_surface_t *s) {
  /* The on-screen log is switched off: a cold boot no longer needs a panel
   * narrating itself, the stage line says where the boot is, and the log is
   * kept in the ring for the serial port and the USB copy. */
#define SPLASH_SHOW_LOG 0
#if !SPLASH_SHOW_LOG
  (void)s;
  g_splash.log = nd_rect(0, 0, 0, 0);
  return;
#else
  INT32 w = s->w - 96;

  if (h > SPLASH_LOG_MAX_H) {
    h = SPLASH_LOG_MAX_H;
  }
  if (w > SPLASH_LOG_MAX_W) {
    w = SPLASH_LOG_MAX_W;
  }
  if (h < SPLASH_LOG_LINE_H * 3 || w < 240) {
    g_splash.log = nd_rect(0, 0, 0, 0);
    return;
  }
  g_splash.log = nd_rect((s->w - w) / 2, s->h - 24 - h, w, h);
#endif /* SPLASH_SHOW_LOG */
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
  /* The board's own mark is preferred: whatever the firmware painted on the
   * scanout reads as "this machine starting", which is what a boot screen is
   * for. Machines whose firmware left no mark (or replaced it before we
   * looked) fall back to the baked Novus logo, so the same code covers both. */
#define SPLASH_USE_OEM_LOGO 1
#if SPLASH_USE_OEM_LOGO
  if (nd_oem_logo_available()) {
    nd_oem_logo_draw(s, r, alpha);
    return;
  }
  /* A captured first frame is reported in the boot log but deliberately not
   * used as the splash field: it is the firmware's whole screen, console text
   * and all, and rescaling it reads as a ghost rather than as a boot screen. */
#endif
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

/* B-52: indeterminate motion on the same rail.
 *
 * The last stretch of the boot ("entering the kernel") runs work whose length
 * is not known in advance, so a percentage would lie. Instead a short comet
 * runs along the rail: pure geometry, no bitmap and no glyph, which is also
 * why it costs a handful of rounded rects per frame. The already-filled
 * portion stays drawn underneath, so the boot still shows where it got to.
 * `tick` only has to increase; the caller owns the frame rate. */
void nd_boot_spin(UINT32 tick) {
  nd_surface_t *s;
  nd_rect_t     r = g_splash.bar;
  INT32         seg;
  INT32         span;
  INT32         pos;

  if (!g_splash.active) {
    return;
  }
  s = splash_surface();
  if (s == 0 || r.w <= 0 || r.h <= 0) {
    return;
  }
  nd_surface_reset_clip(s);
  nd_round_rect(s, r, r.h / 2, SPLASH_BAR_TRACK);
  {
    INT32 fill = (INT32)(((UINT64)r.w * (UINT64)g_splash.percent) / 100u);

    if (fill > 0) {
      if (fill > r.w) {
        fill = r.w;
      }
      nd_round_rect(s, nd_rect(r.x, r.y, fill, r.h),
                    (fill < r.h) ? fill / 2 : r.h / 2, SPLASH_BAR_TRACK_FILL);
    }
  }
  seg = r.w / 4;
  if (seg < 12) {
    seg = 12;
  }
  span = r.w - seg;
  if (span < 1) {
    return;
  }
  /* 40 stops per revolution: at the boot's frame rate that is about one sweep
   * per 0.6 s, and even the two or three frames this usually gets to draw
   * move the head far enough to read as motion. */
  pos = (INT32)((tick % 40u) * (UINT32)span / 40u);
  /* Head brightest, tail dim: a single rect would read as a second progress
   * chunk instead of motion. */
  nd_round_rect(s, nd_rect(r.x + pos, r.y, seg, r.h), r.h / 2, SPLASH_BAR_FILL);
  if (pos > 6) {
    nd_round_rect(s, nd_rect(r.x + pos - 6, r.y, 6, r.h), r.h / 2,
                  SPLASH_BAR_SPIN_TAIL);
  }
}

/* B-53: the same comet, driven from the kernel side.
 *
 * Between the boot hand-off and the scheduler the kernel spends seconds inside
 * drivers that block (USB port walks, reset settles, AHCI/NVMe waits) and the
 * rail's percentage does not move at all - the one stretch where a spinner is
 * genuinely the only honest indicator. There is no timer to call us back yet,
 * so instead the drivers' own wait loops tick this: it self-clocks off
 * cpu_uptime_us() and repaints at most every 16 ms, which keeps a 40 kHz poll
 * loop from turning into a 40 kHz redraw. */
void nd_boot_spin_tick(void) {
  static UINT64 last_us;
  static UINT32 tick;
  static UINT32 drawn;
  UINT64        now;

  if (!g_splash.active) {
    last_us = 0u;
    return;
  }
  now = cpu_uptime_us();
  if (last_us != 0u && now >= last_us && (now - last_us) < 16000u) {
    return;
  }
  last_us = now;
  nd_boot_spin(tick++);
  g_spin_drawn = ++drawn;
}

/* How many frames this hook has actually painted. The boot's screenshot cannot
 * prove motion (the compositor hands out stale surfaces), so the count is what
 * a timeline line reports instead. */
UINT32 nd_boot_spin_ticks(void) { return g_spin_drawn; }

/* Repaint only the widgets whose value actually changed. Nothing here touches
 * the mark: the framebuffer holds it until something paints over it, and the
 * only thing that ever paints over it is a fade this function is told about.
 *
 * This used to rebuild the backdrop of the mark's whole travel range and blit
 * the mark again for every log line and every stage, which on a cache-disabled
 * framebuffer showed as the boot screen being rewritten a band at a time. */
static void splash_refresh(nd_surface_t *s, UINT32 logo_scale, UINT8 logo_alpha) {
  if (logo_scale != g_splash.logo_scale || logo_alpha != g_splash.logo_alpha) {
    nd_rect_t oldr = splash_logo_scaled(g_splash.logo_scale);
    nd_rect_t newr = splash_logo_scaled(logo_scale);
    nd_rect_t d = nd_rect_union(oldr, newr);

    /* Erase both rectangles, then lay the mark down once at the new size and
     * opacity. Scaling happens about the centre, so the union is the only
     * region either position can have touched. */
    d = nd_rect(d.x - 2, d.y - 2, d.w + 4, d.h + 4);
    splash_backdrop(s, d);
    g_splash.logo_scale = logo_scale;
    g_splash.logo_alpha = logo_alpha;
    splash_logo_at(s, newr, logo_alpha);
  }
  if (g_splash.bar_pct != (UINT32)g_splash.percent) {
    g_splash.bar_pct = (UINT32)g_splash.percent;
    splash_bar(s);
  }
  {
    UINT32 i;

    for (i = 0u; i + 1u < SPLASH_LABEL_MAX; i++) {
      if (g_splash.status_seen[i] != g_splash.label[i]) {
        break;
      }
    }
    if (i + 1u < SPLASH_LABEL_MAX || g_splash.label[i] != '\0') {
      for (i = 0u; i + 1u < SPLASH_LABEL_MAX; i++) {
        g_splash.status_seen[i] = g_splash.label[i];
        if (g_splash.label[i] == '\0') {
          break;
        }
      }
      splash_backdrop(s, g_splash.status);
      splash_status(s);
    }
  }
}

/* Record the line that is on the glass, so a later stage repaints it only
 * when the text really changes. */
static void splash_status_seen_mark(void) {
  UINT32 i;

  for (i = 0u; i + 1u < SPLASH_LABEL_MAX; i++) {
    g_splash.status_seen[i] = g_splash.label[i];
    if (g_splash.label[i] == '\0') {
      return;
    }
  }
  g_splash.status_seen[SPLASH_LABEL_MAX - 1u] = '\0';
}

/* Paint the splash backdrop and lay the empty frame out: rail at zero, no
 * mark, no label. This is the state the timeline fades in from, so the
 * first authored frame never has a full-opacity splash to fall back from. */
static int splash_open(nd_surface_t *s) {
  g_splash.active = 1;
  g_splash.percent = 0;
  g_splash.label[0] = '\0';
  g_splash.status_seen[0] = '\0';
  g_splash.logo_scale = 0;
  g_splash.logo_alpha = 0;
  g_splash.bar_pct = 0xFFFFFFFFu;
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
  kputs("[ND] firmware logo: ");
  kputs(nd_oem_logo_reason());
  kputs(nd_oem_logo_available() ? " (mark)" : " (no mark)");
  kputs(nd_oem_frame_available() ? ", first frame kept\n"
                                 : ", first frame absent\n");
  splash_bar(s);
  g_splash.bar_pct = (UINT32)g_splash.percent;
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
  /* Only the mark's own scale and opacity here: a frame carrying the same
   * numbers as the last one costs nothing, which is the common case once the
   * intro has landed on its resting size. */
  splash_refresh(s, logo_scale, logo_alpha);
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
  /* Once the kernel's own text console owns this band, the panel must stop
   * repainting it: a snapshot taken from the ring would erase the live
   * console's scrolled text and the two would fight on every stage change. */
  if (g_splash.log_owned) {
    return;
  }
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
                  (UINT32)(r.w - 2 * SPLASH_LOG_PAD), SPLASH_TEXT, 1u);
  }
}

/* Repaint the panel from the ring on demand. The kernel calls this once per
 * finished log line, so the log on the glass is live instead of a snapshot
 * that freezes at the call which hung - which is the whole point of having a
 * log panel on a machine that has no serial port. */
/* A finished log line used to repaint the whole panel straight away. That
 * panel is a band of the real scanout, and on the laptop its repaint (a
 * backdrop fill, a blend and every visible line of text) costs tens of
 * milliseconds per line: the boot log itself became the longest phase of the
 * boot, not the drivers it was reporting. Coalesce instead - five lines or
 * thirty milliseconds, whichever comes first - and let the stage repaints
 * that already exist catch the remainder, so a burst of eighty lines costs
 * about sixteen panel repaints instead of eighty. A machine that hangs can
 * leave at most four finished lines off the glass; the copy on the stick is
 * written per line and still has them. */
#define SPLASH_LOG_REFRESH_US   30000u
#define SPLASH_LOG_MAX_PENDING  5u

void nd_boot_log_refresh(void) {
  static UINT8  g_painting;
  static UINT8  g_pending;
  static UINT64 g_last_us;
  nd_surface_t *s;

  if (!g_splash.active || g_splash.log_owned || g_painting) {
    return;
  }
  s = splash_surface();
  if (s == 0) {
    return;
  }
  if (g_pending < SPLASH_LOG_MAX_PENDING) {
    g_pending++;
  }
  if (g_pending < SPLASH_LOG_MAX_PENDING &&
      (cpu_uptime_us() - g_last_us) < SPLASH_LOG_REFRESH_US) {
    return;
  }
  /* Painting emits log lines of its own (the lazily built 2D backend says
   * so on its first span), and those would call straight back in here. */
  g_painting = 1u;
  splash_log_paint(s);
  g_painting = 0u;
  g_pending = 0u;
  g_last_us = cpu_uptime_us();
}

void nd_boot_splash_end(void) { g_splash.active = 0; }

/* Where the splash put the machine's logo. The desktop transition lifts
 * the same pixels from the same place, so the handover reads as one
 * continuous movement rather than as two screens that happen to share a
 * colour. Answers for both marks: the firmware's capture, and the baked
 * one on machines that had no vendor logo to capture. */
/* Somebody painted over the splash - the pre-desktop menu does, with its own
 * full-screen card. The mark is not on the glass any more, so the next stage
 * repaint lays it back down, once. */
void nd_boot_splash_mark_dirty(void) {
  g_splash.logo_scale = 0;
  g_splash.logo_alpha = 0;
  g_splash.bar_pct = 0xFFFFFFFFu;
  g_splash.status_seen[0] = '\0';
}

/* Which picture the splash took, in three letters: the firmware's own mark
 * (mk), its whole first frame (fw), or the baked fallback (none). The desktop
 * shows this in its status line - one short word is the only on-screen
 * report, and it says at a glance whether the board logo was found. */
const char *nd_boot_logo_tag(void) {
  if (g_splash.mark.w <= 0 && !g_splash.has_mark) {
    return nd_oem_frame_available() ? "fw" : "none";
  }
  return nd_oem_logo_available() ? "mk" : "bak";
}

int nd_boot_logo_rect(nd_rect_t *out) {  if (out == 0 || !g_splash.has_mark || g_splash.mark.w <= 0) {
    return 0;
  }
  *out = g_splash.mark;
  return 1;
}

/* The rectangle the boot-log panel occupies. The kernel's text console takes
 * this band over so the log on the glass is the live kputs() stream rather
 * than a snapshot that stops at the call that hung. */
int nd_boot_log_rect(nd_rect_t *out) {
  if (out == 0 || g_splash.log.w <= 0 || g_splash.log.h <= 0) {
    return 0;
  }
  *out = g_splash.log;
  return 1;
}

/* Announce that the text console owns the panel, so the splash stops painting
 * its snapshot over it. */
void nd_boot_splash_log_off(void) { g_splash.log_owned = 1; }

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
  nd_rect_t     full;
  nd_rect_t     mark;
  INT32         i;

  if (!g_splash.active || s == 0) {
    return;
  }
  nd_surface_reset_clip(s);
  full = nd_rect(0, 0, s->w, s->h);
  mark = g_splash.has_mark ? g_splash.mark : nd_rect(0, 0, 0, 0);

  /* One present takes the field to black, then only the mark's own
   * rectangle is rewritten as it fades. The screen is a cache-disabled
   * mapping on this laptop, so a full-screen write costs real milliseconds
   * and doing it once per fade step is what read as the whole picture being
   * redrawn over and over. */
  nd_fill_rect(s, full, ND_RGB(0, 0, 0));
  nd_present(full);
  nd_wait_ms(18);

  if (mark.w > 0) {
    const INT32 steps = 5;

    for (i = 1; i <= steps; i++) {
      UINT8     alpha = (UINT8)(255u - (255u * (UINT32)i) / (UINT32)steps);
      nd_rect_t r = nd_rect(mark.x - 2, mark.y - 2, mark.w + 4, mark.h + 4);

      nd_fill_rect(s, r, ND_RGB(0, 0, 0));
      splash_logo_at(s, mark, alpha);
      nd_present(r);
      nd_wait_ms(22);
    }
  }
  kputs("[BA] close faded\n");
}

/* One frame of the splash-to-desktop crossfade, drawn over the composed
 * desktop: the splash field at `veil` opacity, with the mark at the same
 * opacity so the two fade as one picture instead of the mark blinking out
 * before the background. Called only while the desktop canvas is being
 * handed over; `veil` 0 draws nothing. */
void nd_boot_handover_frame(nd_surface_t *dst, nd_rect_t r, UINT8 veil) {
  if (dst == 0 || dst->px == 0 || veil == 0u) {
    return;
  }
  if (g_splash.has_mark) {
    nd_blend_rect(dst, r, ND_RGB(0, 0, 0), veil);
    splash_logo_at(dst, g_splash.mark, veil);
  } else {
    nd_blend_rect(dst, r, ND_RGB(0, 0, 0), veil);
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
  /* One line that names which picture the splash ended up with, and why the
   * firmware's mark was refused when it was. A machine that shows no logo
   * has to be diagnosable from a log read on another computer. */
  kputs("[ND] firmware logo: ");
  kputs(nd_oem_logo_reason());
  kputs(nd_oem_logo_available() ? " (mark)" : " (no mark)");
  kputs(nd_oem_frame_available() ? ", first frame kept\n"
                                 : ", first frame absent\n");
  splash_logo(s);
  /* The firmware stage's lines are already in the handoff ring, so the very
   * first splash frame carries the whole boot log collected so far. */
  splash_log_paint(s);
  splash_bar(s);
  g_splash.logo_scale = 100u;
  g_splash.logo_alpha = 255u;
  g_splash.bar_pct = (UINT32)g_splash.percent;
  splash_status_seen_mark();
  return 1;
}

/* The kernel's second visit to the splash. By this point the kernel has
 * bound the framebuffer to its own page tables and the text console has
 * painted a black box over the boot stage's picture, so the old frame is
 * gone. Recompute the layout against the bound surface and paint the whole
 * thing again - black field, the board's mark, the rail, the status line and
 * the bottom log - so the hand-over does not show as a black screen. */
int nd_boot_splash_rebind(void) {
  nd_surface_t *s = splash_surface();

  if (s == 0) {
    return 0;
  }
  if (!g_splash.active) {
    return nd_boot_splash();
  }
  nd_surface_reset_clip(s);
  __asm__ volatile("wbinvd" ::: "memory");
  splash_layout(s);
  splash_backdrop(s, nd_rect(0, 0, s->w, s->h));
  splash_logo(s);
  splash_log_paint(s);
  splash_bar(s);
  splash_status(s);
  g_splash.logo_scale = 100u;
  g_splash.logo_alpha = 255u;
  g_splash.bar_pct = (UINT32)g_splash.percent;
  splash_status_seen_mark();
  return 1;
}

void nd_boot_stage(UINT32 percent, const char *text) {
  nd_surface_t *s;
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
  /* Only the widgets this stage owns are repainted, and only when they
   * changed: the rail if a percentage arrived, the line if its text did, the
   * log panel if a line landed. The full-screen fill this used to do was
   * eight megabytes of uncached stores per log line, which on a real laptop
   * is a band of the screen visibly rewriting itself - one of the two "the
   * boot draws itself line by line" reports. The firmware's leftovers are
   * already gone: splash_open() cleared the whole panel once. */
  splash_refresh(s, g_splash.logo_scale, g_splash.logo_alpha);
  if (klog_version() != g_splash.log_seen) {
    splash_log_paint(s);
  }
}
