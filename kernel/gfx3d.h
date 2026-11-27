/**
 * Novus OS - minimal 3D engine and its syscall ABI.
 *
 * This is the smallest useful 3D primitive set, not an OpenGL driver: a
 * caller supplies a 4x4 fixed-point matrix and a list of vertices, and the
 * kernel transforms, lights, rasterises and depth-tests triangles into a
 * vidmem object. That object is GPU-visible memory, so presenting it with the
 * existing surface ABI puts the result on the Intel scanout without another
 * pixel copy. Geometry and depth stay in integer math; no FPU state has to be
 * saved across a syscall.
 */

#ifndef NOVUS_GFX3D_H
#define NOVUS_GFX3D_H

#include "uefi.h"
#include "idt.h"

#define SYS_GFX3D_DRAW   38u
#define SYS_GFX3D_QUERY  39u

#define GFX3D_MAX_VERTICES 4096u
#define GFX3D_MAX_PIXELS   (1280u * 800u)
#define GFX3D_FIXED_ONE    ((INT64)1 << 16)

#define GFX3D_F_CLEAR   0x01u  /* clear colour and depth before drawing */
#define GFX3D_F_DEPTH   0x02u  /* z-buffer test/write */
#define GFX3D_F_CULL    0x04u  /* cull clockwise back faces */
#define GFX3D_F_SHADE   0x08u  /* flat directional lighting */

/* Model-space position is 16.16 fixed point; colour is 0xAARRGGBB. */
typedef struct {
  INT32  x;
  INT32  y;
  INT32  z;
  UINT32 color;
} gfx3d_vertex_t; /* 16 bytes */

typedef struct {
  UINT32 target;              /* vidmem handle the caller owns */
  UINT32 flags;
  UINT32 x;                   /* viewport origin in the target */
  UINT32 y;
  UINT32 w;
  UINT32 h;
  UINT32 vertices_va;         /* user address of gfx3d_vertex_t[] */
  UINT32 count;               /* vertex count, a multiple of three */
  INT32  matrix[16];          /* row-major 4x4, 16.16 fixed point */
  INT32  light[3];            /* world-space light direction, 16.16 */
  UINT32 clear;               /* ARGB clear colour */
  UINT32 pad;
} gfx3d_cmd_t;

typedef struct {
  UINT32 available;
  UINT32 intel_ready;
  UINT32 intel_gen;
  UINT32 max_vertices;
  UINT32 max_pixels;
  UINT32 backend;             /* 0 = CPU raster, scanout by GPU */
  UINT32 triangles;           /* triangles rasterised by the last draw */
  UINT32 reserved;
} gfx3d_query_t;

void gfx3d_init(void);
int  gfx3d_selftest(void);
UINT32 gfx3d_last_triangles(void);

void gfx3d_sys_draw(trap_frame_t *f);
void gfx3d_sys_query(trap_frame_t *f);

#endif /* NOVUS_GFX3D_H */
