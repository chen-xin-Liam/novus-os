/**
 * Novus OS - CJK glyph table (generated).
 *
 * The table itself lives in nd_font_cjk.c, produced by
 * tools/fontgen/fontgen.py from the UI corpus plus the IME dictionary.
 * Codepoints ascend, so nd_cjk_find() binary searches it.
 */

#ifndef NOVUS_ND_FONT_CJK_H
#define NOVUS_ND_FONT_CJK_H

#include "uefi.h"

#define ND_CJK_W   16
#define ND_CJK_H   16
#define ND_CJK_ADV 16

extern const UINT16 g_cjk_count;
extern const UINT16 g_cjk_cp[];
extern const UINT16 g_cjk_bits[]; /* 16 rows per glyph, MSB = left column */

/* Rows of `cp`, or NULL when the glyph set has no such character. */
const UINT16 *nd_cjk_find(UINT32 cp);

#endif /* NOVUS_ND_FONT_CJK_H */
