/**
 * Novus OS - NovusGUI widget library (M5).
 *
 * README 11.2 layer 3: window, button, text box, slider, list, tree. The
 * library is retained but immediate-drawing: widgets hold state and are
 * redrawn on demand into whatever surface the caller hands over, clipped
 * to a damage rectangle. That is deliberate - M6 puts a window manager on
 * top, and a WM's job is exactly "who owns which rectangle, redraw only
 * that".
 *
 * Dependency rule: this layer only talks to nd.h (surfaces, primitives,
 * text) and nd_input.h (events). It never touches the framebuffer, the
 * PMM or the scheduler, so it can move into user space unchanged when the
 * M7 compiler can link against it.
 *
 * Widgets come from a fixed pool (`NG_POOL_SIZE`), not from kmalloc: a GUI
 * built from static widgets needs no allocator in interrupt context, and
 * running out is a compile-time-ish bug rather than an OOM path.
 */

#ifndef NOVUS_NVGUI_H
#define NOVUS_NVGUI_H

#include "nd.h"
#include "nd_input.h"

#define NG_POOL_SIZE 192
#define NG_TITLE_MAX 40

/* Widget flags. */
#define NG_WF_VISIBLE   0x0001u
#define NG_WF_ENABLED   0x0002u
#define NG_WF_FOCUSABLE 0x0004u
/* Drawn by ng_render_overlay(), i.e. above client windows: the start menu
 * and anything else that has to float over the desktop. */
#define NG_WF_OVERLAY  0x0008u

/* Runtime state bits. */
#define NG_ST_HOVER   0x0001u
#define NG_ST_PRESSED 0x0002u
#define NG_ST_FOCUS   0x0004u
#define NG_ST_DRAG    0x0008u

/* Theme metrics, in pixels at text scale 2. */
#define NG_TITLE_H  32
/* Drop shadow falloff around a window, in pixels. Damage for a window has
 * to cover this ring as well as the window rectangle. */
#define NG_SHADOW_SPREAD 10
#define NG_BORDER   1
#define NG_PAD      10
#define NG_ROW_H    24
#define NG_TEXT_SCALE 2u

typedef enum {
  NG_DESKTOP = 0,
  NG_WINDOW,
  NG_PANEL,
  NG_LABEL,
  NG_BUTTON,
  NG_CHECKBOX,
  NG_TEXTBOX,
  NG_SLIDER,
  NG_PROGRESS,
  NG_LIST,
  NG_TREE,
  NG_ICON,
  NG_SWATCH
} ng_type_t;

/* Glyphs are drawn from primitives, never loaded from a resource: one
 * monochrome shape set that recolours with the theme and needs no bitmap
 * pipeline in a client that can only send rectangles. */
typedef enum {
  NG_ICON_NONE = 0,
  NG_ICON_START,
  NG_ICON_FILES,
  NG_ICON_TASKS,
  NG_ICON_SETTINGS,
  NG_ICON_NET,
  NG_ICON_SEARCH,
  NG_ICON_POWER,
  NG_ICON_SUN,
  NG_ICON_MOON,
  NG_ICON_AUTO,
  NG_ICON_INSTALL,
  NG_ICON_TERM,
  NG_ICON_CHECK,
  NG_ICON_CHEVRON,
  NG_ICON_DISK,
  NG_ICON_DEVICES
} ng_icon_kind_t;

typedef struct ng_widget ng_widget_t;
typedef struct ng_ctx ng_ctx_t;

typedef void (*ng_action_fn)(ng_ctx_t *ctx, ng_widget_t *w);
typedef void (*ng_value_fn)(ng_ctx_t *ctx, ng_widget_t *w, INT32 value);

/* One row of a tree control. `depth` is the indentation level; a node
 * with `has_children` draws a chevron and `expanded` decides whether the
 * following deeper nodes are visible. */
typedef struct {
  const char *text;
  UINT8       depth;
  UINT8       has_children;
  UINT8       expanded;
  UINT8       pad;
  INT32       id;
} ng_tnode_t;

struct ng_widget {
  ng_type_t    type;
  INT32        id;
  nd_rect_t    rect; /* relative to the parent's client origin */
  nd_rect_t    abs;  /* recomputed by ng_layout */
  UINT32       flags;
  UINT32       state;
  ng_widget_t *parent;
  ng_widget_t *first;
  ng_widget_t *next;
  ng_widget_t *window; /* nearest NG_WINDOW ancestor, for dragging */
  ng_action_fn on_action;
  ng_value_fn  on_value;
  union {
    struct {
      char  title[NG_TITLE_MAX];
      UINT8 closable;
    } window;
    struct {
      const char *text;
      nd_color_t  color;
      UINT32      scale;
      UINT8       align; /* 0 left, 1 centre, 2 right */
    } label;
    struct {
      const char *text;
      UINT8       icon; /* ng_icon_kind_t, 0 = a plain text button */
    } button;
    struct {
      const char *text;
      UINT8       checked;
    } check;
    struct {
      char       *buf;
      UINT32      cap;
      UINT32      len;
      UINT32      cursor;
      UINT32      scroll;
      const char *placeholder;
      UINT8       password;
    } edit;
    struct {
      INT32 min;
      INT32 max;
      INT32 value;
    } slider;
    struct {
      INT32       permille;
      const char *text;
    } progress;
    struct {
      const char **items;
      UINT32       count;
      INT32        sel;
      INT32        top;
    } list;
    struct {
      ng_tnode_t *nodes;
      UINT32      count;
      INT32       sel;
      INT32       top;
    } tree;
    struct {
      nd_color_t color;
      UINT8      frame;  /* 1 px rounded border, for floating cards */
      UINT8      radius;
    } panel;
    struct {
      UINT8 kind; /* ng_icon_kind_t */
      UINT8 size; /* glyph box in pixels, 0 = 20 */
      UINT8 pad[2];
    } icon;
    struct {
      nd_color_t color;
      UINT8      selected;
      UINT8      pad[3];
    } swatch;
  } u;
};

typedef struct {
  nd_color_t desktop_top;
  nd_color_t desktop_bottom;
  nd_color_t win_bg;
  nd_color_t win_title;
  nd_color_t win_title_dim;
  nd_color_t win_border;
  nd_color_t text;
  nd_color_t text_dim;
  nd_color_t accent;
  nd_color_t accent_text;
  nd_color_t ctrl_bg;
  nd_color_t ctrl_hover;
  nd_color_t ctrl_border;
} ng_theme_t;

struct ng_ctx {
  nd_surface_t *surf;
  ng_widget_t  *root;
  ng_widget_t  *focus;
  ng_widget_t  *hover;
  ng_widget_t  *capture;
  ng_widget_t  *drag_win;
  INT32         drag_dx;
  INT32         drag_dy;
  INT32         mouse_x;
  INT32         mouse_y;
  UINT8         buttons;
  UINT32        frame;
  nd_rect_t     damage;
  ng_theme_t    theme;
};

/* Reset the pool and bind the context to a surface. Returns NULL when the
 * surface is not drawable. */
ng_ctx_t *ng_init(nd_surface_t *surf);
ng_ctx_t *ng_ctx_get(void);

/* Create a widget under `parent` (NULL means the desktop root). */
ng_widget_t *ng_create(ng_widget_t *parent, ng_type_t type, INT32 x, INT32 y,
                       INT32 w, INT32 h);

/* Per-type configuration. Each one resets the type-specific slice of the
 * widget and returns it, so calls can be chained. */
ng_widget_t *ng_window(ng_widget_t *w, const char *title, int closable);
ng_widget_t *ng_label(ng_widget_t *w, const char *text, nd_color_t color,
                      UINT32 scale);
ng_widget_t *ng_button(ng_widget_t *w, const char *text, ng_action_fn fn);
ng_widget_t *ng_checkbox(ng_widget_t *w, const char *text, int checked,
                         ng_action_fn fn);
ng_widget_t *ng_textbox(ng_widget_t *w, char *buf, UINT32 cap,
                        const char *placeholder);
ng_widget_t *ng_slider(ng_widget_t *w, INT32 min, INT32 max, INT32 value,
                       ng_value_fn fn);
ng_widget_t *ng_progress(ng_widget_t *w, INT32 permille, const char *text);
ng_widget_t *ng_list(ng_widget_t *w, const char **items, UINT32 count,
                     ng_value_fn fn);
ng_widget_t *ng_tree(ng_widget_t *w, ng_tnode_t *nodes, UINT32 count,
                     ng_value_fn fn);
ng_widget_t *ng_panel(ng_widget_t *w, nd_color_t color);
/* Give a panel a rounded border and corner radius (start menu, taskbar). */
void ng_panel_frame(ng_widget_t *w, int on);

/* A tile button: icon above a one-line caption, no chrome until hovered.
 * Used by the desktop grid, the taskbar and the start menu's pinned grid. */
ng_widget_t *ng_button_icon(ng_widget_t *w, const char *text,
                            ng_icon_kind_t kind, ng_action_fn fn);

/* A colour swatch (accent / wallpaper pickers). */
ng_widget_t *ng_swatch(ng_widget_t *w, nd_color_t color, int selected,
                       ng_action_fn fn);

/* Show/hide and enable/disable (both damage the widget). */
void ng_show(ng_widget_t *w, int visible);
void ng_set_enabled(ng_widget_t *w, int enabled);

/* Compute absolute rectangles from the relative layout. Call after moving
 * or resizing anything. */
void ng_layout(void);

/* Text editing used by the IME: insert one codepoint (or a UTF-8 string)
 * at the caret. Both return 1 on success, 0 when the buffer is full. */
int ng_edit_insert_cp(ng_widget_t *w, UINT32 cp);
int ng_edit_insert_text(ng_widget_t *w, const char *text);

/* Screen rectangle of the focused text box's caret, if it has one. */
int ng_focus_caret(nd_rect_t *out);

/* Windows 11 caption buttons (minimise, maximise, close) at the top right
 * of `frame`. `hovered` is 1..count for the button under the pointer, or 0.
 * Shared by the shell's windows and the window manager's. */
void ng_draw_caption(nd_surface_t *s, nd_rect_t frame, const ng_theme_t *t,
                     int hovered, int count);

/* Mark a widget (and its children) as overlay content. */
void ng_set_overlay(ng_widget_t *w, int on);

/* True when (x,y) is over a visible overlay card (an open start menu). */
int ng_overlay_hit(INT32 x, INT32 y);

/* Draw only the overlay widgets, on top of client windows. The window
 * manager calls this after compositing, so the start menu and the IME
 * candidate window stay visible even when a window covers them. */
void ng_render_overlay(nd_rect_t clip);

/* Draw the scene clipped to `clip`; ng_render_all() is the full screen. */
void ng_render(nd_rect_t clip);
void ng_render_all(void);

/* Route one input event. */
void ng_dispatch(const nd_event_t *ev);

void ng_damage(nd_rect_t r);
void ng_damage_widget(ng_widget_t *w);
int  ng_take_damage(nd_rect_t *out);

void ng_focus_set(ng_widget_t *w);
ng_widget_t *ng_focus_get(void);

/* Client area of a window in absolute coordinates. */
nd_rect_t ng_window_client(ng_widget_t *w);

/* Raise `w`'s top-level window above its siblings. */
void ng_raise(ng_widget_t *w);

/* An icon button: a 40x40 hit area (or the size given to ng_create) that
 * draws `kind` centred and behaves like a button. `size` is the glyph box
 * in pixels (0 = 20). */
ng_widget_t *ng_icon(ng_widget_t *w, ng_icon_kind_t kind, UINT32 size,
                     ng_action_fn fn);

/* Re-read the design tokens into the widget theme and damage the whole
 * canvas. Called by the shell when the light/dark mode changes; windows
 * that read the theme while painting pick the new colours up on the next
 * frame without a rebuild. */
void ng_theme_apply(void);

#endif /* NOVUS_NVGUI_H */
