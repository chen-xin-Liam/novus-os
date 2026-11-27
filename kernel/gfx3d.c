/**
 * Novus OS - minimal 3D engine. See gfx3d.h.
 *
 * The pipeline is deliberately small: fixed-point transform, perspective
 * divide, flat lighting, back-face cull and a z-buffered triangle rasteriser.
 * It draws into a vidmem object, which the GPU can scan out directly, so the
 * only CPU cost is geometry. There is no texture sampler, shader or command
 * stream: this is the "simple 3D" step, not a OpenGL implementation.
 */

#include "kernel.h"
#include "gfx3d.h"
#include "gpu2d.h"
#include "gpu_intel.h"
#include "heap.h"
#include "idt.h"
#include "mm.h"
#include "pmm.h"
#include "sched.h"
#include "vidmem.h"

#define GFX3D_ONE_I64 ((INT64)GFX3D_FIXED_ONE)

typedef struct {
  INT64  sx;      /* screen x, 16.16 */
  INT64  sy;      /* screen y, 16.16 */
  INT32  z;       /* depth, 0 = near, 65535 = far */
  UINT32 color;
} proj_t;

static UINT32 g_triangles;

UINT32 gfx3d_last_triangles(void) { return g_triangles; }

static INT64 iabs64(INT64 v) { return (v < 0) ? -v : v; }
static INT64 imin64(INT64 a, INT64 b) { return (a < b) ? a : b; }
static INT64 imax64(INT64 a, INT64 b) { return (a > b) ? a : b; }

/* Keep the numerator inside 64 bits: __int128 division would pull in
 * libgcc's __divti3, which this freestanding kernel does not link. */
static INT64 div_fixed(INT64 a, INT64 b) {
  if (a > ((INT64)1 << 46)) {
    a = (INT64)1 << 46;
  }
  if (a < -((INT64)1 << 46)) {
    a = -((INT64)1 << 46);
  }
  return (a << 16) / b;
}

/* __int128 keeps the four terms of a 16.16 dot product from wrapping even
 * when a caller passes large model coordinates. */
static INT64 dot4(const INT32 *m, INT64 x, INT64 y, INT64 z, INT64 w) {
  __int128 acc = (__int128)m[0] * x + (__int128)m[1] * y +
                 (__int128)m[2] * z + (__int128)m[3] * w;

  return (INT64)(acc >> 16);
}

static INT32 clamp_z(INT64 z) {
  if (z < 0) {
    return 0;
  }
  if (z > 65535) {
    return 65535;
  }
  return (INT32)z;
}

static INT64 edge(INT64 ax, INT64 ay, INT64 bx, INT64 by,
                  INT64 x, INT64 y) {
  return (bx - ax) * (y - ay) - (by - ay) * (x - ax);
}

/* Flat directional light. The normal comes from model space, so rotating the
 * matrix rotates the object, not the light. */
static UINT32 shade(const gfx3d_cmd_t *cmd, const gfx3d_vertex_t *v) {
  INT64 ax, ay, az, bx, by, bz, nx, ny, nz;
  INT64 lx, ly, lz, dot, mag, lmag, lum, level;
  UINT32 c = v[0].color;
  UINT32 r, g, b;

  if ((cmd->flags & GFX3D_F_SHADE) == 0u) {
    return c;
  }
  /* Shift the model coordinates down before the cross product: a full
   * 16.16 difference squared does not fit in a signed 64-bit word. */
  ax = (((INT64)v[1].x - (INT64)v[0].x) >> 12);
  ay = (((INT64)v[1].y - (INT64)v[0].y) >> 12);
  az = (((INT64)v[1].z - (INT64)v[0].z) >> 12);
  bx = (((INT64)v[2].x - (INT64)v[0].x) >> 12);
  by = (((INT64)v[2].y - (INT64)v[0].y) >> 12);
  bz = (((INT64)v[2].z - (INT64)v[0].z) >> 12);
  nx = ay * bz - az * by;
  ny = az * bx - ax * bz;
  nz = ax * by - ay * bx;
  lx = (INT64)cmd->light[0] >> 12;
  ly = (INT64)cmd->light[1] >> 12;
  lz = (INT64)cmd->light[2] >> 12;
  dot = nx * lx + ny * ly + nz * lz;
  mag = imax64(imax64(iabs64(nx), iabs64(ny)), iabs64(nz));
  lmag = imax64(imax64(iabs64(lx), iabs64(ly)), iabs64(lz));
  if (mag == 0 || lmag == 0) {
    return c;
  }
  /* Dividing by (mag*lmag)>>8 gives the cosine scaled by 256. */
  lum = dot / ((mag * lmag) >> 8);
  level = 96 + lum * 159 / 256;
  if (level < 0) {
    level = 0;
  }
  if (level > 255) {
    level = 255;
  }
  r = ((c >> 16) & 0xFFu) * (UINT32)level >> 8;
  g = ((c >> 8) & 0xFFu) * (UINT32)level >> 8;
  b = (c & 0xFFu) * (UINT32)level >> 8;
  return (c & 0xFF000000u) | (r << 16) | (g << 8) | b;
}

static void project(const gfx3d_cmd_t *cmd, const gfx3d_vertex_t *v,
                    proj_t *out) {
  INT64 x = (INT64)v->x;
  INT64 y = (INT64)v->y;
  INT64 z = (INT64)v->z;
  INT64 px = dot4(cmd->matrix, x, y, z, 0);
  INT64 py = dot4(cmd->matrix + 4, x, y, z, 0);
  INT64 pz = dot4(cmd->matrix + 8, x, y, z, 0);
  INT64 pw = dot4(cmd->matrix + 12, x, y, z, GFX3D_ONE_I64);

  if (pw <= 0) {
    out->z = 0x7FFFFFFFu;
    return;
  }
  out->sx = div_fixed(px, pw) + (((INT64)cmd->w) << 15);
  out->sy = div_fixed(py, pw) + (((INT64)cmd->h) << 15);
  out->z = clamp_z((div_fixed(pz, pw) + GFX3D_ONE_I64) >> 1);
  out->color = v->color;
}

static void raster_triangle(const gfx3d_cmd_t *cmd, const proj_t *p,
                            INT32 *depth, UINT32 *dst) {
  INT64 a = edge(p[0].sx, p[0].sy, p[1].sx, p[1].sy, p[2].sx, p[2].sy);
  INT64 e[3], de_dx[3], de_dy[3];
  INT64 minx, maxx, miny, maxy;
  INT64 inv;
  INT32 x, y;
  UINT32 color;
  INT64 row[3];

  if (a == 0) {
    return;
  }
  if ((cmd->flags & GFX3D_F_CULL) != 0u && a > 0) {
    return;
  }
  minx = imax64((INT64)0, imin64(imin64(p[0].sx, p[1].sx), p[2].sx) >> 16);
  maxx = imin64((INT64)cmd->w - 1,
                imax64(imax64(p[0].sx, p[1].sx), p[2].sx) >> 16);
  miny = imax64((INT64)0, imin64(imin64(p[0].sy, p[1].sy), p[2].sy) >> 16);
  maxy = imin64((INT64)cmd->h - 1,
                imax64(imax64(p[0].sy, p[1].sy), p[2].sy) >> 16);
  if (minx > maxx || miny > maxy) {
    return;
  }
  /* Signed scale: with a clockwise triangle both the area and every edge
   * are negative, and their quotient is still a positive barycentric weight. */
  inv = a >> 16;
  if (inv == 0) {
    return;
  }

  /* Three edge functions; each is linear in x and y, so one add per pixel
   * replaces three multiplies. */
  e[0] = edge(p[1].sx, p[1].sy, p[2].sx, p[2].sy,
              (minx << 16) + 32768, (miny << 16) + 32768);
  e[1] = edge(p[2].sx, p[2].sy, p[0].sx, p[0].sy,
              (minx << 16) + 32768, (miny << 16) + 32768);
  e[2] = edge(p[0].sx, p[0].sy, p[1].sx, p[1].sy,
              (minx << 16) + 32768, (miny << 16) + 32768);
  de_dx[0] = -(p[2].sy - p[1].sy);
  de_dx[1] = -(p[0].sy - p[2].sy);
  de_dx[2] = -(p[1].sy - p[0].sy);
  de_dy[0] = p[2].sx - p[1].sx;
  de_dy[1] = p[0].sx - p[2].sx;
  de_dy[2] = p[1].sx - p[0].sx;
  color = p[0].color;

  for (y = (INT32)miny; y <= (INT32)maxy; y++) {
    row[0] = e[0];
    row[1] = e[1];
    row[2] = e[2];
    for (x = (INT32)minx; x <= (INT32)maxx; x++) {
      int inside = (row[0] >= 0 && row[1] >= 0 && row[2] >= 0) ||
                   (row[0] <= 0 && row[1] <= 0 && row[2] <= 0);
      if (inside) {
        INT64 b0 = row[0] / inv;
        INT64 b1 = row[1] / inv;
        INT64 b2 = row[2] / inv;
        INT64 z = (b0 * p[0].z + b1 * p[1].z + b2 * p[2].z) >> 16;
        UINT32 idx = (UINT32)y * cmd->w + (UINT32)x;
        if (z < 0) {
          z = 0;
        }
        if (z > 65535) {
          z = 65535;
        }
        if ((cmd->flags & GFX3D_F_DEPTH) == 0u || z < depth[idx]) {
          if ((cmd->flags & GFX3D_F_DEPTH) != 0u) {
            depth[idx] = (INT32)z;
          }
          dst[idx] = color;
        }
      }
      row[0] += de_dx[0];
      row[1] += de_dx[1];
      row[2] += de_dx[2];
    }
    e[0] += de_dy[0];
    e[1] += de_dy[1];
    e[2] += de_dy[2];
  }
}

int gfx3d_draw(const gfx3d_cmd_t *cmd, const gfx3d_vertex_t *vertices,
               UINT32 *dst) {
  INT32 *depth;
  void *depth_phys;
  UINT64 depth_pages;
  UINT32 i;
  UINT32 drawn = 0u;

  if (cmd == 0 || vertices == 0 || dst == 0 || cmd->w == 0u ||
      cmd->h == 0u || cmd->count == 0u ||
      cmd->count > GFX3D_MAX_VERTICES ||
      (cmd->count % 3u) != 0u ||
      (UINT64)cmd->w * cmd->h > GFX3D_MAX_PIXELS) {
    return -1;
  }
  depth_pages = (((UINT64)cmd->w * cmd->h * 4u) + 4095u) / 4096u;
  depth_phys = pmm_alloc_contig(depth_pages);
  if (depth_phys == 0) {
    return -1;
  }
  depth = (INT32 *)(UINTN)phys_to_virt((UINT64)(UINTN)depth_phys);

  if ((cmd->flags & GFX3D_F_CLEAR) != 0u) {
    for (i = 0; i < cmd->h; i++) {
      gpu2d_fill_span(dst + (UINTN)i * cmd->w, cmd->w, cmd->clear);
    }
  }
  memset(depth, 0x7F, (UINT64)cmd->w * cmd->h * 4u);

  for (i = 0; i < cmd->count; i += 3u) {
    gfx3d_vertex_t tri[3];
    proj_t p[3];

    tri[0] = vertices[i];
    tri[1] = vertices[i + 1u];
    tri[2] = vertices[i + 2u];
    project(cmd, &tri[0], &p[0]);
    project(cmd, &tri[1], &p[1]);
    project(cmd, &tri[2], &p[2]);
    if (p[0].z == 0x7FFFFFFF || p[1].z == 0x7FFFFFFF ||
        p[2].z == 0x7FFFFFFF) {
      continue;
    }
    {
      UINT32 lit = shade(cmd, tri);
      p[0].color = lit;
      p[1].color = lit;
      p[2].color = lit;
    }
    raster_triangle(cmd, p, depth, dst);
    drawn++;
  }
  g_triangles = drawn;
  pmm_free_contig(depth_phys, depth_pages);
  return (int)drawn;
}

int gfx3d_selftest(void) {
  UINT32 pixels[32 * 32];
  gfx3d_cmd_t cmd;
  gfx3d_vertex_t v[3];
  INT32 one = (INT32)GFX3D_FIXED_ONE;
  int rc;

  memset(&cmd, 0, sizeof(cmd));
  cmd.flags = GFX3D_F_CLEAR | GFX3D_F_DEPTH;
  cmd.w = 32u;
  cmd.h = 32u;
  cmd.clear = 0xFF000000u;
  cmd.matrix[0] = one;
  cmd.matrix[5] = one;
  cmd.matrix[10] = one;
  cmd.matrix[15] = one;
  cmd.count = 3u;
  v[0].x = -12 * one; v[0].y = -10 * one; v[0].z = 0; v[0].color = 0xFFFF0000u;
  v[1].x =  12 * one; v[1].y = -10 * one; v[1].z = 0; v[1].color = 0xFFFF0000u;
  v[2].x =  0;        v[2].y =  12 * one; v[2].z = 0; v[2].color = 0xFFFF0000u;
  rc = gfx3d_draw(&cmd, v, pixels);
  if (rc == 1 && pixels[16 * 32 + 16] == 0xFFFF0000u &&
      pixels[0] == 0xFF000000u) {
    kputs("[GFX3D] selftest PASS\n");
    return 0;
  }
  kputs("[GFX3D] selftest FAIL\n");
  return -1;
}

void gfx3d_init(void) {
  g_triangles = 0u;
  kputs("[GFX3D] raster: fixed-point z-buffer, max ");
  kputs_hex(GFX3D_MAX_VERTICES);
  kputs(" vertices; intel=");
  kputs_hex((UINT64)intel_gpu_ready());
  kputs("\n");
  (void)gfx3d_selftest();
}

void gfx3d_sys_draw(trap_frame_t *f) {
  task_t *t;
  gfx3d_cmd_t cmd;
  gfx3d_vertex_t *vertices;
  vidmem_obj_t *o;
  UINT64 bytes;
  UINT64 needed;

  if (sched_current() == 0 ||
      user_copy(sched_current()->cr3, f->rdi, &cmd, sizeof(cmd), 0) != 0) {
    f->rax = (UINT64)(INT64)-1;
    return;
  }
  t = sched_current();
  if (cmd.count == 0u || cmd.count > GFX3D_MAX_VERTICES ||
      (cmd.count % 3u) != 0u || cmd.w == 0u || cmd.h == 0u ||
      (UINT64)cmd.w * cmd.h > GFX3D_MAX_PIXELS) {
    f->rax = (UINT64)(INT64)-1;
    return;
  }
  bytes = (UINT64)cmd.count * sizeof(gfx3d_vertex_t);
  vertices = (gfx3d_vertex_t *)kmalloc(bytes);
  if (vertices == 0 ||
      user_copy(t->cr3, cmd.vertices_va, vertices, bytes, 0) != 0) {
    if (vertices != 0) {
      kfree(vertices);
    }
    f->rax = (UINT64)(INT64)-1;
    return;
  }
  if (!vidmem_can_access(cmd.target, t->pid) ||
      (o = vidmem_get(cmd.target)) == 0) {
    kfree(vertices);
    f->rax = (UINT64)(INT64)-1;
    return;
  }
  needed = ((UINT64)cmd.x + cmd.w) * ((UINT64)cmd.y + cmd.h) * 4u;
  if (needed > o->size) {
    kfree(vertices);
    f->rax = (UINT64)(INT64)-1;
    return;
  }
  f->rax = (INT64)gfx3d_draw(&cmd, vertices,
                             (UINT32 *)(UINTN)phys_to_virt(o->phys) +
                             (UINTN)cmd.y * cmd.w + cmd.x);
  kfree(vertices);
}

void gfx3d_sys_query(trap_frame_t *f) {
  task_t *t = sched_current();
  gfx3d_query_t q;

  if (t == 0) {
    f->rax = (UINT64)(INT64)-1;
    return;
  }
  memset(&q, 0, sizeof(q));
  q.available = 1u;
  q.intel_ready = (UINT32)intel_gpu_ready();
  q.intel_gen = intel_gpu_generation();
  q.max_vertices = GFX3D_MAX_VERTICES;
  q.max_pixels = GFX3D_MAX_PIXELS;
  q.backend = 0u;
  q.triangles = gfx3d_last_triangles();
  f->rax = (user_copy(t->cr3, f->rdi, &q, sizeof(q), 1) == 0)
               ? (INT64)sizeof(q) : (INT64)-1;
}
