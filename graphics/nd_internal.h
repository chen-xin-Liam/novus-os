/**
 * Novus OS - NovusDisplay internal interfaces (M5).
 *
 * Only the pieces that cross a .c boundary inside the graphics stack.
 * Everything a user of the stack needs is in nd.h / nvgui.h.
 */

#ifndef NOVUS_ND_INTERNAL_H
#define NOVUS_ND_INTERNAL_H

#include "nd.h"

/* One glyph, 7 rows of 5 bits (bit 4 = leftmost). Characters outside
 * 32..126 map to the blank glyph. */
const UINT8 *nd_font_rows(UINT8 ch);

/* Fill `count` pixels starting at (x,y) without any clipping test; the
 * caller has already intersected the span with the scissor rectangle and
 * the surface bounds. Used by the text and sprite paths, where running the
 * public per-pixel entry point would redo that work for every pixel. */
void nd_span_raw(nd_surface_t *s, INT32 x, INT32 y, INT32 count,
                 nd_color_t c);

/* True when (x,y) is inside the surface and its scissor rectangle. */
int nd_visible(const nd_surface_t *s, INT32 x, INT32 y);

/* Blend one already-clipped run of pixels. */
void nd_span_blend_raw(nd_surface_t *s, INT32 x, INT32 y, INT32 count,
                       nd_color_t c, UINT8 alpha);

#endif /* NOVUS_ND_INTERNAL_H */
