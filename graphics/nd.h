/**
 * Novus OS - NovusDisplay: 2D software renderer (M5).
 *
 * README part 11 splits the display stack in four layers. This header is
 * layers 1 and 2: the hardware abstraction that finds a linear
 * framebuffer, and the renderer that draws rectangles, lines, circles,
 * alpha blends and text into any surface. Layer 3 (the widget library)
 * sits on top in nvgui.h and only ever sees `nd_surface_t`.
 *
 * Pixel format: one UINT32 per pixel, 0xAARRGGBB, alpha ignored except by
 * the blend helpers. The framebuffer backend converts to whatever the
 * firmware handed out (BGR or RGB byte order) at draw time, so the rest
 * of the stack never thinks about the hardware.
 *
 * There is no back buffer: every draw goes straight into the target
 * surface. The GUI keeps a damage rectangle per state change and redraws
 * only that region, which is both cheaper than a 4 MiB blit per frame and
 * the same code path a future WM will use for dirty windows.
 */

#ifndef NOVUS_ND_H
#define NOVUS_ND_H

#include "uefi.h"
#include "nd_input.h"

/* ---- colours ---------------------------------------------------------- */

#define ND_ARGB(a, r, g, b)                                            \
  (((UINT32)(a) << 24) | ((UINT32)(r) << 16) | ((UINT32)(g) << 8) |    \
   (UINT32)(b))
#define ND_RGB(r, g, b) ND_ARGB(0xFFu, r, g, b)
#define ND_ALPHA(c) ((UINT8)(((UINT32)(c)) >> 24))
#define ND_RED(c) ((UINT8)(((UINT32)(c)) >> 16))
#define ND_GREEN(c) ((UINT8)(((UINT32)(c)) >> 8))
#define ND_BLUE(c) ((UINT8)((UINT32)(c)))

typedef UINT32 nd_color_t;

/* ---- geometry --------------------------------------------------------- */

typedef struct {
  INT32 x;
  INT32 y;
  INT32 w;
  INT32 h;
} nd_rect_t;

static inline nd_rect_t nd_rect(INT32 x, INT32 y, INT32 w, INT32 h) {
  nd_rect_t r;
  r.x = x;
  r.y = y;
  r.w = w;
  r.h = h;
  return r;
}

static inline int nd_rect_empty(nd_rect_t r) { return r.w <= 0 || r.h <= 0; }

static inline int nd_rect_contains(nd_rect_t r, INT32 x, INT32 y) {
  return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static inline nd_rect_t nd_rect_intersect(nd_rect_t a, nd_rect_t b) {
  INT32 x0 = (a.x > b.x) ? a.x : b.x;
  INT32 y0 = (a.y > b.y) ? a.y : b.y;
  INT32 x1 = (a.x + a.w < b.x + b.w) ? a.x + a.w : b.x + b.w;
  INT32 y1 = (a.y + a.h < b.y + b.h) ? a.y + a.h : b.y + b.h;
  return nd_rect(x0, y0, x1 - x0, y1 - y0);
}

static inline nd_rect_t nd_rect_union(nd_rect_t a, nd_rect_t b) {
  INT32 x0, y0, x1, y1;
  if (nd_rect_empty(a)) {
    return b;
  }
  if (nd_rect_empty(b)) {
    return a;
  }
  x0 = (a.x < b.x) ? a.x : b.x;
  y0 = (a.y < b.y) ? a.y : b.y;
  x1 = (a.x + a.w > b.x + b.w) ? a.x + a.w : b.x + b.w;
  y1 = (a.y + a.h > b.y + b.h) ? a.y + a.h : b.y + b.h;
  return nd_rect(x0, y0, x1 - x0, y1 - y0);
}

/* ---- surfaces --------------------------------------------------------- */

typedef struct {
  UINT32    *px;     /* 0xAARRGGBB; NULL for a surface that failed */
  INT32      w;
  INT32      h;
  UINT32     stride; /* pixels per row, may exceed w (framebuffer) */
  nd_rect_t  clip;   /* current scissor, surface coordinates */
  UINT8      owned;  /* 1 = pixels came from kmalloc -> free them */
  UINT8      fb;     /* 1 = linear framebuffer, so pixels get swapped */
  UINT8      bgr;    /* byte order of that framebuffer */
  UINT8      pad;
} nd_surface_t;

/* Create/destroy an off-screen surface (used by tests and by any future
 * compositor). Returns 0 on success. */
int nd_surface_create(nd_surface_t *s, INT32 w, INT32 h);
void nd_surface_destroy(nd_surface_t *s);
void nd_surface_set_clip(nd_surface_t *s, nd_rect_t clip);
void nd_surface_reset_clip(nd_surface_t *s);

/* ---- display hardware ------------------------------------------------ */

typedef struct {
  UINT8       available; /* 1 = a linear framebuffer was captured */
  UINT8       bgr;       /* 1 = memory byte order B,G,R,X */
  UINT32      width;
  UINT32      height;
  UINT32      pitch;     /* pixels per scanline */
  UINT32      bpp;
  UINT64      fb_phys;
  UINT64      fb_size;
  const char *backend;   /* "GOP framebuffer" or "none" */
} nd_display_t;

/* Boot stage, before ExitBootServices: read the current GOP mode and keep
 * the numbers. Returns 1 when a usable linear framebuffer was found. */
int nd_capture_boot(void *system_table);

/* Kernel stage, after mm_init()/heap_init(): bind a surface to the
 * captured framebuffer and log the mode. Returns 1 when the screen is
 * drawable. */
int nd_init(void);
const nd_display_t *nd_display(void);
nd_surface_t *nd_screen(void);

/* Software mouse pointer. Save-under: the 24x24 pixels below the arrow
 * are copied out before it is drawn and put back before the next frame,
 * so moving the pointer never repaints the scene. */
void nd_cursor_hide(void);
void nd_cursor_set(INT32 x, INT32 y);
void nd_cursor_draw(void);
void nd_cursor_get(INT32 *x, INT32 *y);

/* Convert one canonical colour to what the framebuffer expects. */
UINT32 nd_hw_pixel(const nd_surface_t *s, nd_color_t c);

/* ---- drawing primitives ---------------------------------------------- */

void nd_clear(nd_surface_t *s, nd_color_t c);
void nd_pixel(nd_surface_t *s, INT32 x, INT32 y, nd_color_t c);
void nd_fill_rect(nd_surface_t *s, nd_rect_t r, nd_color_t c);
void nd_blend_rect(nd_surface_t *s, nd_rect_t r, nd_color_t c, UINT8 alpha);
void nd_frame(nd_surface_t *s, nd_rect_t r, INT32 thickness, nd_color_t c);
void nd_hline(nd_surface_t *s, INT32 x0, INT32 x1, INT32 y, nd_color_t c);
void nd_vline(nd_surface_t *s, INT32 x, INT32 y0, INT32 y1, nd_color_t c);
void nd_line(nd_surface_t *s, INT32 x0, INT32 y0, INT32 x1, INT32 y1,
             nd_color_t c);
void nd_fill_circle(nd_surface_t *s, INT32 cx, INT32 cy, INT32 radius,
                    nd_color_t c);
void nd_circle(nd_surface_t *s, INT32 cx, INT32 cy, INT32 radius,
               nd_color_t c);
void nd_round_rect(nd_surface_t *s, nd_rect_t r, INT32 radius, nd_color_t c);
void nd_round_frame(nd_surface_t *s, nd_rect_t r, INT32 radius, INT32 thick,
                    nd_color_t c);
void nd_gradient_v(nd_surface_t *s, nd_rect_t r, nd_color_t top,
                   nd_color_t bottom);
void nd_shadow(nd_surface_t *s, nd_rect_t r, INT32 spread, UINT8 alpha);
void nd_blit(nd_surface_t *dst, INT32 x, INT32 y, const nd_surface_t *src,
             nd_rect_t area);
void nd_blit_alpha(nd_surface_t *dst, INT32 x, INT32 y,
                   const nd_surface_t *src, nd_rect_t area, UINT8 alpha);

/* ---- text ------------------------------------------------------------- */
/* Built-in 5x7 font in a 6x9 cell, drawn at 1x..4x. No font files, no
 * firmware font: the same glyphs look identical on every machine. */

#define ND_GLYPH_W 5
#define ND_GLYPH_H 7
#define ND_FONT_ADV 6
#define ND_FONT_LINE 9

UINT32 nd_text_width(const char *text, UINT32 scale);
UINT32 nd_font_height(UINT32 scale);
void nd_text(nd_surface_t *s, INT32 x, INT32 y, const char *text, nd_color_t c,
             UINT32 scale);
void nd_text_limit(nd_surface_t *s, INT32 x, INT32 y, const char *text,
                   UINT32 max_chars, nd_color_t c, UINT32 scale);

/* ---- M5 demo and self test ------------------------------------------- */

/* Draws into an off-screen surface, checks exact pixels, widget state and
 * event routing, and prints PASS/FAIL on COM1. Runs before ring 3. */
void nd_selftest(void);

/* Builds the demo desktop and paints it once. */
void nd_demo_init(void);

/* Called from the timer tick: poll input, dispatch, repaint damage. The
 * M6 window manager replaces this with its own tick and drives the shell
 * through the four hooks below. */
void nd_demo_tick(void);

/* 1 once the shell scene is built and drawable. */
int nd_demo_ready(void);

/* Hand one input event to the shell's widget tree (absolute screen
 * coordinates, as nd_input produced them). */
void nd_demo_dispatch(const nd_event_t *ev);

/* Take the shell's pending damage rectangle, 1 when there is one. */
int nd_demo_take_damage(nd_rect_t *out);

/* Repaint the shell scene clipped to `clip` (the WM composites windows
 * on top of this). */
void nd_demo_paint(nd_rect_t clip);

#endif /* NOVUS_ND_H */
