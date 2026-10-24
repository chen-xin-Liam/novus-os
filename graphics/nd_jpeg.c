/**
 * Novus OS - minimal JPEG decoder for the built-in wallpapers.
 *
 * Supports what photo encoders actually emit: baseline (SOF0) and
 * progressive (SOF2) huffman JPEG, 8-bit precision, greyscale or YCbCr
 * with 4:4:4 / 4:2:2 / 4:2:0 sampling, restart intervals included.
 *
 * Scaling happens inside the IDCT (the DCT-scaling trick): the caller picks
 * 1/1, 1/2, 1/4 or 1/8 and every 8x8 block produces an N x N tile evaluated
 * at the scaled sample positions, so a 6000x4000 photo decoded at 1/2
 * allocates quarter-resolution planes instead of full ones. Chroma is
 * upsampled by replication during YCbCr -> RGB conversion. Integer math
 * only, freestanding, every buffer from kmalloc().
 */

#include "kernel.h"
#include "heap.h"
#include "nd_jpeg.h"

#define JP_FAIL(msg) { kputs("[JPEG] " msg "\n"); goto fail; }

/* Zig-zag order: index = natural (row-major) position, value = position
 * along the zig-zag. jp_nat inverts it. */
static const UINT8 jp_zz[64] = {
     0,  1,  5,  6, 14, 15, 27, 28,
     2,  4,  7, 13, 16, 26, 29, 42,
     3,  8, 12, 17, 25, 30, 41, 43,
     9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54,
    20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61,
    35, 36, 48, 49, 57, 58, 62, 63,
};

static const UINT8 jp_nat[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
};

/* 1-D IDCT cosine terms at the scaled sample positions:
 *   value = C(k) * cos((2x+1)*k*pi / (2*N)) * 16384
 * with N = 1<<nlog, k < N (frequencies beyond the output Nyquist are
 * dropped, like libjpeg's scaled IDCTs) and C(0) = 1/sqrt(2). Each pass
 * drops 2^14 from the fixed point and the 1/4 of the 2-D formula makes the
 * final shift 2^16. */
static const INT16 jp_g[3][4][4] = {
  { /* N = 1 */
    { 11585 }
  },
  { /* N = 2 */
    { 11585,  11585 },
    { 11585, -11585 }
  },
  { /* N = 4 */
    { 11585,  11585,  11585,  11585 },
    { 15137,   6270,  -6270, -15137 },
    { 11585, -11585, -11585,  11585 },
    {  6270, -15137,  15137,  -6270 }
  }
};

/* N = 8 (the full IDCT). Row = frequency k, column = position x. */
static const INT16 jp_g8[8][8] = {
  { 11585,  11585,  11585,  11585,  11585,  11585,  11585,  11585 },
  { 16069,  13623,   9102,   3196,  -3196,  -9102, -13623, -16069 },
  { 15137,   6270,  -6270, -15137, -15137,  -6270,   6270,  15137 },
  { 13623,  -3196, -16069,  -9102,   9102,  16069,   3196, -13623 },
  { 11585, -11585, -11585,  11585,  11585, -11585, -11585,  11585 },
  {  9102, -16069,   3196,  13623, -13623,  -3196,  16069,  -9102 },
  {  6270, -15137,  15137,  -6270,  -6270,  15137, -15137,   6270 },
  {  3196,  -9102,  13623, -16069,  16069, -13623,   9102,  -3196 }
};

/* One dequantised 8x8 block -> N x N samples at the scaled positions. */
static void jp_idct(const int *blk, UINT8 *out, int out_stride, int nlog) {
  int n = 1 << nlog;
  INT64 tmp[8][8]; /* pass-1 partials: row x, horizontal frequency u */
  int x, u, y;

  for (x = 0; x < n; x++) {
    for (u = 0; u < n; u++) {
      INT64 acc = 0;
      int v;
      for (v = 0; v < n; v++) {
        INT64 g = (nlog == 3) ? (INT64)jp_g8[v][x] : (INT64)jp_g[nlog][v][x];
        acc += g * blk[u + v * 8];
      }
      tmp[x][u] = (acc + (INT64)(1 << 13)) >> 14;
    }
  }
  for (x = 0; x < n; x++) {
    UINT8 *row = out + x * out_stride;
    for (y = 0; y < n; y++) {
      INT64 acc = 0;
      for (u = 0; u < n; u++) {
        INT64 g = (nlog == 3) ? (INT64)jp_g8[u][y] : (INT64)jp_g[nlog][u][y];
        acc += g * tmp[x][u];
      }
      acc = ((acc + (INT64)(1 << 15)) >> 16) + 128;
      row[y] = (UINT8)(acc < 0 ? 0 : (acc > 255 ? 255 : acc));
    }
  }
}

/* ---- huffman ----------------------------------------------------------- */

typedef struct {
  INT32 maxcode[17]; /* longest code of each length */
  INT32 mincode[17];
  INT32 valptr[17];
  UINT8 vals[256];
  int   valid;
} jp_huff_t;

/* ---- stream state ------------------------------------------------------ */

typedef struct {
  const UINT8 *data;
  UINT32       len;
  UINT32       pos;    /* next byte to load */
  UINT32       buf;    /* bit accumulator, MSB first */
  int          cnt;    /* valid bits in buf */
  int          marker; /* non-RST marker seen in the scan, -1 = none */
} jp_bits_t;

static int jp_bits_byte(jp_bits_t *b) {
  int c;

  if (b->marker >= 0) {
    return -1; /* past the scan end: feed 1-bits (huffman padding) */
  }
  if (b->pos >= b->len) {
    b->marker = 0xD9; /* a truncated stream behaves like EOI */
    return -1;
  }
  c = b->data[b->pos++];
  if (c != 0xFF) {
    return c;
  }
  while (b->pos < b->len && b->data[b->pos] == 0xFF) {
    b->pos++; /* fill bytes before a marker */
  }
  if (b->pos >= b->len) {
    b->marker = 0xD9;
    return -1;
  }
  c = b->data[b->pos++];
  if (c == 0x00) {
    return 0xFF; /* byte stuffing */
  }
  if (c >= 0xD0 && c <= 0xD7) {
    return jp_bits_byte(b); /* RSTn: transparent to the bit reader */
  }
  b->marker = c; /* any other marker ends the entropy-coded segment */
  return -1;
}

static int jp_bits_get(jp_bits_t *b, int n) {
  unsigned v;

  while (b->cnt < n) {
    b->buf = (b->buf << 8) | (UINT32)(jp_bits_byte(b) & 0xFF);
    b->cnt += 8;
  }
  v = (b->buf >> (b->cnt - n)) & ((1u << n) - 1u);
  b->cnt -= n;
  return (int)v;
}

static int jp_bits_bit(jp_bits_t *b) {
  return jp_bits_get(b, 1);
}

/* Variable-length integer: n raw bits, MSB first. */
static int jp_bits_receive(jp_bits_t *b, int n) {
  int v = 0;
  while (n-- > 0) {
    v = (v << 1) | jp_bits_bit(b);
  }
  return v;
}

/* Signed magnitude extension of an n-bit value. */
static int jp_extend(int v, int n) {
  return (v < (1 << (n - 1))) ? v - (1 << n) + 1 : v;
}

/* ---- frame ------------------------------------------------------------- */

typedef struct {
  int    id;
  int    h, v;            /* sampling factors (1..2) */
  int    tq;              /* quantisation table */
  int    td, ta;          /* huffman table selectors */
  INT32  comp_w, comp_h;  /* true sample dimensions */
  INT32  bw, bh;          /* blocks across / down inside the image */
  INT32  bw2, bh2;        /* blocks across / down on the MCU grid (padded) */
  UINT8 *plane;           /* decoded samples at scale, stride sw */
  INT32  sw, sh;          /* scaled plane dimensions */
  short *coeffs;          /* progressive coefficient buffer (or NULL) */
} jp_comp_t;

typedef struct {
  const UINT8 *p;
  UINT32       len;
  int          progressive;
  int          width, height;
  int          ncomp;
  jp_comp_t    comp[4];
  int          max_h, max_v;
  int          mcux, mcuy;  /* MCU grid, interleaved scans */
  int          ri;          /* restart interval, in MCUs / blocks */
  int          nlog;        /* IDCT scale log2 (0..3) */
  UINT16       qt[4][64];
  jp_huff_t    hdc[4], hac[4];
  /* current scan */
  int          ns, sc[4];
  int          ss, se, ah, al;
  int          eobrun;
} jp_dec_t;

static int jp_huff_build(jp_huff_t *h, const UINT8 *counts, const UINT8 *sym,
                         int total) {
  int k = 0, i;
  INT32 code = 0;

  h->valid = 0;
  for (i = 1; i <= 16; i++) {
    h->valptr[i] = k;
    h->mincode[i] = code;
    code += counts[i - 1];
    h->maxcode[i] = code - 1;
    code <<= 1;
    k += counts[i - 1];
  }
  if (k != total || total > 256) {
    return -1;
  }
  for (i = 0; i < total; i++) {
    h->vals[i] = sym[i];
  }
  h->valid = 1;
  return 0;
}

static int jp_huff_decode(jp_bits_t *b, const jp_huff_t *h) {
  INT32 code = 0;
  int l;

  for (l = 1; l <= 16; l++) {
    code = (code << 1) | jp_bits_bit(b);
    if (code <= h->maxcode[l]) {
      return h->vals[h->valptr[l] + code - h->mincode[l]];
    }
  }
  return -1;
}

static UINT16 jp_be16(const UINT8 *p) {
  return (UINT16)((p[0] << 8) | p[1]);
}

static int jp_parse_sof(jp_dec_t *d, UINT32 pos) {
  int precision, i;

  if (pos + 10u > d->len) {
    return -1;
  }
  precision = d->p[pos + 4];
  d->height = jp_be16(&d->p[pos + 5]);
  d->width = jp_be16(&d->p[pos + 7]);
  d->ncomp = d->p[pos + 9];
  if (precision != 8 || d->width <= 0 || d->height <= 0 ||
      (d->ncomp != 1 && d->ncomp != 3)) {
    return -1;
  }
  if (pos + 10u + (UINT32)d->ncomp * 3u > d->len) {
    return -1;
  }
  d->max_h = 1;
  d->max_v = 1;
  for (i = 0; i < d->ncomp; i++) {
    jp_comp_t *c = &d->comp[i];
    const UINT8 *p = &d->p[pos + 10 + i * 3];

    c->id = p[0];
    c->h = p[1] >> 4;
    c->v = p[1] & 15;
    c->tq = p[2] & 15;
    c->coeffs = 0;
    c->plane = 0;
    if (c->h < 1 || c->h > 2 || c->v < 1 || c->v > 2 || c->tq > 3) {
      return -1;
    }
    if (c->h > d->max_h) { d->max_h = c->h; }
    if (c->v > d->max_v) { d->max_v = c->v; }
  }
  d->mcux = (d->width + 8 * d->max_h - 1) / (8 * d->max_h);
  d->mcuy = (d->height + 8 * d->max_v - 1) / (8 * d->max_v);
  return 0;
}

static int jp_alloc_planes(jp_dec_t *d) {
  int i;

  for (i = 0; i < d->ncomp; i++) {
    jp_comp_t *c = &d->comp[i];

    c->comp_w = (d->width * c->h + d->max_h - 1) / d->max_h;
    c->comp_h = (d->height * c->v + d->max_v - 1) / d->max_v;
    c->bw = (c->comp_w + 7) / 8;
    c->bh = (c->comp_h + 7) / 8;
    /* Interleaved scans run over whole MCUs, so the right/bottom padding
     * blocks are coded too: allocate for them, else their entropy data would
     * be skipped and the bitstream would desync. */
    c->bw2 = d->mcux * c->h;
    c->bh2 = d->mcuy * c->v;
    c->sw = c->bw2 << d->nlog;
    c->sh = c->bh2 << d->nlog;
    c->plane = (UINT8 *)kmalloc((UINT64)c->sw * (UINT64)c->sh);
    if (c->plane == 0) {
      return -1;
    }
    if (d->progressive) {
      c->coeffs = (short *)kmalloc((UINT64)c->bw2 * (UINT64)c->bh2 * 64u * 2u);
      if (c->coeffs == 0) {
        return -1;
      }
    }
  }
  return 0;
}

static void jp_free_all(jp_dec_t *d) {
  int i;

  for (i = 0; i < 4; i++) {
    if (d->comp[i].plane != 0) {
      kfree(d->comp[i].plane);
      d->comp[i].plane = 0;
    }
    if (d->comp[i].coeffs != 0) {
      kfree(d->comp[i].coeffs);
      d->comp[i].coeffs = 0;
    }
  }
}

/* ---- block decoding ---------------------------------------------------- */

/* One baseline/extended-sequential block: huffman -> dequant -> IDCT into
 * the component plane at the scaled position of block (bx, by). */
static int jp_block_seq(jp_dec_t *d, jp_bits_t *b, jp_comp_t *c, int bx,
                        int by, int *pred) {
  int blk[64];
  const UINT16 *qt = d->qt[c->tq];
  int t, diff, k = 1;

  t = jp_huff_decode(b, &d->hdc[c->td]);
  if (t < 0 || t > 15) {
    return -1;
  }
  diff = (t == 0) ? 0 : jp_extend(jp_bits_receive(b, t), t);
  *pred += diff;

  for (t = 0; t < 64; t++) {
    blk[t] = 0;
  }
  blk[0] = *pred * qt[0];

  while (k < 64) {
    int rs = jp_huff_decode(b, &d->hac[c->ta]);
    int r, s, nat;

    if (rs < 0) {
      return -1;
    }
    r = rs >> 4;
    s = rs & 15;
    if (s == 0) {
      if (r != 15) {
        break; /* EOB */
      }
      k += 16;
      continue;
    }
    k += r;
    if (k > 63) {
      return -1;
    }
    nat = jp_nat[k];
    blk[nat] = jp_extend(jp_bits_receive(b, s), s) * qt[nat];
    k++;
  }

  jp_idct(blk, c->plane + (by << d->nlog) * c->sw + (bx << d->nlog),
          c->sw, d->nlog);
  return 0;
}

/* Progressive, first scan of a coefficient: decode into this block's slot
 * of c->coeffs (natural coefficient order, values shifted up by Al). */
static int jp_block_prog_first(jp_dec_t *d, jp_bits_t *b, jp_comp_t *c,
                               int bx, int by, int *pred) {
  short *co = c->coeffs + (((by * c->bw2 + bx) << 6));
  int t, diff, k = d->ss;

  if (d->ss == 0) {
    t = jp_huff_decode(b, &d->hdc[c->td]);
    if (t < 0 || t > 15) {
      return -1;
    }
    diff = (t == 0) ? 0 : jp_extend(jp_bits_receive(b, t), t);
    *pred += diff;
    co[0] = (short)(*pred << d->al);
    return 0;
  }

  if (d->eobrun > 0) {
    d->eobrun--;
    return 0;
  }
  while (k <= d->se) {
    int rs = jp_huff_decode(b, &d->hac[c->ta]);
    int r, s;

    if (rs < 0) {
      return -1;
    }
    r = rs >> 4;
    s = rs & 15;
    if (s == 0) {
      if (r != 15) {
        d->eobrun = (1 << r);
        if (r != 0) {
          d->eobrun += jp_bits_receive(b, r);
        }
        return 0;
      }
      k += 16;
      continue;
    }
    k += r;
    if (k > d->se) {
      return -1;
    }
    co[jp_nat[k]] = (short)(jp_extend(jp_bits_receive(b, s), s) << d->al);
    k++;
  }
  return 0;
}

/* Progressive refinement of already-stored coefficients. */
static int jp_block_prog_refine(jp_dec_t *d, jp_bits_t *b, jp_comp_t *c,
                                int bx, int by) {
  short *co = c->coeffs + (((by * c->bw2 + bx) << 6));
  int k = d->ss;
  int new_coef = 0, rs, r, s;

  if (d->ss == 0) {
    /* DC refinement: one correction bit. */
    if (jp_bits_bit(b) != 0 && (co[0] & (1 << d->al)) == 0) {
      co[0] |= (short)(1 << d->al);
    }
    return 0;
  }

  if (d->eobrun > 0) {
    /* Correction bits for every nonzero coefficient in the band. */
    while (k <= d->se) {
      int v = co[jp_nat[k]];
      if (v != 0 && jp_bits_bit(b) != 0 && (v & (1 << d->al)) == 0) {
        co[jp_nat[k]] = (short)(v + ((v > 0) ? (1 << d->al) : -(1 << d->al)));
      }
      k++;
    }
    d->eobrun--;
    return 0;
  }

  rs = jp_huff_decode(b, &d->hac[c->ta]);
  if (rs < 0) {
    return -1;
  }
  r = rs >> 4;
  s = rs & 15;
  if (s == 0) {
    if (r != 15) {
      d->eobrun = (1 << r);
      if (r != 0) {
        d->eobrun += jp_bits_receive(b, r);
      }
      return 0;
    }
  } else {
    if (s != 1) {
      return -1;
    }
    new_coef = jp_bits_bit(b) ? (1 << d->al) : -(1 << d->al);
  }

  /* Walk the band: history coefficients get a correction bit, the zero run
   * skips r of them and drops the (possibly new) coefficient on the last
   * one. */
  while (k <= d->se) {
    int idx = jp_nat[k];
    int v = co[idx];

    if (v != 0) {
      if (jp_bits_bit(b) != 0 && (v & (1 << d->al)) == 0) {
        co[idx] = (short)(v + ((v > 0) ? (1 << d->al) : -(1 << d->al)));
      }
    } else {
      if (r == 0) {
        if (new_coef != 0) {
          co[idx] = (short)new_coef;
        }
        return 0;
      }
      r--;
    }
    k++;
  }
  return (new_coef != 0) ? -1 : 0;
}

/* ---- scans ------------------------------------------------------------- */

static int jp_parse_sos(jp_dec_t *d, UINT32 sos_pos) {
  int ns = d->p[sos_pos + 4];
  int i, j;

  if (ns < 1 || ns > 4) {
    return -1;
  }
  if (sos_pos + 8u + (UINT32)ns * 2u > d->len) {
    return -1;
  }
  for (i = 0; i < ns; i++) {
    int cid = d->p[sos_pos + 5 + i * 2];
    int tdta = d->p[sos_pos + 6 + i * 2];
    int found = -1;
    for (j = 0; j < d->ncomp; j++) {
      if (d->comp[j].id == cid) {
        found = j;
        break;
      }
    }
    if (found < 0 || (tdta >> 4) > 3 || (tdta & 15) > 3) {
      return -1;
    }
    d->comp[found].td = tdta >> 4;
    d->comp[found].ta = tdta & 15;
    d->sc[i] = found;
  }
  d->ss = d->p[sos_pos + 5 + ns * 2];
  d->se = d->p[sos_pos + 6 + ns * 2];
  d->ah = d->p[sos_pos + 7 + ns * 2] >> 4;
  d->al = d->p[sos_pos + 7 + ns * 2] & 15;
  d->ns = ns;
  return 0;
}

static void jp_run_scan(jp_dec_t *d, UINT32 pos) {
  jp_bits_t bits;
  int pred[4];
  int i;

  bits.data = d->p;
  bits.len = d->len;
  bits.pos = pos;
  bits.buf = 0;
  bits.cnt = 0;
  bits.marker = -1;
  d->eobrun = 0;
  pred[0] = pred[1] = pred[2] = pred[3] = 0;

  if (d->ns == 1) {
    /* Non-interleaved scan: every block of one component, in order. */
    jp_comp_t *c = &d->comp[d->sc[0]];
    int bx, by;
    int left = d->ri;

    for (by = 0; by < c->bh; by++) {
      for (bx = 0; bx < c->bw; bx++) {
        if (!d->progressive) {
          if (jp_block_seq(d, &bits, c, bx, by, &pred[d->sc[0]]) != 0) {
            return;
          }
        } else if (d->ah == 0) {
          if (jp_block_prog_first(d, &bits, c, bx, by, &pred[d->sc[0]]) != 0) {
            return;
          }
        } else {
          if (jp_block_prog_refine(d, &bits, c, bx, by) != 0) {
            return;
          }
        }
        if (d->ri != 0 && --left == 0) {
          left = d->ri;
          bits.buf = 0;
          bits.cnt = 0;
          pred[0] = pred[1] = pred[2] = pred[3] = 0;
          d->eobrun = 0;
        }
      }
    }
    return;
  }

  /* Interleaved MCU scan over the scan's components. */
  {
    int mx, my;
    int left = d->ri;

    for (my = 0; my < d->mcuy; my++) {
      for (mx = 0; mx < d->mcux; mx++) {
        for (i = 0; i < d->ns; i++) {
          jp_comp_t *c = &d->comp[d->sc[i]];
          int sbx, sby;
          for (sby = 0; sby < c->v; sby++) {
            for (sbx = 0; sbx < c->h; sbx++) {
              int bx = mx * c->h + sbx;
              int by = my * c->v + sby;

              /* bx/by may run past the image into the MCU padding; those
               * blocks are still coded, so decode them into the padded
               * plane rather than skipping their entropy data. */
              if (!d->progressive) {
                if (jp_block_seq(d, &bits, c, bx, by, &pred[d->sc[i]]) != 0) {
                  return;
                }
              } else if (d->ah == 0) {
                if (jp_block_prog_first(d, &bits, c, bx, by,
                                        &pred[d->sc[i]]) != 0) {
                  return;
                }
              } else {
                if (jp_block_prog_refine(d, &bits, c, bx, by) != 0) {
                  return;
                }
              }
            }
          }
        }
        if (d->ri != 0 && --left == 0) {
          left = d->ri;
          bits.buf = 0;
          bits.cnt = 0;
          pred[0] = pred[1] = pred[2] = pred[3] = 0;
          d->eobrun = 0;
        }
      }
    }
  }
}

/* Progressive: turn the stored coefficients into samples. */
static void jp_render_progressive(jp_dec_t *d) {
  int ci;

  for (ci = 0; ci < d->ncomp; ci++) {
    jp_comp_t *c = &d->comp[ci];
    const UINT16 *qt = d->qt[c->tq];
    int blk[64];
    int bx, by;

    for (by = 0; by < c->bh; by++) {
      for (bx = 0; bx < c->bw; bx++) {
        const short *co = c->coeffs + ((by * c->bw2 + bx) << 6);
        int i;
        for (i = 0; i < 64; i++) {
          blk[i] = (int)co[i] * qt[i];
        }
        jp_idct(blk, c->plane + (by << d->nlog) * c->sw + (bx << d->nlog),
                c->sw, d->nlog);
      }
    }
  }
}

/* ---- colour conversion ------------------------------------------------- */

static int jp_clamp8(INT32 v) {
  return v < 0 ? 0 : (v > 255 ? 255 : v);
}

static void jp_convert(jp_dec_t *d, nd_surface_t *out) {
  int x, y;

  for (y = 0; y < out->h; y++) {
    UINT32 *row = out->px + (UINT64)y * (UINT32)out->stride;
    const UINT8 *yp = d->comp[0].plane + (UINT64)y * (UINT32)d->comp[0].sw;

    if (d->ncomp == 1) {
      for (x = 0; x < out->w; x++) {
        UINT8 g = yp[x];
        row[x] = 0xFF000000u | ((UINT32)g << 16) | ((UINT32)g << 8) | g;
      }
      continue;
    }
    for (x = 0; x < out->w; x++) {
      const jp_comp_t *cb = &d->comp[1];
      const jp_comp_t *cr = &d->comp[2];
      int cx = (x * cb->h) / d->max_h;
      int cy = (y * cb->v) / d->max_v;
      int cxs = (x * cr->h) / d->max_h;
      int cys = (y * cr->v) / d->max_v;
      INT32 Y, Cb, Cr, r, g, b;

      if (cx >= cb->sw) { cx = (int)cb->sw - 1; }
      if (cy >= cb->sh) { cy = (int)cb->sh - 1; }
      if (cxs >= cr->sw) { cxs = (int)cr->sw - 1; }
      if (cys >= cr->sh) { cys = (int)cr->sh - 1; }
      Y = yp[x];
      Cb = cb->plane[(UINT64)cy * (UINT32)cb->sw + cx] - 128;
      Cr = cr->plane[(UINT64)cys * (UINT32)cr->sw + cxs] - 128;
      r = Y + ((91881 * Cr) >> 16);
      g = Y - ((22554 * Cb + 46802 * Cr) >> 16);
      b = Y + ((116130 * Cb) >> 16);
      row[x] = 0xFF000000u | ((UINT32)jp_clamp8(r) << 16) |
               ((UINT32)jp_clamp8(g) << 8) | (UINT32)jp_clamp8(b);
    }
  }
}

/* ---- public entry ------------------------------------------------------ */

int nd_jpeg_size(const UINT8 *data, UINT32 len, int *w, int *h) {
  UINT32 pos = 2;

  if (data == 0 || len < 4 || data[0] != 0xFF || data[1] != 0xD8) {
    return -1;
  }
  while (pos + 4 <= len) {
    UINT32 m;
    UINT16 plen;

    if (data[pos] != 0xFF) {
      pos++;
      continue;
    }
    m = data[pos + 1];
    if (m == 0xFF || m == 0x00 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) {
      pos += 2;
      continue;
    }
    if (m == 0xD9) {
      return -1;
    }
    plen = jp_be16(&data[pos + 2]);
    if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
      if (pos + 9u > len) {
        return -1;
      }
      *h = jp_be16(&data[pos + 5]);
      *w = jp_be16(&data[pos + 7]);
      return (*w > 0 && *h > 0) ? 0 : -1;
    }
    pos += 2u + plen;
  }
  return -1;
}

int nd_jpeg_decode(const UINT8 *data, UINT32 len, int scale, nd_surface_t *out) {
  jp_dec_t dec;
  UINT32 pos = 2; /* past SOI */
  int scans_done = 0;
  int nlog, ow, oh;
  int rc = -1;

  if (data == 0 || len < 4 || out == 0 || data[0] != 0xFF || data[1] != 0xD8) {
    return -1;
  }
  /* scale = 1 -> full 8x8 IDCT (nlog 3); larger scales use a shorter IDCT
   * (n = 8/scale samples per block), like libjpeg's scaled IDCTs. */
  switch (scale) {
  case 1:  nlog = 3; break;
  case 2:  nlog = 2; break;
  case 4:  nlog = 1; break;
  case 8:  nlog = 0; break;
  default: return -1;
  }
  memset(&dec, 0, sizeof(dec));
  dec.p = data;
  dec.len = len;
  dec.nlog = nlog;

  while (pos + 4 <= len) {
    UINT32 m;
    UINT16 plen;

    if (data[pos] != 0xFF) {
      pos++;
      continue;
    }
    m = data[pos + 1];
    if (m == 0xFF || m == 0x00 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) {
      pos += 2;
      continue;
    }
    if (m == 0xD9) { /* EOI */
      break;
    }
    if (pos + 4 > len) {
      break;
    }
    plen = jp_be16(&data[pos + 2]);

    if (m == 0xDB) { /* DQT */
      UINT32 q = pos + 4;
      UINT32 end = pos + 2u + plen;
      if (end > len) {
        end = len;
      }
      while (q + 1u <= end) {
        int pq = data[q] >> 4;
        int tq = data[q] & 15;
        int i;

        q++;
        if (tq > 3 || q + (pq ? 128u : 64u) > end) {
          break;
        }
        for (i = 0; i < 64; i++) {
          UINT32 zi = jp_zz[i];
          dec.qt[tq][i] = pq ? jp_be16(&data[q + zi * 2u]) : data[q + zi];
        }
        q += pq ? 128u : 64u;
      }
    } else if (m == 0xC4) { /* DHT */
      UINT32 h = pos + 4;
      UINT32 end = pos + 2u + plen;
      if (end > len) {
        end = len;
      }
      while (h + 17u <= end) {
        int tchv = data[h];
        int tc = tchv >> 4;
        int th = tchv & 15;
        int total = 0, i;
        jp_huff_t *tbl;

        h++;
        for (i = 0; i < 16; i++) {
          total += data[h + i];
        }
        if (tc > 1 || th > 1 || h + 16u + (UINT32)total > end) {
          break;
        }
        tbl = (tc == 0) ? &dec.hdc[th] : &dec.hac[th];
        if (jp_huff_build(tbl, &data[h], &data[h + 16], total) != 0) {
          JP_FAIL("bad huffman table");
        }
        h += 16u + (UINT32)total;
      }
    } else if (m == 0xC0 || m == 0xC1 || m == 0xC2) { /* SOF0/1/2 */
      if (dec.ncomp != 0) {
        JP_FAIL("multiple SOF markers");
      }
      dec.progressive = (m == 0xC2) ? 1 : 0;
      if (jp_parse_sof(&dec, pos) != 0) {
        JP_FAIL("unsupported SOF");
      }
      if (jp_alloc_planes(&dec) != 0) {
        JP_FAIL("out of memory");
      }
    } else if (m == 0xDD) { /* DRI */
      if (pos + 6u <= len) {
        dec.ri = jp_be16(&data[pos + 4]);
      }
    } else if (m == 0xDA) { /* SOS */
      if (dec.ncomp == 0) {
        JP_FAIL("SOS before SOF");
      }
      if (jp_parse_sos(&dec, pos) != 0) {
        JP_FAIL("bad SOS");
      }
      jp_run_scan(&dec, pos + 8u + (UINT32)dec.ns * 2u);
      scans_done = 1;
      if (!dec.progressive) {
        break; /* a sequential scan carries the whole image */
      }
    } else if (m >= 0xC3 && m <= 0xCF) { /* unsupported SOF variants */
      JP_FAIL("unsupported SOF type");
    }
    /* APPn, COM, DNL and anything unknown is skipped wholesale. */
    pos += 2u + plen;
  }

  if (!scans_done || dec.width <= 0 || dec.ncomp == 0) {
    JP_FAIL("no image data");
  }
  if (dec.progressive) {
    jp_render_progressive(&dec);
  }

  /* nlog 3 -> full size; each halving of nlog halves the output. */
  {
    int s = 8 >> nlog;

    ow = (dec.width + s - 1) / s;
    oh = (dec.height + s - 1) / s;
  }
  if (nd_surface_create(out, ow, oh) != 0) {
    JP_FAIL("out of memory");
  }
  jp_convert(&dec, out);
  rc = 0;

fail:
  jp_free_all(&dec);
  return rc;
}
