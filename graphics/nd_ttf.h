/**
 * Novus OS - TrueType / OpenType font loader (M12).
 *
 * The desktop ships with a bitmap Unicode font (/Novus/fonts/font.bin).
 * That table is fixed at build time: adding a glyph means regenerating a
 * 3.5 MiB blob with a host tool. This module instead reads a real
 * TrueType outline font from the system tree at boot (/Novus/font, files
 * ending in .ttf, .otf or .ttc) and rasterises glyphs on demand into the
 * very same 16x16 packed 1bpp cell the text renderer already consumes, so
 * a user drops a font file into the folder and the next boot uses it.
 *
 * Scope, stated honestly:
 *   - glyf outlines only (TrueType). OpenType fonts whose outlines live in
 *     the CFF table are detected and rejected with a clear message, because
 *     rasterising CFF means a Type 2 charstring interpreter, which is a
 *     different project.
 *   - .ttc collections are accepted: the first usable face is used.
 *   - cmap formats 4 (BMP) and 12 (full Unicode).
 *   - Simple and composite glyphs, with one level of 1x/2x integer
 *     translation per component; component scaling is approximated by
 *     identity, which is what CJK faces use anyway.
 *   - No hinting, no kerning, no variable-font deltas: the default instance
 *     of a variable font is used as-is.
 */
#ifndef NOVUS_ND_TTF_H
#define NOVUS_ND_TTF_H

#include "nd.h"

/* Rasterise one codepoint at a 16 pixel em. Returns the 32-byte packed cell
 * (16 rows of two bytes, MSB leftmost) or NULL when the font has no glyph
 * for it. `*wide` is 8 or 16, matching the advance the renderer must use. */
const UINT8 *nd_ttf_glyph(UINT32 cp, UINT32 *wide);

/* Antialiased companion: 16 rows of 16 4-bit coverage levels (128 bytes,
 * row-major, high nibble is the even column), 0 = no ink, 15 = full. Same
 * cache and same `*wide` rule as nd_ttf_glyph. */
/* Coverage map for `cp` rasterised at `px` pixels per em (clamped to
 * [16, 48]): px rows of px 4-bit nibbles, high nibble first. `*wide` gets
 * the cell width that glyph's advance justifies. */
const UINT8 *nd_ttf_glyph_gray(UINT32 cp, UINT32 px, UINT32 *wide);

/* Pixel advance for `cp`, or 0 when the font has no glyph for it. */
UINT32 nd_ttf_advance(UINT32 cp);

/* Load a face from an already-open file image. `name` is only used for log
 * lines. Returns 0 on success, and on failure leaves any previously loaded
 * face in place. */
int nd_ttf_load_image(const UINT8 *data, UINT64 size, const char *name);

int  nd_ttf_active(void);

/* Face description for logs and for the font folder listing. */
const char *nd_ttf_family(void);
UINT32 nd_ttf_glyph_count(void);

#endif /* NOVUS_ND_TTF_H */
