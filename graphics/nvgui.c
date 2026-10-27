/**
 * Novus OS - NovusGUI widget library (M5).
 *
 * The controls are drawn from primitives only, so the whole look lives in
 * this file and depends on nothing but nd.h. Every visual state change
 * reports a damage rectangle; the caller (the demo today, the WM in M6)
 * redraws exactly that rectangle through ng_render().
 *
 * Event routing is the usual GUI contract: the topmost visible widget
 * under the pointer gets the event, clicks raise the window and set focus,
 * Tab/Shift+Tab walk the focusable widgets in tree order, and mouse drags
 * are captured by the widget that started them (slider knob, window title
 * bar). Nothing here assumes a particular screen size.
 */

#include "kernel.h"
#include "nvgui.h"
#include "nd_theme.h"
#include "nd_wallpaper.h"
#include "nd_ime.h"

static ng_ctx_t    g_ctx;
static ng_widget_t g_pool[NG_POOL_SIZE];
static UINT32      g_used;
static int         g_ready;

/* ---- small helpers ---------------------------------------------------- */

static int rect_hits(nd_rect_t a, nd_rect_t b) {
  return !nd_rect_empty(nd_rect_intersect(a, b));
}

static UINT32 default_flags(ng_type_t t) {
  switch (t) {
  case NG_BUTTON:
  case NG_CHECKBOX:
  case NG_TEXTBOX:
  case NG_SLIDER:
  case NG_LIST:
  case NG_TREE:
  case NG_ICON:
  case NG_SWATCH:
    return NG_WF_VISIBLE | NG_WF_ENABLED | NG_WF_FOCUSABLE;
  case NG_LABEL:
  case NG_PROGRESS:
    return NG_WF_VISIBLE;
  default:
    return NG_WF_VISIBLE | NG_WF_ENABLED;
  }
}

static void fmt_int(char *buf, INT32 value) {
  char  tmp[12];
  UINT32 n = 0;
  UINT32 i = 0;
  UINT32 v = (value < 0) ? (UINT32)(-value) : (UINT32)value;

  if (value < 0) {
    buf[i++] = '-';
  }
  if (v == 0u) {
    tmp[n++] = '0';
  }
  while (v > 0u) {
    tmp[n++] = (char)('0' + (v % 10u));
    v /= 10u;
  }
  while (n > 0u) {
    buf[i++] = tmp[--n];
  }
  buf[i] = '\0';
}

/* One palette, two resolutions: the same semantic roles map into the
 * widget theme in light and dark. Nothing here invents a colour - the
 * token file is the only place a literal lives, and the apps read the
 * same table through apps/src/theme.rs. */
static void theme_from_tokens(ng_theme_t *t) {
  const nd_tokens_t *tok = nd_tokens();

  t->desktop_top = tok->canvas_top;
  t->desktop_bottom = tok->canvas_bottom;
  t->win_bg = tok->surface;
  t->win_title = tok->card;
  t->win_title_dim = tok->card;
  t->win_border = tok->stroke;
  t->text = tok->text;
  t->text_dim = tok->dim;
  t->accent = tok->accent;
  t->accent_text = tok->on_accent;
  t->ctrl_bg = tok->card;
  t->ctrl_hover = tok->subtle;
  t->ctrl_border = tok->stroke;
}

static void theme_init(ng_theme_t *t) {
  (void)nd_theme_resolve();
  theme_from_tokens(t);
}

/* ---- damage ----------------------------------------------------------- */

void ng_damage(nd_rect_t r) {
  if (!g_ready || nd_rect_empty(r)) {
    return;
  }
  g_ctx.damage = nd_rect_union(g_ctx.damage, r);
}

/* A window's drop shadow is painted *outside* its rectangle (nd_shadow
 * spreads it by NG_SHADOW_SPREAD), so the pixels a window occupies are
 * not the pixels it has to repaint. Damaging the rectangle alone leaves
 * the shadow ring behind: unused by the next repaint, it survives every
 * move and darkens into a trail after a title-bar drag. */
static nd_rect_t window_visual(nd_rect_t r) {
  return nd_rect(r.x - NG_SHADOW_SPREAD, r.y - NG_SHADOW_SPREAD + 2,
                 r.w + NG_SHADOW_SPREAD * 2, r.h + NG_SHADOW_SPREAD * 2);
}

void ng_damage_widget(ng_widget_t *w) {
  if (w != 0) {
    ng_damage(w->type == NG_WINDOW ? window_visual(w->abs) : w->abs);
  }
}

int ng_take_damage(nd_rect_t *out) {
  nd_rect_t d;

  if (!g_ready) {
    return 0;
  }
  d = g_ctx.damage;
  g_ctx.damage = nd_rect(0, 0, 0, 0);
  if (nd_rect_empty(d)) {
    return 0;
  }
  if (out != 0) {
    *out = d;
  }
  return 1;
}

void ng_theme_apply(void) {
  if (!g_ready) {
    return;
  }
  theme_from_tokens(&g_ctx.theme);
  ng_damage(nd_rect(0, 0, g_ctx.surf->w, g_ctx.surf->h));
}

/* ---- layout ----------------------------------------------------------- */

nd_rect_t ng_window_client(ng_widget_t *w) {
  return nd_rect(w->abs.x + NG_BORDER, w->abs.y + NG_TITLE_H,
                 w->abs.w - NG_BORDER * 2,
                 w->abs.h - NG_TITLE_H - NG_BORDER);
}

static void layout_children(ng_widget_t *w, INT32 ox, INT32 oy,
                            ng_widget_t *win) {
  ng_widget_t *c;

  for (c = w->first; c != 0; c = c->next) {
    c->abs = nd_rect(ox + c->rect.x, oy + c->rect.y, c->rect.w, c->rect.h);
    if (c->type == NG_WINDOW) {
      nd_rect_t client = ng_window_client(c);
      c->window = c;
      layout_children(c, client.x, client.y, c);
    } else {
      c->window = win;
      layout_children(c, c->abs.x, c->abs.y, win);
    }
  }
}

void ng_layout(void) {
  if (!g_ready) {
    return;
  }
  g_ctx.root->abs = nd_rect(0, 0, g_ctx.surf->w, g_ctx.surf->h);
  layout_children(g_ctx.root, 0, 0, 0);
}

/* ---- creation --------------------------------------------------------- */

ng_ctx_t *ng_init(nd_surface_t *surf) {
  memset(&g_ctx, 0, sizeof(g_ctx));
  memset(g_pool, 0, sizeof(g_pool));
  g_used = 0;
  if (surf == 0 || surf->px == 0) {
    g_ready = 0;
    return 0;
  }
  g_ctx.surf = surf;
  g_ctx.damage = nd_rect(0, 0, 0, 0);
  theme_init(&g_ctx.theme);
  g_ready = 1;

  g_ctx.root = ng_create(0, NG_DESKTOP, 0, 0, surf->w, surf->h);
  if (g_ctx.root == 0) {
    g_ready = 0;
    return 0;
  }
  ng_layout();
  return &g_ctx;
}

ng_ctx_t *ng_ctx_get(void) { return g_ready ? &g_ctx : 0; }

ng_widget_t *ng_create(ng_widget_t *parent, ng_type_t type, INT32 x, INT32 y,
                       INT32 w, INT32 h) {
  ng_widget_t *t;
  ng_widget_t *tail;

  if (!g_ready || g_used >= NG_POOL_SIZE) {
    return 0;
  }
  t = &g_pool[g_used++];
  memset(t, 0, sizeof(*t));
  t->type = type;
  t->id = (INT32)g_used;
  t->rect = nd_rect(x, y, w, h);
  t->abs = t->rect;
  t->flags = default_flags(type);
  t->parent = parent;

  if (parent != 0) {
    if (parent->first == 0) {
      parent->first = t;
    } else {
      tail = parent->first;
      while (tail->next != 0) {
        tail = tail->next;
      }
      tail->next = t;
    }
  }
  return t;
}

ng_widget_t *ng_window(ng_widget_t *w, const char *title, int closable) {
  UINT32 i;

  memset(&w->u.window, 0, sizeof(w->u.window));
  w->u.window.closable = (UINT8)(closable != 0);
  for (i = 0; i + 1u < NG_TITLE_MAX && title[i] != '\0'; i++) {
    w->u.window.title[i] = title[i];
  }
  w->on_action = 0;
  return w;
}

ng_widget_t *ng_label(ng_widget_t *w, const char *text, nd_color_t color,
                      UINT32 scale) {
  w->u.label.text = text;
  w->u.label.color = color;
  w->u.label.scale = (scale != 0u) ? scale : NG_TEXT_SCALE;
  w->u.label.align = 0;
  return w;
}

ng_widget_t *ng_button(ng_widget_t *w, const char *text, ng_action_fn fn) {
  w->u.button.text = text;
  w->u.button.icon = 0;
  w->on_action = fn;
  return w;
}

ng_widget_t *ng_button_icon(ng_widget_t *w, const char *text,
                            ng_icon_kind_t kind, ng_action_fn fn) {
  w->u.button.text = text;
  w->u.button.icon = (UINT8)kind;
  w->on_action = fn;
  return w;
}

ng_widget_t *ng_swatch(ng_widget_t *w, nd_color_t color, int selected,
                       ng_action_fn fn) {
  w->u.swatch.color = color;
  w->u.swatch.selected = (UINT8)(selected != 0);
  w->on_action = fn;
  return w;
}

ng_widget_t *ng_checkbox(ng_widget_t *w, const char *text, int checked,
                         ng_action_fn fn) {
  w->u.check.text = text;
  w->u.check.checked = (UINT8)(checked != 0);
  w->on_action = fn;
  return w;
}

ng_widget_t *ng_textbox(ng_widget_t *w, char *buf, UINT32 cap,
                        const char *placeholder) {
  UINT32 len = 0;

  while (buf != 0 && len + 1u < cap && buf[len] != '\0') {
    len++;
  }
  w->u.edit.buf = buf;
  w->u.edit.cap = cap;
  w->u.edit.len = len;
  w->u.edit.cursor = len;
  w->u.edit.scroll = 0;
  w->u.edit.placeholder = placeholder;
  w->u.edit.password = 0;
  return w;
}

ng_widget_t *ng_slider(ng_widget_t *w, INT32 min, INT32 max, INT32 value,
                       ng_value_fn fn) {
  if (max <= min) {
    max = min + 1;
  }
  if (value < min) {
    value = min;
  }
  if (value > max) {
    value = max;
  }
  w->u.slider.min = min;
  w->u.slider.max = max;
  w->u.slider.value = value;
  w->on_value = fn;
  return w;
}

ng_widget_t *ng_progress(ng_widget_t *w, INT32 permille, const char *text) {
  w->u.progress.permille = permille;
  w->u.progress.text = text;
  return w;
}

ng_widget_t *ng_list(ng_widget_t *w, const char **items, UINT32 count,
                     ng_value_fn fn) {
  w->u.list.items = items;
  w->u.list.count = count;
  w->u.list.sel = -1;
  w->u.list.top = 0;
  w->on_value = fn;
  return w;
}

ng_widget_t *ng_tree(ng_widget_t *w, ng_tnode_t *nodes, UINT32 count,
                     ng_value_fn fn) {
  w->u.tree.nodes = nodes;
  w->u.tree.count = count;
  w->u.tree.sel = -1;
  w->u.tree.top = 0;
  w->on_value = fn;
  return w;
}

ng_widget_t *ng_panel(ng_widget_t *w, nd_color_t color) {
  w->u.panel.color = color;
  w->u.panel.frame = 0;
  w->u.panel.radius = 0;
  return w;
}

ng_widget_t *ng_icon(ng_widget_t *w, ng_icon_kind_t kind, UINT32 size,
                     ng_action_fn fn) {
  w->u.icon.kind = (UINT8)kind;
  w->u.icon.size = (UINT8)((size != 0u) ? size : 20u);
  w->on_action = fn;
  return w;
}

void ng_panel_frame(ng_widget_t *w, int on) {
  if (w == 0 || w->type != NG_PANEL) {
    return;
  }
  w->u.panel.frame = (on != 0) ? 1u : 0u;
  w->u.panel.radius = 12u;
  ng_damage_widget(w);
}

/* ---- visibility, focus, z-order --------------------------------------- */

void ng_show(ng_widget_t *w, int visible) {
  if (w == 0) {
    return;
  }
  if (visible) {
    w->flags |= NG_WF_VISIBLE;
  } else {
    w->flags &= ~NG_WF_VISIBLE;
    if (g_ctx.focus == w) {
      g_ctx.focus = 0;
    }
    if (g_ctx.hover == w) {
      g_ctx.hover = 0;
    }
  }
  ng_damage_widget(w);
}

void ng_set_enabled(ng_widget_t *w, int enabled) {
  if (w == 0) {
    return;
  }
  if (enabled) {
    w->flags |= NG_WF_ENABLED;
  } else {
    w->flags &= ~NG_WF_ENABLED;
  }
  ng_damage_widget(w);
}

void ng_focus_set(ng_widget_t *w) {
  if (g_ctx.focus == w) {
    return;
  }
  if (g_ctx.focus != 0) {
    g_ctx.focus->state &= ~NG_ST_FOCUS;
    ng_damage_widget(g_ctx.focus);
  }
  g_ctx.focus = w;
  if (w != 0) {
    w->state |= NG_ST_FOCUS;
    ng_damage_widget(w);
  }
}

ng_widget_t *ng_focus_get(void) { return g_ctx.focus; }

static ng_widget_t *top_window(ng_widget_t *w) {
  while (w != 0 && w->type != NG_WINDOW) {
    w = w->parent;
  }
  return w;
}

void ng_raise(ng_widget_t *w) {
  ng_widget_t *win = top_window(w);
  ng_widget_t *p;
  ng_widget_t *prev = 0;

  if (win == 0 || win->parent == 0) {
    return;
  }
  /* Already the topmost child (last in the list)? Nothing to do. */
  for (p = win->parent->first; p != 0; p = p->next) {
    if (p->next == 0 && p == win) {
      return;
    }
  }
  for (p = win->parent->first; p != 0 && p != win; p = p->next) {
    prev = p;
  }
  if (p != win) {
    return;
  }
  if (prev != 0) {
    prev->next = win->next;
  } else {
    win->parent->first = win->next;
  }
  win->next = 0;
  for (p = win->parent->first; p->next != 0; p = p->next) {
  }
  p->next = win;
  ng_damage_widget(win);
}

/* ---- hit testing ------------------------------------------------------ */

static int interactive(const ng_widget_t *w) {
  return (w->flags & NG_WF_ENABLED) != 0u &&
         (w->type == NG_BUTTON || w->type == NG_CHECKBOX ||
          w->type == NG_TEXTBOX || w->type == NG_SLIDER ||
          w->type == NG_LIST || w->type == NG_TREE);
}

static ng_widget_t *hit_deep(ng_widget_t *w, INT32 x, INT32 y) {
  ng_widget_t *c;
  ng_widget_t *best = 0;

  for (c = w->first; c != 0; c = c->next) {
    if ((c->flags & NG_WF_VISIBLE) != 0u &&
        nd_rect_contains(c->abs, x, y)) {
      best = c; /* later siblings draw on top */
    }
  }
  if (best == 0) {
    return 0;
  }
  {
    ng_widget_t *deeper = hit_deep(best, x, y);
    return (deeper != 0) ? deeper : best;
  }
}

/* Walk up from the deepest hit to the nearest thing that reacts to input.
 * A label inside a window yields the window, a label inside nothing yields
 * NULL. */
static ng_widget_t *hit_test(INT32 x, INT32 y) {
  ng_widget_t *w = hit_deep(g_ctx.root, x, y);

  while (w != 0 && w->type != NG_WINDOW) {
    if (interactive(w)) {
      return w;
    }
    w = w->parent;
  }
  return w; /* the window itself, or NULL */
}

/* ---- drawing ---------------------------------------------------------- */

static void icon_glyph(nd_surface_t *s, INT32 cx, INT32 cy, INT32 b,
                       UINT8 kind, nd_color_t c);

static void draw_label(ng_widget_t *w) {
  const ng_ctx_t *c = &g_ctx;
  nd_surface_t   *s = c->surf;
  UINT32          scale = (w->u.label.scale != 0u) ? w->u.label.scale : 1u;
  UINT32          tw = nd_text_width(w->u.label.text, scale);
  INT32           x = w->abs.x;
  INT32           y = w->abs.y +
                (w->abs.h - (INT32)nd_text_height(w->u.label.text, scale)) / 2;

  if (w->u.label.align == 1u) {
    x += (w->abs.w - (INT32)tw) / 2;
  } else if (w->u.label.align == 2u) {
    x += w->abs.w - (INT32)tw;
  }
  nd_text(s, x, y, w->u.label.text, w->u.label.color, scale);
}

static void draw_button(ng_widget_t *w) {
  nd_surface_t  *s = g_ctx.surf;
  const ng_theme_t *t = &g_ctx.theme;
  nd_color_t     bg;
  nd_color_t     fg;
  UINT32         tw;
  INT32          ty;

  /* Tile style: icon above a caption, chrome only under the pointer. */
  if (w->u.button.icon != 0u) {
    UINT8 hot = (UINT8)((w->state & (NG_ST_HOVER | NG_ST_PRESSED)) != 0u);
    /* One caption size for every tile: a CJK glyph is 32 px at 2x, so a
     * five-character name would need 160 px and still be clipped at 1280.
     * Small captions under a 24 px icon also keep the column uniform. */
    UINT32 scale = 1u;
    INT32 icon_y = (w->abs.h >= 64) ? w->abs.y + 22 : w->abs.y + w->abs.h / 2;

    if (hot) {
      nd_round_rect(s, nd_rect(w->abs.x + 2, w->abs.y + 2, w->abs.w - 4,
                               w->abs.h - 4), 8,
                    (w->state & NG_ST_PRESSED) != 0u ? t->ctrl_border
                                                     : t->ctrl_hover);
    }
    icon_glyph(s, w->abs.x + w->abs.w / 2, icon_y, 24, w->u.button.icon,
               hot ? t->accent : t->text);
    if (w->u.button.text != 0 && w->u.button.text[0] != '\0') {
      tw = nd_text_width(w->u.button.text, scale);
      ty = (w->abs.h >= 64) ? w->abs.y + w->abs.h - 24
                            : w->abs.y + w->abs.h - 16;
      nd_text(s, w->abs.x + (w->abs.w - (INT32)tw) / 2, ty,
              w->u.button.text, t->text, scale);
    }
    return;
  }

  if ((w->state & NG_ST_PRESSED) != 0u) {
    bg = t->accent;
    fg = t->accent_text;
  } else if ((w->flags & NG_WF_ENABLED) == 0u) {
    bg = t->ctrl_bg;
    fg = t->text_dim;
  } else if ((w->state & NG_ST_HOVER) != 0u) {
    bg = t->ctrl_hover;
    fg = t->text;
  } else {
    bg = t->ctrl_bg;
    fg = t->text;
  }
  nd_round_rect(s, w->abs, 6, bg);
  nd_round_frame(s, w->abs, 6, 1,
                 (w->state & (NG_ST_HOVER | NG_ST_PRESSED)) != 0u
                     ? (w->state & NG_ST_PRESSED) != 0u ? t->accent
                                                        : t->ctrl_border
                     : t->ctrl_border);
  if ((w->state & NG_ST_FOCUS) != 0u && (w->state & NG_ST_PRESSED) == 0u) {
    nd_round_frame(s, nd_rect(w->abs.x + 2, w->abs.y + 2, w->abs.w - 4,
                              w->abs.h - 4),
                   4, 1, t->accent);
  }
  tw = nd_text_width(w->u.button.text, NG_TEXT_SCALE);
  ty = w->abs.y +
       (w->abs.h - (INT32)nd_text_height(w->u.button.text, NG_TEXT_SCALE)) / 2;
  nd_text(s, w->abs.x + ((w->abs.w - (INT32)tw) / 2), ty, w->u.button.text, fg,
          NG_TEXT_SCALE);
}

static void draw_checkbox(ng_widget_t *w) {
  nd_surface_t  *s = g_ctx.surf;
  const ng_theme_t *t = &g_ctx.theme;
  nd_rect_t      box = nd_rect(w->abs.x, w->abs.y + (w->abs.h - 18) / 2, 18, 18);
  nd_color_t     fg =
      ((w->flags & NG_WF_ENABLED) != 0u) ? t->text : t->text_dim;

  nd_round_rect(s, box, 4, (w->state & NG_ST_HOVER) != 0u ? t->ctrl_hover
                                                          : t->ctrl_bg);
  nd_round_frame(s, box, 4, 1,
                 (w->state & NG_ST_FOCUS) != 0u ? t->accent : t->ctrl_border);
  if (w->u.check.checked != 0u) {
    nd_line(s, box.x + 4, box.y + 9, box.x + 8, box.y + 13, t->accent);
    nd_line(s, box.x + 8, box.y + 13, box.x + 14, box.y + 5, t->accent);
    nd_line(s, box.x + 5, box.y + 9, box.x + 9, box.y + 13, t->accent);
    nd_line(s, box.x + 9, box.y + 13, box.x + 15, box.y + 5, t->accent);
  }
  nd_text(s, box.x + 28,
          w->abs.y +
              (w->abs.h - (INT32)nd_text_height(w->u.check.text, 2)) / 2,
          w->u.check.text, fg, NG_TEXT_SCALE);
}

/* Byte offset one codepoint after `off`. */
/* Edit positions are byte offsets into the UTF-8 buffer, not character
 * counts. Moving via the decoder preserves boundaries so backspace, caret
 * scrolling, and CJK hit placement never split a multibyte glyph. */
static UINT32 edit_skip(ng_widget_t *w, UINT32 off, int steps) {
  while (steps-- > 0 && off < w->u.edit.len) {
    const char *p = w->u.edit.buf + off;

    (void)nd_utf8_next(&p);
    off = (UINT32)(p - w->u.edit.buf);
  }
  return off;
}

/* Previous codepoint boundary before `off`. */
static UINT32 edit_prev(ng_widget_t *w, UINT32 off) {
  if (off == 0u) {
    return 0u;
  }
  off--;
  while (off > 0u && ((UINT8)w->u.edit.buf[off] & 0xC0u) == 0x80u) {
    off--;
  }
  return off;
}

/* Insert one codepoint at the caret. Returns 0 when the buffer is full or
 * `cp` is not a Unicode scalar value. Four-byte sequences are supported, so
 * supplementary-plane characters and emoji insert like any other glyph. */
int ng_edit_insert_cp(ng_widget_t *w, UINT32 cp) {
  char   enc[4];
  UINT32 i;
  UINT32 len;
  UINT32 off;

  if (w == 0 || w->type != NG_TEXTBOX) {
    return 0;
  }
  len = nd_utf8_encode(cp, enc);
  if (len == 0u) {
    return 0;
  }
  if (w->u.edit.len + len + 1u > w->u.edit.cap) {
    return 0;
  }
  off = w->u.edit.cursor;
  for (i = w->u.edit.len + len; i > off; i--) {
    w->u.edit.buf[i] = w->u.edit.buf[i - len];
  }
  for (i = 0; i < len; i++) {
    w->u.edit.buf[off + i] = (char)enc[i];
  }
  w->u.edit.len += len;
  w->u.edit.cursor = off + len;
  w->u.edit.buf[w->u.edit.len] = '\0';
  ng_damage_widget(w);
  return 1;
}

/* Insert UTF-8 text (used by the IME for a whole candidate). */
int ng_edit_insert_text(ng_widget_t *w, const char *text) {
  int ok = 1;

  while (text != 0 && *text != '\0') {
    UINT32 cp = nd_utf8_next(&text);

    if (!ng_edit_insert_cp(w, cp)) {
      ok = 0;
      break;
    }
  }
  (void)nd_utf8_next;
  return ok;
}

static nd_rect_t g_caret; /* last drawn caret, screen coordinates */
static int       g_caret_ok;

int ng_focus_caret(nd_rect_t *out) {
  if (!g_caret_ok) {
    return 0;
  }
  if (out != 0) {
    *out = g_caret;
  }
  return 1;
}

static void draw_textbox(ng_widget_t *w) {
  nd_surface_t  *s = g_ctx.surf;
  const ng_theme_t *t = &g_ctx.theme;
  INT32          tx = w->abs.x + NG_PAD;
  INT32          ty;
  UINT32         max_px;
  UINT32         cell;
  const char    *shown;

  nd_round_rect(s, w->abs, 5, t->ctrl_bg);
  nd_round_frame(s, w->abs, 5, 1,
                 (w->state & NG_ST_FOCUS) != 0u ? t->accent : t->ctrl_border);

  if (w->abs.w <= NG_PAD * 2) {
    return;
  }
  cell = nd_text_height(w->u.edit.buf, NG_TEXT_SCALE);
  ty = w->abs.y + (w->abs.h - (INT32)cell) / 2;
  max_px = (UINT32)(w->abs.w - NG_PAD * 2);
  if (w->u.edit.len == 0u && w->u.edit.placeholder != 0) {
    nd_text_limit(s, tx, ty, w->u.edit.placeholder, max_px, t->text_dim,
                  NG_TEXT_SCALE);
    return;
  }

  /* Scroll by codepoint so the caret stays inside the box: pull the window
   * forward while the text before the caret is wider than the box. */
  if (w->u.edit.cursor < w->u.edit.scroll) {
    w->u.edit.scroll = w->u.edit.cursor;
  }
  while (w->u.edit.scroll < w->u.edit.cursor &&
         nd_text_width_n(w->u.edit.buf + w->u.edit.scroll,
                         w->u.edit.cursor - w->u.edit.scroll,
                         NG_TEXT_SCALE) > max_px) {
    w->u.edit.scroll = edit_skip(w, w->u.edit.scroll, 1);
  }
  shown = w->u.edit.buf + w->u.edit.scroll;
  if (w->u.edit.password != 0u) {
    static char dots[128];
    UINT32      i = 0;
    UINT32      off = w->u.edit.scroll;

    while (off < w->u.edit.len && i + 1u < sizeof(dots)) {
      dots[i++] = '*';
      off = edit_skip(w, off, 1);
    }
    dots[i] = '\0';
    shown = dots;
  }
  nd_text_limit(s, tx, ty, shown, max_px, t->text, NG_TEXT_SCALE);

  if ((w->state & NG_ST_FOCUS) != 0u) {
    INT32 cx = tx + (INT32)nd_text_width_n(w->u.edit.buf + w->u.edit.scroll,
                                           w->u.edit.cursor - w->u.edit.scroll,
                                           NG_TEXT_SCALE) +
               3;
    nd_fill_rect(s, nd_rect(cx, ty - 1, 2, (INT32)cell + 2), t->accent);
    g_caret = nd_rect(cx, ty, 2, (INT32)cell);
    g_caret_ok = 1;
  } else if (g_ctx.focus == w) {
    g_caret_ok = 0;
  }
}


static void draw_slider(ng_widget_t *w) {
  nd_surface_t  *s = g_ctx.surf;
  const ng_theme_t *t = &g_ctx.theme;
  INT32          cy = w->abs.y + w->abs.h / 2;
  INT32          x0 = w->abs.x + 8;
  INT32          x1 = w->abs.x + w->abs.w - 8;
  INT32          span = (w->u.slider.max > w->u.slider.min)
                           ? (w->u.slider.max - w->u.slider.min)
                           : 1;
  INT32          knob = x0 + (INT32)(((INT64)(w->u.slider.value - w->u.slider.min) *
                                    (x1 - x0)) / span);
  INT32          r = (w->abs.h < 26) ? w->abs.h / 2 - 2 : 10;

  if (x1 <= x0) {
    return;
  }
  nd_fill_rect(s, nd_rect(x0, cy - 2, x1 - x0, 4), t->ctrl_border);
  nd_fill_rect(s, nd_rect(x0, cy - 2, knob - x0, 4), t->accent);
  nd_fill_circle(s, knob, cy, r, t->ctrl_hover);
  nd_circle(s, knob, cy, r, (w->state & NG_ST_FOCUS) != 0u ? t->accent
                                                           : t->win_border);
  nd_fill_circle(s, knob, cy, 3, t->accent);
}

static void draw_progress(ng_widget_t *w) {
  nd_surface_t     *s = g_ctx.surf;
  const ng_theme_t *t = &g_ctx.theme;
  INT32             pw = w->u.progress.permille;
  char              text[16];
  UINT32            tw;

  if (pw < 0) {
    pw = 0;
  }
  if (pw > 1000) {
    pw = 1000;
  }
  nd_round_rect(s, w->abs, 5, t->ctrl_bg);
  if (pw > 0) {
    nd_round_rect(s, nd_rect(w->abs.x, w->abs.y,
                             w->abs.w * pw / 1000, w->abs.h),
                  5, t->accent);
  }
  nd_round_frame(s, w->abs, 5, 1, t->ctrl_border);
  if (w->u.progress.text != 0) {
    text[0] = '\0';
    nd_text(s, w->abs.x + NG_PAD,
            w->abs.y + (w->abs.h - (INT32)nd_font_height(NG_TEXT_SCALE)) / 2,
            w->u.progress.text, t->text, NG_TEXT_SCALE);
  } else {
    fmt_int(text, pw / 10);
    {
      UINT32 n = 0;
      while (text[n] != '\0') {
        n++;
      }
      text[n] = '%';
      text[n + 1] = '\0';
    }
    tw = nd_text_width(text, NG_TEXT_SCALE);
    nd_text(s, w->abs.x + (w->abs.w - (INT32)tw) / 2,
            w->abs.y + (w->abs.h - (INT32)nd_font_height(NG_TEXT_SCALE)) / 2,
            text, t->accent_text, NG_TEXT_SCALE);
  }
}

static INT32 list_visible_rows(ng_widget_t *w) {
  INT32 rows = (w->abs.h - 2) / NG_ROW_H;
  return (rows > 0) ? rows : 1;
}

static void draw_list(ng_widget_t *w) {
  nd_surface_t     *s = g_ctx.surf;
  const ng_theme_t *t = &g_ctx.theme;
  INT32             rows = list_visible_rows(w);
  INT32             i;

  nd_round_rect(s, w->abs, 5, t->ctrl_bg);
  nd_round_frame(s, w->abs, 5, 1,
                 (w->state & NG_ST_FOCUS) != 0u ? t->accent : t->ctrl_border);
  for (i = 0; i < rows; i++) {
    INT32 idx = w->u.list.top + i;
    nd_rect_t row;
    if (idx < 0 || (UINT32)idx >= w->u.list.count) {
      break;
    }
    row = nd_rect(w->abs.x + 2, w->abs.y + 1 + i * NG_ROW_H, w->abs.w - 4,
                  NG_ROW_H);
    if (idx == w->u.list.sel) {
      nd_round_rect(s, row, 4, t->accent);
    }
    nd_text(s, row.x + 8,
            row.y + (NG_ROW_H -
                     (INT32)nd_text_height(w->u.list.items[idx],
                                           NG_TEXT_SCALE)) / 2,
            w->u.list.items[idx],
            (idx == w->u.list.sel) ? t->accent_text : t->text, NG_TEXT_SCALE);
  }
  if (w->u.list.count > (UINT32)rows) {
    INT32 track_h = w->abs.h - 8;
    INT32 thumb_h = track_h * rows / (INT32)w->u.list.count;
    INT32 max_top = (INT32)w->u.list.count - rows;
    INT32 ty = w->abs.y + 4;
    if (thumb_h < 12) {
      thumb_h = 12;
    }
    if (max_top > 0) {
      ty += (track_h - thumb_h) * w->u.list.top / max_top;
    }
    nd_round_rect(s, nd_rect(w->abs.x + w->abs.w - 7, ty, 4, thumb_h), 2,
                  t->ctrl_border);
  }
}

/* Visible rows of the tree: a collapsed node hides every following node
 * with a greater depth. */
static INT32 tree_next_visible(ng_widget_t *w, INT32 from) {
  INT32 i;
  INT32 hide_below = 0x7FFF;

  for (i = 0; i <= from && (UINT32)i < w->u.tree.count; i++) {
    ng_tnode_t *n = &w->u.tree.nodes[i];
    if (n->depth > hide_below) {
      continue;
    }
    hide_below = (n->has_children != 0u && n->expanded == 0u)
                     ? (INT32)n->depth
                     : 0x7FFF;
  }
  for (i = from + 1; (UINT32)i < w->u.tree.count; i++) {
    ng_tnode_t *n = &w->u.tree.nodes[i];
    if (n->depth > hide_below) {
      continue;
    }
    hide_below = (n->has_children != 0u && n->expanded == 0u)
                     ? (INT32)n->depth
                     : 0x7FFF;
    return i;
  }
  return -1;
}

/* The node index of a visible row, or -1. Collapsed subtrees mean row
 * numbers and array indexes diverge, so every hit test goes through
 * here. */
static INT32 tree_row_node(ng_widget_t *w, INT32 row) {
  INT32 idx = -1;

  while (row-- >= 0) {
    idx = tree_next_visible(w, idx);
    if (idx < 0) {
      return -1;
    }
  }
  return idx;
}

static void draw_tree(ng_widget_t *w) {
  nd_surface_t     *s = g_ctx.surf;
  const ng_theme_t *t = &g_ctx.theme;
  INT32             rows = list_visible_rows(w);
  INT32             idx = -1;
  INT32             skip = w->u.tree.top;
  INT32             i;

  nd_round_rect(s, w->abs, 5, t->ctrl_bg);
  nd_round_frame(s, w->abs, 5, 1,
                 (w->state & NG_ST_FOCUS) != 0u ? t->accent : t->ctrl_border);
  while (skip-- > 0) {
    idx = tree_next_visible(w, idx);
    if (idx < 0) {
      return;
    }
  }
  for (i = 0; i < rows; i++) {
    ng_tnode_t *n;

    idx = tree_next_visible(w, idx);
    if (idx < 0) {
      break;
    }
    n = &w->u.tree.nodes[idx];
    nd_rect_t   row = nd_rect(w->abs.x + 2, w->abs.y + 1 + i * NG_ROW_H,
                              w->abs.w - 4, NG_ROW_H);
    INT32       x = row.x + 6 + (INT32)n->depth * 16;
    INT32       ty = row.y + (NG_ROW_H -
                              (INT32)nd_text_height(n->text, NG_TEXT_SCALE)) / 2;

    if (idx == w->u.tree.sel) {
      nd_round_rect(s, row, 4, t->accent);
    }
    if (n->has_children != 0u) {
      nd_rect_t box = nd_rect(x, row.y + 6, 12, 12);
      nd_round_rect(s, box, 2, t->ctrl_hover);
      nd_fill_rect(s, nd_rect(box.x + 3, box.y + 5, 6, 2), t->text);
      if (n->expanded == 0u) {
        nd_fill_rect(s, nd_rect(box.x + 5, box.y + 3, 2, 6), t->text);
      }
    }
    nd_text(s, x + 18, ty, n->text,
            (idx == w->u.tree.sel) ? t->accent_text : t->text, NG_TEXT_SCALE);
  }
}

/* Windows 11 caption buttons: 46x32 hit areas, right-aligned, close turns
 * red under the pointer. Shared shape with the WM's own chrome so a shell
 * window and a client window look like the same system. */
void ng_draw_caption(nd_surface_t *s, nd_rect_t frame, const ng_theme_t *t,
                     int hovered, int count) {
  INT32        bw = 46;
  INT32        bh = 32;
  INT32        i;

  for (i = 0; i < count; i++) {
    nd_rect_t b = nd_rect(frame.x + frame.w - (INT32)(i + 1) * bw,
                          frame.y, bw, bh);
    int       is_close = (i == 0);
    int       hot = (hovered == (INT32)i + 1);

    if (hot) {
      nd_fill_rect(s, b, is_close ? ND_RGB(0xC4, 0x2B, 0x1C) : t->ctrl_hover);
    }
    if (is_close) {
      INT32 c = b.x + bw / 2;
      INT32 m = b.y + bh / 2;

      nd_line(s, c - 5, m - 5, c + 5, m + 5,
              hot ? ND_RGB(0xFF, 0xFF, 0xFF) : t->text);
      nd_line(s, c + 5, m - 5, c - 5, m + 5,
              hot ? ND_RGB(0xFF, 0xFF, 0xFF) : t->text);
    } else if (i == 1) {
      nd_frame(s, nd_rect(b.x + bw / 2 - 5, b.y + bh / 2 - 5, 10, 10), 1,
               t->text);
    } else {
      nd_hline(s, b.x + bw / 2 - 5, b.x + bw / 2 + 6, b.y + bh / 2, t->text);
    }
  }
}

static void draw_window(ng_widget_t *w) {
  nd_surface_t     *s = g_ctx.surf;
  const ng_theme_t *t = &g_ctx.theme;
  nd_color_t        title_bg;

  nd_shadow(s, w->abs, 14, 42);
  nd_round_rect(s, w->abs, 10, t->win_bg);

  title_bg = (w->state & NG_ST_DRAG) != 0u ? t->win_title : t->win_title;
  {
    INT32 i;
    /* Rounded top: a radius-10 corner curve, row by row. */
    static const INT32 corner[10] = {10, 7, 5, 4, 3, 2, 2, 1, 1, 1};

    for (i = 0; i < NG_TITLE_H && i < w->abs.h; i++) {
      INT32 inset = (i < 10) ? corner[i] : 0;

      nd_fill_rect(s, nd_rect(w->abs.x + inset, w->abs.y + i,
                              w->abs.w - inset * 2, 1),
                   title_bg);
    }
  }
  nd_hline(s, w->abs.x + 1, w->abs.x + w->abs.w - 2,
           w->abs.y + NG_TITLE_H - 1, t->win_border);
  nd_round_frame(s, w->abs, 10, 1, t->win_border);

  nd_text(s, w->abs.x + 12,
          w->abs.y + (NG_TITLE_H - (INT32)nd_text_height(w->u.window.title, 2)) / 2,
          w->u.window.title, t->text, NG_TEXT_SCALE);

  if (w->u.window.closable != 0u) {
    ng_draw_caption(s, w->abs, t, -1, 3);
  }
}


/* ---- icons ------------------------------------------------------------ */
/* Monochrome glyphs built from the same primitives a client can send.
 * `box` is the square the glyph is drawn in; the stroke weight is 2 px at
 * the 20 px default so it stays legible on the 16 px CJK grid. */
static void icon_glyph(nd_surface_t *s, INT32 cx, INT32 cy, INT32 b,
                       UINT8 kind, nd_color_t c) {
  INT32 h = b / 2;

  switch (kind) {
  case NG_ICON_START: {
    INT32 q = (b * 9) / 20;
    INT32 g = (b * 3) / 20;

    nd_round_rect(s, nd_rect(cx - q - g / 2, cy - q - g / 2, q, q), 1, c);
    nd_round_rect(s, nd_rect(cx + g / 2, cy - q - g / 2, q, q), 1, c);
    nd_round_rect(s, nd_rect(cx - q - g / 2, cy + g / 2, q, q), 1, c);
    nd_round_rect(s, nd_rect(cx + g / 2, cy + g / 2, q, q), 1, c);
    break;
  }
  case NG_ICON_FILES:
    nd_round_rect(s, nd_rect(cx - h, cy - h + 3, h, 5), 2, c);
    nd_round_rect(s, nd_rect(cx - h, cy - h + 6, b, b - 8), 2, c);
    break;
  case NG_ICON_TASKS: {
    INT32 i;

    for (i = 0; i < 3; i++) {
      INT32 y = cy - h + 4 + i * (b / 3);

      nd_fill_circle(s, cx - h + 2, y + 1, 2, c);
      nd_fill_rect(s, nd_rect(cx - h + 7, y, b - 9, 2), c);
    }
    break;
  }
  case NG_ICON_SETTINGS: {
    INT32 i;

    nd_circle(s, cx, cy, h - 3, c);
    nd_fill_circle(s, cx, cy, 3, c);
    for (i = 0; i < 4; i++) {
      static const INT32 dx[4] = {0, 1, 0, -1};
      static const INT32 dy[4] = {-1, 0, 1, 0};

      nd_line(s, cx + dx[i] * (h - 2), cy + dy[i] * (h - 2),
              cx + dx[i] * (h - 5), cy + dy[i] * (h - 5), c);
    }
    break;
  }
  case NG_ICON_NET: {
    INT32 i;

    for (i = 0; i < 3; i++) {
      INT32 w = h - i * (b / 7);

      nd_hline(s, cx - w, cx + w, cy - h + 4 + i * 4, c);
    }
    nd_fill_circle(s, cx, cy + h - 2, 2, c);
    break;
  }
  case NG_ICON_SEARCH:
    nd_circle(s, cx - 1, cy - 1, h - 3, c);
    nd_line(s, cx + 3, cy + 3, cx + h - 1, cy + h - 1, c);
    break;
  case NG_ICON_POWER:
    nd_circle(s, cx, cy + 1, h - 3, c);
    nd_fill_rect(s, nd_rect(cx - 1, cy - h, 3, h + 2), c);
    break;
  case NG_ICON_SUN: {
    INT32 i;

    nd_fill_circle(s, cx, cy, 4, c);
    for (i = 0; i < 8; i++) {
      static const INT32 dx[8] = {0, 1, 1, 1, 0, -1, -1, -1};
      static const INT32 dy[8] = {-1, -1, 0, 1, 1, 1, 0, -1};

      nd_line(s, cx + dx[i] * 6, cy + dy[i] * 6, cx + dx[i] * (h - 1),
              cy + dy[i] * (h - 1), c);
    }
    break;
  }
  case NG_ICON_MOON:
    /* A crescent: a filled disc with a second disc punched out in the
     * surface colour. Both colours come from the theme, so it works on a
     * card in light and dark alike. */
    nd_fill_circle(s, cx - 1, cy, h - 2, c);
    nd_fill_circle(s, cx + 5, cy - 3, h - 3, g_ctx.theme.ctrl_bg);
    break;
  case NG_ICON_AUTO: {
    INT32 i;

    nd_circle(s, cx, cy, h - 2, c);
    for (i = -h + 3; i <= 0; i++) {
      INT32 w = 0;
      INT32 r = h - 2;

      while (w * w + i * i <= r * r && cx - w > cx - r) {
        w++;
      }
      nd_hline(s, cx - w + 1, cx - 1, cy + i, c);
    }
    break;
  }
  case NG_ICON_INSTALL:
    nd_fill_rect(s, nd_rect(cx - 1, cy - h, 3, h + 2), c);
    nd_line(s, cx - 5, cy + 2, cx, cy + h - 3, c);
    nd_line(s, cx + 5, cy + 2, cx, cy + h - 3, c);
    nd_fill_rect(s, nd_rect(cx - h, cy + h - 1, b, 2), c);
    break;
  case NG_ICON_TERM:
    /* A terminal window: rounded outline with a `>` prompt and a baseline. */
    nd_round_rect(s, nd_rect(cx - h, cy - h + 1, b, b - 2), 3, c);
    nd_line(s, cx - h + 3, cy - 1, cx - 1, cy + h - 5, c);
    nd_line(s, cx - 1, cy + h - 5, cx + h - 5, cy - 1, c);
    nd_hline(s, cx - h + 3, cx + h - 5, cy + h - 2, c);
    break;
  case NG_ICON_CHECK:
    nd_line(s, cx - h + 2, cy, cx - 2, cy + h - 3, c);
    nd_line(s, cx - 2, cy + h - 3, cx + h - 1, cy - h + 2, c);
    break;
  case NG_ICON_CHEVRON:
    nd_line(s, cx - 2, cy - 4, cx + 2, cy, c);
    nd_line(s, cx + 2, cy, cx - 2, cy + 4, c);
    break;
  case NG_ICON_DISK:
    nd_round_rect(s, nd_rect(cx - h, cy - h + 1, b, b - 3), 3, c);
    nd_hline(s, cx - h + 3, cx + h - 3, cy + h - 4, g_ctx.theme.ctrl_bg);
    break;
  default:
    break;
  }
}

static void draw_icon(ng_widget_t *w) {
  nd_surface_t     *s = g_ctx.surf;
  const ng_theme_t *t = &g_ctx.theme;
  UINT8             b = (w->u.icon.size != 0u) ? w->u.icon.size : 20u;
  UINT8             hot = (UINT8)((w->state & (NG_ST_HOVER | NG_ST_PRESSED)) != 0u);
  nd_color_t        fg = hot ? t->accent : t->text;
  INT32             cx = w->abs.x + w->abs.w / 2;
  INT32             cy = w->abs.y + w->abs.h / 2;

  if ((w->state & NG_ST_HOVER) != 0u) {
    nd_round_rect(s, nd_rect(w->abs.x + 2, w->abs.y + 2, w->abs.w - 4,
                             w->abs.h - 4), 6, t->ctrl_hover);
  }
  if ((w->state & NG_ST_PRESSED) != 0u) {
    nd_round_rect(s, nd_rect(w->abs.x + 3, w->abs.y + 3, w->abs.w - 6,
                             w->abs.h - 6), 5, t->ctrl_border);
  }
  if ((w->flags & NG_WF_ENABLED) == 0u) {
    fg = t->text_dim;
  }
  icon_glyph(s, cx, cy, (INT32)b, w->u.icon.kind, fg);
}

static void draw_swatch(ng_widget_t *w) {
  nd_surface_t     *s = g_ctx.surf;
  const ng_theme_t *t = &g_ctx.theme;
  INT32             r = (w->abs.w < w->abs.h ? w->abs.w : w->abs.h) / 2 - 5;
  INT32             cx = w->abs.x + w->abs.w / 2;
  INT32             cy = w->abs.y + w->abs.h / 2;

  if (r < 3) {
    r = 3;
  }
  if ((w->state & (NG_ST_HOVER | NG_ST_PRESSED)) != 0u) {
    nd_circle(s, cx, cy, r + 3, t->text_dim);
  }
  nd_fill_circle(s, cx, cy, r, w->u.swatch.color);
  if (w->u.swatch.selected != 0u) {
    nd_circle(s, cx, cy, r + 3, t->accent);
    nd_circle(s, cx, cy, r + 4, t->accent);
  }
}

static void draw_self(ng_widget_t *w) {
  switch (w->type) {
  case NG_DESKTOP:
    /* Photo wallpaper when one is decoded, a flat design-token colour
     * otherwise (no package on the medium, or a decode failure). */
    nd_wallpaper_paint(g_ctx.surf, &w->abs, g_ctx.theme.desktop_top,
                      g_ctx.theme.desktop_bottom);
    break;
  case NG_WINDOW:
    draw_window(w);
    break;
  case NG_PANEL:
    if (w->u.panel.radius != 0u) {
      nd_round_rect(g_ctx.surf, w->abs, (INT32)w->u.panel.radius,
                    w->u.panel.color);
      if (w->u.panel.frame != 0u) {
        nd_round_frame(g_ctx.surf, w->abs, (INT32)w->u.panel.radius, 1,
                       g_ctx.theme.win_border);
      }
    } else {
      nd_fill_rect(g_ctx.surf, w->abs, w->u.panel.color);
    }
    break;
  case NG_LABEL:
    draw_label(w);
    break;
  case NG_BUTTON:
    draw_button(w);
    break;
  case NG_CHECKBOX:
    draw_checkbox(w);
    break;
  case NG_TEXTBOX:
    draw_textbox(w);
    break;
  case NG_SLIDER:
    draw_slider(w);
    break;
  case NG_PROGRESS:
    draw_progress(w);
    break;
  case NG_LIST:
    draw_list(w);
    break;
  case NG_TREE:
    draw_tree(w);
    break;
  case NG_ICON:
    draw_icon(w);
    break;
  case NG_SWATCH:
    draw_swatch(w);
    break;
  default:
    break;
  }
}

void ng_set_overlay(ng_widget_t *w, int on) {
  if (w == 0) {
    return;
  }
  if (on != 0) {
    w->flags |= NG_WF_OVERLAY;
  } else {
    w->flags &= ~NG_WF_OVERLAY;
  }
  ng_damage_widget(w);
}

static void draw_widget(ng_widget_t *w) {
  ng_widget_t *c;

  if ((w->flags & NG_WF_VISIBLE) == 0u) {
    return;
  }
  if ((w->flags & NG_WF_OVERLAY) != 0u) {
    return; /* drawn by the overlay pass, above the client windows */
  }
  if (w->type != NG_DESKTOP && !rect_hits(w->abs, g_ctx.surf->clip)) {
    return; /* outside the damage rectangle, children included */
  }
  draw_self(w);
  for (c = w->first; c != 0; c = c->next) {
    draw_widget(c);
  }
}

void ng_render(nd_rect_t clip) {
  nd_rect_t area;

  if (!g_ready) {
    return;
  }
  area = nd_rect_intersect(clip, nd_rect(0, 0, g_ctx.surf->w, g_ctx.surf->h));
  if (nd_rect_empty(area)) {
    return;
  }
  nd_surface_set_clip(g_ctx.surf, area);
  draw_widget(g_ctx.root);
  /* The IME popup is a layer of its own: drawn last, inside the damage
   * rectangle like everything else. */
  nd_ime_draw(g_ctx.surf);
  nd_surface_reset_clip(g_ctx.surf);
  g_ctx.frame++;
}

/* Draw one overlay subtree: same recursion as draw_widget, but only
 * widgets that are visible (the overlay flag is a property of the root of
 * the subtree). */
static void draw_widget_overlay(ng_widget_t *w) {
  ng_widget_t *c;

  if ((w->flags & NG_WF_VISIBLE) == 0u) {
    return;
  }
  if (w->type != NG_DESKTOP && !rect_hits(w->abs, g_ctx.surf->clip)) {
    return;
  }
  draw_self(w);
  for (c = w->first; c != 0; c = c->next) {
    draw_widget_overlay(c);
  }
}

/* Is (x,y) inside a visible overlay card? The window manager asks before
 * its own hit test, so an open start menu owns the pointer instead of the
 * window that happens to be under it. */
int ng_overlay_hit(INT32 x, INT32 y) {
  UINT32 i;

  if (!g_ready) {
    return 0;
  }
  for (i = 0; i < g_used; i++) {
    ng_widget_t *w = &g_pool[i];

    if ((w->flags & NG_WF_OVERLAY) == 0u ||
        (w->flags & NG_WF_VISIBLE) == 0u) {
      continue;
    }
    if (nd_rect_contains(w->abs, x, y)) {
      return 1;
    }
  }
  return 0;
}

/* Second pass: overlay widgets and the IME popup, after the windows. */
void ng_render_overlay(nd_rect_t clip) {
  nd_rect_t area;
  UINT32    i;

  if (!g_ready) {
    return;
  }
  area = nd_rect_intersect(clip, nd_rect(0, 0, g_ctx.surf->w, g_ctx.surf->h));
  if (nd_rect_empty(area)) {
    return;
  }
  nd_surface_set_clip(g_ctx.surf, area);
  for (i = 0; i < g_used; i++) {
    if ((g_pool[i].flags & NG_WF_OVERLAY) != 0u &&
        (g_pool[i].flags & NG_WF_VISIBLE) != 0u) {
      if (g_pool[i].parent == 0 || (g_pool[i].parent->flags & NG_WF_OVERLAY) == 0u) {
        draw_widget_overlay(&g_pool[i]);
      }
    }
  }
  nd_ime_draw(g_ctx.surf);
  nd_surface_reset_clip(g_ctx.surf);
}

void ng_render_all(void) {
  if (g_ready) {
    ng_render(nd_rect(0, 0, g_ctx.surf->w, g_ctx.surf->h));
  }
}

/* ---- events ----------------------------------------------------------- */

static void slider_set_from_x(ng_widget_t *w, INT32 x) {
  INT32 x0 = w->abs.x + 8;
  INT32 x1 = w->abs.x + w->abs.w - 8;
  INT32 span = x1 - x0;
  INT32 v;

  if (span <= 0) {
    return;
  }
  if (x < x0) {
    x = x0;
  }
  if (x > x1) {
    x = x1;
  }
  v = w->u.slider.min +
      (INT32)(((INT64)(x - x0) * (w->u.slider.max - w->u.slider.min)) / span);
  if (v != w->u.slider.value) {
    w->u.slider.value = v;
    ng_damage_widget(w);
    if (w->on_value != 0) {
      w->on_value(&g_ctx, w, v);
    }
  }
}

static void button_fire(ng_widget_t *w) {
  if ((w->flags & NG_WF_ENABLED) != 0u && w->on_action != 0) {
    w->on_action(&g_ctx, w);
  }
}

static void checkbox_toggle(ng_widget_t *w) {
  w->u.check.checked = (UINT8)(w->u.check.checked == 0u ? 1u : 0u);
  ng_damage_widget(w);
  button_fire(w);
}

/* Remove the bytes [from, to) and put the caret at `from`. */
static void edit_delete(ng_widget_t *w, UINT32 from, UINT32 to) {
  UINT32 i;

  if (to <= from || to > w->u.edit.len) {
    return;
  }
  for (i = from; i + (to - from) <= w->u.edit.len; i++) {
    w->u.edit.buf[i] = w->u.edit.buf[i + (to - from)];
  }
  w->u.edit.len -= (to - from);
  w->u.edit.cursor = from;
  ng_damage_widget(w);
}

static void textbox_key(ng_widget_t *w, UINT32 key) {
  switch (key) {
  case ND_KEY_LEFT:
    w->u.edit.cursor = edit_prev(w, w->u.edit.cursor);
    ng_damage_widget(w);
    return;
  case ND_KEY_RIGHT:
    w->u.edit.cursor = edit_skip(w, w->u.edit.cursor, 1);
    ng_damage_widget(w);
    return;
  case ND_KEY_HOME:
    w->u.edit.cursor = 0;
    ng_damage_widget(w);
    return;
  case ND_KEY_END:
    w->u.edit.cursor = w->u.edit.len;
    ng_damage_widget(w);
    return;
  case ND_KEY_BACK:
    edit_delete(w, edit_prev(w, w->u.edit.cursor), w->u.edit.cursor);
    return;
  case ND_KEY_DELETE:
    edit_delete(w, w->u.edit.cursor, edit_skip(w, w->u.edit.cursor, 1));
    return;
  case ND_KEY_ENTER:
    button_fire(w);
    return;
  default:
    break;
  }

  /* Any printable codepoint goes straight in. Control keys live above the
   * Unicode range, so the test does not need to enumerate them. */
  if (key >= 0x20u && key <= 0x10FFFFu) {
    (void)ng_edit_insert_cp(w, key);
  }
}


static void list_click(ng_widget_t *w, INT32 y) {
  INT32 row = w->u.list.top + (y - (w->abs.y + 1)) / NG_ROW_H;

  if (row < 0 || (UINT32)row >= w->u.list.count) {
    return;
  }
  if (row == w->u.list.sel) {
    return;
  }
  w->u.list.sel = row;
  ng_damage_widget(w);
  if (w->on_value != 0) {
    w->on_value(&g_ctx, w, row);
  }
}

static void list_scroll(ng_widget_t *w, INT32 delta) {
  INT32 rows = list_visible_rows(w);
  INT32 max_top = (INT32)w->u.list.count - rows;

  if (max_top < 0) {
    max_top = 0;
  }
  w->u.list.top += delta;
  if (w->u.list.top < 0) {
    w->u.list.top = 0;
  }
  if (w->u.list.top > max_top) {
    w->u.list.top = max_top;
  }
  ng_damage_widget(w);
}

static void tree_click(ng_widget_t *w, INT32 x, INT32 y) {
  INT32 row;
  INT32 idx;

  if (y < w->abs.y + 1) {
    return;
  }
  row = (y - (w->abs.y + 1)) / NG_ROW_H;
  idx = tree_row_node(w, row);
  if (idx < 0) {
    return;
  }
  {
    ng_tnode_t *n = &w->u.tree.nodes[idx];
    INT32 box_x = w->abs.x + 2 + 6 + (INT32)n->depth * 16;

    if (n->has_children != 0u && x >= box_x && x < box_x + 12) {
      n->expanded = (UINT8)(n->expanded == 0u ? 1u : 0u);
      ng_damage_widget(w);
      return;
    }
  }
  if (w->u.tree.sel != row) {
    w->u.tree.sel = row;
    ng_damage_widget(w);
    if (w->on_value != 0) {
      w->on_value(&g_ctx, w, row);
    }
  }
}

static void set_hover(ng_widget_t *w) {
  if (g_ctx.hover == w) {
    return;
  }
  if (g_ctx.hover != 0) {
    g_ctx.hover->state &= ~NG_ST_HOVER;
    ng_damage_widget(g_ctx.hover);
  }
  g_ctx.hover = w;
  if (w != 0) {
    w->state |= NG_ST_HOVER;
    ng_damage_widget(w);
  }
}

static void window_drag_to(ng_widget_t *win, INT32 x, INT32 y) {
  nd_rect_t old = win->abs;
  INT32 nx = x - g_ctx.drag_dx;
  INT32 ny = y - g_ctx.drag_dy;

  if (nx < -win->abs.w + 60) {
    nx = -win->abs.w + 60;
  }
  if (ny < 0) {
    ny = 0;
  }
  if (nx > g_ctx.surf->w - 60) {
    nx = g_ctx.surf->w - 60;
  }
  if (ny > g_ctx.surf->h - NG_TITLE_H) {
    ny = g_ctx.surf->h - NG_TITLE_H;
  }
  if (nx == old.x && ny == old.y) {
    return;
  }
  win->rect.x = nx;
  win->rect.y = ny;
  ng_layout();
  ng_damage(window_visual(old));
  ng_damage_widget(win);
}

/* Tab traversal in tree order. The list is rebuilt on every Tab rather
 * than maintained incrementally: a few dozen widgets make that free, and
 * it stays correct when widgets are shown or hidden. */
static ng_widget_t *g_order[NG_POOL_SIZE];
static UINT32      g_order_n;

static void collect_focusable(ng_widget_t *w) {
  ng_widget_t *c;

  if (w != g_ctx.root && (w->flags & NG_WF_VISIBLE) != 0u &&
      (w->flags & NG_WF_FOCUSABLE) != 0u) {
    if (g_order_n < NG_POOL_SIZE) {
      g_order[g_order_n++] = w;
    }
  }
  for (c = w->first; c != 0; c = c->next) {
    collect_focusable(c);
  }
}

static ng_widget_t *focus_step(ng_widget_t *cur, int forward) {
  UINT32 i;
  UINT32 start;
  UINT32 step;
  int    found = 0;

  g_order_n = 0;
  collect_focusable(g_ctx.root);
  if (g_order_n == 0u) {
    return 0;
  }
  start = 0;
  for (i = 0; i < g_order_n; i++) {
    if (g_order[i] == cur) {
      start = i;
      found = 1;
      break;
    }
  }
  step = forward ? 1u : (g_order_n - 1u);
  if (!found) {
    /* Nothing focused yet: the first Tab must land on the first widget,
     * not on the one after it. Choose the index whose successor is 0 (or
     * n-1 when going backwards). */
    start = forward ? (g_order_n - 1u) : 0u;
  }
  return g_order[(start + step) % g_order_n];
}

/* Keyboard routing starts from the stored focus rather than a pointer hit.
 * For textboxes the IME gets first refusal; only keys it leaves unconsumed
 * reach the editor's ordinary caret and deletion handling. */
static void key_dispatch(const nd_event_t *ev) {
  ng_widget_t *w = g_ctx.focus;

  if (ev->key == ND_KEY_TAB) {
    ng_focus_set(focus_step(w, (ev->mods & ND_MOD_SHIFT) == 0u));
    return;
  }
  if (w == 0) {
    return;
  }
  switch (w->type) {
  case NG_BUTTON:
  case NG_ICON:
  case NG_SWATCH:
    if (ev->key == ND_KEY_ENTER || ev->key == ' ') {
      w->state |= NG_ST_PRESSED;
      ng_damage_widget(w);
      button_fire(w);
      w->state &= ~NG_ST_PRESSED;
      ng_damage_widget(w);
    }
    break;
  case NG_CHECKBOX:
    if (ev->key == ' ') {
      checkbox_toggle(w);
    }
    break;
  case NG_TEXTBOX:
    /* Ctrl+Space toggles Chinese (pinyin) / Latin passthrough. The IME
     * hands a key back only when it is idle and the key is not a Latin
     * letter in Chinese mode, or when Latin mode is active. */
    if (ev->key == ' ' && (ev->mods & ND_MOD_CTRL) != 0u) {
      nd_ime_set_en(!nd_ime_en());
      ng_damage_widget(w);
      return;
    }
    if (!nd_ime_key(w, ev->key)) {
      textbox_key(w, ev->key);
    }
    break;
  case NG_SLIDER:
    if (ev->key == ND_KEY_LEFT) {
      w->u.slider.value--;
      if (w->u.slider.value < w->u.slider.min) {
        w->u.slider.value = w->u.slider.min;
      }
      ng_damage_widget(w);
      if (w->on_value != 0) {
        w->on_value(&g_ctx, w, w->u.slider.value);
      }
    } else if (ev->key == ND_KEY_RIGHT) {
      w->u.slider.value++;
      if (w->u.slider.value > w->u.slider.max) {
        w->u.slider.value = w->u.slider.max;
      }
      ng_damage_widget(w);
      if (w->on_value != 0) {
        w->on_value(&g_ctx, w, w->u.slider.value);
      }
    }
    break;
  case NG_LIST:
    if (ev->key == ND_KEY_UP || ev->key == ND_KEY_DOWN) {
      INT32 delta = (ev->key == ND_KEY_UP) ? -1 : 1;
      INT32 next = w->u.list.sel + delta;
      if (next < 0) {
        next = 0;
      }
      if ((UINT32)next >= w->u.list.count) {
        next = (INT32)w->u.list.count - 1;
      }
      if (next != w->u.list.sel) {
        w->u.list.sel = next;
        if (w->u.list.sel < w->u.list.top) {
          w->u.list.top = w->u.list.sel;
        } else if (w->u.list.sel >= w->u.list.top + list_visible_rows(w)) {
          w->u.list.top = w->u.list.sel - list_visible_rows(w) + 1;
        }
        ng_damage_widget(w);
        if (w->on_value != 0) {
          w->on_value(&g_ctx, w, next);
        }
      }
    }
    break;
  case NG_TREE:
    if (ev->key == ND_KEY_UP || ev->key == ND_KEY_DOWN) {
      INT32 next = w->u.tree.sel;
      if (next < 0) {
        next = w->u.tree.top;
      } else if (ev->key == ND_KEY_DOWN) {
        next = tree_next_visible(w, next);
      } else {
        INT32 i = 0;
        while (i < (INT32)w->u.tree.count && i < next) {
          i++;
        }
        next = (i > 0) ? i - 1 : 0;
      }
      if (next >= 0 && (UINT32)next < w->u.tree.count) {
        w->u.tree.sel = next;
        ng_damage_widget(w);
      }
    } else if (ev->key == ND_KEY_ENTER && w->u.tree.sel >= 0 &&
               (UINT32)w->u.tree.sel < w->u.tree.count) {
      ng_tnode_t *n = &w->u.tree.nodes[w->u.tree.sel];
      if (n->has_children != 0u) {
        n->expanded = (UINT8)(n->expanded == 0u ? 1u : 0u);
        ng_damage_widget(w);
      }
    }
    break;
  default:
    break;
  }
}

void ng_dispatch(const nd_event_t *ev) {
  if (!g_ready || ev == 0) {
    return;
  }
  g_ctx.mouse_x = ev->x;
  g_ctx.mouse_y = ev->y;

  switch (ev->type) {
  case ND_EV_MOUSE_MOVE:
    if (g_ctx.drag_win != 0) {
      window_drag_to(g_ctx.drag_win, ev->x, ev->y);
      return;
    }
    if (g_ctx.capture != 0 && g_ctx.capture->type == NG_SLIDER) {
      slider_set_from_x(g_ctx.capture, ev->x);
      return;
    }
    set_hover(hit_test(ev->x, ev->y));
    break;

  case ND_EV_MOUSE_DOWN: {
    ng_widget_t *w = hit_test(ev->x, ev->y);

    g_ctx.buttons |= ev->button;
    if (w == 0) {
      ng_focus_set(0);
      break;
    }
    if (w->type == NG_WINDOW) {
      nd_rect_t close_box =
          nd_rect(w->abs.x + w->abs.w - 24, w->abs.y + 8, 16, 14);
      ng_raise(w);
      if (ev->y < w->abs.y + NG_TITLE_H) {
        if (w->u.window.closable != 0u &&
            nd_rect_contains(close_box, ev->x, ev->y)) {
          button_fire(w);
          break;
        }
        g_ctx.drag_win = w;
        g_ctx.drag_dx = ev->x - w->abs.x;
        g_ctx.drag_dy = ev->y - w->abs.y;
        w->state |= NG_ST_DRAG;
        ng_focus_set(0);
        break;
      }
      ng_focus_set(0);
      break;
    }
    ng_raise(w);
    if ((w->flags & NG_WF_FOCUSABLE) != 0u) {
      ng_focus_set(w);
    }
    switch (w->type) {
    case NG_BUTTON:
    case NG_ICON:
    case NG_SWATCH:
      w->state |= NG_ST_PRESSED;
      /* Capture the press: the release must be judged against the same
       * button even if the pointer drifts a pixel or two, which is what
       * a physical mouse always does. */
      g_ctx.capture = w;
      ng_damage_widget(w);
      break;
    case NG_CHECKBOX:
      w->state |= NG_ST_PRESSED;
      g_ctx.capture = w;
      ng_damage_widget(w);
      break;
    case NG_TEXTBOX:
      /* Caret placement by measured width, so a click between two Chinese
       * characters lands on the boundary between them. */
      {
        INT32  rel = ev->x - (w->abs.x + NG_PAD);
        UINT32 off = w->u.edit.scroll;
        INT32  acc = 0;

        if (rel <= 0) {
          off = w->u.edit.scroll;
        } else {
          while (off < w->u.edit.len) {
            const char *p = w->u.edit.buf + off;
            UINT32      cp = nd_utf8_next(&p);
            UINT32      nxt = (UINT32)(p - w->u.edit.buf);
            INT32       adv = (INT32)nd_char_adv(cp, NG_TEXT_SCALE);

            if (acc + adv / 2 > rel) {
              break;
            }
            acc += adv;
            off = nxt;
          }
        }
        w->u.edit.cursor = off;
        ng_damage_widget(w);
      }
      break;
    case NG_SLIDER:
      g_ctx.capture = w;
      slider_set_from_x(w, ev->x);
      break;
    case NG_LIST:
      list_click(w, ev->y);
      break;
    case NG_TREE:
      tree_click(w, ev->x, ev->y);
      break;
    default:
      break;
    }
    break;
  }

  case ND_EV_MOUSE_UP: {
    ng_widget_t *w = g_ctx.capture != 0 ? g_ctx.capture
                                        : hit_test(ev->x, ev->y);

    g_ctx.buttons &= (UINT8)~ev->button;
    if (g_ctx.drag_win != 0) {
      g_ctx.drag_win->state &= ~NG_ST_DRAG;
      ng_damage_widget(g_ctx.drag_win);
      g_ctx.drag_win = 0;
      break;
    }
    g_ctx.capture = 0;
    if (w == 0) {
      break;
    }
    if ((w->type == NG_BUTTON || w->type == NG_ICON ||
         w->type == NG_SWATCH) && (w->state & NG_ST_PRESSED) != 0u) {
      w->state &= ~NG_ST_PRESSED;
      ng_damage_widget(w);
      if (nd_rect_contains(w->abs, ev->x, ev->y)) {
        button_fire(w);
      }
    } else if (w->type == NG_CHECKBOX && (w->state & NG_ST_PRESSED) != 0u) {
      w->state &= ~NG_ST_PRESSED;
      if (nd_rect_contains(w->abs, ev->x, ev->y)) {
        checkbox_toggle(w);
      } else {
        ng_damage_widget(w);
      }
    }
    break;
  }

  case ND_EV_KEY:
    key_dispatch(ev);
    break;

  case ND_EV_WHEEL: {
    ng_widget_t *w = g_ctx.hover;
    if (w == 0) {
      w = hit_test(ev->x, ev->y);
    }
    if (w != 0 && w->type == NG_LIST) {
      list_scroll(w, (ev->dy > 0) ? 3 : -3);
    }
    break;
  }

  default:
    break;
  }
}
