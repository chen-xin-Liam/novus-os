/**
 * Novus OS - NovusDisplay 2D renderer (M5).
 *
 * Every primitive is clipped twice: against the surface bounds and against
 * whatever scissor the caller last set. Clipping is the only thing that
 * makes partial redraws safe, which is why it lives in this file and not
 * in the widget library: a button that redraws itself can never paint
 * outside its own rectangle, no matter what its caller asks for.
 *
 * The renderer is deliberately immediate-mode and allocation-free on the
 * drawing path. Off-screen surfaces come from kmalloc (tests, and later a
 * compositor's window buffers); the framebuffer surface is just a view of
 * MMIO, with `fb` set so pixel writes go through the byte-order
 * conversion the firmware's mode needs.
 */

#include "kernel.h"
#include "heap.h"
#include "nd.h"
#include "nd_internal.h"

#define ND_ALPHA_CHANNEL 0xFF000000u

static INT32 isqrt_i32(INT32 v) {
  INT32 r = 0;
  INT32 bit = 1 << 15;

  if (v <= 0) {
    return 0;
  }
  while (bit > v) {
    bit >>= 2;
  }
  while (bit != 0) {
    if (v >= r + bit) {
      v -= r + bit;
      r = (r >> 1) + bit;
    } else {
      r >>= 1;
    }
    bit >>= 2;
  }
  return r;
}

/* ---- pixel conversion ------------------------------------------------- */

UINT32 nd_hw_pixel(const nd_surface_t *s, nd_color_t c) {
  if (s->fb != 0 && s->bgr == 0) {
    /* The firmware stores R first; the renderer's canonical word is
     * 0x00RRGGBB, so the red and blue bytes trade places. */
    return (c & 0xFF00FF00u) | ((c & 0x000000FFu) << 16) |
           ((c & 0x00FF0000u) >> 16);
  }
  return c & (ND_ALPHA_CHANNEL | 0x00FFFFFFu);
}

static nd_color_t hw_to_nd(const nd_surface_t *s, UINT32 px) {
  if (s->fb != 0 && s->bgr == 0) {
    return (px & 0xFF00FF00u) | ((px & 0x000000FFu) << 16) |
           ((px & 0x00FF0000u) >> 16);
  }
  return px;
}

void nd_span_raw(nd_surface_t *s, INT32 x, INT32 y, INT32 count,
                 nd_color_t c) {
  UINT32  hw = nd_hw_pixel(s, c);
  UINT32 *row = s->px + (UINT32)y * s->stride + (UINT32)x;

  while (count-- > 0) {
    *row++ = hw;
  }
}

void nd_span_blend_raw(nd_surface_t *s, INT32 x, INT32 y, INT32 count,
                       nd_color_t c, UINT8 alpha) {
  UINT32 *row = s->px + (UINT32)y * s->stride + (UINT32)x;
  UINT32  sr = ND_RED(c);
  UINT32  sg = ND_GREEN(c);
  UINT32  sb = ND_BLUE(c);
  UINT32  ia = 255u - alpha;

  while (count-- > 0) {
    nd_color_t d = hw_to_nd(s, *row);
    nd_color_t o = ND_RGB((sr * alpha + ND_RED(d) * ia) / 255u,
                          (sg * alpha + ND_GREEN(d) * ia) / 255u,
                          (sb * alpha + ND_BLUE(d) * ia) / 255u);
    *row++ = nd_hw_pixel(s, o);
  }
}

int nd_visible(const nd_surface_t *s, INT32 x, INT32 y) {
  return s->px != 0 && x >= s->clip.x && y >= s->clip.y &&
         x < s->clip.x + s->clip.w && y < s->clip.y + s->clip.h;
}

/* ---- surfaces --------------------------------------------------------- */

int nd_surface_create(nd_surface_t *s, INT32 w, INT32 h) {
  memset(s, 0, sizeof(*s));
  if (w <= 0 || h <= 0) {
    return -1;
  }
  s->px = (UINT32 *)kmalloc((UINT64)w * (UINT64)h * 4u);
  if (s->px == 0) {
    return -1;
  }
  s->w = w;
  s->h = h;
  s->stride = (UINT32)w;
  s->owned = 1;
  s->clip = nd_rect(0, 0, w, h);
  memset(s->px, 0, (UINT64)w * (UINT64)h * 4u);
  return 0;
}

void nd_surface_destroy(nd_surface_t *s) {
  if (s->owned != 0 && s->px != 0) {
    kfree(s->px);
  }
  memset(s, 0, sizeof(*s));
}

void nd_surface_set_clip(nd_surface_t *s, nd_rect_t clip) {
  s->clip = nd_rect_intersect(clip, nd_rect(0, 0, s->w, s->h));
}

void nd_surface_reset_clip(nd_surface_t *s) {
  s->clip = nd_rect(0, 0, s->w, s->h);
}

/* ---- primitives ------------------------------------------------------- */

void nd_pixel(nd_surface_t *s, INT32 x, INT32 y, nd_color_t c) {
  if (!nd_visible(s, x, y)) {
    return;
  }
  s->px[(UINT32)y * s->stride + (UINT32)x] = nd_hw_pixel(s, c);
}

void nd_clear(nd_surface_t *s, nd_color_t c) {
  nd_fill_rect(s, nd_rect(0, 0, s->w, s->h), c);
}

void nd_fill_rect(nd_surface_t *s, nd_rect_t r, nd_color_t c) {
  nd_rect_t clip;
  INT32     y;

  if (s->px == 0) {
    return;
  }
  clip = nd_rect_intersect(r, s->clip);
  if (nd_rect_empty(clip)) {
    return;
  }
  for (y = clip.y; y < clip.y + clip.h; y++) {
    nd_span_raw(s, clip.x, y, clip.w, c);
  }
}

void nd_blend_rect(nd_surface_t *s, nd_rect_t r, nd_color_t c, UINT8 alpha) {
  nd_rect_t clip;
  INT32     y;

  if (s->px == 0 || alpha == 0u) {
    return;
  }
  if (alpha == 255u) {
    nd_fill_rect(s, r, c);
    return;
  }
  clip = nd_rect_intersect(r, s->clip);
  if (nd_rect_empty(clip)) {
    return;
  }
  for (y = clip.y; y < clip.y + clip.h; y++) {
    nd_span_blend_raw(s, clip.x, y, clip.w, c, alpha);
  }
}

void nd_hline(nd_surface_t *s, INT32 x0, INT32 x1, INT32 y, nd_color_t c) {
  if (x1 < x0) {
    INT32 t = x0;
    x0 = x1;
    x1 = t;
  }
  nd_fill_rect(s, nd_rect(x0, y, x1 - x0 + 1, 1), c);
}

void nd_vline(nd_surface_t *s, INT32 x, INT32 y0, INT32 y1, nd_color_t c) {
  if (y1 < y0) {
    INT32 t = y0;
    y0 = y1;
    y1 = t;
  }
  nd_fill_rect(s, nd_rect(x, y0, 1, y1 - y0 + 1), c);
}

void nd_frame(nd_surface_t *s, nd_rect_t r, INT32 thickness, nd_color_t c) {
  if (thickness <= 0 || nd_rect_empty(r)) {
    return;
  }
  if (thickness * 2 >= r.w || thickness * 2 >= r.h) {
    nd_fill_rect(s, r, c);
    return;
  }
  nd_fill_rect(s, nd_rect(r.x, r.y, r.w, thickness), c);
  nd_fill_rect(s, nd_rect(r.x, r.y + r.h - thickness, r.w, thickness), c);
  nd_fill_rect(s, nd_rect(r.x, r.y + thickness, thickness,
                          r.h - thickness * 2),
               c);
  nd_fill_rect(s, nd_rect(r.x + r.w - thickness, r.y + thickness, thickness,
                          r.h - thickness * 2),
               c);
}

void nd_line(nd_surface_t *s, INT32 x0, INT32 y0, INT32 x1, INT32 y1,
             nd_color_t c) {
  INT32 dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
  INT32 dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
  INT32 sx = (x0 < x1) ? 1 : -1;
  INT32 sy = (y0 < y1) ? 1 : -1;
  INT32 err = dx - dy;

  for (;;) {
    nd_pixel(s, x0, y0, c);
    if (x0 == x1 && y0 == y1) {
      break;
    }
    {
      INT32 e2 = err * 2;
      if (e2 > -dy) {
        err -= dy;
        x0 += sx;
      }
      if (e2 < dx) {
        err += dx;
        y0 += sy;
      }
    }
  }
}

void nd_fill_circle(nd_surface_t *s, INT32 cx, INT32 cy, INT32 radius,
                    nd_color_t c) {
  INT32 dy;

  if (radius < 0) {
    return;
  }
  for (dy = -radius; dy <= radius; dy++) {
    INT32 half = isqrt_i32(radius * radius - dy * dy);
    nd_fill_rect(s, nd_rect(cx - half, cy + dy, half * 2 + 1, 1), c);
  }
}

void nd_circle(nd_surface_t *s, INT32 cx, INT32 cy, INT32 radius,
               nd_color_t c) {
  INT32 x = radius;
  INT32 y = 0;
  INT32 err = 1 - radius;

  if (radius < 0) {
    return;
  }
  while (x >= y) {
    nd_pixel(s, cx + x, cy + y, c);
    nd_pixel(s, cx + y, cy + x, c);
    nd_pixel(s, cx - y, cy + x, c);
    nd_pixel(s, cx - x, cy + y, c);
    nd_pixel(s, cx - x, cy - y, c);
    nd_pixel(s, cx - y, cy - x, c);
    nd_pixel(s, cx + y, cy - x, c);
    nd_pixel(s, cx + x, cy - y, c);
    y++;
    if (err < 0) {
      err += 2 * y + 1;
    } else {
      x--;
      err += 2 * (y - x) + 1;
    }
  }
}

/* Inset of row `y` inside a rounded rectangle, measured from both sides. */
static INT32 round_inset(INT32 y, INT32 h, INT32 radius) {
  INT32 dy;

  if (radius <= 0) {
    return 0;
  }
  if (y < radius) {
    dy = radius - y;
  } else if (y >= h - radius) {
    dy = y - (h - radius - 1);
  } else {
    return 0;
  }
  return radius - isqrt_i32(radius * radius - dy * dy);
}

void nd_round_rect(nd_surface_t *s, nd_rect_t r, INT32 radius, nd_color_t c) {
  INT32 y;
  INT32 max_r;

  if (nd_rect_empty(r)) {
    return;
  }
  max_r = (r.w < r.h) ? r.w : r.h;
  if (radius > max_r / 2) {
    radius = max_r / 2;
  }
  if (radius <= 0) {
    nd_fill_rect(s, r, c);
    return;
  }
  for (y = 0; y < r.h; y++) {
    INT32 inset = round_inset(y, r.h, radius);
    nd_fill_rect(s, nd_rect(r.x + inset, r.y + y, r.w - inset * 2, 1), c);
  }
}

void nd_round_frame(nd_surface_t *s, nd_rect_t r, INT32 radius, INT32 thick,
                    nd_color_t c) {
  INT32 y;
  INT32 max_r;
  INT32 inner_r;
  INT32 inner_h;

  if (nd_rect_empty(r) || thick <= 0) {
    return;
  }
  max_r = (r.w < r.h) ? r.w : r.h;
  if (radius > max_r / 2) {
    radius = max_r / 2;
  }
  if (radius <= 0 || thick * 2 >= r.h || thick * 2 >= r.w) {
    nd_frame(s, r, thick, c);
    return;
  }
  inner_r = radius - thick;
  if (inner_r < 0) {
    inner_r = 0;
  }
  inner_h = r.h - thick * 2;

  /* Row by row: the outer boundary moves in at the corners, the inner
   * boundary moves in behind it. What is left between them is the frame.
   * Top and bottom rows have no inner span at all, so the horizontal
   * bands fall out of the same loop instead of needing a special case. */
  for (y = 0; y < r.h; y++) {
    INT32 outer = round_inset(y, r.h, radius);
    INT32 inner;

    if (y < thick || y >= r.h - thick) {
      nd_fill_rect(s, nd_rect(r.x + outer, r.y + y, r.w - outer * 2, 1), c);
      continue;
    }
    inner = thick + round_inset(y - thick, inner_h, inner_r);
    if (inner > outer) {
      nd_fill_rect(s, nd_rect(r.x + outer, r.y + y, inner - outer, 1), c);
    }
    if (inner > outer) {
      nd_fill_rect(s,
                   nd_rect(r.x + r.w - inner, r.y + y, inner - outer, 1), c);
    }
  }
}

void nd_gradient_v(nd_surface_t *s, nd_rect_t r, nd_color_t top,
                   nd_color_t bottom) {
  INT32 y;

  if (nd_rect_empty(r)) {
    return;
  }
  for (y = 0; y < r.h; y++) {
    UINT32 t = (r.h > 1) ? ((UINT32)y * 255u) / (UINT32)(r.h - 1) : 0u;
    UINT32 inv = 255u - t;
    nd_color_t c = ND_RGB(
        (ND_RED(top) * inv + ND_RED(bottom) * t) / 255u,
        (ND_GREEN(top) * inv + ND_GREEN(bottom) * t) / 255u,
        (ND_BLUE(top) * inv + ND_BLUE(bottom) * t) / 255u);
    nd_fill_rect(s, nd_rect(r.x, r.y + y, r.w, 1), c);
  }
}

void nd_shadow(nd_surface_t *s, nd_rect_t r, INT32 spread, UINT8 alpha) {
  INT32 i;

  if (spread <= 0) {
    return;
  }
  /* Outermost first: each ring is thinner in alpha than the one below it,
   * which is enough to read as a soft edge without a blur pass. */
  for (i = spread; i > 0; i--) {
    UINT32 a = ((UINT32)alpha * (UINT32)(spread - i + 1)) /
               ((UINT32)spread * 2u);
    nd_blend_rect(s, nd_rect(r.x - i, r.y - i + 2, r.w + i * 2, r.h + i * 2),
                  ND_RGB(0, 0, 0), (UINT8)a);
  }
}

void nd_blit(nd_surface_t *dst, INT32 x, INT32 y, const nd_surface_t *src,
             nd_rect_t area) {
  nd_rect_t clip;
  INT32     row;

  if (dst->px == 0 || src->px == 0) {
    return;
  }
  area = nd_rect_intersect(area, nd_rect(0, 0, src->w, src->h));
  if (nd_rect_empty(area)) {
    return;
  }
  clip = nd_rect_intersect(nd_rect(x, y, area.w, area.h), dst->clip);
  if (nd_rect_empty(clip)) {
    return;
  }
  for (row = 0; row < clip.h; row++) {
    INT32     sy = area.y + (clip.y - y) + row;
    const UINT32 *srow = src->px + (UINT32)sy * src->stride +
                         (UINT32)(area.x + (clip.x - x));
    UINT32   *drow = dst->px + (UINT32)(clip.y + row) * dst->stride +
                     (UINT32)clip.x;
    INT32     i;

    for (i = 0; i < clip.w; i++) {
      drow[i] = nd_hw_pixel(dst, srow[i]);
    }
  }
}

void nd_blit_alpha(nd_surface_t *dst, INT32 x, INT32 y,
                   const nd_surface_t *src, nd_rect_t area, UINT8 alpha) {
  nd_rect_t clip;
  INT32     row;

  if (dst->px == 0 || src->px == 0 || alpha == 0u) {
    return;
  }
  if (alpha == 255u) {
    nd_blit(dst, x, y, src, area);
    return;
  }
  area = nd_rect_intersect(area, nd_rect(0, 0, src->w, src->h));
  clip = nd_rect_intersect(nd_rect(x, y, area.w, area.h), dst->clip);
  if (nd_rect_empty(clip)) {
    return;
  }
  for (row = 0; row < clip.h; row++) {
    INT32     sy = area.y + (clip.y - y) + row;
    const UINT32 *srow = src->px + (UINT32)sy * src->stride +
                         (UINT32)(area.x + (clip.x - x));
    INT32     i;

    for (i = 0; i < clip.w; i++) {
      nd_span_blend_raw(dst, clip.x + i, clip.y + row, 1, srow[i], alpha);
    }
  }
}

/* ---- text ------------------------------------------------------------- */

void nd_text_limit(nd_surface_t *s, INT32 x, INT32 y, const char *text,
                   UINT32 max_chars, nd_color_t c, UINT32 scale) {
  UINT32 consumed = 0;
  INT32  cx = x;

  if (text == 0 || s->px == 0) {
    return;
  }
  if (scale == 0u) {
    scale = 1u;
  }
  while (text[consumed] != '\0') {
    const UINT8 *rows;
    UINT32       gy;

    if (max_chars != 0u && consumed >= max_chars) {
      break;
    }
    rows = nd_font_rows((UINT8)text[consumed]);
    for (gy = 0; gy < ND_GLYPH_H; gy++) {
      UINT8 bits = rows[gy];
      UINT32 gx = 0;

      /* One fill per run of set bits, not one per pixel: a glyph row is
       * usually a single run, so a line of text costs tens of fills. */
      while (gx < ND_GLYPH_W) {
        UINT32 start;

        while (gx < ND_GLYPH_W && (bits & (1u << (4u - gx))) == 0u) {
          gx++;
        }
        start = gx;
        while (gx < ND_GLYPH_W && (bits & (1u << (4u - gx))) != 0u) {
          gx++;
        }
        if (gx > start) {
          nd_fill_rect(s, nd_rect(cx + (INT32)start * (INT32)scale,
                                  y + (INT32)gy * (INT32)scale,
                                  (INT32)(gx - start) * (INT32)scale,
                                  (INT32)scale),
                       c);
        }
      }
    }
    cx += (INT32)ND_FONT_ADV * (INT32)scale;
    consumed++;
  }
}

void nd_text(nd_surface_t *s, INT32 x, INT32 y, const char *text, nd_color_t c,
             UINT32 scale) {
  nd_text_limit(s, x, y, text, 0, c, scale);
}
