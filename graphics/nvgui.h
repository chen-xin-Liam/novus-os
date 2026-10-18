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

/* Runtime state bits. */
#define NG_ST_HOVER   0x0001u
#define NG_ST_PRESSED 0x0002u
#define NG_ST_FOCUS   0x0004u
#define NG_ST_DRAG    0x0008u

/* Theme metrics, in pixels at text scale 2. */
#define NG_TITLE_H  30
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
  NG_TREE
} ng_type_t;

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
    } panel;
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

/* Show/hide and enable/disable (both damage the widget). */
void ng_show(ng_widget_t *w, int visible);
void ng_set_enabled(ng_widget_t *w, int enabled);

/* Compute absolute rectangles from the relative layout. Call after moving
 * or resizing anything. */
void ng_layout(void);

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

#endif /* NOVUS_NVGUI_H */
