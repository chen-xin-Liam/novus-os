/**
 * Novus OS - boot splash and progress bar.
 *
 * The boot path has two halves: the UEFI stage, which can still read disks,
 * and the kernel stage, which takes over after ExitBootServices. Both show
 * the same screen - a mark, the wordmark and a progress bar - so the user
 * sees one continuous boot instead of a wall of firmware text followed by a
 * black screen.
 *
 * It is drawn with the ordinary renderer (nd_fill_rect/nd_text), not with a
 * separate "boot drawing" path: the same primitives, the same glyphs, no
 * second implementation to keep in sync. The only special case is the
 * surface: before nd_init() there is no heap and possibly no high mapping,
 * so the splash binds a static surface straight to the framebuffer the boot
 * stage captured, as long as it sits in the low 4 GiB.
 *
 * Updates repaint the bar and the label only. Repainting the whole screen on
 * every step would be the very flicker this milestone is about.
 */

#include "kernel.h"
#include "nd.h"

#define SPLASH_TOP    0x0A0F17u
#define SPLASH_BOTTOM 0x131C2Bu
#define SPLASH_TEXT   0xE8EEF8u
#define SPLASH_DIM    0x9AA6BAu
#define SPLASH_TRACK  0x24303Fu
#define SPLASH_ACCENT 0x4CC2FFu
#define SPLASH_CARD   0x1B2532u
#define SPLASH_LINE   0x33415Au

#define SPLASH_MARK   84
#define SPLASH_BAR_H  6
#define SPLASH_LABEL_MAX 48

/* .data, not .bss: the boot stage paints the splash before ExitBootServices
 * and kernel_entry zeroes .bss on its way into kmain, which would otherwise
 * make the kernel believe there is no splash and clear the screen. The
 * framebuffer keeps the pixels; this keeps the state that goes with them. */
static struct {
  UINT8        active;
  UINT32       percent;
  char         label[SPLASH_LABEL_MAX];
  nd_rect_t    bar;
  nd_rect_t    label_rect;
  nd_surface_t scratch; /* used only before nd_init() */
} g_splash __attribute__((section(".data")));

/* The surface to draw on: the real screen once the kernel has bound it,
 * otherwise a temporary view of the captured framebuffer. */
static nd_surface_t *splash_surface(void) {
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

/* Everything on the splash is centred on the primary output, and the block
 * as a whole is centred vertically, so a 768- and a 1080-line screen both
 * look deliberate. */
static void splash_layout(nd_surface_t *s) {
  INT32 cx = s->w / 2;
  INT32 block = SPLASH_MARK + 26 + 21 + 56 + 18;
  INT32 top = (s->h - block) / 2;
  INT32 bar_w = s->w / 3;

  if (bar_w < 240) {
    bar_w = 240;
  }
  if (bar_w > 520) {
    bar_w = 520;
  }
  if (bar_w > s->w - 48) {
    bar_w = s->w - 48;
  }
  if (top < 40) {
    top = 40;
  }
  g_splash.bar = nd_rect(cx - bar_w / 2, top + SPLASH_MARK + 26 + 21 + 56,
                         bar_w, SPLASH_BAR_H);
  g_splash.label_rect = nd_rect(0, g_splash.bar.y + SPLASH_BAR_H + 12, s->w,
                                ND_CJK_H);
}

static void splash_mark(nd_surface_t *s) {
  INT32      cx = s->w / 2;
  INT32      block = SPLASH_MARK + 26 + 21 + 56 + 18;
  INT32      top = (s->h - block) / 2;
  nd_rect_t  mark;
  nd_rect_t  title;
  UINT32     tw;

  if (top < 40) {
    top = 40;
  }
  mark = nd_rect(cx - SPLASH_MARK / 2, top, SPLASH_MARK, SPLASH_MARK);
  nd_round_rect(s, mark, 20, SPLASH_CARD);
  nd_round_frame(s, mark, 20, 1, SPLASH_LINE);

  /* The mark's letter, centred in the card. */
  {
    const char *n = "N";
    INT32       nw = (INT32)nd_text_width(n, 6);
    INT32       nh = (INT32)nd_text_height(n, 6);

    nd_text(s, cx - nw / 2, mark.y + (SPLASH_MARK - nh) / 2, n, SPLASH_ACCENT,
            6);
  }

  /* Wordmark under the card, in the same centred column. */
  title = nd_rect(0, top + SPLASH_MARK + 26, s->w, 0);
  tw = nd_text_width("Novus OS", 3);
  nd_text(s, cx - (INT32)tw / 2, title.y, "Novus OS", SPLASH_TEXT, 3);
}

static void splash_bar(nd_surface_t *s) {
  nd_rect_t fill = g_splash.bar;

  nd_round_rect(s, g_splash.bar, SPLASH_BAR_H / 2, SPLASH_TRACK);
  fill.w = (INT32)(((INT64)g_splash.bar.w * (INT64)g_splash.percent) / 100);
  if (fill.w <= 0) {
    return;
  }
  if (fill.w < SPLASH_BAR_H) {
    fill.w = SPLASH_BAR_H; /* keep the rounded cap from collapsing */
  }
  nd_round_rect(s, fill, SPLASH_BAR_H / 2, SPLASH_ACCENT);
}

/* "<stage label> NN%" on one line, centred. The percentage is formatted by
 * hand: the boot stage has no printf, and pulling one in for three digits
 * is not worth it. */
static void splash_label(nd_surface_t *s) {
  char   buf[SPLASH_LABEL_MAX + 8];
  UINT32 n = 0;
  UINT32 p = g_splash.percent;
  UINT32 i;

  for (i = 0; i < SPLASH_LABEL_MAX && g_splash.label[i] != '\0'; i++) {
    buf[n++] = g_splash.label[i];
  }
  if (n != 0) {
    buf[n++] = ' ';
  }
  if (p >= 100u) {
    buf[n++] = '1';
    buf[n++] = '0';
    buf[n++] = '0';
  } else {
    if (p >= 10u) {
      buf[n++] = (char)('0' + (p / 10u));
    }
    buf[n++] = (char)('0' + (p % 10u));
  }
  buf[n++] = '%';
  buf[n] = '\0';

  nd_text(s, (s->w - (INT32)nd_text_width(buf, 1)) / 2, g_splash.label_rect.y,
          buf, SPLASH_DIM, 1);
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
  g_splash.percent = 0;
  g_splash.label[0] = '\0';
  splash_layout(s);
  nd_surface_reset_clip(s);
  splash_backdrop(s, nd_rect(0, 0, s->w, s->h));
  splash_mark(s);
  splash_bar(s);
  splash_label(s);
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
  if (percent > 100u) {
    percent = 100u;
  }
  if (percent < g_splash.percent) {
    percent = g_splash.percent; /* the bar never walks backwards */
  }
  if (percent != g_splash.percent) {
    g_splash.percent = percent;
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
  if (!changed) {
    return;
  }
  s = splash_surface();
  if (s == 0) {
    return;
  }
  splash_layout(s);
  nd_surface_reset_clip(s);
  /* Only the bar and the label line are touched; the rest of the splash is
   * already correct on the glass. */
  dirty = nd_rect_union(g_splash.bar, g_splash.label_rect);
  dirty = nd_rect(dirty.x - 4, dirty.y - 6, dirty.w + 8, dirty.h + 12);
  splash_backdrop(s, dirty);
  splash_bar(s);
  splash_label(s);
}
