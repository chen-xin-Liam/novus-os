/**
 * Novus OS - NovusDisplay: 2D software renderer (M5).
 *
 * README part 11 splits the display stack in four layers. This header is
 * layers 1 and 2: the hardware abstraction that finds a linear
 * framebuffer, and the renderer that draws rectangles, lines, circles,
 * alpha blends and text into any surface. Layer 3 (the widget library)
 * sits on top in nvgui.h and only ever sees `nd_surface_t`.
 *
 * Pixel format: one UINT32 per pixel, 0xAARRGGBB, alpha ignored except by
 * the blend helpers. The framebuffer backend converts to whatever the
 * firmware handed out (BGR or RGB byte order) at draw time, so the rest
 * of the stack never thinks about the hardware.
 *
 * Drawing is damage driven: the GUI keeps one rectangle per state change
 * and redraws only that region. Since M11 the stack also composes
 * off-screen (nd_frame) and shows the result with a single nd_present, so
 * the visible screen only ever holds finished frames - the model Windows
 * uses, and the reason a drag or a theme switch does not flicker.
 */

#ifndef NOVUS_ND_H
#define NOVUS_ND_H

#include "uefi.h"
#include "nd_input.h"

/* ---- colours ---------------------------------------------------------- */

#define ND_ARGB(a, r, g, b)                                            \
  (((UINT32)(a) << 24) | ((UINT32)(r) << 16) | ((UINT32)(g) << 8) |    \
   (UINT32)(b))
#define ND_RGB(r, g, b) ND_ARGB(0xFFu, r, g, b)
#define ND_ALPHA(c) ((UINT8)(((UINT32)(c)) >> 24))
#define ND_RED(c) ((UINT8)(((UINT32)(c)) >> 16))
#define ND_GREEN(c) ((UINT8)(((UINT32)(c)) >> 8))
#define ND_BLUE(c) ((UINT8)((UINT32)(c)))

typedef UINT32 nd_color_t;

/* ---- geometry --------------------------------------------------------- */

typedef struct {
  INT32 x;
  INT32 y;
  INT32 w;
  INT32 h;
} nd_rect_t;

static inline nd_rect_t nd_rect(INT32 x, INT32 y, INT32 w, INT32 h) {
  nd_rect_t r;
  r.x = x;
  r.y = y;
  r.w = w;
  r.h = h;
  return r;
}

static inline int nd_rect_empty(nd_rect_t r) { return r.w <= 0 || r.h <= 0; }

static inline int nd_rect_contains(nd_rect_t r, INT32 x, INT32 y) {
  return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static inline nd_rect_t nd_rect_intersect(nd_rect_t a, nd_rect_t b) {
  INT32 x0 = (a.x > b.x) ? a.x : b.x;
  INT32 y0 = (a.y > b.y) ? a.y : b.y;
  INT32 x1 = (a.x + a.w < b.x + b.w) ? a.x + a.w : b.x + b.w;
  INT32 y1 = (a.y + a.h < b.y + b.h) ? a.y + a.h : b.y + b.h;
  return nd_rect(x0, y0, x1 - x0, y1 - y0);
}

static inline nd_rect_t nd_rect_union(nd_rect_t a, nd_rect_t b) {
  INT32 x0, y0, x1, y1;
  if (nd_rect_empty(a)) {
    return b;
  }
  if (nd_rect_empty(b)) {
    return a;
  }
  x0 = (a.x < b.x) ? a.x : b.x;
  y0 = (a.y < b.y) ? a.y : b.y;
  x1 = (a.x + a.w > b.x + b.w) ? a.x + a.w : b.x + b.w;
  y1 = (a.y + a.h > b.y + b.h) ? a.y + a.h : b.y + b.h;
  return nd_rect(x0, y0, x1 - x0, y1 - y0);
}

/* ---- surfaces --------------------------------------------------------- */

typedef struct {
  UINT32    *px;     /* 0xAARRGGBB; NULL for a surface that failed */
  INT32      w;
  INT32      h;
  UINT32     stride; /* pixels per row, may exceed w (framebuffer) */
  nd_rect_t  clip;   /* current scissor, surface coordinates */
  UINT8      owned;  /* 1 = pixels came from kmalloc -> free them */
  UINT8      fb;     /* 1 = linear framebuffer, so pixels get swapped */
  UINT8      bgr;    /* byte order of that framebuffer */
  UINT8      pad;
} nd_surface_t;

/* Create/destroy an off-screen surface (used by tests and by any future
 * compositor). Returns 0 on success. */
int nd_surface_create(nd_surface_t *s, INT32 w, INT32 h);
void nd_surface_destroy(nd_surface_t *s);
void nd_surface_set_clip(nd_surface_t *s, nd_rect_t clip);
void nd_surface_reset_clip(nd_surface_t *s);

/* ---- display hardware ------------------------------------------------ */

typedef struct {
  UINT8       available; /* 1 = a linear framebuffer was captured */
  UINT8       bgr;       /* 1 = memory byte order B,G,R,X */
  UINT8       fb_boot_usable; /* 1 = the boot stage's page tables reach it */
  UINT32      width;
  UINT32      height;
  UINT32      pitch;     /* pixels per scanline */
  UINT32      bpp;
  UINT64      fb_phys;
  UINT64      fb_size;
  const char *backend;   /* "GOP framebuffer" or "none" */
} nd_display_t;

/* One graphics output. The boot stage finds every GOP device the firmware
 * offers, reads its EDID and picks the mode that matches the display's
 * preferred timing; the kernel renders into the primary one and mirrors
 * the picture onto the others. */
#define ND_MAX_OUTPUTS 4u

typedef struct {
  UINT8  available;      /* 1 = usable framebuffer */
  UINT8  bgr;            /* 1 = memory byte order B,G,R,X */
  UINT8  digital;        /* 1 = digital link (DP/eDP/HDMI), 0 = VGA */
  UINT8  primary;        /* 1 = the output the desktop is drawn on */
  UINT32 width;
  UINT32 height;
  UINT32 pitch;
  UINT32 pref_width;     /* EDID preferred timing, 0 when unknown */
  UINT32 pref_height;
  UINT64 fb_phys;
  UINT64 fb_size;
} nd_output_t;

/* The outputs the firmware offered, primary first. */
const nd_output_t *nd_outputs(UINT32 *count);

/* Copy a rectangle of the primary output onto the other ones. Pixels are
 * converted when the two framebuffers use a different byte order, and the
 * copy is clipped to the smaller output. */
void nd_mirror_rect(nd_rect_t r);

/* Boot stage, before ExitBootServices: read the current GOP mode and keep
 * the numbers. Returns 1 when a usable linear framebuffer was found. */
int nd_capture_boot(void *system_table);

/* The boot stage's millisecond clock, handed in by the caller of
 * nd_capture_boot(): firmware calls burn host time without advancing the
 * guest TSC on an emulated machine, so capture timing is only meaningful
 * against a real-time clock. */
void nd_boot_clock_set(UINT64 (*fn)(void));

/* Kernel stage, after mm_init()/heap_init(): bind a surface to the
 * captured framebuffer and log the mode. Returns 1 when the screen is
 * drawable. */
int nd_init(void);
const nd_display_t *nd_display(void);
nd_surface_t *nd_screen(void);

/* ---- presentation ----------------------------------------------------- */

/* The surface a compositor draws into. It is an off-screen buffer the size
 * of the primary output when one could be allocated, and the framebuffer
 * itself when it could not (no heap yet, or a screen too large): callers
 * draw into whatever this returns and never care which. */
nd_surface_t *nd_canvas(void);

/* 1 when nd_canvas() is a separate buffer, i.e. a present is one short copy
 * rather than every drawing call landing on the glass. */
int nd_canvas_buffered(void);

/* 1 when the scanout mapping really is write-combining (PAT or a WC MTRR
 * honoured). Full-frame copies are the only thing this decides: an
 * uncached scanout can be tens of milliseconds per frame, so the boot
 * hand-over must not animate on one. */
int nd_framebuffer_is_wc(void);

/* B-18: a one-line verdict on the scanout's memory type (page attribute +
 * MTRR + the late PAT pass's own state) followed by a timed non-temporal
 * burst into the last visible row. Printed once, after the first frame, so the
 * boot log actually keeps it. */
void nd_report_scanout(void);
void nd_fb_bench(void);

/* Copy one finished rectangle from nd_canvas() to the visible screen and to
 * every mirror output, then repaint the software pointer on top.
 *
 * Every pixel a user sees goes through here. Composing off-screen and
 * presenting once keeps a half-painted frame off the glass (the flicker a
 * direct-to-framebuffer renderer shows while a window is dragged), waiting
 * for the vertical blank keeps a present from crossing the scanout (the
 * tear line a long blit leaves), and the pointer being an overlay rather
 * than save-under means it never blinks out between two draws. */
void nd_present(nd_rect_t r);

/* 1 when presents are synchronised to the display's vertical blank. */
int nd_present_vsync(void);

/* Wait for the start of the vertical blank on the primary output. Returns 1
 * when a usable retrace source was found and the wait actually synchronised,
 * 0 when the machine has no probeable VBlank (caller may proceed at once).
 * Bounded: it never spins longer than one blank interval. */
int nd_wait_vsync(void);

/* ---- CRTC compositing seam (Mini-DWM) --------------------------------- */

/* Blit canonical 0xAARRGGBB pixels from `src` (stride in pixels) into output
 * `index` (0 = primary, 1.. = mirror outputs in nd_outputs() order) at (`x`,
 * `y`). Byte order is converted for framebuffers that store B,G,R,X. The
 * rectangle is clipped to the output; nothing is done for an invalid index.
 * No VSync wait or cursor overlay is performed here - callers compose first
 * and call nd_present()/crtc_present() to scan out. */
void nd_crtc_blit(UINT32 index, const UINT32 *src, UINT32 src_stride,
                  INT32 x, INT32 y, UINT32 w, UINT32 h);

/* ---- frame timing ----------------------------------------------------- */

/* Raw TSC and its calibrated rate. Shell paint code uses the same clock as
 * the HAL so a paint and its present can be compared in one log stream. */
UINT64 nd_tsc(void);
UINT64 nd_tsc_khz(void);

/* Write one `[NDF]` line with a measured interval and the rectangle it
 * covers. Called after the measurement, so serial output is not timed. */
void nd_frame_timing(const char *label, UINT64 t0, UINT64 t1,
                     UINT32 w, UINT32 h);

/* Running totals for every nd_present() since boot. Full-screen frames are
 * logged individually; small damage frames are counted here and summarised
 * every 60 presents. */
typedef struct {
  UINT64 frames;
  UINT64 cycles;
  UINT64 pixels;
  UINT64 max_cycles;
} nd_frame_stats_t;

void nd_frame_stats(nd_frame_stats_t *out);

/* Software mouse pointer, composited on top of a present.
 *
 * `nd_cursor_set` only moves it and records the damage the move exposes;
 * the arrow itself is painted onto the framebuffer at the end of every
 * present, so no caller has to hide it before drawing and restore it
 * afterwards. A compositor merges nd_cursor_take_damage() into its own
 * damage rectangle, which is what erases the old arrow. */
void nd_cursor_hide(void);
void nd_cursor_set(INT32 x, INT32 y);
void nd_cursor_draw(void);
void nd_cursor_get(INT32 *x, INT32 *y);

/* Pending pointer damage (old and new position), 1 when there is some. */
int nd_cursor_take_damage(nd_rect_t *out);

/* ---- boot splash ------------------------------------------------------ */

/* Paint the boot screen: mark, wordmark and an empty progress bar centred
 * on the primary output, on a dark backdrop. Safe in the boot stage (it
 * only needs the framebuffer nd_capture_boot() found) and in the kernel
 * before nd_init(); returns 0 when there is no drawable output. */
int nd_boot_splash(void);

/* Re-paint the whole splash after the kernel has rebound the framebuffer
 * (nd_init()). The boot stage's frame was painted through the firmware's
 * mapping and is covered by the text console by then, so this redraws the
 * black field, the mark, the rail, the status line and the bottom log on
 * the bound surface. Returns 0 when there is no drawable output. */
int nd_boot_splash_rebind(void);

/* Advance the bar. `percent` is clamped and never goes backwards; `text`
 * replaces the stage label under the bar when it is not NULL. Only the bar
 * and the label are repainted, so the splash never flickers. */
void nd_boot_stage(UINT32 percent, const char *text);

/* 1 while the splash owns the screen: nd_init() then leaves the pixels
 * alone instead of clearing them. */
int nd_boot_splash_active(void);

/* The framebuffer surface the boot splash draws into. Valid while the splash
 * is active (before the desktop presents); used by the boot menu, which runs
 * before nd_demo_init() binds the live compositor surface. */
nd_surface_t *splash_surface(void);

/* The desktop is about to be presented: the splash stops owning the
 * screen. */
void nd_boot_splash_end(void);

/* Something painted over the splash frame (the pre-desktop menu draws its own
 * full-screen card). The mark is no longer on the glass, so the next
 * nd_boot_stage() lays it down again - once, not once per log line. */
void nd_boot_splash_mark_dirty(void);

/* Where the splash drew the machine's boot logo (its resting rectangle on
 * the splash surface), for the boot-to-desktop transition. Answers for both
 * the firmware's captured logo and the baked mark. Returns 0 when the splash
 * drew no mark at all. */
int nd_boot_logo_rect(nd_rect_t *out);

/* "mk" / "fw" / "bak" / "none": which boot picture the splash actually used.
 * Shown in the desktop status line so the board-logo verdict is readable
 * without a log. */
const char *nd_boot_logo_tag(void);

/* The rectangle the splash reserves for the boot log. The kernel's text
 * console takes that band over, so what the user reads at the bottom of the
 * panel is the live kputs() stream and the last line is where the boot
 * actually stopped. Returns 0 when the splash never laid a panel out. */
int nd_boot_log_rect(nd_rect_t *out);

/* Tells the splash to stop painting its snapshot of the log: the text console
 * owns the band from that point on. */
void nd_boot_splash_log_off(void);

/* Eagerly repaint the boot-log panel from the kernel's ring. Called once per
 * completed log line while the splash is up; without this the panel is only
 * as fresh as the last nd_boot_stage(), and a machine that stops in the
 * middle of a driver probe would show a log that already looks finished. */
void nd_boot_log_refresh(void);

/* Draw that same mark into `s` at `r` with `alpha` (0 = invisible). The
 * desktop transition uses it to lift the mark off the splash; the sampler is
 * the splash's, so the first transition frame is identical to the last
 * splash frame. */
void nd_boot_logo_draw(nd_surface_t *s, nd_rect_t r, UINT8 alpha);

/* Play the boot animation's opening: paint the empty frame (backdrop, rail
 * at zero, no mark, no label) and take ownership of the screen. The frames
 * themselves are authored by the caller and handed over one at a time with
 * nd_boot_anim_frame(), so the timeline - and the clock it runs on - lives
 * in the boot stage. Returns 0 when there is no drawable output. */
int nd_boot_anim_begin(void);

/* One frame of the timeline. `logo_scale` is the mark's size as a percentage
 * of its resting rectangle (>100 is clamped), `logo_alpha` its opacity,
 * `bar` the rail fill in percent (monotonic: a smaller value is ignored) and
 * `label` the stage line under the rail (NULL keeps the current one). Only
 * the marks strip is repainted, at a fixed cost per frame: the animation
 * runs before the heap exists and cannot afford full-panel repaints. */
void nd_boot_anim_frame(UINT32 logo_scale, UINT8 logo_alpha, UINT32 bar,
                        const char *label);

/* B-52: indeterminate motion on the same rail. Used for the last stretch
 * ("entering the kernel"), where the remaining work has no honest
 * percentage: a short comet runs along the bar, drawn from primitives only
 * (no bitmap, no glyph). `tick` is the caller's frame counter - any
 * increasing value works, and the caller owns the pace. Whole rail only. */
void nd_boot_spin(UINT32 tick);

/* The closing move: dim the boot screen to black while the mark stays
 * opaque, leaving the exact frame the desktop transition starts from. Call
 * it immediately before nd_boot_transition(); it is the last thing the boot
 * animation does, and it is what makes the hand-over a fade rather than a
 * cut. Kernel stage only (it sleeps on the scheduler tick). */
void nd_boot_anim_close(void);

/* One frame of the splash-to-desktop crossfade over `r`: the boot screen's
 * field and mark at `veil` opacity, composited over what `dst` already holds.
 * Called with the mark's rectangle, the only area that still changes once the
 * desktop itself is on the glass. */
void nd_boot_handover_frame(nd_surface_t *dst, nd_rect_t r, UINT8 veil);

/* Convert one canonical colour to what the framebuffer expects. */
UINT32 nd_hw_pixel(const nd_surface_t *s, nd_color_t c);

/* ---- drawing primitives ---------------------------------------------- */

void nd_clear(nd_surface_t *s, nd_color_t c);
void nd_pixel(nd_surface_t *s, INT32 x, INT32 y, nd_color_t c);
nd_color_t nd_get_pixel(const nd_surface_t *s, INT32 x, INT32 y);
void nd_fill_rect(nd_surface_t *s, nd_rect_t r, nd_color_t c);
void nd_blend_rect(nd_surface_t *s, nd_rect_t r, nd_color_t c, UINT8 alpha);
void nd_frame(nd_surface_t *s, nd_rect_t r, INT32 thickness, nd_color_t c);
void nd_hline(nd_surface_t *s, INT32 x0, INT32 x1, INT32 y, nd_color_t c);
void nd_vline(nd_surface_t *s, INT32 x, INT32 y0, INT32 y1, nd_color_t c);
void nd_line(nd_surface_t *s, INT32 x0, INT32 y0, INT32 x1, INT32 y1,
             nd_color_t c);
void nd_fill_circle(nd_surface_t *s, INT32 cx, INT32 cy, INT32 radius,
                    nd_color_t c);
void nd_circle(nd_surface_t *s, INT32 cx, INT32 cy, INT32 radius,
               nd_color_t c);
/* Axis-aligned ellipse: FILL paints the interior, the outline version
 * walks the same spans and touches only their two ends. */
void nd_fill_ellipse(nd_surface_t *s, INT32 cx, INT32 cy, INT32 rx, INT32 ry,
                     nd_color_t c);
void nd_ellipse(nd_surface_t *s, INT32 cx, INT32 cy, INT32 rx, INT32 ry,
                nd_color_t c);
void nd_round_rect(nd_surface_t *s, nd_rect_t r, INT32 radius, nd_color_t c);
void nd_round_frame(nd_surface_t *s, nd_rect_t r, INT32 radius, INT32 thick,
                    nd_color_t c);
void nd_gradient_v(nd_surface_t *s, nd_rect_t r, nd_color_t top,
                   nd_color_t bottom);
void nd_shadow(nd_surface_t *s, nd_rect_t r, INT32 spread, UINT8 alpha);
void nd_blit(nd_surface_t *dst, INT32 x, INT32 y, const nd_surface_t *src,
             nd_rect_t area);
void nd_blit_alpha(nd_surface_t *dst, INT32 x, INT32 y,
                   const nd_surface_t *src, nd_rect_t area, UINT8 alpha);

/* ---- text ------------------------------------------------------------- */
/* Built-in 5x7 font in a 6x9 cell, drawn at 1x..4x. No font files, no
 * firmware font: the same glyphs look identical on every machine. */

#define ND_GLYPH_W 5
#define ND_GLYPH_H 7
#define ND_FONT_ADV 6
#define ND_FONT_LINE 9

/* Everything outside ASCII is a 16x16 (full width) or 8x16 (half width)
 * bitmap from the generated Unicode table: one height, independent of the
 * ASCII scale. Mixed lines take the taller cell and share a baseline, so a
 * Chinese label next to a number sits on the same line. */
#define ND_CJK_W 16
#define ND_CJK_H 16
#define ND_CJK_ADV 16

/* Decode one UTF-8 codepoint and advance the cursor past it. Overlong
 * forms, surrogates and values above U+10FFFF decode as 0xFFFD and consume
 * one byte, so a broken string still renders something and cannot loop. */
UINT32 nd_utf8_next(const char **text);

/* Encode one codepoint into `out` (at least four bytes). Returns 1..4, or 0
 * when `cp` is not a Unicode scalar value. */
UINT32 nd_utf8_encode(UINT32 cp, char out[4]);

/* Advance of one codepoint at `scale`: 0 for control and combining marks,
 * 6 for printable ASCII, 8/16 for the rest of Unicode. */
UINT32 nd_char_adv(UINT32 cp, UINT32 scale);

UINT32 nd_text_width(const char *text, UINT32 scale);

/* Width of at most `bytes` bytes of `text`, stopping on a codepoint
 * boundary. Used by the text box for its scroll window and caret. */
UINT32 nd_text_width_n(const char *text, UINT32 bytes, UINT32 scale);
UINT32 nd_font_height(UINT32 scale);

/* Height of the cell this string needs: ASCII-only keeps 7*scale, a line
 * with CJK needs 16. Callers centre with this, not with nd_font_height. */
UINT32 nd_text_height(const char *text, UINT32 scale);

/* The 7 rows of one glyph, bit 4 the leftmost of the five columns. Values
 * outside 32..126 come back blank rather than as a garbage shape. The
 * kernel's fallback text console draws with these directly, straight onto
 * the framebuffer, before nd_init() exists. */
const UINT8 *nd_font_rows(UINT8 ch);

void nd_text(nd_surface_t *s, INT32 x, INT32 y, const char *text, nd_color_t c,
             UINT32 scale);
/* `max_px` limits the painted width (0 = unlimited); clipping at a
 * character boundary, never mid-glyph. */
void nd_text_limit(nd_surface_t *s, INT32 x, INT32 y, const char *text,
                   UINT32 max_px, nd_color_t c, UINT32 scale);

/* ---- M5 demo and self test ------------------------------------------- */

/* Shell housekeeping the WM tick calls: the taskbar clock, and anything
 * else that must advance even when the shell is not being painted. */
void nd_demo_idle(void);

/* Draws into an off-screen surface, checks exact pixels, widget state and
 * event routing, and prints PASS/FAIL on COM1. Runs before ring 3. */
void nd_selftest(void);

/* Builds the demo desktop and paints it once. */
void nd_demo_init(void);

/* Called from the timer tick: poll input, dispatch, repaint damage. The
 * M6 window manager replaces this with its own tick and drives the shell
 * through the four hooks below. */
void nd_demo_tick(void);

/* 1 once the shell scene is built and drawable. */
int nd_demo_ready(void);

/* Hand one input event to the shell's widget tree (absolute screen
 * coordinates, as nd_input produced them). */
void nd_demo_dispatch(const nd_event_t *ev);

/* Take the shell's pending damage rectangle, 1 when there is one. */
int nd_demo_take_damage(nd_rect_t *out);

/* Repaint the shell scene clipped to `clip` (the WM composites windows
 * on top of this). */
void nd_demo_paint(nd_rect_t clip);

/* Replace the desktop's bottom-left status line. Used for a one-off pointer
 * verdict that has to survive the splash going away. */
void nd_demo_status(const char *s);

/* A one-off strip across the top of the desktop, held for `hold_ms` and then
 * repainted away. Nothing a client window can cover. */
void nd_demo_banner(const char *text, UINT32 hold_ms);

/* Full-screen boot report: one line per item, kept on the glass for
 * `hold_ms` before the desktop takes over. This is the report a machine with
 * no serial port gives: the numbers are on the screen, so a photograph of
 * this page is the whole diagnosis. */
void nd_demo_report(const char *text, UINT32 hold_ms);

/* Paint the full-screen report without the hold. The boot diagnostic loop
 * calls this between feed slices so the page reflects the latest port pass. */
void nd_demo_report_draw(const char *text);

/* Rebuild the shell widget tree after a theme change. Client windows are
 * owned by the WM and survive; the shell's own demo windows are recreated
 * hidden, which is the price of a static widget pool. */
void nd_demo_theme_sync(void);

/* Open the title-bar context menu for a WM client window at screen (x, y).
 * `state` is the WM window state: 2 means maximized, which swaps the
 * maximize item for restore. The items call back into nwm_window_command().
 * Called by the WM on a right press over a caption; the shared menu is an
 * overlay, so it is composited above the windows. */
void nd_shell_window_menu(INT32 x, INT32 y, UINT32 win_id, UINT8 state);

/* Close the shell's shared context menu if it is open. The WM calls this
 * before it delivers a press to a client window or to the desktop, so the
 * menu cannot outlive the click that dismissed it. */
void nd_shell_menu_close(void);

/* ---- shell animation ------------------------------------------------- */

/* Sleep on hlt until `ms` have elapsed on the 10 ms tick. Used by the two
 * transitions below, which run before and outside the normal tick loop. */
void nd_wait_ms(UINT32 ms);

/* The dock - the floating taskbar pill and every icon, run dot and clock
 * inside it - is one unit that can be moved as a whole: `dy` is the pixel
 * offset from where build_desktop() placed it, 0 restores that. Both the
 * old and the new band are damaged, so a slide repaints only the strip
 * that changed. */
void  nd_dock_shift(INT32 dy);
/* Distance from the dock's built position to fully off the bottom edge;
 * the shutdown slide uses it, and the boot slide starts from it. */
INT32 nd_dock_span(void);

/* Close the shell's floating cards (start menu, quick settings), its
 * context menu and any hover/press highlight, without touching windows:
 * the first step of a shutdown. */
void nd_shell_close_overlays(void);

/* Splash to desktop: the vendor logo lifts and fades off the top, the dock
 * rises into place, the wallpaper and desktop fade up from black. Runs
 * once, from nd_demo_init(), on the finished desktop scene. */
void nd_boot_transition(void);

#endif /* NOVUS_ND_H */
