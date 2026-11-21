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

static struct {
  UINT32 *px;       /* first pixel the console owns */
  UINT32  w;        /* box width in pixels, a whole number of cells */
  UINT32  h;
  UINT32  stride;   /* pixels per scanline of the framebuffer */
  UINT32  bg;
  UINT32  fg;
  UINT32  rule;
  UINT32  cols;
  UINT32  rows;
  UINT32  col;
  UINT32  row;
  UINT64  phys;
  UINT64  size;
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

int fbcon_begin(void) {
  const nd_display_t *d = nd_display();

  if (d == 0 || d->available == 0u || d->fb_phys == 0u || d->fb_size == 0u ||
      d->width == 0u || d->height == 0u || d->pitch < d->width) {
    return 0;
  }
  if (d->fb_size < (UINT64)d->pitch * (UINT64)d->height * 4u) {
    return 0;
  }
  g_fb.phys = d->fb_phys;
  g_fb.size = d->fb_size;
  g_fb.stride = d->pitch;
  g_fb.bgr = d->bgr;
  g_fb.cols = ((d->width > FBCON_MAX_W) ? FBCON_MAX_W : d->width) / FBCON_CELL_W;
  g_fb.rows = ((d->height > FBCON_MAX_H) ? FBCON_MAX_H : d->height) / FBCON_CELL_H;
  if (g_fb.cols == 0u || g_fb.rows == 0u) {
    return 0;
  }
  g_fb.w = g_fb.cols * FBCON_CELL_W;
  g_fb.h = g_fb.rows * FBCON_CELL_H;
  g_fb.bg = fbcon_pixel(FBCON_RGB_BG);
  g_fb.fg = fbcon_pixel(FBCON_RGB_FG);
  g_fb.rule = fbcon_pixel(FBCON_RGB_RULE);
  g_fb.col = 0;
  g_fb.px = (UINT32 *)(UINTN)d->fb_phys;
  g_fb.wanted = 1u;
  g_fb.state = 1u;
  /* Paint the box and the hairline the header sits under, so the log reads
   * as a console and not as pixels dropped on a stale firmware screen. */
  fbcon_fill(0, 0, g_fb.w, g_fb.h, g_fb.bg);
  fbcon_fill(0, FBCON_CELL_H, g_fb.w, 1u, g_fb.rule);
  return 1;
}

/* Nothing to do around mm_init() any more: the console draws through the
 * framebuffer's own physical address, and mm_init() keeps that range
 * identity-mapped uncached before it switches CR3 (see the framebuffer
 * identity-map step there). The console therefore stays live straight
 * through the switch, which is the whole point: a machine that faults or
 * hangs inside mm_init() now prints it instead of going dark. */

void fbcon_end(void) {
  g_fb.state = 0u;
  g_fb.wanted = 0u;
}
