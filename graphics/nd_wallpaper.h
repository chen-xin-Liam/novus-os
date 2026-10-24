/**
 * Novus OS - desktop photo wallpaper.
 */

#ifndef NOVUS_ND_WALLPAPER_H
#define NOVUS_ND_WALLPAPER_H

#include "uefi.h"
#include "nd.h"

/* Number of photo wallpapers; keep in sync with graphics/wpkg.h's
 * WPKG_MAX_IMAGES ceiling, the Makefile's WP_IMGS list and the theme's
 * paper colours. */
#define ND_WALLPAPER_COUNT 7

/* Built-in photo wallpapers, packed into WPAPERS.PKG from
 * assets/backgrounds-screen by tools/wpkg and staged by the boot stage
 * (graphics/wpkg.h describes the layout). Index 0 is the default. The order
 * matches the theme's paper colours, the settings pickers and the Makefile's
 * WP_IMGS list. */
int         nd_wallpaper_count(void);
int         nd_wallpaper_default(void);
UINT32      nd_wallpaper_current(void);
int         nd_wallpaper_ready(void);
const char *nd_wallpaper_name(UINT32 index);

/* Select `index` and decode it for the screen size nd_wallpaper_init()
 * received. Returns 0 when the image is ready; on failure - including a
 * medium that carries no package - the previous image (or the flat colour)
 * stays active. */
int         nd_wallpaper_set(UINT32 index);

/* Remember the screen size and decode the current selection. Call once
 * after nd_init(); safe to call again (e.g. after a mode change). */
int         nd_wallpaper_init(INT32 screen_w, INT32 screen_h);

/* Paint the wallpaper over `area`, falling back to a flat `fallback` colour
 * while no image is decoded: no package on the medium, or a decode
 * failure. */
void        nd_wallpaper_paint(nd_surface_t *s, const nd_rect_t *area,
                               nd_color_t fallback);

#endif /* NOVUS_ND_WALLPAPER_H */
