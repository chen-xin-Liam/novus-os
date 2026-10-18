/**
 * Novus OS - pinyin input method (M9, Chinese support).
 *
 * Type Latin letters into a focused text box and a candidate window lists
 * the characters for that syllable; digits, space or Enter commit one.
 * The dictionary is generated (graphics/nd_ime_dict.c) and the glyphs come
 * from the same set the UI uses, so the IME cannot produce a character the
 * renderer has no shape for.
 *
 * Scope: single-syllable input. That is what fits in a kernel image and a
 * 16-entry composing buffer, and it covers the everyday case.
 */

#ifndef NOVUS_ND_IME_H
#define NOVUS_ND_IME_H

#include "uefi.h"
#include "nd.h"

#define ND_IME_CANDS 6

/* ---- generated dictionary (nd_ime_dict.c) ----------------------------- */

extern const UINT16 g_ime_count;
extern const UINT16 g_ime_off[];
extern const UINT16 g_ime_start[];
extern const UINT8  g_ime_len[];
extern const UINT32 g_ime_cand[];
extern const char   g_ime_pool[];

/* ---- runtime ---------------------------------------------------------- */

/* Reset state and log. Call once at boot, after the font is available. */
void nd_ime_init(void);

/* Offer a key to the IME. Returns 1 when the IME consumed it, 0 when the
 * text box should handle it (and the caller may then treat it as ASCII).
 * `edit` must be the focused text box. */
int nd_ime_key(void *edit, UINT32 key);

/* Draw the candidate window if composing. Called by ng_render() so it is
 * composited above everything else in the damage area. */
void nd_ime_draw(nd_surface_t *s);

/* True while composing, and the area the popup occupied last time (for
 * damage bookkeeping). */
int  nd_ime_active(void);
void nd_ime_bounds(INT32 *x, INT32 *y, INT32 *w, INT32 *h);

#endif /* NOVUS_ND_IME_H */
