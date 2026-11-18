/**
 * Novus OS - ICO decoder and icon cache (M10).
 *
 * Layout of the file this reads (all little endian):
 *
 *   ICONDIR      6 bytes: reserved(0) type(1) count
 *   ICONDIRENTRY 16 bytes each: w h colors res planes bpp bytesInRes offset
 *     w/h are width and height, 0 meaning 256; the payload is either a
 *     BITMAPINFOHEADER DIB or a whole PNG.
 *
 * A DIB payload stores its height doubled (the XOR image plus the 1bpp
 * AND mask that follows it). This decoder takes 32bpp and 24bpp DIB
 * entries, reads the mask when the alpha channel is empty, and bilinear
 * scales the result to the size the shell asked for. PNG entries and SVG
 * are refused with a log line - see nd_icon.h for why.
 */

#include "nd_internal.h"
#include "nd_icon.h"
#include "heap.h"
#include "../fs/novusfs/novusfs.h"
#include "../kernel/nvus_paths.h"

#include <icons_blobs.h>

#define ND_ICON_CACHE    20u
#define ND_ICON_PATH_MAX 48u

static struct novusfs_fs *g_fs;
static int g_said_png;

static UINT16 rd16(const UINT8 *p) { return (UINT16)(p[0] | (p[1] << 8)); }

static UINT32 rd32(const UINT8 *p) {
  return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) |
         ((UINT32)p[3] << 24);
}

static INT32 rds32(const UINT8 *p) { return (INT32)rd32(p); }

/* Built-in set: the same names the shell asks for, generated from the
 * .ico files in assets/icons by the Makefile. sizeof() rather than the
 * generated _len variables, which are not constant expressions. */
typedef struct {
  const char  *name;
  const UINT8 *data;
  UINT32       len;
} nd_icon_blob_t;

static const nd_icon_blob_t g_blobs[] = {
    {"devices", k_ico_devices, (UINT32)sizeof(k_ico_devices)},
    {"files", k_ico_files, (UINT32)sizeof(k_ico_files)},
    {"net", k_ico_net, (UINT32)sizeof(k_ico_net)},
    {"power", k_ico_power, (UINT32)sizeof(k_ico_power)},
    {"search", k_ico_search, (UINT32)sizeof(k_ico_search)},
    {"settings", k_ico_settings, (UINT32)sizeof(k_ico_settings)},
    {"start", k_ico_start, (UINT32)sizeof(k_ico_start)},
    {"tasks", k_ico_tasks, (UINT32)sizeof(k_ico_tasks)},
    {"term", k_ico_term, (UINT32)sizeof(k_ico_term)},
    {"volume", k_ico_volume, (UINT32)sizeof(k_ico_volume)},
    {"wifi", k_ico_wifi, (UINT32)sizeof(k_ico_wifi)},
};

#define ND_ICON_BLOBS (sizeof(g_blobs) / sizeof(g_blobs[0]))

static const nd_icon_blob_t *find_blob(const char *name) {
  UINT32 i;

  for (i = 0; i < ND_ICON_BLOBS; i++) {
    const char *a = g_blobs[i].name;
    const char *b = name;
    while (*a != '\0' && *a == *b) {
      a++;
      b++;
    }
    if (*a == '\0' && *b == '\0') {
      return &g_blobs[i];
    }
  }
  return NULL;
}

/* ---- scaling --------------------------------------------------------- */

static UINT32 lerp_ch(UINT32 a, UINT32 b, UINT32 wa) {
  return (a * wa + b * (256u - wa) + 128u) >> 8;
}

/* Bilinear scale, in premultiplied space so a semi-transparent edge does
 * not pick up the colour of the transparent pixels around it. */
static int scale_icon(const nd_icon_t *src, UINT32 px, nd_icon_t *out) {
  UINT32 *dst;
  UINT32  y;

  dst = (UINT32 *)kmalloc(px * px * sizeof(UINT32));
  if (dst == NULL) {
    return -1;
  }
  for (y = 0; y < px; y++) {
    /* Sample at pixel centres: (y + 0.5) * src / dst - 0.5. */
    UINT32 sy2 = (UINT32)(((UINT64)(y * 2u + 1u) * src->h * 128u) / px);
    UINT32 sy0 = sy2 / 256u;
    UINT32 fy  = sy2 % 256u;
    UINT32 x;

    if (sy0 >= src->h) {
      sy0 = src->h - 1u;
    }
    for (x = 0; x < px; x++) {
      UINT32 sx2 = (UINT32)(((UINT64)(x * 2u + 1u) * src->w * 128u) / px);
      UINT32 sx0 = sx2 / 256u;
      UINT32 fx  = sx2 % 256u;
      UINT32 s00, s10, s01, s11;
      UINT32 a, r, g, b;

      if (sx0 >= src->w) {
        sx0 = src->w - 1u;
      }
      s00 = src->px[sy0 * src->w + sx0];
      s10 = src->px[sy0 * src->w + (sx0 + 1u < src->w ? sx0 + 1u : sx0)];
      s01 = src->px[(sy0 + 1u < src->h ? sy0 + 1u : sy0) * src->w + sx0];
      s11 = src->px[(sy0 + 1u < src->h ? sy0 + 1u : sy0) * src->w +
                    (sx0 + 1u < src->w ? sx0 + 1u : sx0)];

      /* Premultiply: each channel is scaled by its own alpha. */
#define PM(c, al) (((c) * (al)) / 255u)
      {
        UINT32 a00 = (s00 >> 24) & 0xFFu, a10 = (s10 >> 24) & 0xFFu;
        UINT32 a01 = (s01 >> 24) & 0xFFu, a11 = (s11 >> 24) & 0xFFu;

        r = lerp_ch(lerp_ch(PM((s00 >> 16) & 0xFFu, a00), PM((s10 >> 16) & 0xFFu, a10), fx),
                    lerp_ch(PM((s01 >> 16) & 0xFFu, a01), PM((s11 >> 16) & 0xFFu, a11), fx),
                    fy);
        g = lerp_ch(lerp_ch(PM((s00 >> 8) & 0xFFu, a00), PM((s10 >> 8) & 0xFFu, a10), fx),
                    lerp_ch(PM((s01 >> 8) & 0xFFu, a01), PM((s11 >> 8) & 0xFFu, a11), fx),
                    fy);
        b = lerp_ch(lerp_ch(PM(s00 & 0xFFu, a00), PM(s10 & 0xFFu, a10), fx),
                    lerp_ch(PM(s01 & 0xFFu, a01), PM(s11 & 0xFFu, a11), fx),
                    fy);
        a = lerp_ch(lerp_ch(a00, a10, fx), lerp_ch(a01, a11, fx), fy);
      }
#undef PM
      if (a == 0) {
        dst[y * px + x] = 0;
      } else {
        r = (r * 255u) / a;
        g = (g * 255u) / a;
        b = (b * 255u) / a;
        if (r > 255u) r = 255u;
        if (g > 255u) g = 255u;
        if (b > 255u) b = 255u;
        dst[y * px + x] = (a << 24) | (r << 16) | (g << 8) | b;
      }
    }
  }
  out->w = px;
  out->h = px;
  out->px = dst;
  return 0;
}

void nd_icon_free(nd_icon_t *ic) {
  if (ic == NULL) {
    return;
  }
  if (ic->px != NULL) {
    kfree(ic->px);
  }
  ic->px = NULL;
  ic->w = 0;
  ic->h = 0;
}

/* ---- ICO ------------------------------------------------------------- */

static int decode_dib(const UINT8 *img, UINT32 len, UINT32 px, nd_icon_t *out) {
  const UINT8 *head = img;
  const UINT8 *bits;
  const UINT8 *mask;
  nd_icon_t    raw;
  UINT32       hdr_size, w, h, bpp, comp, stride, mask_stride, x, y;
  INT32        height;
  int          use_alpha = 1; /* 32bpp icons normally carry real alpha */
  if (len < 40u) {
    return -1;
  }
  hdr_size = rd32(head);
  if (hdr_size < 40u || hdr_size > len) {
    return -2;
  }
  w = (UINT32)rds32(head + 4);
  height = rds32(head + 8);
  bpp = rd16(head + 14);
  comp = rd32(head + 16);
  if (w == 0 || w > 256u || height == 0) {
    return -3;
  }
  h = (UINT32)(height < 0 ? -height : height) / 2u;
  if (h == 0) {
    return -4;
  }
  if (bpp != 32u && bpp != 24u) {
    kputs("[ICON] unsupported bpp\n");
    return -5;
  }
  if (comp != 0u && comp != 3u) {
    kputs("[ICON] compressed DIB unsupported\n");
    return -6;
  }
  stride = ((w * bpp + 31u) / 32u) * 4u;
  if (hdr_size + (UINT64)stride * h > len) {
    return -7;
  }
  bits = img + hdr_size;
  mask_stride = ((w + 31u) / 32u) * 4u;
  mask = bits + (UINT64)stride * h;
  if (hdr_size + (UINT64)stride * h + (UINT64)mask_stride * h > len) {
    mask = NULL; /* no mask: treat every pixel as opaque */
  }

  if (bpp == 32u) {
    /* Prefer the alpha channel. A 32bpp icon with any alpha at all is
     * alpha blended, and its AND mask is usually written as all-zero
     * padding - honouring that mask would paint the whole square. Only
     * when every alpha byte is zero is the mask (or plain opacity) the
     * real coverage. */
    int    found = 0;
    UINT32 ry;

    for (ry = 0; ry < h && !found; ry++) {
      const UINT8 *srow = bits + (UINT64)((height > 0) ? (h - 1u - ry) : ry) * stride;
      UINT32       rx;

      for (rx = 0; rx < w; rx++) {
        if (srow[rx * 4u + 3u] != 0u) {
          found = 1;
          break;
        }
      }
    }
    if (!found) {
      use_alpha = 0; /* fourth byte is padding: coverage comes from the mask */
    }
  }

  raw.w = w;
  raw.h = h;
  raw.px = (UINT32 *)kmalloc(w * h * sizeof(UINT32));
  if (raw.px == NULL) {
    return -8;
  }
  for (y = 0; y < h; y++) {
    /* Positive height means bottom-up rows. */
    UINT32     sy = (height > 0) ? (h - 1u - y) : y;
    const UINT8 *row = bits + (UINT64)sy * stride;
    for (x = 0; x < w; x++) {
      const UINT8 *p = row + (bpp == 32u ? x * 4u : x * 3u);
      UINT32 b = p[0], g = p[1], r = p[2], a;

      if (bpp == 32u && use_alpha) {
        a = p[3];
      } else {
        a = 255u;
        if (mask != NULL &&
            ((mask[(UINT64)sy * mask_stride + (x / 8u)] >> (7u - (x % 8u))) & 1u) != 0u) {
          a = 0u; /* AND mask bit set = transparent */
        }
      }
      raw.px[y * w + x] = (a << 24) | (r << 16) | (g << 8) | b;
    }
  }

  if (raw.w == px && raw.h == px) {
    *out = raw;
    return 0;
  }
  {
    int rc = scale_icon(&raw, px, out);
    nd_icon_free(&raw);
    return rc;
  }
}

int nd_icon_decode(const UINT8 *data, UINT64 len, UINT32 px, nd_icon_t *out) {
  UINT32 count, i, best = 0xFFFFFFFFu, best_rank = 0xFFFFFFFFu, best_size = 0;
  UINT8  best_w = 0;

  if (data == NULL || out == NULL || px == 0) {
    return -1;
  }
  if (len < 6u || rd16(data) != 0u || rd16(data + 2) != 1u) {
    return -2;
  }
  count = rd16(data + 4);
  if (count == 0u || len < 6u + (UINT64)count * 16u) {
    return -3;
  }
  /* Prefer an exact match, then the smallest image that is still big
   * enough, then the largest one available. */
  for (i = 0; i < count; i++) {
    const UINT8 *e = data + 6u + i * 16u;
    UINT32 w = e[0] ? e[0] : 256u;
    UINT32 rank;

    if (w == px) {
      rank = 0u;
    } else if (w > px) {
      rank = 1u;
    } else {
      rank = 2u;
    }
    if (rank < best_rank ||
        (rank == best_rank && ((rank == 2u) ? w > best_w : w < best_w))) {
      best_rank = rank;
      best_w = (UINT8)(w & 0xFFu);
      best = i;
    }
  }
  if (best == 0xFFFFFFFFu) {
    return -4;
  }
  {
    const UINT8 *e = data + 6u + best * 16u;
    UINT32 off = rd32(e + 12);
    UINT32 sz = rd32(e + 8);

    if (off > len || sz == 0u || (UINT64)off + sz > len) {
      return -5;
    }
    best_size = sz;
    if (sz >= 8u && e[0] == 0x89u && data[off] == 0x89u && data[off + 1] == 'P' &&
        data[off + 2] == 'N' && data[off + 3] == 'G') {
      if (!g_said_png) {
        g_said_png = 1;
        kputs("[ICON] PNG-compressed ICO entry skipped (need a PNG decoder)\n");
      }
      return -6;
    }
    return decode_dib(data + off, best_size, px, out);
  }
}

/* ---- loading --------------------------------------------------------- */

#define ND_ICON_CACHE_N 20u

static nd_icon_t g_cache[ND_ICON_CACHE_N];
static char      g_cache_name[ND_ICON_CACHE_N][16];
static UINT32    g_cache_px[ND_ICON_CACHE_N];
static UINT32    g_cache_used;

const nd_icon_t *nd_icon_get(const char *name, UINT32 px) {
  nd_icon_t tmp;
  UINT32    i;
  int       rc = -1;

  if (name == NULL || name[0] == '\0' || px == 0u) {
    return NULL;
  }
  if (px > 256u) {
    px = 256u;
  }
  for (i = 0; i < g_cache_used; i++) {
    if (g_cache_px[i] != px) {
      continue;
    }
    {
      const char *a = g_cache_name[i];
      const char *b = name;
      while (*a != '\0' && *a == *b) {
        a++;
        b++;
      }
      if (*a == '\0' && *b == '\0') {
        return &g_cache[i];
      }
    }
  }

  memset(&tmp, 0, sizeof(tmp));
  if (g_fs != NULL) {
    char   path[ND_ICON_PATH_MAX];
    UINT32 ino;
    UINT64 size;
    UINT16 mode;
    UINT32 n = 0;

    /* "/Novus/icons/" + name + ".ico" */
    {
      const char *dir = NVUS_ICON_DIR "/";
      const char *p = dir;
      while (*p != '\0' && n + 1u < ND_ICON_PATH_MAX) {
        path[n++] = *p++;
      }
      p = name;
      while (*p != '\0' && n + 1u < ND_ICON_PATH_MAX) {
        path[n++] = *p++;
      }
      if (n + 4u >= ND_ICON_PATH_MAX) {
        n = 0;
      } else {
        path[n++] = '.';
        path[n++] = 'i';
        path[n++] = 'c';
        path[n++] = 'o';
      }
      path[n] = '\0';
    }
    if (n != 0u) {
      ino = novusfs_lookup_path(g_fs, NOVUSFS_ROOT_INO, path);
      if (ino != 0 && novusfs_stat(g_fs, ino, &size, &mode) == 0 && size > 0 &&
          size < 65536u) {
        UINT8 *buf = (UINT8 *)kmalloc(size);
        if (buf != NULL) {
          if (novusfs_read(g_fs, ino, 0, buf, size) == (INT64)size) {
            rc = nd_icon_decode(buf, size, px, &tmp);
          }
          kfree(buf);
        }
        if (rc == 0) {
          kputs("[ICON] loaded ");
          kputs(name);
          kputs(".ico from /Novus/icons\n");
        }
      }
    }
  }
  if (rc != 0) {
    const nd_icon_blob_t *b = find_blob(name);
    if (b == NULL) {
      return NULL;
    }
    rc = nd_icon_decode(b->data, b->len, px, &tmp);
    if (rc != 0) {
      kputs("[ICON] builtin decode failed\n");
      return NULL;
    }
  }

  if (g_cache_used >= ND_ICON_CACHE_N) {
    /* The shell asks for a fixed set at a fixed size; a full cache means
     * this is a fresh request, so reuse slot 0 rather than growing. */
    nd_icon_free(&g_cache[0]);
    g_cache_used = ND_ICON_CACHE_N - 1u;
    for (i = 1; i < g_cache_used; i++) {
      g_cache[i - 1] = g_cache[i];
      g_cache_px[i - 1] = g_cache_px[i];
      memcpy(g_cache_name[i - 1], g_cache_name[i], sizeof(g_cache_name[i]));
    }
  }
  i = g_cache_used;
  g_cache[i] = tmp;
  g_cache_px[i] = px;
  {
    char *dst = g_cache_name[i];
    const char *src = name;
    UINT32 k = 0;
    while (src[k] != '\0' && k + 1u < sizeof(g_cache_name[i])) {
      dst[k] = src[k];
      k++;
    }
    dst[k] = '\0';
  }
  g_cache_used++;
  return &g_cache[i];
}

/* ---- drawing --------------------------------------------------------- */

void nd_icon_blit(nd_surface_t *s, const nd_icon_t *ic, INT32 x, INT32 y,
                  nd_color_t tint, UINT8 alpha) {
  UINT32 hw, tr, tg, tb;
  UINT32 row;
  nd_rect_t clip;

  if (s == NULL || s->px == NULL || ic == NULL || ic->px == NULL || alpha == 0u) {
    return;
  }
  /* The tint arrives in canonical order; the surface may be byte-swapped,
   * and blending is per channel, so convert the tint once and blend in the
   * surface's own byte order. */
  hw = nd_hw_pixel(s, tint);
  tr = (hw >> 16) & 0xFFu;
  tg = (hw >> 8) & 0xFFu;
  tb = hw & 0xFFu;

  clip = s->clip;
  for (row = 0; row < ic->h; row++) {
    INT32 sy = y + (INT32)row;
    UINT32 col;

    if (sy < clip.y || sy >= clip.y + clip.h) {
      continue;
    }
    for (col = 0; col < ic->w; col++) {
      INT32  sx = x + (INT32)col;
      UINT32 src, a;
      UINT32 *dst;

      if (sx < clip.x || sx >= clip.x + clip.w) {
        continue;
      }
      src = ic->px[row * ic->w + col];
      a = ((src >> 24) & 0xFFu) * alpha / 255u;
      if (a == 0u) {
        continue;
      }
      dst = &s->px[(UINT64)sy * s->stride + (UINT32)sx];
      {
        /* Per-byte interpolation keeps this correct for either byte
         * order: each destination byte moves toward the matching tint
         * byte by the same amount. */
        UINT32 tbytes[4];
        UINT8 *d8 = (UINT8 *)dst;
        int    k;
        tbytes[0] = tb;
        tbytes[1] = tg;
        tbytes[2] = tr;
        tbytes[3] = 0u;
        for (k = 0; k < 3; k++) {
          d8[k] = (UINT8)((d8[k] * (255u - a) + tbytes[k] * a + 127u) / 255u);
        }
      }
    }
  }
}

void nd_icon_init(struct novusfs_fs *fs) {
  g_fs = fs;
  g_cache_used = 0;
  kputs("[ICON] image icons ready\n");
}
