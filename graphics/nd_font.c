/**
 * Novus OS - the built-in NovusDisplay font (M5).
 *
 * A 5x7 font in a 6x9 cell: readable at 1x on a 320x200 fallback screen,
 * and at 2x on a 1280x800 framebuffer. The shapes are drawn here rather
 * than loaded from firmware or a font file, so the same text looks the
 * same on every machine and M5 has no external dependency at all.
 *
 * Encoding: 7 bytes per glyph, one per row, bit 4 is the leftmost of the
 * five columns. Printable ASCII only (32..126).
 */

#include "nd_internal.h"
#include "nd_font_uni.h"
#include "nd_lang.h"
#include "nd_ttf.h"
#include "heap.h"
#include "../fs/novusfs/novusfs.h"
#include "../kernel/nvus_paths.h"
#include <nd_font_fallback.h>

static const UINT8 g_font[95][ND_GLYPH_H] = {
    /* 32 ' ' */ {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    /* 33 '!' */ {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04},
    /* 34 '"' */ {0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00},
    /* 35 '#' */ {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A},
    /* 36 '$' */ {0x04, 0x0F, 0x14, 0x0E, 0x05, 0x1E, 0x04},
    /* 37 '%' */ {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03},
    /* 38 '&' */ {0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D},
    /* 39 ''' */ {0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00},
    /* 40 '(' */ {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02},
    /* 41 ')' */ {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08},
    /* 42 '*' */ {0x00, 0x0A, 0x04, 0x1F, 0x04, 0x0A, 0x00},
    /* 43 '+' */ {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00},
    /* 44 ',' */ {0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x08},
    /* 45 '-' */ {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00},
    /* 46 '.' */ {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04},
    /* 47 '/' */ {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10},
    /* 48 '0' */ {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
    /* 49 '1' */ {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    /* 50 '2' */ {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    /* 51 '3' */ {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    /* 52 '4' */ {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    /* 53 '5' */ {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    /* 54 '6' */ {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},
    /* 55 '7' */ {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    /* 56 '8' */ {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    /* 57 '9' */ {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    /* 58 ':' */ {0x00, 0x00, 0x04, 0x00, 0x00, 0x04, 0x00},
    /* 59 ';' */ {0x00, 0x00, 0x04, 0x00, 0x00, 0x04, 0x08},
    /* 60 '<' */ {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02},
    /* 61 '=' */ {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00},
    /* 62 '>' */ {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08},
    /* 63 '?' */ {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},
    /* 64 '@' */ {0x0E, 0x11, 0x01, 0x0D, 0x15, 0x15, 0x0E},
    /* 65 'A' */ {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
    /* 66 'B' */ {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},
    /* 67 'C' */ {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},
    /* 68 'D' */ {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C},
    /* 69 'E' */ {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},
    /* 70 'F' */ {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
    /* 71 'G' */ {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F},
    /* 72 'H' */ {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
    /* 73 'I' */ {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},
    /* 74 'J' */ {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C},
    /* 75 'K' */ {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11},
    /* 76 'L' */ {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
    /* 77 'M' */ {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},
    /* 78 'N' */ {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11},
    /* 79 'O' */ {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    /* 80 'P' */ {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},
    /* 81 'Q' */ {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D},
    /* 82 'R' */ {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},
    /* 83 'S' */ {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E},
    /* 84 'T' */ {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
    /* 85 'U' */ {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    /* 86 'V' */ {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},
    /* 87 'W' */ {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11},
    /* 88 'X' */ {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},
    /* 89 'Y' */ {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04},
    /* 90 'Z' */ {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F},
    /* 91 '[' */ {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E},
    /* 92 '\' */ {0x10, 0x08, 0x08, 0x04, 0x02, 0x02, 0x01},
    /* 93 ']' */ {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E},
    /* 94 '^' */ {0x04, 0x0A, 0x11, 0x00, 0x00, 0x00, 0x00},
    /* 95 '_' */ {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F},
    /* 96 '`' */ {0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00},
    /* 97 'a' */ {0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F},
    /* 98 'b' */ {0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x1E},
    /* 99 'c' */ {0x00, 0x00, 0x0E, 0x11, 0x10, 0x11, 0x0E},
    /* 100 'd' */ {0x01, 0x01, 0x0F, 0x11, 0x11, 0x11, 0x0F},
    /* 101 'e' */ {0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E},
    /* 102 'f' */ {0x06, 0x09, 0x08, 0x1C, 0x08, 0x08, 0x08},
    /* 103 'g' */ {0x00, 0x0F, 0x11, 0x11, 0x0F, 0x01, 0x0E},
    /* 104 'h' */ {0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x11},
    /* 105 'i' */ {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E},
    /* 106 'j' */ {0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0C},
    /* 107 'k' */ {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12},
    /* 108 'l' */ {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},
    /* 109 'm' */ {0x00, 0x00, 0x1A, 0x15, 0x15, 0x15, 0x15},
    /* 110 'n' */ {0x00, 0x00, 0x1E, 0x11, 0x11, 0x11, 0x11},
    /* 111 'o' */ {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E},
    /* 112 'p' */ {0x00, 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10},
    /* 113 'q' */ {0x00, 0x0F, 0x11, 0x11, 0x0F, 0x01, 0x01},
    /* 114 'r' */ {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10},
    /* 115 's' */ {0x00, 0x00, 0x0F, 0x10, 0x0E, 0x01, 0x1E},
    /* 116 't' */ {0x08, 0x08, 0x1C, 0x08, 0x08, 0x09, 0x06},
    /* 117 'u' */ {0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0D},
    /* 118 'v' */ {0x00, 0x00, 0x11, 0x11, 0x11, 0x0A, 0x04},
    /* 119 'w' */ {0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0A},
    /* 120 'x' */ {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11},
    /* 121 'y' */ {0x00, 0x11, 0x11, 0x11, 0x0F, 0x01, 0x0E},
    /* 122 'z' */ {0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F},
    /* 123 '{' */ {0x02, 0x04, 0x04, 0x08, 0x04, 0x04, 0x02},
    /* 124 '|' */ {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
    /* 125 '}' */ {0x08, 0x04, 0x04, 0x02, 0x04, 0x04, 0x08},
    /* 126 '~' */ {0x00, 0x00, 0x08, 0x15, 0x02, 0x00, 0x00},
};

const UINT8 *nd_font_rows(UINT8 ch) {
  if (ch < 32u || ch > 126u) {
    return g_font[0]; /* blank, not a garbage glyph */
  }
  return g_font[ch - 32u];
}

/* ---- UTF-8 ------------------------------------------------------------ */

/* The caller owns the byte cursor: successful decoding advances by one
 * complete UTF-8 sequence, while malformed input consumes one byte and
 * returns U+FFFD so a bad byte cannot make a text walk loop forever.
 *
 * Strictness matters now that every codepoint can be rendered: overlong
 * forms, UTF-16 surrogates and anything above U+10FFFF are rejected rather
 * than turned into a glyph or a width that no font table agrees on. */
UINT32 nd_utf8_next(const char **text) {
  const UINT8 *p = (const UINT8 *)*text;
  UINT32       cp;
  UINT32       extra;
  UINT32       min;
  UINT32       i;

  if (p == 0 || p[0] == 0u) {
    return 0;
  }
  if (p[0] < 0x80u) {
    *text += 1;
    return p[0];
  }
  if ((p[0] & 0xE0u) == 0xC0u) {
    cp = (UINT32)(p[0] & 0x1Fu);
    extra = 1u;
    min = 0x80u;
  } else if ((p[0] & 0xF0u) == 0xE0u) {
    cp = (UINT32)(p[0] & 0x0Fu);
    extra = 2u;
    min = 0x800u;
  } else if ((p[0] & 0xF8u) == 0xF0u) {
    cp = (UINT32)(p[0] & 0x07u);
    extra = 3u;
    min = 0x10000u;
  } else {
    *text += 1;
    return 0xFFFDu; /* stray continuation byte or 0xF8..0xFF */
  }
  for (i = 1; i <= extra; i++) {
    if ((p[i] & 0xC0u) != 0x80u) {
      *text += 1;
      return 0xFFFDu; /* truncated sequence */
    }
    cp = (cp << 6) | (UINT32)(p[i] & 0x3Fu);
  }
  if (cp < min || cp > 0x10FFFFu ||
      (cp >= 0xD800u && cp <= 0xDFFFu)) {
    *text += 1;
    return 0xFFFDu; /* overlong, surrogate or out of range */
  }
  *text += (INT32)(extra + 1u);
  return cp;
}

/* Encode one codepoint; returns the byte count, or 0 when `cp` is not a
 * Unicode scalar value. The caller provides at least four bytes. */
UINT32 nd_utf8_encode(UINT32 cp, char out[4]) {
  if (cp < 0x80u) {
    out[0] = (char)cp;
    return 1u;
  }
  if (cp < 0x800u) {
    out[0] = (char)(0xC0u | (cp >> 6));
    out[1] = (char)(0x80u | (cp & 0x3Fu));
    return 2u;
  }
  if (cp < 0x10000u) {
    if (cp >= 0xD800u && cp <= 0xDFFFu) {
      return 0u;
    }
    out[0] = (char)(0xE0u | (cp >> 12));
    out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[2] = (char)(0x80u | (cp & 0x3Fu));
    return 3u;
  }
  if (cp <= 0x10FFFFu) {
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4u;
  }
  return 0u;
}

/* ---- glyph lookup and metrics ----------------------------------------- */

static UINT32 popcount8(UINT8 v) {
  v = (UINT8)(v - ((v >> 1) & 0x55u));
  v = (UINT8)((v & 0x33u) + ((v >> 2) & 0x33u));
  return (UINT32)((v + (v >> 4)) & 0x0Fu);
}

/* Glyph bitmap data, loaded at boot from /Novus/fonts/font.bin in the system partition
 * (see nd_font_load_from). NULL until that happens, which only affects the
 * non-ASCII faces; the ASCII 5x7 face is embedded and always available. */
static const UINT8 *g_font_data = NULL;

void nd_font_set_data(const UINT8 *data) {
  g_font_data = data;
}

/* Read /Novus/fonts/font.bin from an already-mounted NovusFS instance and cache it in a
 * kernel buffer. Called during the boot-stage mount window (the live root)
 * and again for an installed GPT system partition. Returns 0 once loaded. */
int nd_font_load_from(struct novusfs_fs *fs) {
  UINT32 ino;
  UINT64 size;
  UINT16 mode;
  UINT8  *buf;
  INT64  n;

  if (g_font_data != NULL) {
    return 0;
  }
  if (fs == NULL) {
    kputs("[FONT] no system filesystem: embedded UI glyphs active\n");
    return -1;
  }
  ino = novusfs_lookup_path(fs, NOVUSFS_ROOT_INO, NVUS_FONT_FILE);
  if (ino == 0) {
    kputs("[FONT] /Novus/fonts/font.bin missing in system partition\n");
    return -1;
  }
  if (novusfs_stat(fs, ino, &size, &mode) != 0 || size == 0) {
    kputs("[FONT] stat /Novus/fonts/font.bin failed\n");
    return -1;
  }
  if (size != ND_FONT_DATA_SIZE) {
    kputs("[FONT] invalid font size: using embedded UI glyphs\n");
    return -1;
  }
  buf = (UINT8 *)kmalloc(size);
  if (buf == NULL) {
    kputs("[FONT] alloc for /Novus/fonts/font.bin failed\n");
    return -1;
  }
  n = novusfs_read(fs, ino, 0, buf, size);
  if (n < 0 || (UINT64)n != size) {
    kputs("[FONT] read /Novus/fonts/font.bin failed\n");
    kfree(buf);
    return -1;
  }
  g_font_data = buf;
  kputs("[FONT] loaded ");
  kputs_hex(size);
  kputs(" bytes from system partition\n");
  return 0;
}

/* Antialiased variant of nd_glyph_find: returns the 16-row 4-bit coverage
 * map (128 bytes, row-major, high nibble is the even column) for codepoints
 * the outline face covers, or NULL so the caller falls back to the packed
 * 1bpp cell. Only the outline path has sub-pixel coverage; the shipped
 * bitmap table has none by definition. */
/* Pick the raster cell for a text scale: one em is ND_CJK_W pixels, and
 * larger text is rasterised larger rather than magnified, up to three times
 * the base cell. A scale that is not a clean multiple of that falls back to
 * plain magnification of the 16 pixel raster.
 * `*px` is the cell in pixels, `*blk` how many pixels each coverage cell
 * covers on screen. */
const UINT8 *nd_glyph_gray(UINT32 cp, UINT32 scale, UINT32 *px,
                           UINT32 *blk) {
  UINT32 k;
  UINT32 b;
  UINT32 cell;

  if (scale == 0u) {
    scale = 1u;
  }
  /* Largest cell multiplier the cache holds that divides the scale, so
   * text is rasterised as close to its final size as possible: 1 -> 16 px,
   * 2 -> 32 px, 4 -> 32 px at 2x, 6 -> 48 px at 2x. */
  k = 1u;
  if ((scale & 1u) == 0u && scale >= 6u) {
    k = 3u;
  } else if ((scale & 1u) == 0u) {
    k = 2u;
  } else if (scale >= 3u) {
    k = 3u;
  }
  if (k > scale) {
    k = scale;
  }
  b = scale / k;
  cell = ND_CJK_W * k;
  if (cp >= 0x20u && nd_ttf_active() != 0) {
    UINT32       wide = cell;
    const UINT8 *gray = nd_ttf_glyph_gray(cp, cell, &wide);

    if (gray != 0) {
      if (px != 0) {
        *px = cell;
      }
      if (blk != 0) {
        *blk = b;
      }
      return gray;
    }
  }
  return 0;
}

const UINT8 *nd_glyph_find(UINT32 cp, UINT32 *wide) {
  UINT16       slot;

  /* A TrueType face dropped into /Novus/font wins over the shipped bitmap
   * table: the user put it there precisely to change how text looks. Any
   * codepoint the face does not cover falls through to the bitmap paths
   * below, so a Latin-only face does not lose CJK. */
  if (cp >= 0x20u && nd_ttf_active() != 0) {
    const UINT8 *rows = nd_ttf_glyph(cp, wide);

    if (rows != 0) {
      return rows;
    }
  }

  if (g_font_data == NULL) {
    UINT32 lo = 0, hi = sizeof(g_fallback_font) / sizeof(g_fallback_font[0]);
    while (lo < hi) {
      UINT32 mid = lo + (hi - lo) / 2;
      if (cp < g_fallback_font[mid].cp) hi = mid;
      else if (cp > g_fallback_font[mid].cp) lo = mid + 1;
      else {
        if (wide != 0) *wide = g_fallback_font[mid].wide;
        return g_fallback_font[mid].bits;
      }
    }
    return 0;
  }
  UINT32       byte;
  UINT32       bit;
  UINT32       rank;
  UINT32       i;
  const UINT8 *valid;

  if (cp < 0x80u || cp > 0x10FFFFu) {
    return 0;
  }
  slot = g_uni_page[cp >> 8];
  if (slot == ND_UNI_EMPTY) {
    return 0;
  }
  byte = (cp & 0xFFu) >> 3;
  bit = cp & 7u;
  valid = &g_uni_valid[(UINT32)slot * 32u];
  if ((valid[byte] & (UINT8)(1u << bit)) == 0u) {
    return 0;
  }
  rank = 0;
  for (i = 0; i < byte; i++) {
    rank += popcount8(valid[i]);
  }
  rank += popcount8((UINT8)(valid[byte] & (UINT8)((1u << bit) - 1u)));
  if (wide != 0) {
    *wide = ((g_uni_wide[(UINT32)slot * 32u + byte] & (UINT8)(1u << bit)) != 0u)
                ? ND_CJK_ADV
                : 8u;
  }
  return g_font_data + (g_uni_base[slot] + rank) * ND_UNI_GLYPH_BYTES;
}

/* Binary search over the generated advance ranges. The table starts at
 * U+0080; ASCII keeps its own 5x7 face and 6 pixel cell. */
UINT32 nd_uni_adv(UINT32 cp) {
  UINT32 lo = 0;
  UINT32 hi = g_uni_width_count;
  UINT32 ttf;

  if (cp < 0x20u) {
    return 0u;
  }
  /* The advance must come from whatever face actually draws the glyph,
   * otherwise a loaded TTF renders into a column width taken from the
   * bitmap table and the text drifts apart. ASCII is no exception: the
   * 6 pixel monospace cell made digits and letters narrower and shorter
   * than the hanzi they sit next to. */
  ttf = nd_ttf_advance(cp);
  if (ttf != 0u) {
    return ttf;
  }
  if (cp < 0x80u) {
    return ND_FONT_ADV;
  }
  while (lo < hi) {
    UINT32 mid = (lo + hi) / 2u;

    if (cp < g_uni_width[mid].start) {
      hi = mid;
    } else if (cp > g_uni_width[mid].end) {
      lo = mid + 1u;
    } else {
      return g_uni_width[mid].adv;
    }
  }
  return 8u;
}

UINT32 nd_char_adv(UINT32 cp, UINT32 scale) {
  if (scale == 0u) {
    scale = 1u;
  }
  return nd_uni_adv(cp) * scale;
}

UINT32 nd_text_width(const char *text, UINT32 scale) {
  const char *p = nd_lang_tr(text);
  UINT32      w = 0;

  if (text == 0 || scale == 0u) {
    return 0;
  }
  while (p != 0 && *p != '\0') {
    UINT32 cp = nd_utf8_next(&p);

    w += nd_char_adv(cp, scale);
  }
  return w;
}

UINT32 nd_text_width_n(const char *text, UINT32 bytes, UINT32 scale) {
  const char *p = text;
  const char *end;
  UINT32      w = 0;

  if (text == 0 || scale == 0u) {
    return 0;
  }
  end = text + bytes;
  while (p < end && *p != '\0') {
    UINT32 cp = nd_utf8_next(&p);

    if (p > end) {
      break; /* the range ended inside a sequence: stop before it */
    }
    w += nd_char_adv(cp, scale);
  }
  return w;
}

/* Height is chosen for the whole line: any codepoint the TrueType face can
 * actually draw selects the 16 pixel cell, so mixed Latin/CJK text shares
 * one baseline and one cap height. Only text that still falls back to the
 * built-in 5x7 cells stays on the short line. Combining marks advance by
 * zero and never raise the line on their own. */
UINT32 nd_text_height(const char *text, UINT32 scale) {
  const char *p = nd_lang_tr(text);
  UINT32      h = (scale != 0u) ? ND_GLYPH_H * scale : ND_GLYPH_H;
  UINT32      uni = (scale != 0u) ? ND_CJK_H * scale : ND_CJK_H;

  if (p == 0) {
    return h;
  }
  while (p != 0 && *p != '\0') {
    UINT32 cp = nd_utf8_next(&p);

    if (cp < 0x20u) {
      continue;
    }
    if (nd_uni_adv(cp) != 0u) {
      if (cp >= 0x80u || (nd_ttf_active() != 0 && nd_ttf_advance(cp) != 0u)) {
        return (h > uni) ? h : uni;
      }
    }
  }
  return h;
}

UINT32 nd_font_height(UINT32 scale) { return (scale != 0u) ? ND_GLYPH_H * scale : 0u; }

/* ---- TrueType faces from the system tree -------------------------------- */

#define FONT_CAND_MAX 8u

typedef struct {
  char   name[FONT_CAND_MAX][NOVUSFS_NAME_MAX];
  UINT32 n;
} font_cands_t;

static UINT8 *g_ttf_file; /* the face's bytes: owned here, never freed */

static char font_lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int font_is_sfnt(const char *name) {
  UINT32 len = 0;
  const char *ext;

  while (name[len] != '\0') {
    len++;
  }
  if (len < 5u) {
    return 0;
  }
  ext = name + len - 4u;
  if (ext[0] != '.') {
    return 0;
  }
  if (font_lower(ext[1]) != 't') {
    return 0;
  }
  if (font_lower(ext[2]) == 't' && font_lower(ext[3]) == 'f') {
    return 1; /* .ttf */
  }
  if (font_lower(ext[2]) == 't' && font_lower(ext[3]) == 'c') {
    return 1; /* .ttc */
  }
  if (font_lower(ext[2]) == 'o' && font_lower(ext[3]) == 'f') {
    return 1; /* .otf */
  }
  return 0;
}

static void font_collect(void *ctx, UINT32 ino, const char *name) {
  font_cands_t *c = (font_cands_t *)ctx;
  UINT32        i = 0;

  (void)ino;
  if (c->n >= FONT_CAND_MAX || font_is_sfnt(name) == 0) {
    return;
  }
  while (name[i] != '\0' && i + 1u < NOVUSFS_NAME_MAX) {
    c->name[c->n][i] = name[i];
    i++;
  }
  c->name[c->n][i] = '\0';
  c->n++;
}

static int font_load_one(struct novusfs_fs *fs, const char *dir,
                         const char *name) {
  char   path[96];
  UINT32 ino;
  UINT64 size = 0;
  UINT16 mode = 0;
  UINT8 *buf;
  INT64  n;
  UINT32 k = 0;
  UINT32 i = 0;
  int    rc;

  while (dir[k] != '\0' && k + 1u < sizeof(path)) {
    path[k] = dir[k];
    k++;
  }
  if (k > 0u && k + 1u < sizeof(path) && path[k - 1u] != '/') {
    path[k++] = '/';
  }
  while (name[i] != '\0' && k + 1u < sizeof(path)) {
    path[k++] = name[i++];
  }
  path[k] = '\0';

  ino = novusfs_lookup_path(fs, NOVUSFS_ROOT_INO, path);
  if (ino == 0) {
    return -1;
  }
  if (novusfs_stat(fs, ino, &size, &mode) != 0 || size < 64u ||
      size > (32u * 1024u * 1024u)) {
    kputs("[FONT] ");
    kputs(path);
    kputs(": size out of range, skipped\n");
    return -1;
  }
  buf = (UINT8 *)kmalloc(size);
  if (buf == NULL) {
    kputs("[FONT] ");
    kputs(path);
    kputs(": no memory for ");
    kputs_hex(size);
    kputs(" bytes\n");
    return -1;
  }
  n = novusfs_read(fs, ino, 0, buf, size);
  if (n < 0 || (UINT64)n != size) {
    kputs("[FONT] ");
    kputs(path);
    kputs(": read failed\n");
    kfree(buf);
    return -1;
  }
  rc = nd_ttf_load_image(buf, size, name);
  if (rc != 0) {
    kfree(buf);
    return -1;
  }
  if (g_ttf_file != NULL) {
    kfree(g_ttf_file); /* replace the previous face */
  }
  g_ttf_file = buf;
  kputs("[FONT] outline face ");
  kputs(name);
  kputs(": ");
  kputs_hex(size);
  kputs(" bytes, ");
  kputs_hex(nd_ttf_glyph_count());
  kputs(" glyphs, ");
  kputs_hex(ND_CJK_ADV);
  kputs("px cell\n");
  /* Boot-time proof that the outline path really produces ink: rasterise
   * one Han glyph and one Latin glyph through the same call the renderer
   * uses, and report the number of set bits in each cell. A zero here
   * means the face loaded but nothing would be drawn. */
  {
    static const UINT32 probes[2] = {0x4E2Du /* 中 */, 0x41u /* A */};
    UINT32 p;

    for (p = 0; p < 2u; p++) {
      UINT32 wide = 0u;
      const UINT8 *cell = nd_ttf_glyph(probes[p], &wide);
      UINT32 ink = 0u;
      UINT32 i;

      if (cell == NULL) {
        continue;
      }
      for (i = 0; i < 32u; i++) {
        UINT8 v = cell[i];

        while (v != 0u) {
          ink += (v & 1u);
          v = (UINT8)(v >> 1);
        }
      }
      kputs("[FONT] self-check U+");
      kputs_hex(probes[p]);
      kputs(": ");
      kputs_hex(wide);
      kputs("px advance, ");
      kputs_hex(ink);
      kputs(" covered pixels\n");
    }
  }
  return 0;
}

/* Scan the drop folder (and the older fonts folder) for a usable outline
 * face. Files are tried in directory order: the first one that parses wins,
 * which is why a bad download does not disable the whole feature. */
int nd_font_load_ttf(struct novusfs_fs *fs) {
  static font_cands_t cands;
  static const char  *dirs[2] = {NVUS_FONT_DIR, "/Novus/fonts"};
  UINT32              d;
  UINT32              i;

  if (fs == NULL) {
    return -1;
  }
  for (d = 0; d < 2u; d++) {
    UINT32 dir_ino = novusfs_lookup_path(fs, NOVUSFS_ROOT_INO, dirs[d]);

    if (dir_ino == 0) {
      continue;
    }
    cands.n = 0;
    (void)novusfs_dir_readdir(fs, dir_ino, font_collect, &cands);
    if (cands.n == 0u) {
      if (d == 0u) {
        kputs("[FONT] " NVUS_FONT_DIR ": no .ttf/.otf present, bitmap font in use\n");
      }
      continue;
    }
    for (i = 0; i < cands.n; i++) {
      if (font_load_one(fs, dirs[d], cands.name[i]) == 0) {
        return 0;
      }
    }
  }
  return -1;
}
