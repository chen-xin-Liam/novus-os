/**
 * Novus OS - TrueType outline font rasteriser.
 *
 * Everything here works in 16.16 fixed point: the kernel runs without an
 * FPU context worth saving, and a glyph is a few hundred points, so integer
 * arithmetic is both fast enough and predictable. See nd_ttf.h for what the
 * module deliberately does not support.
 */

#include "nd_ttf.h"
#include "nd_internal.h"
#include "../kernel/kernel.h"

#define TTF_EM_PX      16u
#define TTF_MAX_POINTS  1536u
#define TTF_MAX_CONTS   64u
#define TTF_MAX_EDGES   3072u
#define TTF_CACHE_N     256u
#define TTF_MAX_FILE    (32u * 1024u * 1024u)
#define TTF_MAX_DEPTH   4u

/* One cache slot per (codepoint). Each slot carries the 1bpp cell (32 bytes)
 * the bitmap path expects plus a 4-bit antialiased coverage map (16 rows of
 * 16 nibbles = 128 bytes), so the renderer can blend grey edges instead of
 * thresholding them into staircases. A miss only costs one raster. */
typedef struct {
  UINT32 cp;
  UINT8  cell[32];
  UINT8  gray[128];
  UINT32 wide;
  UINT32 used;
} ttf_cache_t;

typedef struct {
  INT16 x, y;
  UINT8 on;
} ttf_point_t;

typedef struct {
  const UINT8 *file;
  UINT64       file_size;
  UINT32       head;
  UINT32       maxp;
  UINT32       hhea;
  UINT32       hmtx;
  UINT32       hmtx_len;
  UINT32       cmap;
  UINT32       cmap_fmt;
  UINT32       loca;
  UINT32       loca_len;
  UINT32       glyf;
  UINT32       glyf_len;
  UINT16       upem;
  UINT16       num_glyphs;
  UINT16       num_hm;
  UINT16       index_loc;
  INT16        ascender;
  char         name[48];
  int          loaded;
} ttf_face_t;

static ttf_face_t    g_face;
static ttf_cache_t   g_cache[TTF_CACHE_N];
static UINT32        g_cache_clock;

/* Outline of one glyph, appended component by component for composites. */
static ttf_point_t g_pts[TTF_MAX_POINTS];
static UINT32      g_pts_n;
static UINT16      g_cont[TTF_MAX_CONTS];
static UINT32      g_cont_n;

typedef struct {
  INT32 x0, y0, x1, y1; /* 1/16 pixel units, y already flipped to screen */
} ttf_edge_t;

static ttf_edge_t g_edge[TTF_MAX_EDGES];
static UINT32     g_edge_n;

static UINT16 g_cov[TTF_EM_PX][TTF_EM_PX];

/* Per-glyph working arrays. Static on purpose: a Han glyph carries
 * hundreds of points and the kernel stack is measured in kilobytes. */
static UINT8 g_flags[TTF_MAX_POINTS];
static INT32 g_vx[TTF_MAX_POINTS];
static INT32 g_vy[TTF_MAX_POINTS];

/* ---------------------------------------------------------------- read */

static UINT16 rd16(const UINT8 *p) {
  return (UINT16)(((UINT16)p[0] << 8) | (UINT16)p[1]);
}

static INT16 rds16(const UINT8 *p) { return (INT16)rd16(p); }

static UINT32 rd32(const UINT8 *p) {
  return ((UINT32)p[0] << 24) | ((UINT32)p[1] << 16) | ((UINT32)p[2] << 8) |
         (UINT32)p[3];
}

static void zero(void *p, UINT64 n) {
  UINT8 *b = (UINT8 *)p;
  while (n-- != 0u) {
    *b++ = 0u;
  }
}

/* ---------------------------------------------------------------- sfnt */

/* Directory entry lookup. Returns the table offset, or 0 when absent or
 * when the entry would run past the file (a truncated download must not
 * turn into an out-of-bounds read). */
static UINT32 table_at(const UINT8 *dir, UINT32 num_tables, const char *tag,
                       UINT32 *out_len) {
  UINT32 i;

  for (i = 0; i < num_tables; i++) {
    /* Table records start right after the 12-byte sfnt header. */
    const UINT8 *rec = dir + 12u + i * 16u;
    UINT32       off;
    UINT32       len;

    if (rec[0] != (UINT8)tag[0] || rec[1] != (UINT8)tag[1] ||
        rec[2] != (UINT8)tag[2] || rec[3] != (UINT8)tag[3]) {
      continue;
    }
    off = rd32(rec + 8);
    len = rd32(rec + 12);
    if ((UINT64)off + (UINT64)len > g_face.file_size) {
      return 0;
    }
    if (out_len != 0) {
      *out_len = len;
    }
    return off;
  }
  return 0;
}

static const UINT8 *at(UINT32 off, UINT32 need) {
  if ((UINT64)off + (UINT64)need > g_face.file_size) {
    return 0;
  }
  return g_face.file + off;
}

/* Pick a Unicode cmap subtable: format 12 first (it reaches beyond the
 * BMP), then format 4. Symbol fonts (3,0) are not mapped: they would need
 * a private-use translation nobody agreed on. */
static int cmap_pick(UINT32 cmap_off) {
  const UINT8 *c = at(cmap_off, 4);
  UINT32       n;
  UINT32       i;
  UINT32       best_off = 0;
  UINT32       best_fmt = 0;

  if (c == 0) {
    return -1;
  }
  n = rd16(c + 2);
  for (i = 0; i < n; i++) {
    const UINT8 *rec = at(cmap_off + 4u + i * 8u, 8);
    UINT16       plat;
    UINT16       enc;
    UINT32       sub;
    UINT32       fmt;

    if (rec == 0) {
      return -1;
    }
    plat = rd16(rec);
    enc = rd16(rec + 2);
    sub = cmap_off + rd32(rec + 4);
    if (sub + 4u > g_face.file_size || sub < cmap_off) {
      continue;
    }
    fmt = rd16(g_face.file + sub);
    if (plat == 3u && enc == 10u && fmt == 12u) {
      best_off = sub;
      best_fmt = 12u;
      break;
    }
    if (best_off == 0u && plat == 3u && enc == 1u && fmt == 4u) {
      best_off = sub;
      best_fmt = 4u;
    } else if (best_off == 0u && plat == 0u && fmt == 4u) {
      best_off = sub;
      best_fmt = 4u;
    }
  }
  if (best_off == 0u) {
    return -1;
  }
  g_face.cmap = best_off;
  g_face.cmap_fmt = best_fmt;
  return 0;
}

/* --------------------------------------------------------------- lookup */

static UINT32 gid_format12(UINT32 cp) {
  const UINT8 *s = at(g_face.cmap, 16);
  UINT32       n;
  UINT32       lo;
  UINT32       hi;

  if (s == 0) {
    return 0;
  }
  n = rd32(s + 12);
  if ((UINT64)n * 12u + 16u > g_face.file_size - g_face.cmap) {
    return 0;
  }
  lo = 0;
  hi = n;
  while (lo < hi) {
    UINT32       mid = (lo + hi) / 2u;
    const UINT8 *g = s + 16u + mid * 12u;

    if (cp < rd32(g)) {
      hi = mid;
    } else if (cp > rd32(g + 4)) {
      lo = mid + 1u;
    } else {
      return rd32(g + 8) + (cp - rd32(g));
    }
  }
  return 0;
}

static UINT32 gid_format4(UINT32 cp) {
  const UINT8 *s = at(g_face.cmap, 16);
  UINT16       seg;
  UINT32       lo;
  UINT32       hi;
  UINT32       off_end;
  UINT32       off_start;
  UINT32       off_delta;
  UINT32       off_range;

  if (s == 0 || cp > 0xFFFFu) {
    return 0;
  }
  seg = (UINT16)(rd16(s + 6) / 2u);
  off_end = g_face.cmap + 14u;
  off_start = off_end + (UINT32)seg * 2u + 2u;
  off_delta = off_start + (UINT32)seg * 2u;
  off_range = off_delta + (UINT32)seg * 2u;
  if ((UINT64)off_range + (UINT64)seg * 2u + 2u > g_face.file_size) {
    return 0;
  }
  lo = 0;
  hi = seg;
  while (lo < hi) {
    UINT32 mid = (lo + hi) / 2u;
    UINT16 e = rd16(g_face.file + off_end + mid * 2u);

    if (cp > e) {
      lo = mid + 1u;
    } else {
      hi = mid;
    }
  }
  if (lo >= seg) {
    return 0;
  }
  {
    UINT16 start = rd16(g_face.file + off_start + lo * 2u);
    UINT16 delta = rd16(g_face.file + off_delta + lo * 2u);
    UINT16 range = rd16(g_face.file + off_range + lo * 2u);
    UINT32 gid;

    if (cp < start) {
      return 0;
    }
    if (range == 0u) {
      return (UINT16)((cp + delta) & 0xFFFFu);
    }
    {
      UINT32 at_range = off_range + lo * 2u + range + (cp - start) * 2u;

      if (at_range + 2u > g_face.file_size) {
        return 0;
      }
      gid = rd16(g_face.file + at_range);
      if (gid == 0u) {
        return 0;
      }
      return (UINT16)((gid + delta) & 0xFFFFu);
    }
  }
}

static UINT32 gid_for(UINT32 cp) {
  UINT32 gid;

  if (g_face.loaded == 0) {
    return 0;
  }
  gid = (g_face.cmap_fmt == 12u) ? gid_format12(cp) : gid_format4(cp);
  if (gid >= g_face.num_glyphs) {
    return 0;
  }
  return gid;
}

static UINT32 advance_px(UINT32 gid) {
  const UINT8 *h;
  UINT32       aw;

  h = at(g_face.hmtx + gid * 4u, 2);
  if (h != 0 && gid < g_face.num_hm) {
    aw = rd16(h);
  } else {
    /* Monospaced tail: every glyph past numOfLongHorMetrics repeats the
     * last advance. */
    UINT32 last = g_face.num_hm ? g_face.num_hm - 1u : 0u;
    const UINT8 *p = at(g_face.hmtx + last * 4u, 2);

    aw = (p != 0) ? rd16(p) : (UINT32)g_face.upem / 2u;
  }
  if (aw == 0u) {
    aw = (UINT32)g_face.upem / 2u;
  }
  return aw * TTF_EM_PX / g_face.upem;
}

/* -------------------------------------------------------------- outline */

static void push_edge(INT32 x0, INT32 y0, INT32 x1, INT32 y1) {
  if (g_edge_n >= TTF_MAX_EDGES) {
    return;
  }
  g_edge[g_edge_n].x0 = x0;
  g_edge[g_edge_n].y0 = y0;
  g_edge[g_edge_n].x1 = x1;
  g_edge[g_edge_n].y1 = y1;
  g_edge_n++;
}

/* Design units to sixteenths of a pixel: one unit is upem/TTF_EM_PX of a
 * pixel, so the multiplier is TTF_EM_PX*16/upem. */
static INT32 to_16(INT32 v) {
  return v * (INT32)(TTF_EM_PX * 16u) / (INT32)g_face.upem;
}

/* y is flipped about the baseline so row 0 is the top of the cell. */
static INT32 map_x(INT16 v) { return to_16((INT32)v); }

static INT32 map_y(INT16 v) {
  return to_16((INT32)g_face.ascender) - to_16((INT32)v);
}

/* Build screen-space edges for one contour. The walk handles the on/off
 * curve flags of the glyf format: a run of two consecutive off-curve
 * points implies an on-curve point halfway between them. */
static void contour_edges(UINT32 start, UINT32 end) {
  UINT32 n;
  UINT32 first_on = 0u; /* index of an on-curve point, if any */
  UINT32 i;
  UINT32 cur;
  UINT32 guard = 0u;
  INT32  px;
  INT32  py;

  if (start >= g_pts_n || end >= g_pts_n || end < start) {
    return;
  }
  n = end - start + 1u;
  if (n == 0u) {
    return;
  }
  first_on = n;
  for (i = 0; i < n; i++) {
    if (g_pts[start + i].on != 0u) {
      first_on = i;
      break;
    }
  }
  if (first_on == n) {
    /* All off-curve: the contour is a quadratic loop about its centroid,
     * which no real font uses; treat the first point as on-curve. */
    first_on = 0u;
  }
  cur = (first_on + 1u) % n;
  px = map_x(g_pts[start + first_on].x);
  py = map_y(g_pts[start + first_on].y);
  /* n + 2 steps always suffice for one closed contour; the bound keeps a
   * malformed outline from spinning the renderer. */
  while (cur != first_on && guard++ <= n + 2u) {
    UINT32 idx = (start + cur);
    UINT32 next;

    if (g_pts[idx].on != 0u) {
      INT32 nx = map_x(g_pts[idx].x);
      INT32 ny = map_y(g_pts[idx].y);

      push_edge(px, py, nx, ny);
      px = nx;
      py = ny;
      cur = (cur + 1u) % n;
      continue;
    }
    next = (cur + 1u) % n;
    {
      INT32 cx = map_x(g_pts[start + cur].x);
      INT32 cy = map_y(g_pts[start + cur].y);
      INT32 nx;
      INT32 ny;
      UINT32 k;

      if (g_pts[start + next].on != 0u) {
        nx = map_x(g_pts[start + next].x);
        ny = map_y(g_pts[start + next].y);
      } else {
        /* Two off-curve points: the midpoint between them is on-curve. */
        nx = (cx + map_x(g_pts[start + next].x)) / 2;
        ny = (cy + map_y(g_pts[start + next].y)) / 2;
      }
      for (k = 1; k <= 8u; k++) {
        INT32 a = (INT32)(8u - k);
        INT32 b = (INT32)k;
        INT32 w0 = a * a;
        INT32 w1 = 2 * a * b;
        INT32 w2 = b * b;
        INT32 qx = (w0 * px + w1 * cx + w2 * nx) / 64;
        INT32 qy = (w0 * py + w1 * cy + w2 * ny) / 64;

        push_edge(px, py, qx, qy);
        px = qx;
        py = qy;
      }
      px = nx;
      py = ny;
      if (next == first_on) {
        /* The closing quadratic lands on the start point: stop here instead
         * of stepping past it, which would wrap forever. */
        cur = first_on;
      } else {
        cur = (g_pts[start + next].on != 0u) ? ((next + 1u) % n) : next;
      }
    }
  }
  /* Close the contour back to its starting point. */
  push_edge(px, py, map_x(g_pts[start + first_on].x),
            map_y(g_pts[start + first_on].y));
}

static int glyph_outline(UINT32 gid, UINT32 depth);

static int composite_outline(const UINT8 *g, UINT32 gid, UINT32 depth) {
  UINT32 off = 10u;
  UINT32 guard = 0u;

  while (guard++ < 32u) {
    UINT16 flags;
    UINT16 gi;
    INT32  dx;
    INT32  dy;
    UINT32 base_pts;
    UINT32 i;

    flags = rd16(g + off);
    gi = rd16(g + off + 2u);
    off += 4u;
    if (flags & 0x0001u) {
      dx = rds16(g + off);
      dy = rds16(g + off + 2u);
      off += 4u;
    } else {
      dx = (INT32)(INT8)g[off];
      dy = (INT32)(INT8)g[off + 1u];
      off += 2u;
    }
    if (flags & 0x0008u) {
      off += 2u; /* WE_HAVE_A_SCALE: approximated by identity */
    } else if (flags & 0x0040u) {
      off += 4u;
    } else if (flags & 0x0080u) {
      off += 8u;
    }
    base_pts = g_pts_n;
    if (glyph_outline(gi, depth + 1u) != 0) {
      return -1;
    }
    for (i = base_pts; i < g_pts_n; i++) {
      g_pts[i].x = (INT16)(g_pts[i].x + dx);
      g_pts[i].y = (INT16)(g_pts[i].y + dy);
    }
    if ((flags & 0x0020u) == 0u) {
      break;
    }
  }
  (void)gid;
  return 0;
}

/* Append the points of one glyph to the shared outline buffer. */
static int glyph_outline(UINT32 gid, UINT32 depth) {
  UINT32 off_start;
  UINT32 off_end;
  UINT32 g_off;
  const UINT8 *g;
  INT16  nc;

  if (depth > TTF_MAX_DEPTH || gid >= g_face.num_glyphs) {
    return -1;
  }
  if (g_face.index_loc == 0u) {
    const UINT8 *p = at(g_face.loca + gid * 2u, 4);

    if (p == 0) {
      return -1;
    }
    off_start = (UINT32)rd16(p) * 2u;
    off_end = (UINT32)rd16(p + 2u) * 2u;
  } else {
    const UINT8 *p = at(g_face.loca + gid * 4u, 8);

    if (p == 0) {
      return -1;
    }
    off_start = rd32(p);
    off_end = rd32(p + 4u);
  }
  if (off_end <= off_start || off_end > g_face.glyf_len) {
    return 0; /* empty glyph: space, or a glyph with no outline */
  }
  g_off = g_face.glyf + off_start;
  g = at(g_off, 10);
  if (g == 0) {
    return -1;
  }
  nc = rds16(g);
  if (nc < 0) {
    return composite_outline(g, gid, depth);
  }
  if (nc == 0) {
    return 0;
  }
  if ((UINT32)nc > TTF_MAX_CONTS ||
      g_cont_n + (UINT32)nc > TTF_MAX_CONTS) {
    return -1; /* a nested composite would overflow the contour table */
  }
  {
    INT32  npts;
    UINT32 i;
    UINT32 off;
    UINT16 il;

    if ((UINT64)g_off + 10u + (UINT32)nc * 2u > g_face.file_size) {
      return -1;
    }
    for (i = 0; i < (UINT32)nc; i++) {
      g_cont[g_cont_n + i] = rd16(g + 10u + i * 2u);
    }
    npts = (INT32)g_cont[g_cont_n + (UINT32)nc - 1u] + 1;
    if (npts <= 0 || g_pts_n + (UINT32)npts > TTF_MAX_POINTS) {
      return -1;
    }
    off = 10u + (UINT32)nc * 2u;
    il = rd16(g + off);
    off += 2u + (UINT32)il;
    if ((UINT64)g_off + off > g_face.file_size) {
      return -1;
    }
    /* Flags, with the repeat bit expanded. */
    i = 0;
    while (i < (UINT32)npts) {
      UINT8 f;
      UINT8 rep = 0u;

      if ((UINT64)g_off + off > g_face.file_size) {
        return -1;
      }
      f = g[off++];
      g_flags[i++] = f;
      if ((f & 0x08u) != 0u) {
        if ((UINT64)g_off + off > g_face.file_size) {
          return -1;
        }
        rep = g[off++];
        while (rep-- > 0u && i < (UINT32)npts) {
          g_flags[i++] = f;
        }
      }
    }
    /* X deltas. */
    {
      INT32 v = 0;
      for (i = 0; i < (UINT32)npts; i++) {
        UINT8 f = g_flags[i];
        if ((f & 0x02u) != 0u) {
          UINT8 d;
          if ((UINT64)g_off + off > g_face.file_size) {
            return -1;
          }
          d = g[off++];
          v += ((f & 0x10u) != 0u) ? (INT32)d : -(INT32)d;
        } else if ((f & 0x10u) == 0u) {
          if ((UINT64)g_off + off + 2u > g_face.file_size) {
            return -1;
          }
          v += rds16(g + off);
          off += 2u;
        }
        g_vx[i] = v;
      }
    }
    /* Y deltas. */
    {
      INT32 v = 0;
      for (i = 0; i < (UINT32)npts; i++) {
        UINT8 f = g_flags[i];
        if ((f & 0x04u) != 0u) {
          UINT8 d;
          if ((UINT64)g_off + off > g_face.file_size) {
            return -1;
          }
          d = g[off++];
          v += ((f & 0x20u) != 0u) ? (INT32)d : -(INT32)d;
        } else if ((f & 0x20u) == 0u) {
          if ((UINT64)g_off + off + 2u > g_face.file_size) {
            return -1;
          }
          v += rds16(g + off);
          off += 2u;
        }
        g_vy[i] = v;
      }
    }
    for (i = 0; i < (UINT32)npts; i++) {
      UINT32 dst = g_pts_n + i;

      g_pts[dst].x = (INT16)g_vx[i];
      g_pts[dst].y = (INT16)g_vy[i];
      g_pts[dst].on = (UINT8)((g_flags[i] & 0x01u) != 0u);
    }
    g_pts_n += (UINT32)npts;
    g_cont_n += (UINT32)nc;
    return 0;
  }
}

/* --------------------------------------------------------------- raster */

#define TTF_CROSS_MAX 128u

static INT32 g_xs[TTF_CROSS_MAX];
static INT8  g_dirs[TTF_CROSS_MAX];

/* Vertical sampling: four sub-scans per pixel row. Horizontal coverage is
 * accumulated in sixteenths, so a hairline thinner than a pixel still
 * leaves a trace instead of vanishing. */
static void raster(void) {
  UINT32 cont;
  UINT32 start = 0u;
  UINT32 row;
  UINT32 sub;

  zero(g_cov, sizeof(g_cov));
  g_edge_n = 0u;
  for (cont = 0; cont < g_cont_n; cont++) {
    UINT32 end = g_cont[cont];

    if (start > end || end >= g_pts_n) {
      break; /* truncated or inconsistent contour table */
    }
    contour_edges(start, end);
    start = end + 1u;
  }
  for (row = 0; row < TTF_EM_PX; row++) {
    for (sub = 0; sub < 4u; sub++) {
      INT32  y = (INT32)(row * 16u + sub * 4u + 2u);
      UINT32 n = 0u;
      UINT32 i;

      for (i = 0; i < g_edge_n && n < TTF_CROSS_MAX; i++) {
        const ttf_edge_t *e = &g_edge[i];
        INT32 dy = e->y1 - e->y0;

        if (dy == 0) {
          continue;
        }
        if ((y >= e->y0 && y < e->y1) || (y >= e->y1 && y < e->y0)) {
          INT32 x = e->x0 + (y - e->y0) * (e->x1 - e->x0) / dy;

          g_xs[n] = x;
          g_dirs[n] = (INT8)(dy > 0 ? 1 : -1);
          n++;
        }
      }
      if (n < 2u) {
        continue;
      }
      /* Insertion sort: n is a few dozen at most. */
      for (i = 1u; i < n; i++) {
        INT32 x = g_xs[i];
        INT8  d = g_dirs[i];
        UINT32 j = i;

        while (j > 0u && g_xs[j - 1u] > x) {
          g_xs[j] = g_xs[j - 1u];
          g_dirs[j] = g_dirs[j - 1u];
          j--;
        }
        g_xs[j] = x;
        g_dirs[j] = d;
      }
      {
        INT32 wind = 0;
        UINT32 k;

        for (k = 0; k + 1u < n; k++) {
          INT32 a;
          INT32 b;

          wind += (INT32)g_dirs[k];
          if (wind == 0) {
            continue;
          }
          a = g_xs[k];
          b = g_xs[k + 1u];
          if (a >= b) {
            continue;
          }
          if (a < 0) {
            a = 0;
          }
          if (b > (INT32)(TTF_EM_PX * 16u)) {
            b = (INT32)(TTF_EM_PX * 16u);
          }
          while (a < b) {
            UINT32 px = (UINT32)(a / 16);
            INT32  edge = (INT32)((px + 1u) * 16u);
            INT32  hi = (b < edge) ? b : edge;

            if (px >= TTF_EM_PX) {
              break;
            }
            g_cov[row][px] = (UINT16)(g_cov[row][px] + (UINT16)(hi - a));
            a = hi;
          }
        }
      }
    }
  }
}

/* -------------------------------------------------------------- caching */

static ttf_cache_t *cache_slot(UINT32 cp) {
  UINT32 h = (cp * 2654435761u) % TTF_CACHE_N;
  UINT32 i;

  for (i = 0; i < TTF_CACHE_N; i++) {
    UINT32 idx = (h + i) % TTF_CACHE_N;

    if (g_cache[idx].cp == cp && g_cache[idx].used != 0u) {
      return &g_cache[idx];
    }
    if (g_cache[idx].used == 0u) {
      return &g_cache[idx];
    }
  }
  /* Full: evict the least recently touched slot in the probe chain. */
  {
    UINT32 idx = h;
    UINT32 best = idx;
    UINT32 i2;

    for (i2 = 0; i2 < TTF_CACHE_N; i2++) {
      UINT32 cand = (h + i2) % TTF_CACHE_N;

      if (g_cache[cand].used < g_cache[best].used) {
        best = cand;
      }
    }
    idx = best;
    return &g_cache[idx];
  }
}

const UINT8 *nd_ttf_glyph(UINT32 cp, UINT32 *wide) {
  ttf_cache_t *c;
  UINT32       gid;
  UINT32       i;

  if (g_face.loaded == 0 || cp > 0x10FFFFu) {
    return 0;
  }
  c = cache_slot(cp);
  if (c->used != 0u && c->cp == cp) {
    c->used = ++g_cache_clock;
    if (wide != 0) {
      *wide = c->wide;
    }
    return c->cell;
  }
  gid = gid_for(cp);
  if (gid == 0u) {
    return 0;
  }
  /* Blank the slot before rasterising: a glyph with no outline (space)
   * must come out empty rather than keep the previous cell's ink. */
  zero(c->cell, sizeof(c->cell));
  zero(c->gray, sizeof(c->gray));
  g_pts_n = 0u;
  g_cont_n = 0u;
  if (glyph_outline(gid, 0u) != 0) {
    return 0; /* malformed: fall through to the bitmap font upstream */
  }
  if (g_cont_n != 0u) {
    raster();
    for (i = 0; i < TTF_EM_PX; i++) {
      UINT32 col;
      UINT16 bits = 0u;

      for (col = 0; col < TTF_EM_PX; col++) {
        /* Four sub-scanlines times 16 steps is full coverage 64; the
         * 4-bit map keeps 16 levels so edges can be blended. The 1bpp
         * cell stays as the thresholded fallback (34% ink) for the
         * monochrome path. */
        UINT32 cov = g_cov[i][col];
        UINT32 lev = (cov * 15u + 32u) / 64u;
        UINT8 *row = &c->gray[i * 8u];

        if (lev != 0u) {
          UINT8 nib = (UINT8)lev;

          if ((col & 1u) != 0u) {
            row[col >> 1] = (UINT8)(row[col >> 1] | nib);
          } else {
            row[col >> 1] = (UINT8)(row[col >> 1] | (UINT8)(nib << 4));
          }
        }
        if (cov >= 34u) {
          bits = (UINT16)(bits | (UINT16)(1u << (15u - col)));
        }
      }
      c->cell[i * 2u] = (UINT8)(bits >> 8);
      c->cell[i * 2u + 1u] = (UINT8)(bits & 0xFFu);
    }
  }
  c->cp = cp;
  c->wide = (advance_px(gid) >= 13u) ? 16u : 8u;
  c->used = ++g_cache_clock;
  if (wide != 0) {
    *wide = c->wide;
  }
  return c->cell;
}

const UINT8 *nd_ttf_glyph_gray(UINT32 cp, UINT32 *wide) {
  (void)nd_ttf_glyph(cp, wide);
  {
    ttf_cache_t *c;

    if (g_face.loaded == 0 || cp > 0x10FFFFu) {
      return 0;
    }
    c = cache_slot(cp);
    if (c->used != 0u && c->cp == cp) {
      if (wide != 0) {
        *wide = c->wide;
      }
      return c->gray;
    }
  }
  return 0;
}

UINT32 nd_ttf_advance(UINT32 cp) {
  UINT32 gid;

  if (g_face.loaded == 0) {
    return 0u;
  }
  gid = gid_for(cp);
  if (gid == 0u) {
    return 0u;
  }
  return advance_px(gid);
}

int nd_ttf_active(void) { return g_face.loaded; }

const char *nd_ttf_family(void) { return g_face.name; }

UINT32 nd_ttf_glyph_count(void) { return g_face.num_glyphs; }

/* --------------------------------------------------------------- loading */

int nd_ttf_load_image(const UINT8 *data, UINT64 size, const char *name) {
  UINT32 sfnt;
  UINT32 num_tables;
  UINT32 head;
  UINT32 maxp;
  UINT32 hhea;
  UINT32 hmtx;
  UINT32 cmap;
  UINT32 loca;
  UINT32 glyf;
  UINT32 len;
  UINT32 i;

  if (data == 0 || size < 64u || size > TTF_MAX_FILE) {
    return -1;
  }
  sfnt = 0u;
  if ((data[0] == 't' && data[1] == 't' && data[2] == 'c' && data[3] == 'f')) {
    /* Collection: take the first face. */
    if (rd32(data + 4) < 1u) {
      kputs("[FONT] ttc collection is empty\n");
      return -2;
    }
    sfnt = rd32(data + 12);
    if (sfnt + 12u > size) {
      kputs("[FONT] ttc offset outside the file\n");
      return -2;
    }
  }
  if (sfnt + 12u > size) {
    return -2;
  }
  {
    UINT32 tag = rd32(data + sfnt);

    if (tag == 0x4F54544Fu) { /* 'OTTO': CFF outlines */
      kputs("[FONT] ");
      kputs(name);
      kputs(": OpenType/CFF outlines are not supported, use a TrueType (.ttf)\n");
      return -3;
    }
    if (tag != 0x00010000u && tag != 0x74726675u /* 'true' */) {
      kputs("[FONT] ");
      kputs(name);
      kputs(": not a TrueType font\n");
      return -2;
    }
  }
  num_tables = rd16(data + sfnt + 4u);
  if (num_tables == 0u || num_tables > 64u) {
    return -2;
  }
  /* Point the lookup helper at this file before reading tables. */
  g_face.file = data;
  g_face.file_size = size;
  {
    const UINT8 *dir = data + sfnt;

    head = table_at(dir, num_tables, "head", &len);
    maxp = table_at(dir, num_tables, "maxp", &len);
  }
  if (head == 0u || maxp == 0u) {
    kputs("[FONT] ");
    kputs(name);
    kputs(": missing head/maxp\n");
    return -2;
  }
  {
    const UINT8 *h = at(head, 54);

    if (h == 0) {
      return -2;
    }
    g_face.upem = rd16(h + 18);
    g_face.index_loc = rd16(h + 50);
    g_face.ascender = 0;
    if (g_face.upem < 64u || g_face.upem > 16384u) {
      kputs("[FONT] ");
      kputs(name);
      kputs(": implausible unitsPerEm\n");
      return -2;
    }
  }
  {
    const UINT8 *m = at(maxp, 6);

    if (m == 0) {
      return -2;
    }
    g_face.num_glyphs = rd16(m + 4);
    if (g_face.num_glyphs == 0u) {
      return -2;
    }
  }
  {
    const UINT8 *dir = data + sfnt;

    hhea = table_at(dir, num_tables, "hhea", &len);
    hmtx = table_at(dir, num_tables, "hmtx", &g_face.hmtx_len);
    cmap = table_at(dir, num_tables, "cmap", &len);
    loca = table_at(dir, num_tables, "loca", &g_face.loca_len);
    glyf = table_at(dir, num_tables, "glyf", &g_face.glyf_len);
  }
  if (hhea == 0u || hmtx == 0u || cmap == 0u || loca == 0u || glyf == 0u) {
    kputs("[FONT] ");
    kputs(name);
    kputs(": missing hhea/hmtx/cmap/loca/glyf\n");
    return -2;
  }
  {
    const UINT8 *h = at(hhea, 36);

    if (h == 0) {
      return -2;
    }
    g_face.ascender = rds16(h + 4);
    g_face.num_hm = rd16(h + 34);
    if (g_face.num_hm == 0u) {
      g_face.num_hm = 1u;
    }
    /* A font whose ascent does not fit a 16 px cell still has to place
     * ink somewhere sane: clamp to the cell. */
    if (g_face.ascender <= 0 || (UINT32)g_face.ascender > g_face.upem) {
      g_face.ascender = (INT16)(g_face.upem * 7u / 8u);
    }
  }
  g_face.hmtx = hmtx;
  g_face.loca = loca;
  g_face.glyf = glyf;
  if (cmap_pick(cmap) != 0) {
    kputs("[FONT] ");
    kputs(name);
    kputs(": no Unicode cmap (format 4 or 12)\n");
    g_face.loaded = 0;
    return -4;
  }
  for (i = 0; i < TTF_CACHE_N; i++) {
    g_cache[i].cp = 0u;
    g_cache[i].used = 0u;
  }
  g_cache_clock = 0u;
  g_face.loaded = 1;
  {
    UINT32 n = 0u;

    while (name != 0 && name[n] != '\0' && n + 1u < sizeof(g_face.name)) {
      g_face.name[n] = name[n];
      n++;
    }
    g_face.name[n] = '\0';
  }
  return 0;
}
