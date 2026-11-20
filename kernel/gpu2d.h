/**
 * Novus OS - the 2D engine seam.
 *
 * Every pixel the desktop draws ends up as a horizontal run of 32-bit words
 * (a span) or as a row copy (a blit). Those two operations are the whole of
 * 2D: fills, borders, rounded corners, glyph bitmaps, window moves, cursor
 * saves. This module owns them so that a hardware 2D engine can be plugged in
 * later without touching a single drawing routine.
 *
 * Today there is exactly one working backend: the CPU, writing 64-bit pairs
 * when the destination allows it. The Intel, AMD and NVIDIA entries exist so
 * that the day a driver lands it registers here and the compositor starts
 * using it - they deliberately report "no device" until then, because a
 * backend that pretends to work on unknown hardware corrupts the screen
 * silently.
 */

#ifndef NOVUS_GPU2D_H
#define NOVUS_GPU2D_H

#include "uefi.h"

/* Called once after gpu_init(). Chooses a backend and says so in the boot
 * log. Safe to call twice. */
void gpu2d_init(void);

/* "software" until a hardware path is wired up. Never NULL. */
const char *gpu2d_backend(void);

/* Non-zero when the selected backend is a hardware engine. */
int gpu2d_hardware(void);

/* Fill `count` consecutive pixels of one row with the hardware pixel word.
 * This is the hot path of every repaint: a window move on a 1024x768 screen
 * is hundreds of thousands of these. */
void gpu2d_fill_span(UINT32 *row, UINT32 count, UINT32 hw);

/* Copy `count` pixels. `dst` and `src` may not overlap. */
void gpu2d_copy_row(UINT32 *dst, const UINT32 *src, UINT32 count);

/* Copy a w*h block between two surfaces (or between a surface and the
 * framebuffer). Strides are in pixels, not bytes. */
void gpu2d_blit(UINT32 *dst, UINT32 dstride, const UINT32 *src, UINT32 sstride,
                UINT32 w, UINT32 h);

/* Totals, for the boot log and for a settings page that wants to show
 * whether 2D is being done by the CPU or the GPU. */
UINT64 gpu2d_span_count(void);
UINT64 gpu2d_blit_count(void);

#endif /* NOVUS_GPU2D_H */
