/**
 * Novus OS - the firmware's own boot logo, captured once and reused.
 *
 * Every UEFI machine paints its vendor logo (HP, Dell, Lenovo, a board
 * vendor's mark) before it hands control to the boot option. That image
 * already exists on the scanout at the instant our boot stage starts, and
 * it is the one picture the firmware is willing to show the owner of the
 * machine - so the splash shows it instead of a mark of our own, which
 * makes a Novus boot look like the machine's boot rather than an OS
 * advertisement.
 *
 * Nothing about the image is assumed: the logo is found by differencing
 * the scanout against its own corner colour, so the same code works on
 * any vendor, at any resolution, with no table of models to maintain.
 * Pixels close to that background colour become transparent, which is what
 * lets the logo sit on the splash gradient instead of inside a black
 * rectangle, and lets the boot-to-desktop animation fade it out cleanly.
 *
 * The capture runs in the boot stage, before nd.h's display stack has
 * anything to bind and before the firmware console is cleared; the buffer
 * it fills lives in .data so the kernel stage still sees it after .bss is
 * zeroed on the way into kmain.
 */

#ifndef NOVUS_ND_OEMLOGO_H
#define NOVUS_ND_OEMLOGO_H

#include "uefi.h"
#include "nd.h"

/* Read the scanout the firmware is showing right now and keep the
 * region that differs from its background. Called from the boot stage
 * with the EFI system table; returns 1 when something logo-shaped
 * was found, 0 when the screen was blank or held a full-screen picture
 * (in which case the splash falls back to its own mark). */
int nd_oem_logo_capture(void *system_table);

int   nd_oem_logo_available(void);
INT32 nd_oem_logo_width(void);
INT32 nd_oem_logo_height(void);

/* Why the last capture returned 0, in one short phrase ("the marked box
 * covers most of the screen (full-screen splash)", ...). The boot log prints
 * it so a machine that shows no logo says which shape check refused it -
 * that is the line a "loosen the matching conditions" decision is made from,
 * and the real machine is the only place it can be observed. */
const char *nd_oem_logo_reason(void);

/* Draw the logo into `dst`, scaled to fit it exactly, composited with
 * per-pixel transparency and `fade` (0 = invisible, 255 = as captured).
 * Self-contained on purpose: it has to work in the boot stage, where there
 * is no heap and no off-screen surface to scale into. */
void nd_oem_logo_draw(nd_surface_t *dst, nd_rect_t dst_rect, UINT8 fade);

/* Fallback for the machine whose firmware splash fills the screen instead of
 * holding a badge: the whole first frame is kept, reduced to a coarse grid so
 * it costs a third of a megabyte, and painted as a backdrop. `dim` scales the
 * brightness (255 keeps it as captured), `fade` blends it over what is under
 * it. Returns 1 when a frame was captured. */
int  nd_oem_frame_available(void);
void nd_oem_frame_draw(nd_surface_t *dst, nd_rect_t dst_rect, UINT8 fade,
                       UINT8 dim);

#endif /* NOVUS_ND_OEMLOGO_H */
