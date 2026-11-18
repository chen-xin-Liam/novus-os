/**
 * Novus OS - real image icons (M10).
 *
 * Until now every icon in the shell was a vector shape compiled into
 * nvgui.c: crisp at any size, but a fixed set the user could not change.
 * This module adds the other half - an ICO reader, so an icon is a file
 * again: /Novus/icons/<name>.ico on the system partition, decoded at boot
 * (or lazily on first use) and drawn as a tinted bitmap.
 *
 * The built-in set ships as C blobs generated from assets/icons by the
 * Makefile, so the shell still has icons on a medium that carries no
 * system partition. A file dropped into /Novus/icons wins over the blob.
 *
 * Only ICO is decoded here. SVG would need a path rasteriser and XML
 * parser inside the kernel; PNG payloads inside an ICO would need the
 * JPEG decoder's cousin. Both are refused with a log line rather than
 * silently drawing nothing.
 */
#ifndef NOVUS_ND_ICON_H
#define NOVUS_ND_ICON_H

#include "nd.h"

struct novusfs_fs;

/* A decoded icon: square, straight-alpha 0xAARRGGBB pixels, top-down.
 * The shipped icons are white-on-transparent so the shell can tint them
 * with the theme colour instead of hard-coding one. */
typedef struct nd_icon_s {
  UINT32  w;
  UINT32  h;
  UINT32 *px;
} nd_icon_t;

/* Remember the system partition so later loads can look for user icons.
 * Call once the root filesystem is up; NULL means "built-ins only". */
void nd_icon_init(struct novusfs_fs *fs);

/* Fetch an icon by file stem ("files", "settings", ...) at `px` pixels.
 * The result is cached and owned by this module - do not free it. NULL
 * means neither the user folder nor the built-in set had that name. */
const nd_icon_t *nd_icon_get(const char *name, UINT32 px);

/* Decode one ICO image from memory, scaled to px x px. 0 on success.
 * `out->px` is kmalloc'ed; free with nd_icon_free(). */
int nd_icon_decode(const UINT8 *data, UINT64 len, UINT32 px, nd_icon_t *out);
void nd_icon_free(nd_icon_t *ic);

/* Draw the icon's alpha as `tint`, `alpha` 0..255, at (x, y). The call is
 * clipped; pixels outside the surface or the scissor are dropped. */
void nd_icon_blit(nd_surface_t *s, const nd_icon_t *ic, INT32 x, INT32 y,
                  nd_color_t tint, UINT8 alpha);

#endif /* NOVUS_ND_ICON_H */
