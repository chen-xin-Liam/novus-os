/**
 * Novus OS - full-Unicode font index (generated).
 *
 * Data lives in nd_font_uni.c (index) and nd_font_data.bin (glyphs).
 */

#ifndef NOVUS_ND_FONT_UNI_H
#define NOVUS_ND_FONT_UNI_H

#include "uefi.h"

#define ND_UNI_EMPTY 0xFFFFu
#define ND_UNI_GLYPH_BYTES 32u

typedef struct {
  UINT32 start;
  UINT32 end;
  UINT8  adv;
} nd_uni_range_t;

extern const UINT16 g_uni_page_count;
extern const UINT16 g_uni_page[];
extern const UINT16 g_uni_slot_count;
extern const UINT32 g_uni_base[];
extern const UINT8  g_uni_valid[];
extern const UINT8  g_uni_wide[];
extern const UINT16 g_uni_width_count;
extern const nd_uni_range_t g_uni_width[];

/* Glyph bitmap data. Historically incbin'd into NOVUS.EFI by
 * nd_font_blob.S; now loaded at boot from the system partition (/font.bin)
 * into a kernel buffer and pointed at here. NULL until loaded, in which
 * case nd_glyph_find() returns NULL and callers fall back to ASCII. */
struct novusfs_fs;
void nd_font_set_data(const UINT8 *data);
int  nd_font_load_from(struct novusfs_fs *fs);

/* 32 glyph bytes (16 rows, big-endian) or NULL. *wide is 16 or 8. */
const UINT8 *nd_glyph_find(UINT32 cp, UINT32 *wide);

/* Advance in pixels at 1x: 0 for combining marks, 6 for printable ASCII,
 * 8 or 16 for everything else. */
UINT32 nd_uni_adv(UINT32 cp);

#endif /* NOVUS_ND_FONT_UNI_H */
