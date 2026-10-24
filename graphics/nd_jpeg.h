/**
 * Novus OS - minimal JPEG decoder for the built-in wallpapers.
 */

#ifndef NOVUS_ND_JPEG_H
#define NOVUS_ND_JPEG_H

#include "uefi.h"
#include "nd.h"

/* Decode a JPEG into an RGB surface (0xAARRGGBB, opaque). `scale` selects
 * the DCT-scaling output: 1 = full size, 2/4/8 = 1/2, 1/4, 1/8 of the
 * stored image. The IDCT is evaluated at the scaled positions directly, so
 * a 6000x4000 photo decoded at 1/2 never allocates full-resolution planes.
 * Baseline (SOF0) and progressive (SOF2) huffman streams, 8-bit, greyscale
 * or YCbCr 4:4:4 / 4:2:2 / 4:2:0. The pixel buffer is kmalloc'd; release it
 * with nd_surface_destroy(). Returns 0 on success, -1 on any unsupported
 * or malformed stream (the surface is left zeroed). */
int nd_jpeg_decode(const UINT8 *data, UINT32 len, int scale, nd_surface_t *out);

/* Read the stored image dimensions from the SOF marker without decoding.
 * Returns 0 on success. */
int nd_jpeg_size(const UINT8 *data, UINT32 len, int *w, int *h);

#endif /* NOVUS_ND_JPEG_H */
