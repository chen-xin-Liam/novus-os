/**
 * Novus OS - kernel-stage text console on the firmware framebuffer.
 *
 * Draws the kputs() stream into a box in the top-left corner of the
 * captured framebuffer, with the same 5x7 glyphs NovusDisplay uses at 2x.
 * Only the box is ever touched, so a scroll moves a few hundred kilobytes
 * rather than the full 8 MiB of a 1080-line screen; on a machine whose
 * framebuffer is mapped uncacheable that is the difference between a
 * readable boot and a slideshow.
 */

#include "kernel.h"
#include "fbcon.h"
#include "mm.h"
#include "nd.h"
#include "pmm.h"

/* 5x7 glyph at 2x is 10x14, in a 12x18 cell: one pixel of air on each side
 * and two rows between baselines. */
#define FBCON_SCALE   2u
#define FBCON_GLYPH_W 5u
#define FBCON_GLYPH_H 7u
#define FBCON_CELL_W  12u
#define FBCON_CELL_H  18u

/* The box never grows past this, whatever the screen offers: a boot log is
 * read from the top-left corner, and a full-width box on a 4K panel would
 * make every scroll expensive for no benefit. 1200x648 gives 100 columns
 * and 36 lines. */
#define FBCON_MAX_W   1200u
#define FBCON_MAX_H   648u

#define FBCON_RGB_BG  0x0C1219u
#define FBCON_RGB_FG  0xD5DEEAu
#define FBCON_RGB_RULE 0x24344Au

/* How long a finished line may wait for the window blit. */
#define FBCON_FLUSH_US 8000u

/* The shadow run is kept across console restarts (the splash hands the
 * console a smaller box in its own panel), so a second begin_box does not
 * allocate again. g_shadow_base == 0 means none was ever granted. */
static UINT32 *g_shadow_base;
static UINT32  g_shadow_w;
static UINT32  g_shadow_h;

static struct {
  UINT32 *px;       /* first pixel the console owns (shadow or the glass) */
  UINT32  w;        /* box width in pixels, a whole number of cells */
  UINT32  h;
  UINT32  stride;   /* pixels per scanline where px lives */
  UINT32  bg;
  UINT32  fg;
  UINT32  rule;
  UINT32  cols;
  UINT32  rows;
  UINT32  col;
  UINT32  row;
  UINT64  phys;
  UINT64  size;
  /* Shadow mode (B-16): the console draws into a cacheable copy and the
   * window is blitted to the glass in one go. hw == 0 means the old direct
   * path, where px is the framebuffer itself. */
  UINT32 *hw;       /* framebuffer base, no bias */
  UINT32  fb_stride; /* pixels per scanline of the framebuffer */
  UINT32  x0;       /* box origin inside the framebuffer */
  UINT32  y0;
  UINT64  last_us;  /* when the window was last blitted */
  UINT8   shadow;
  UINT8   dirty;
  UINT8   pending;  /* finished lines not on the glass yet */
  UINT8   bgr;
  UINT8   wanted;   /* 1 once the console was asked to run */
  UINT8   state;    /* 1 = drawing */
} g_fb;

/* Pixel words are written little-endian: for the BGR byte order the word
 * 0x00RRGGBB lands as B,G,R,X, and the RGB order needs the channels
 * swapped. Everything above is written as 0x00RRGGBB. */
static UINT32 fbcon_pixel(UINT32 rgb) {
  if (g_fb.bgr) {
    return rgb;
  }
  return ((rgb & 0xFF0000u) >> 16) | (rgb & 0xFF00u) | ((rgb & 0xFFu) << 16);
}

static void fbcon_fill(UINT32 x, UINT32 y, UINT32 w, UINT32 h, UINT32 c) {
  UINT32 yy;
  UINT32 xx;

  if (g_fb.px == 0 || x >= g_fb.w || y >= g_fb.h) {
    return;
  }
  if (x + w > g_fb.w) {
    w = g_fb.w - x;
  }
  if (y + h > g_fb.h) {
    h = g_fb.h - y;
  }
  for (yy = 0; yy < h; yy++) {
    UINT32 *line = g_fb.px + (y + yy) * g_fb.stride + x;

    for (xx = 0; xx < w; xx++) {
      line[xx] = c;
    }
  }
}

/* Present the box: copy the shadow into the framebuffer, one scanline at a
 * time. Writes only, never a read-modify-write on the glass - on a machine
 * whose scanout is uncached a blended pixel costs a read *and* a write to
 * uncacheable memory, and at the cost this laptop measures (~1.2 ms per
 * printed character) the console was by far the longest part of the boot. */
static void fbcon_present(void) {
  UINT32 yy;
  UINT32 x;

  if (g_fb.shadow == 0u || g_fb.hw == 0) {
    return;
  }
  for (yy = 0; yy < g_fb.h; yy++) {
    UINT32       *dst = g_fb.hw + (UINT64)(g_fb.y0 + yy) * g_fb.fb_stride +
                        g_fb.x0;
    const UINT32 *src = g_fb.px + (UINT64)yy * g_fb.stride;

    for (x = 0; x < g_fb.w; x++) {
      dst[x] = src[x];
    }
  }
  __asm__ volatile("sfence" ::: "memory");
  g_fb.dirty = 0u;
  g_fb.pending = 0u;
  g_fb.last_us = cpu_uptime_us();
}

/* Move the box up one cell and clear the row that scrolled into view. The
 * source is above the destination, so a forward word copy cannot clobber
 * anything it still has to read. */
static void fbcon_scroll(void) {
  UINT32  y;
  UINT32 *dst = g_fb.px;
  UINT32 *src = g_fb.px + FBCON_CELL_H * g_fb.stride;
  UINT32  words = (g_fb.h - FBCON_CELL_H) * g_fb.stride;

  for (y = 0; y < words; y++) {
    dst[y] = src[y];
  }
  fbcon_fill(0, g_fb.h - FBCON_CELL_H, g_fb.w, FBCON_CELL_H, g_fb.bg);
}

static void fbcon_newline(void) {
  g_fb.col = 0;
  if (g_fb.row + 1u >= g_fb.rows) {
    fbcon_scroll();
    g_fb.row = g_fb.rows - 1u;
    return;
  }
  g_fb.row++;
}

static void fbcon_glyph(UINT32 col, UINT32 row, char ch) {
  const UINT8 *cell = nd_font_rows((UINT8)ch);
  UINT32       gy;
  UINT32       gx;

  if (cell == 0) {
    return;
  }
  for (gy = 0; gy < FBCON_GLYPH_H; gy++) {
    UINT8 bits = cell[gy];

    for (gx = 0; gx < FBCON_GLYPH_W; gx++) {
      if ((bits & (UINT8)(0x10u >> gx)) == 0u) {
        continue;
      }
      fbcon_fill(col * FBCON_CELL_W + 1u + gx * FBCON_SCALE,
                 row * FBCON_CELL_H + 1u + gy * FBCON_SCALE,
                 FBCON_SCALE, FBCON_SCALE, g_fb.fg);
    }
  }
}

void fbcon_puts(const char *text) {
  if (g_fb.state == 0u || g_fb.px == 0 || text == 0) {
    return;
  }
  while (*text != '\0') {
    char ch = *text++;

    if (ch == '\n') {
      fbcon_newline();
      /* The window goes to the glass two lines behind, or eight milliseconds
       * behind, whichever comes first. A burst of a hundred lines therefore
       * costs one blit per three lines instead of one per line, and the
       * eight-millisecond rule keeps a slow line visible while it is being
       * written. A machine that hangs mid-burst can lose at most the two
       * lines that were not presented yet; the copy on the stick, which is
       * written per line, still has them. */
      if (g_fb.pending < 3u) {
        g_fb.pending++;
      }
      if (g_fb.pending >= 3u ||
          (cpu_uptime_us() - g_fb.last_us) >= FBCON_FLUSH_US) {
        fbcon_present();
      }
      continue;
    }
    if (ch == '\r') {
      continue;
    }
    if (ch == '\t') {
      UINT32 n;

      for (n = 0; n < 4u; n++) {
        if (g_fb.col >= g_fb.cols) {
          fbcon_newline();
        }
        g_fb.col++;
      }
      continue;
    }
    if ((UINT8)ch < 0x20u || (UINT8)ch > 0x7Eu) {
      ch = '.';
    }
    if (g_fb.col >= g_fb.cols) {
      fbcon_newline();
    }
    fbcon_glyph(g_fb.col, g_fb.row, ch);
    g_fb.col++;
  }
}

int fbcon_active(void) { return g_fb.state != 0u; }

void *fbcon_framebuffer(void) { return (void *)g_fb.px; }

/* Shared start path: the console draws into a w x h box whose top-left corner
 * is (x, y) in framebuffer pixels. The pointer is biased to that corner and
 * every fill/scroll below works in box-local coordinates, so the same code
 * serves the firmware-stage box in the corner and the splash's log panel at
 * the bottom of the panel. */
static int fbcon_start(const nd_display_t *d, UINT32 x, UINT32 y, UINT32 w,
                       UINT32 h) {
  if (d->fb_size < (UINT64)d->pitch * (UINT64)d->height * 4u) {
    return 0;
  }
  if (x >= d->width || y >= d->height) {
    return 0;
  }
  if (x + w > d->width) {
    w = d->width - x;
  }
  if (y + h > d->height) {
    h = d->height - y;
  }
  g_fb.cols = w / FBCON_CELL_W;
  g_fb.rows = h / FBCON_CELL_H;
  if (g_fb.cols == 0u || g_fb.rows == 0u) {
    return 0;
  }
  g_fb.phys = d->fb_phys;
  g_fb.size = d->fb_size;
  g_fb.bgr = d->bgr;
  g_fb.w = g_fb.cols * FBCON_CELL_W;
  g_fb.h = g_fb.rows * FBCON_CELL_H;
  g_fb.bg = fbcon_pixel(FBCON_RGB_BG);
  g_fb.fg = fbcon_pixel(FBCON_RGB_FG);
  g_fb.rule = fbcon_pixel(FBCON_RGB_RULE);
  g_fb.col = 0;
  g_fb.row = 0;
  g_fb.x0 = x;
  g_fb.y0 = y;
  g_fb.hw = (UINT32 *)(UINTN)d->fb_phys;
  g_fb.fb_stride = d->pitch;
  /* Shadow first: draw into ordinary cacheable RAM and blit the window to
   * the glass in one write-only pass. The box is at most 1200x648, so the
   * copy is 3 MiB and the framebuffer never sees a blend. If the run cannot
   * be had, fall back to drawing straight into the scanout exactly as before
   * - slower, but the console still works. */
  {
    UINT32 need_w = g_fb.w;
    UINT32 need_h = g_fb.h;

    if (g_fb.shadow != 0u && need_w <= g_shadow_w && need_h <= g_shadow_h) {
      g_fb.px = (UINT32 *)g_shadow_base;
      g_fb.stride = g_shadow_w;
    } else {
      UINT32  bytes = need_w * need_h * 4u;
      UINT32  pages = (bytes + 0xFFFu) / 0x1000u;
      UINT32 *run = (UINT32 *)pmm_alloc_contig(pages);

      if (run != 0) {
        g_shadow_base = run;
        g_shadow_w = need_w;
        g_shadow_h = need_h;
        g_fb.shadow = 1u;
        g_fb.px = run;
        g_fb.stride = need_w;
      }
    }
    if (g_fb.shadow == 0u) {
      g_fb.px = g_fb.hw + (UINT64)y * d->pitch + x;
      g_fb.stride = d->pitch;
    }
  }
  g_fb.last_us = cpu_uptime_us();
  g_fb.pending = 0u;
  g_fb.wanted = 1u;
  g_fb.state = 1u;
  /* Paint the box and the hairline the header sits under, so the log reads
   * as a console and not as pixels dropped on a stale firmware screen. */
  fbcon_fill(0, 0, g_fb.w, g_fb.h, g_fb.bg);
  if (g_fb.rows > 1u) {
    fbcon_fill(0, FBCON_CELL_H, g_fb.w, 1u, g_fb.rule);
  }
  fbcon_present();
  return 1;
}

int fbcon_begin(void) {
  const nd_display_t *d = nd_display();

  if (d == 0 || d->available == 0u || d->fb_phys == 0u || d->fb_size == 0u ||
      d->width == 0u || d->height == 0u || d->pitch < d->width) {
    return 0;
  }
  return fbcon_start(d, 0u, 0u, (d->width > FBCON_MAX_W) ? FBCON_MAX_W : d->width,
                     (d->height > FBCON_MAX_H) ? FBCON_MAX_H : d->height);
}

/* Put the console inside an arbitrary rectangle of the panel. The kernel
 * hands it the splash's log panel, so the boot log keeps scrolling in the
 * bottom band the splash design reserves for it instead of covering the
 * lockup. */
int fbcon_begin_box(UINT32 x, UINT32 y, UINT32 w, UINT32 h) {
  const nd_display_t *d = nd_display();

  if (d == 0 || d->available == 0u || d->fb_phys == 0u || d->fb_size == 0u ||
      d->width == 0u || d->height == 0u || d->pitch < d->width) {
    return 0;
  }
  return fbcon_start(d, x, y, w, h);
}

/* Nothing to do around mm_init() any more: the console draws into its shadow
 * run, which is ordinary cacheable RAM allocated from the physical manager,
 * and only fbcon_present() touches the framebuffer - at the same physical
 * address mm_init() keeps identity-mapped (uncached) through and after the
 * CR3 switch. A machine that faults or hangs inside mm_init() still prints
 * it: the shadow is RAM, and the blit path re-reads the framebuffer base the
 * same way the old direct path did. */

void fbcon_end(void) {
  fbcon_present();
  g_fb.state = 0u;
  g_fb.wanted = 0u;
}
