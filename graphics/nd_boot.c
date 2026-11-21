/**
 * Novus OS - boot splash.
 *
 * The boot path has two halves: the UEFI stage, which can still read disks,
 * and the kernel stage, which takes over after ExitBootServices. Both show
 * the same screen so the user sees one continuous boot instead of a wall of
 * firmware text followed by a black screen.
 *
 * The composition is deliberately spare: the brand lockup (the Novus mark on
 * its card, the wordmark under it) sits in the upper third, and the single
 * line that matters - the step the boot is on right now - sits centred in the
 * lower fifth. One line, no percentage, no progress bar: a boot screen is
 * something to glance at, and a bar that walks 4, 88, 90, 91 percent in three
 * tenths of a second reads as a stutter rather than as progress.
 *
 * It is drawn with the ordinary renderer (nd_fill_rect/nd_round_rect/nd_text),
 * not with a separate "boot drawing" path: the same primitives, the same
 * glyphs, no second implementation to keep in sync. The only special case is
 * the surface: before nd_init() there is no heap and possibly no high
 * mapping, so the splash binds a static surface straight to the framebuffer
 * the boot stage captured, as long as it sits in the low 4 GiB.
 *
 * Updates repaint the status line only. Repainting the whole screen on every
 * step would be the very flicker this milestone is about.
 */

#include "kernel.h"
#include "nd.h"
#include "nd_oemlogo.h"

#define SPLASH_TOP    0x0A0F17u
#define SPLASH_BOTTOM 0x131C2Bu
#define SPLASH_TEXT   0xE8EEF8u
#define SPLASH_DIM    0x9AA6BAu
#define SPLASH_TRACK  0x24303Fu
#define SPLASH_ACCENT 0x4CC2FFu
#define SPLASH_CARD   0x1B2532u
#define SPLASH_LINE   0x33415Au

#define SPLASH_MARK     112  /* the brand card, pixels */
#define SPLASH_MARK_R   26   /* its corner radius */
#define SPLASH_WORD     4    /* "Novus OS" scale */
#define SPLASH_STATUS_H 44   /* the one status line's band */
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

/* Upper third: the brand lockup, centred. Lower fifth: the status band.
 * Both are centred horizontally on the whole screen, so a 768- and a
 * 1080-line panel put the mark and the line in the same places relative to
 * their own height instead of hugging one edge. */
static void splash_layout(nd_surface_t *s) {
  INT32 cx = s->w / 2;
  INT32 band = SPLASH_MARK;
  INT32 mark_top = (s->h * 26) / 100 - SPLASH_MARK / 2;
  INT32 status_y = s->h - (s->h / 5) - SPLASH_STATUS_H / 2;

  /* The machine's own logo takes the place of the card, scaled only when
   * the capture is larger than the band the splash keeps for it: the
   * firmware paints it at panel resolution, so on most laptops it lands
   * here at its native size and stays crisp. */
  if (nd_oem_logo_available()) {
    INT32 lw = nd_oem_logo_width();
    INT32 lh = nd_oem_logo_height();
    INT32 maxw = (s->w * 62) / 100;
    INT32 maxh = (s->h * 22) / 100;

    if (maxw < 120) {
      maxw = s->w - 48;
    }
    if (lw > maxw && lw > 0) {
      lh = (lh * maxw) / lw;
      lw = maxw;
    }
    if (lh > maxh && lh > 0) {
      lw = (lw * maxh) / lh;
      lh = maxh;
    }
    band = lh;
    mark_top = (s->h * 26) / 100 - lh / 2;
    g_splash.logo = nd_rect(cx - lw / 2, mark_top, lw, lh);
  } else {
    g_splash.logo = nd_rect(cx - SPLASH_MARK / 2, mark_top, SPLASH_MARK,
                            SPLASH_MARK);
  }

  if (mark_top < 40) {
    mark_top = 40;
  }
  if (nd_oem_logo_available()) {
    g_splash.logo.y = mark_top;
  }
  if (status_y < mark_top + band + 72) {
    status_y = mark_top + band + 72;
  }
  g_splash.mark = nd_rect(cx - SPLASH_MARK / 2, mark_top, SPLASH_MARK,
                          SPLASH_MARK);
  g_splash.status = nd_rect(0, status_y, s->w, SPLASH_STATUS_H);
}

/* The mark: three strokes, drawn as rectangles rather than as a glyph. The
 * 5x7 font's "N" at 6x expands into a staircase that reads as a mirrored N;
 * a logo should not depend on the fallback bitmap looking right at an
 * unusual scale, and three rectangles are crisper at any size anyway. */
static void splash_letter(nd_surface_t *s, nd_rect_t card) {
  const INT32 pad = 28;
  const INT32 w = 7;
  INT32 top = card.y + pad;
  INT32 bot = card.y + card.h - pad - 1;
  INT32 lx = card.x + pad;
  INT32 rx = card.x + card.w - pad - 1 - w;
  INT32 span = rx - (lx + w - 1);
  INT32 rows = bot - top;
  INT32 i;

  nd_fill_rect(s, nd_rect(lx, top, w, rows + 1), SPLASH_ACCENT);
  nd_fill_rect(s, nd_rect(rx, top, w, rows + 1), SPLASH_ACCENT);
  if (span <= 0 || rows <= 0) {
    return;
  }
  for (i = 0; i <= span; i++) {
    INT32 y = top + (INT32)(((INT64)i * (INT64)rows) / (INT64)span);

    nd_fill_rect(s, nd_rect(lx + w - 1 + i, y, 1, w), SPLASH_ACCENT);
  }
}

static void splash_brand(nd_surface_t *s) {
  nd_rect_t  mark = g_splash.mark;
  const char *word = "Novus OS";
  UINT32     ww, wy;

  /* The machine's own logo, when the boot stage caught one: nothing else
   * is drawn under it, because the point of showing the vendor mark is
   * that the screen looks like this laptop's boot screen and not like an
   * advertisement for an operating system. */
  if (nd_oem_logo_available()) {
    nd_oem_logo_draw(s, g_splash.logo, 255);
    return;
  }

  nd_round_rect(s, mark, SPLASH_MARK_R, SPLASH_CARD);
  nd_round_frame(s, mark, SPLASH_MARK_R, 1, SPLASH_LINE);
  splash_letter(s, mark);

  ww = nd_text_width(word, SPLASH_WORD);
  wy = mark.y + SPLASH_MARK + 30;
  nd_text(s, (s->w - (INT32)ww) / 2, wy, word, SPLASH_TEXT, SPLASH_WORD);
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
  const INT32   frames = 10;
  const INT32   rise = 24;
  INT32         i;

  if (!g_splash.active || s == 0 || !nd_oem_logo_available() || delay_ms == 0) {
    return;
  }
  nd_surface_reset_clip(s);
  for (i = 0; i <= frames; i++) {
    UINT8     fade = (UINT8)((255 * i) / frames);
    INT32     dy = rise - (rise * i) / frames;

    splash_backdrop(s, splash_logo_band(rise));
    nd_oem_logo_draw(s, nd_rect(g_splash.logo.x, g_splash.logo.y + dy,
                                g_splash.logo.w, g_splash.logo.h),
                     (i == 0) ? 0u : (UINT8)fade);
    if (i == 0) {
      continue; /* the empty band is the start state, nothing to hold on */
    }
    delay_ms(20);
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
  splash_brand(s);
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
