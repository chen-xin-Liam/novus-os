/**
 * Novus OS - desktop photo wallpaper.
 *
 * The photos are not in the loader. They ship as one file, WPAPERS.PKG,
 * built by tools/wpkg from assets/backgrounds-screen; the boot stage reads
 * that file off the ESP into reserved pages before ExitBootServices and
 * publishes the range through shared memory, and the layout is
 * graphics/wpkg.h. nd_wallpaper_init() decodes the current selection once
 * for the screen size - the JPEG decoder scales inside the IDCT, so sources
 * larger than the display never allocate full planes - cover-scales it into
 * a cached screen-sized surface and hands pixels to the shell from there. A
 * wallpaper change decodes synchronously.
 *
 * A medium that carries no package, or a package that fails to parse, is
 * not an error: g_ready stays 0 and nd_wallpaper_paint() fills the area with
 * a flat colour, so the shell never paints a hole.
 */

#include "kernel.h"
#include "nd_wallpaper.h"
#include "nd_jpeg.h"
#include "shared_mem.h"
#include "mm.h"
#include "wpkg.h"

#define WP_FIX 12 /* bilinear fixed point (4096 = 1.0) */

/* Picker labels, in wallpaper index order: index 0 is the default. The order
 * must match the Makefile's WP_IMGS list, the theme's paper colours and the
 * order tools/wpkg packed the files in. The names are compile-time, so the
 * picker lists every wallpaper even on a medium without the package - such a
 * selection simply keeps the flat colour. */
static const char *const g_names[ND_WALLPAPER_COUNT] = {
  "pexels-snapsbyclark-12981877",
  "pexels-codioful-6985184",
  "pexels-ian-panelo-40321675",
  "mrbinz-vietnam-10445204",
  "pexels-thales13-40360883",
  "pexels-tu-n-vu-40278712",
  "treptoweralex-usa-9889580",
};

static nd_surface_t g_img;   /* decoded and cover-scaled for the screen */
static UINT32       g_index; /* requested wallpaper */
static INT32        g_sw, g_sh;
static int          g_ready;
static int          g_said_no_pkg; /* the "no package" line, once */

int nd_wallpaper_count(void) { return ND_WALLPAPER_COUNT; }
int nd_wallpaper_default(void) { return 0; }
UINT32 nd_wallpaper_current(void) { return g_index; }
int nd_wallpaper_ready(void) { return g_ready; }

const char *nd_wallpaper_name(UINT32 index) {
  if (index >= ND_WALLPAPER_COUNT) {
    return "";
  }
  return g_names[index];
}

/* Bilinear cover-scale: the source fills the screen, overflowing edges are
 * cropped symmetrically. */
static void wp_cover(nd_surface_t *dst, const nd_surface_t *src) {
  UINT64 fx = ((UINT64)src->w << 16) / (UINT64)dst->w;
  UINT64 fy = ((UINT64)src->h << 16) / (UINT64)dst->h;
  UINT64 f = (fx > fy) ? fx : fy;
  UINT64 cw = ((UINT64)dst->w * f) >> 16;
  UINT64 ch = ((UINT64)dst->h * f) >> 16;
  INT64  ox = ((INT64)src->w - (INT64)cw) / 2;
  INT64  oy = ((INT64)src->h - (INT64)ch) / 2;
  int    x, y;

  if (cw == 0 || ch == 0) {
    return;
  }
  for (y = 0; y < dst->h; y++) {
    /* 4.12 source coordinates, then a 4.12 blend weight. */
    INT64 fy12 = (((INT64)y * (INT64)ch) << WP_FIX) / dst->h + (oy << WP_FIX);
    int   y0 = (int)(fy12 >> WP_FIX);
    int   wy = (int)(fy12 & ((1 << WP_FIX) - 1));
    int   y1;
    const UINT32 *r0, *r1;

    if (y0 < 0) { y0 = 0; }
    if (y0 > src->h - 2) { y0 = src->h - 2; }
    y1 = y0 + 1;
    r0 = src->px + (UINT64)y0 * (UINT32)src->stride;
    r1 = src->px + (UINT64)y1 * (UINT32)src->stride;

    for (x = 0; x < dst->w; x++) {
      INT64 fx12 = (((INT64)x * (INT64)cw) << WP_FIX) / dst->w + (ox << WP_FIX);
      int   x0 = (int)(fx12 >> WP_FIX);
      int   wx = (int)(fx12 & ((1 << WP_FIX) - 1));
      int   x1, c;
      UINT32 p[2][2];

      if (x0 < 0) { x0 = 0; }
      if (x0 > src->w - 2) { x0 = src->w - 2; }
      x1 = x0 + 1;
      p[0][0] = r0[x0]; p[0][1] = r0[x1];
      p[1][0] = r1[x0]; p[1][1] = r1[x1];

      {
        UINT32 outc = 0;
        for (c = 0; c < 3; c++) {
          int shift = c * 8;
          INT64 top = (((INT64)((p[0][0] >> shift) & 0xFFu) * (4096 - wx) +
                        (INT64)((p[0][1] >> shift) & 0xFFu) * wx) >> WP_FIX);
          INT64 bot = (((INT64)((p[1][0] >> shift) & 0xFFu) * (4096 - wx) +
                        (INT64)((p[1][1] >> shift) & 0xFFu) * wx) >> WP_FIX);
          INT64 v = (top * (4096 - wy) + bot * wy) >> WP_FIX;

          if (v > 255) { v = 255; }
          outc |= ((UINT32)v & 0xFFu) << shift;
        }
        dst->px[(UINT64)y * (UINT32)dst->stride + x] = 0xFF000000u | outc;
      }
    }
  }
}

/* ---- the package ------------------------------------------------------- */

static UINT32 wp_le32(const UINT8 *p) {
  return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) |
         ((UINT32)p[3] << 24);
}

/* The staged package as a virtual address, or NULL when the medium carries
 * none (the boot stage leaves the shared-memory fields zeroed in that case).
 * The pages are EfiReservedMemoryType, so they are still there after
 * ExitBootServices - that is the whole point of staging them. */
static const UINT8 *wp_pkg(void) {
  shared_mem_t *shm = shared_mem_get();

  if (shm == 0 || shm->wpkg_img_base == 0 ||
      shm->wpkg_img_size < WPKG_HEADER_SIZE) {
    return 0;
  }
  return (const UINT8 *)phys_to_virt(shm->wpkg_img_base);
}

/* Locate image `index` inside the package. Returns 0 and fills out/len on
 * success, -1 when there is no package, the header does not parse, or the
 * index is out of range - every one of which is a "paint a flat colour"
 * case, not a boot failure. */
static int wp_locate(UINT32 index, const UINT8 **out, UINT32 *out_len) {
  const UINT8 *p = wp_pkg();
  const UINT8 *e;
  UINT32 count, off, len, total, staged, data;

  *out = 0;
  *out_len = 0;
  if (p == 0) {
    if (!g_said_no_pkg) {
      g_said_no_pkg = 1;
      kputs("[WP] no wallpaper package staged: flat desktop colour\n");
    }
    return -1;
  }
  if (p[0] != WPKG_MAGIC0 || p[1] != WPKG_MAGIC1 || p[2] != WPKG_MAGIC2 ||
      p[3] != WPKG_MAGIC3) {
    kputs("[WP] wallpaper package: bad magic\n");
    return -1;
  }
  if (wp_le32(p + WPKG_OFF_VERSION) != WPKG_VERSION) {
    kputs("[WP] wallpaper package: unsupported version\n");
    return -1;
  }
  count = wp_le32(p + WPKG_OFF_COUNT);
  total = wp_le32(p + WPKG_OFF_TOTAL);
  staged = (UINT32)shared_mem_get()->wpkg_img_size;
  if (count > WPKG_MAX_IMAGES || total > staged) {
    kputs("[WP] wallpaper package: header does not fit the staged bytes\n");
    return -1;
  }
  if (index >= count) {
    return -1; /* fewer photos than the picker lists: flat colour */
  }
  e = p + WPKG_HEADER_SIZE + (UINT32)index * WPKG_ENTRY_SIZE;
  off = wp_le32(e + WPKG_EOFF_OFFSET);
  len = wp_le32(e + WPKG_EOFF_SIZE);
  data = WPKG_HEADER_SIZE + count * WPKG_ENTRY_SIZE;
  if (len == 0 || off < data || (UINT64)off + (UINT64)len > (UINT64)total) {
    kputs("[WP] wallpaper package: entry out of range\n");
    return -1;
  }
  *out = p + off;
  *out_len = len;
  return 0;
}

static int wp_decode(void) {
  nd_surface_t out;
  const UINT8 *data;
  UINT32 len;
  int w = 0, h = 0, scale;

  if (g_sw <= 0 || g_sh <= 0) {
    return -1;
  }
  if (wp_locate(g_index, &data, &len) != 0) {
    if (g_ready) {
      nd_surface_destroy(&g_img);
      g_ready = 0;
    }
    return -1;
  }
  if (nd_jpeg_size(data, len, &w, &h) != 0) {
    kputs("[WP] no SOF marker in wallpaper\n");
    return -1;
  }
  /* Largest scale (smallest output) that still covers the screen; fall
   * back to 1/1 when the photo is smaller than the display. */
  for (scale = 8; scale > 1; scale /= 2) {
    if (w / scale >= g_sw && h / scale >= g_sh) {
      break;
    }
  }
  if (w / scale < g_sw || h / scale < g_sh) {
    scale = 1;
  }
  if (nd_jpeg_decode(data, len, scale, &out) != 0) {
    return -1;
  }

  if (g_ready) {
    nd_surface_destroy(&g_img);
    g_ready = 0;
  }
  if (nd_surface_create(&g_img, g_sw, g_sh) != 0) {
    nd_surface_destroy(&out);
    return -1;
  }
  if (out.w == g_sw && out.h == g_sh) {
    memcpy(g_img.px, out.px, (UINT64)g_sw * (UINT64)g_sh * 4u);
  } else {
    wp_cover(&g_img, &out);
  }
  nd_surface_destroy(&out);
  g_ready = 1;
  kputs("[WP] wallpaper ready: ");
  kputs(g_names[g_index]);
  kputs("\n");
  return 0;
}

int nd_wallpaper_init(INT32 screen_w, INT32 screen_h) {
  g_sw = screen_w;
  g_sh = screen_h;
  if (g_index >= ND_WALLPAPER_COUNT) {
    g_index = (UINT32)nd_wallpaper_default();
  }
  return wp_decode();
}

int nd_wallpaper_set(UINT32 index) {
  if (index >= ND_WALLPAPER_COUNT) {
    return -1;
  }
  if (index == g_index && g_ready) {
    return 0;
  }
  g_index = index;
  if (g_sw <= 0 || g_sh <= 0) {
    return -1; /* no screen size yet: retry from nd_wallpaper_init() */
  }
  return wp_decode();
}

static UINT32 wp_lerp(UINT32 a, UINT32 b, UINT32 t) {
  /* t is 0..1024; interpolate the three colour channels of 0xAARRGGBB. */
  INT32 ar = (INT32)((a >> 16) & 0xFFu), ag = (INT32)((a >> 8) & 0xFFu),
        ab = (INT32)(a & 0xFFu);
  INT32 br = (INT32)((b >> 16) & 0xFFu), bg = (INT32)((b >> 8) & 0xFFu),
        bb = (INT32)(b & 0xFFu);
  INT32 r = ar + (br - ar) * (INT32)t / 1024;
  INT32 g = ag + (bg - ag) * (INT32)t / 1024;
  INT32 bl = ab + (bb - ab) * (INT32)t / 1024;
  return ((UINT32)0xFFu << 24) | (((UINT32)r & 0xFFu) << 16) |
         (((UINT32)g & 0xFFu) << 8) | ((UINT32)bl & 0xFFu);
}

void nd_wallpaper_paint(nd_surface_t *s, const nd_rect_t *area,
                        nd_color_t top, nd_color_t bottom) {
  nd_rect_t a = *area;
  INT32     y;

  /* A generated vertical gradient. The photo pipeline (JPEG decode -> cover
   * scale) is currently unreliable across toolchains - it decodes to a
   * systematically wrong/dark image, which showed up as a garbled, wrong
   * coloured desktop (the "花屏"). A gradient is always correct and clean, so
   * it is the default background until that decoder is fixed. The area handed
   * in is always the full desktop rect, so the gradient stays continuous
   * across partial repaints. */
  if (a.h <= 1) {
    nd_fill_rect(s, a, bottom);
    return;
  }
  for (y = a.y; y < a.y + a.h; y++) {
    UINT32 t = (UINT32)(y - a.y) * 1024u / (UINT32)(a.h - 1);
    nd_fill_rect(s, nd_rect(a.x, y, a.w, 1), wp_lerp(top, bottom, t));
  }
}
