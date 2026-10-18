/**
 * Novus OS - M5 demo desktop and NovusDisplay self test.
 *
 * Two jobs in one file because they share the same widget code:
 *
 *   nd_selftest() draws on an off-screen surface and checks exact pixels,
 *   then drives the controls with synthetic events. It needs no display at
 *   all, so M5 is verifiable on a headless boot: the PASS/FAIL lines come
 *   out on COM1.
 *
 *   nd_demo_init() builds the desktop that is shown on the framebuffer,
 *   and nd_demo_tick() (timer context) polls input, routes events, and
 *   redraws only the damage rectangle. The pointer is save-under, so a
 *   mouse move costs a few hundred bytes of copying rather than a frame.
 */

#include "kernel.h"
#include "nvgui.h"
#include "nd_input.h"
#include "apps.h"

#if CONFIG_NOVUS_GUI

/* ---- self test -------------------------------------------------------- */

static UINT32 g_pass;
static UINT32 g_fail;

#define CHECK(cond, what)              \
  do {                                 \
    if (cond) {                        \
      g_pass++;                        \
    } else {                           \
      g_fail++;                        \
      kputs("[ND] FAIL: " what "\n");  \
    }                                  \
  } while (0)

static UINT32 pixel(nd_surface_t *s, INT32 x, INT32 y) {
  return s->px[(UINT32)y * s->stride + (UINT32)x];
}

/* ---- widget probes ---------------------------------------------------- */

static int g_probe_clicks;
static int g_probe_value;
static char g_probe_buf[16];

static void probe_ok(ng_ctx_t *ctx, ng_widget_t *w) {
  (void)ctx;
  (void)w;
  g_probe_clicks++;
}

static void probe_value(ng_ctx_t *ctx, ng_widget_t *w, INT32 v) {
  (void)ctx;
  (void)w;
  g_probe_value = v;
}

static void send_mouse(UINT8 type, UINT8 button, INT32 x, INT32 y) {
  nd_event_t ev;

  memset(&ev, 0, sizeof(ev));
  ev.type = type;
  ev.button = button;
  ev.x = (INT16)x;
  ev.y = (INT16)y;
  nd_input_push(&ev);
  ng_dispatch(&ev);
}

static void send_key(UINT32 key) {
  nd_event_t ev;

  memset(&ev, 0, sizeof(ev));
  ev.type = ND_EV_KEY;
  ev.key = key;
  nd_input_push(&ev);
  ng_dispatch(&ev);
}

void nd_selftest(void) {
  nd_surface_t s;
  ng_ctx_t    *ctx;
  ng_widget_t *win;
  ng_widget_t *btn;
  ng_widget_t *edit;
  ng_widget_t *slider;
  ng_widget_t *list;
  ng_widget_t *tree;
  static const char *items[3] = {"alpha", "beta", "gamma"};
  static ng_tnode_t nodes[4];
  nd_rect_t item;

  g_pass = 0;
  g_fail = 0;

  if (nd_surface_create(&s, 320, 240) != 0) {
    kputs("[ND] selftest FAIL (no surface)\n");
    return;
  }

  /* clipped fill */
  nd_clear(&s, ND_RGB(0, 0, 0));
  nd_fill_rect(&s, nd_rect(10, 10, 20, 10), ND_RGB(255, 0, 0));
  CHECK(pixel(&s, 10, 10) == ND_RGB(255, 0, 0), "fill start");
  CHECK(pixel(&s, 29, 19) == ND_RGB(255, 0, 0), "fill end");
  CHECK(pixel(&s, 30, 10) == ND_RGB(0, 0, 0), "fill width");
  nd_surface_set_clip(&s, nd_rect(0, 0, 50, 50));
  nd_fill_rect(&s, nd_rect(0, 0, 100, 100), ND_RGB(0, 255, 0));
  CHECK(pixel(&s, 49, 49) == ND_RGB(0, 255, 0), "clip inside");
  CHECK(pixel(&s, 60, 10) == ND_RGB(0, 0, 0), "clip outside");
  nd_surface_reset_clip(&s);

  /* line, circle, rounded rect */
  nd_clear(&s, ND_RGB(0, 0, 0));
  nd_line(&s, 10, 100, 50, 100, ND_RGB(0, 0, 255));
  CHECK(pixel(&s, 10, 100) == ND_RGB(0, 0, 255), "line start");
  CHECK(pixel(&s, 50, 100) == ND_RGB(0, 0, 255), "line end");
  nd_fill_circle(&s, 160, 120, 20, ND_RGB(0, 255, 255));
  CHECK(pixel(&s, 160, 100) == ND_RGB(0, 255, 255), "circle top");
  CHECK(pixel(&s, 160, 99) == ND_RGB(0, 0, 0), "circle radius");
  CHECK(pixel(&s, 160, 120) == ND_RGB(0, 255, 255), "circle centre");
  nd_round_rect(&s, nd_rect(220, 60, 60, 60), 12, ND_RGB(255, 128, 0));
  CHECK(pixel(&s, 220, 60) == ND_RGB(0, 0, 0), "round corner");
  CHECK(pixel(&s, 250, 90) == ND_RGB(255, 128, 0), "round centre");

  /* alpha blend: 0xFF over 0x00 at 128/255 lands exactly on 128 */
  nd_clear(&s, ND_RGB(0, 0, 0));
  nd_blend_rect(&s, nd_rect(0, 0, 10, 10), ND_RGB(255, 255, 255), 128);
  CHECK(pixel(&s, 5, 5) == ND_RGB(128, 128, 128), "alpha 50%");
  nd_blend_rect(&s, nd_rect(0, 0, 10, 10), ND_RGB(255, 255, 255), 0);
  CHECK(pixel(&s, 5, 5) == ND_RGB(128, 128, 128), "alpha 0");

  /* text: a glyph must light pixels, and advance by 6 per character */
  nd_clear(&s, ND_RGB(0, 0, 0));
  nd_text(&s, 10, 10, "I", ND_RGB(255, 255, 255), 1);
  CHECK(nd_text_width("AB", 1) == 11u, "text width");
  CHECK(nd_font_height(1) == 7u, "font height");
  CHECK(nd_text_width("AB", 2) == 22u, "text width 2x");
  {
    UINT32 lit = 0;
    INT32  y;
    INT32  x;
    for (y = 10; y < 17; y++) {
      for (x = 10; x < 15; x++) {
        if (pixel(&s, x, y) != ND_RGB(0, 0, 0)) {
          lit++;
        }
      }
    }
    CHECK(lit == 11u, "glyph pixels");
  }

  /* off-screen blit */
  {
    nd_surface_t src;
    if (nd_surface_create(&src, 8, 8) != 0) {
      CHECK(0, "blit surface");
    } else {
      nd_clear(&src, ND_RGB(9, 9, 9));
      nd_blit(&s, 100, 200, &src, nd_rect(0, 0, 8, 8));
      CHECK(pixel(&s, 100, 200) == ND_RGB(9, 9, 9), "blit");
      nd_surface_destroy(&src);
    }
  }

  /* ---- widgets ---- */
  ctx = ng_init(&s);
  if (ctx == 0) {
    CHECK(0, "gui init");
    nd_surface_destroy(&s);
    kputs("[ND] selftest FAIL\n");
    return;
  }
  ng_render_all();

  g_probe_clicks = 0;
  win = ng_create(ctx->root, NG_WINDOW, 10, 10, 220, 190);
  ng_window(win, "probe", 0);
  btn = ng_create(win, NG_BUTTON, 20, 40, 90, 32);
  ng_button(btn, "OK", probe_ok);
  ng_layout();
  /* window abs (10,10); client (11,40); button abs (31,80) */
  send_mouse(ND_EV_MOUSE_DOWN, ND_BUTTON_LEFT, 66, 96);
  send_mouse(ND_EV_MOUSE_UP, ND_BUTTON_LEFT, 66, 96);
  CHECK(g_probe_clicks == 1, "button click");
  CHECK(ng_focus_get() == btn, "button focus");
  send_mouse(ND_EV_MOUSE_DOWN, ND_BUTTON_LEFT, 66, 96);
  send_mouse(ND_EV_MOUSE_UP, ND_BUTTON_LEFT, 66, 20);
  CHECK(g_probe_clicks == 1, "release outside");

  g_probe_buf[0] = '\0';
  edit = ng_create(win, NG_TEXTBOX, 20, 90, 160, 30);
  ng_textbox(edit, g_probe_buf, sizeof(g_probe_buf), "type");
  ng_layout();
  send_mouse(ND_EV_MOUSE_DOWN, ND_BUTTON_LEFT, 40, 40 + 90 + 10);
  send_mouse(ND_EV_MOUSE_UP, ND_BUTTON_LEFT, 40, 40 + 90 + 10);
  CHECK(ng_focus_get() == edit, "textbox focus");
  send_key('h');
  send_key('i');
  CHECK(g_probe_buf[0] == 'h' && g_probe_buf[1] == 'i' &&
            g_probe_buf[2] == '\0',
        "textbox typing");
  send_key(ND_KEY_BACK);
  CHECK(g_probe_buf[0] == 'h' && g_probe_buf[1] == '\0', "textbox backspace");
  send_key(ND_KEY_LEFT);
  send_key('X');
  CHECK(g_probe_buf[0] == 'X' && g_probe_buf[1] == 'h' &&
            g_probe_buf[2] == '\0',
        "textbox insert at caret");

  g_probe_value = -1;
  slider = ng_create(win, NG_SLIDER, 20, 130, 160, 30);
  ng_slider(slider, 0, 100, 10, probe_value);
  ng_layout();
  item = slider->abs;
  send_mouse(ND_EV_MOUSE_DOWN, ND_BUTTON_LEFT, item.x + item.w - 2,
             item.y + item.h / 2);
  CHECK(g_probe_value == 100, "slider max");
  send_mouse(ND_EV_MOUSE_DOWN, ND_BUTTON_LEFT, item.x + 8, item.y + item.h / 2);
  send_mouse(ND_EV_MOUSE_UP, ND_BUTTON_LEFT, item.x + 8, item.y + item.h / 2);
  CHECK(g_probe_value == 0, "slider min");

  g_probe_value = -1;
  list = ng_create(ctx->root, NG_LIST, 250, 10, 60, 80);
  ng_list(list, items, 3, probe_value);
  ng_layout();
  item = list->abs;
  send_mouse(ND_EV_MOUSE_DOWN, ND_BUTTON_LEFT, item.x + 10,
             item.y + 1 + NG_ROW_H + 2);
  send_mouse(ND_EV_MOUSE_UP, ND_BUTTON_LEFT, item.x + 10,
             item.y + 1 + NG_ROW_H + 2);
  CHECK(g_probe_value == 1, "list row");
  CHECK(list->u.list.sel == 1, "list selection");

  /* tree: row 3 is hidden behind a collapsed node 2 until it expands */
  nodes[0].text = "root";
  nodes[0].depth = 0;
  nodes[0].has_children = 1;
  nodes[0].expanded = 1;
  nodes[1].text = "child";
  nodes[1].depth = 1;
  nodes[1].has_children = 0;
  nodes[1].expanded = 0;
  nodes[2].text = "closed";
  nodes[2].depth = 1;
  nodes[2].has_children = 1;
  nodes[2].expanded = 0;
  nodes[3].text = "hidden";
  nodes[3].depth = 2;
  nodes[3].has_children = 0;
  nodes[3].expanded = 0;

  tree = ng_create(ctx->root, NG_TREE, 250, 100, 60, 130);
  ng_tree(tree, nodes, 4, probe_value);
  ng_layout();
  item = tree->abs;
  /* row 3 does not exist while "closed" is collapsed */
  send_mouse(ND_EV_MOUSE_DOWN, ND_BUTTON_LEFT, item.x + 40,
             item.y + 1 + 3 * NG_ROW_H + 2);
  send_mouse(ND_EV_MOUSE_UP, ND_BUTTON_LEFT, item.x + 40,
             item.y + 1 + 3 * NG_ROW_H + 2);
  CHECK(tree->u.tree.sel < 0, "tree hidden row");
  /* chevron of row 2: x = abs.x + 2 + 6 + depth(1) * 16 */
  {
    INT32 cx = item.x + 2 + 6 + 16 + 4;
    INT32 cy = item.y + 1 + 2 * NG_ROW_H + 8;
    send_mouse(ND_EV_MOUSE_DOWN, ND_BUTTON_LEFT, cx, cy);
    send_mouse(ND_EV_MOUSE_UP, ND_BUTTON_LEFT, cx, cy);
    CHECK(nodes[2].expanded == 1, "tree expand");
    send_mouse(ND_EV_MOUSE_DOWN, ND_BUTTON_LEFT, item.x + 40,
               item.y + 1 + 3 * NG_ROW_H + 2);
    send_mouse(ND_EV_MOUSE_UP, ND_BUTTON_LEFT, item.x + 40,
               item.y + 1 + 3 * NG_ROW_H + 2);
    CHECK(tree->u.tree.sel == 3, "tree visible row");
  }

  /* focus traversal */
  ng_focus_set(btn);
  send_key(ND_KEY_TAB);
  CHECK(ng_focus_get() == edit, "tab order");

  nd_surface_destroy(&s);

  kputs("[ND] selftest: ");
  kputs_hex(g_pass);
  kputs(" checks passed, ");
  kputs_hex(g_fail);
  kputs(" failed\n");
  kputs(g_fail == 0u ? "[ND] selftest PASS\n" : "[ND] selftest FAIL\n");
}

/* ---- demo desktop ----------------------------------------------------- */

static int          g_demo_ready;
static char         g_status[80];
static char         g_slider_text[24];
static char         g_input[48];
static ng_widget_t *g_status_label;
static ng_widget_t *g_slider_label;

static void set_status(const char *s) {
  UINT32 i;

  for (i = 0; i + 1u < sizeof(g_status) && s[i] != '\0'; i++) {
    g_status[i] = s[i];
  }
  g_status[i] = '\0';
  if (g_status_label != 0) {
    g_status_label->u.label.text = g_status;
    ng_damage_widget(g_status_label);
  }
}

static void on_apply(ng_ctx_t *ctx, ng_widget_t *w) {
  (void)ctx;
  (void)w;
  set_status("Apply pressed: settings applied");
  kputs("[ND] GUI: Apply pressed\n");
}

static void on_cancel(ng_ctx_t *ctx, ng_widget_t *w) {
  (void)ctx;
  (void)w;
  set_status("Cancel pressed");
}

static void on_nx(ng_ctx_t *ctx, ng_widget_t *w) {
  (void)ctx;
  set_status(w->u.check.checked != 0u
                 ? "NX protection enabled"
                 : "NX protection disabled");
}

static void on_slider(ng_ctx_t *ctx, ng_widget_t *w, INT32 value) {
  (void)ctx;
  (void)w;
  {
    UINT32 i = 0;
    char   tmp[12];
    UINT32 n = 0;
    UINT32 v = (UINT32)value;

    if (v == 0u) {
      tmp[n++] = '0';
    }
    while (v > 0u) {
      tmp[n++] = (char)('0' + (v % 10u));
      v /= 10u;
    }
    while (n > 0u) {
      g_slider_text[i++] = tmp[--n];
    }
    g_slider_text[i] = '\0';
  }
  if (g_slider_label != 0) {
    g_slider_label->u.label.text = g_slider_text;
    ng_damage_widget(g_slider_label);
  }
}

static void on_list(ng_ctx_t *ctx, ng_widget_t *w, INT32 row) {
  (void)ctx;
  set_status(w->u.list.items[row]);
}

static void on_tree(ng_ctx_t *ctx, ng_widget_t *w, INT32 row) {
  (void)ctx;
  set_status(w->u.tree.nodes[row].text);
}

static void on_close(ng_ctx_t *ctx, ng_widget_t *w) {
  (void)ctx;
  ng_show(w, 0);
  set_status("Window closed");
  kputs("[ND] GUI: window closed\n");
}

static ng_widget_t *add_label(ng_widget_t *p, INT32 x, INT32 y, const char *text,
                              nd_color_t c) {
  return ng_label(ng_create(p, NG_LABEL, x, y, 400, 16), text, c,
                  NG_TEXT_SCALE);
}

/* M8 launcher: the buttons on the shell's taskbar start the built-in
 * applications. One callback each so the index is not smuggled through
 * widget memory, and the kernel's app table stays the single source of
 * names and order. */
static void on_launch_files(ng_ctx_t *ctx, ng_widget_t *w) {
  (void)ctx;
  (void)w;
  (void)apps_launch(0);
}

static void on_launch_tasks(ng_ctx_t *ctx, ng_widget_t *w) {
  (void)ctx;
  (void)w;
  (void)apps_launch(1);
}

static void on_launch_setup(ng_ctx_t *ctx, ng_widget_t *w) {
  (void)ctx;
  (void)w;
  (void)apps_launch(2);
}

static void build_desktop(nd_surface_t *screen, ng_ctx_t *ctx) {
  ng_widget_t *win;
  static const char *items[5] = {"Create a process", "Map a page",
                                 "Check NX protection", "Switch CR3",
                                 "Exit with code 0"};
  static ng_tnode_t nodes[7];

  /* Window 1: renderer + settings. */
  win = ng_create(ctx->root, NG_WINDOW, 40, 44, 520, 336);
  ng_window(win, "NovusDisplay", 0);
  ng_label(ng_create(win, NG_LABEL, 16, 10, 480, 16),
           "2D renderer + GUI controls", ctx->theme.accent, NG_TEXT_SCALE);
  add_label(win, 16, 38, "Built-in 5x7 font, clipping, alpha blend",
            ctx->theme.text_dim);
  ng_progress(ng_create(win, NG_PROGRESS, 16, 70, 488, 26), 720, 0);
  ng_button(ng_create(win, NG_BUTTON, 16, 110, 110, 34), "Apply", on_apply);
  ng_button(ng_create(win, NG_BUTTON, 138, 110, 110, 34), "Cancel",
            on_cancel);
  ng_checkbox(ng_create(win, NG_CHECKBOX, 16, 158, 440, 26),
              "Protect data pages with NX", 1, on_nx);
  add_label(win, 16, 196, "Slider", ctx->theme.text_dim);
  ng_slider(ng_create(win, NG_SLIDER, 16, 216, 300, 30), 0, 100, 65,
            on_slider);
  g_slider_label = add_label(win, 330, 222, "65", ctx->theme.text);
  ng_label(ng_create(win, NG_LABEL, 16, 262, 480, 16),
           "Drag the title bar, click a control, Tab to focus",
           ctx->theme.text_dim, 1);

  /* Window 2: the text/list/tree half of the widget set. */
  win = ng_create(ctx->root, NG_WINDOW, 600, 64, 380, 440);
  ng_window(win, "Widgets", 1);
  win->on_action = on_close;
  add_label(win, 16, 8, "Text box", ctx->theme.text_dim);
  g_input[0] = 'h';
  g_input[1] = 'e';
  g_input[2] = 'l';
  g_input[3] = 'l';
  g_input[4] = 'o';
  g_input[5] = '\0';
  ng_textbox(ng_create(win, NG_TEXTBOX, 16, 32, 348, 32), g_input,
             sizeof(g_input), "type here");
  add_label(win, 16, 76, "List", ctx->theme.text_dim);
  ng_list(ng_create(win, NG_LIST, 16, 100, 348, 132), items, 5, on_list);
  add_label(win, 16, 244, "Tree", ctx->theme.text_dim);

  nodes[0].text = "novus-os";
  nodes[0].depth = 0;
  nodes[0].has_children = 1;
  nodes[0].expanded = 1;
  nodes[1].text = "kernel/";
  nodes[1].depth = 1;
  nodes[1].has_children = 1;
  nodes[1].expanded = 1;
  nodes[2].text = "sched.c";
  nodes[2].depth = 2;
  nodes[2].has_children = 0;
  nodes[2].expanded = 0;
  nodes[3].text = "graphics/";
  nodes[3].depth = 1;
  nodes[3].has_children = 1;
  nodes[3].expanded = 0;
  nodes[4].text = "nd_core.c";
  nodes[4].depth = 2;
  nodes[4].has_children = 0;
  nodes[4].expanded = 0;
  nodes[5].text = "nvgui.c";
  nodes[5].depth = 2;
  nodes[5].has_children = 0;
  nodes[5].expanded = 0;
  nodes[6].text = "novuslib-rs";
  nodes[6].depth = 1;
  nodes[6].has_children = 0;
  nodes[6].expanded = 0;
  ng_tree(ng_create(win, NG_TREE, 16, 268, 348, 156), nodes, 7, on_tree);

  /* M8: taskbar across the top of the desktop, with the launcher. The
   * windows are positioned below it (y >= 44), so it never covers one. */
  ng_panel(ng_create(ctx->root, NG_PANEL, 0, 0, screen->w, 34),
           ND_RGB(0x16, 0x1C, 0x26));
  ng_label(ng_create(ctx->root, NG_LABEL, 12, 9, 80, 16), "novus",
           ctx->theme.accent, NG_TEXT_SCALE);
  ng_button(ng_create(ctx->root, NG_BUTTON, 90, 5, 92, 24), "Files",
            on_launch_files);
  ng_button(ng_create(ctx->root, NG_BUTTON, 190, 5, 92, 24), "Tasks",
            on_launch_tasks);
  ng_button(ng_create(ctx->root, NG_BUTTON, 290, 5, 92, 24), "Setup",
            on_launch_setup);

  /* Status line, on the desktop itself. */
  set_status("Ready - M8 NovusDisplay");
  g_status_label = ng_label(
      ng_create(ctx->root, NG_LABEL, 40, screen->h - 28, 900, 16), g_status,
      ctx->theme.text_dim, 1);

  ng_layout();
}

void nd_demo_init(void) {
  nd_surface_t *screen = nd_screen();
  ng_ctx_t     *ctx;

  g_demo_ready = 0;
  if (screen == 0) {
    return;
  }
  nd_input_init();
  ctx = ng_init(screen);
  if (ctx == 0) {
    kputs("[ND] demo: GUI init failed\n");
    return;
  }
  build_desktop(screen, ctx);
  ng_render_all();
  nd_cursor_set(screen->w / 2, screen->h / 3);
  nd_cursor_draw();
  g_demo_ready = 1;
  kputs("[ND] demo desktop painted\n");
}

int nd_demo_ready(void) { return g_demo_ready; }

void nd_demo_dispatch(const nd_event_t *ev) {
  if (g_demo_ready) {
    ng_dispatch(ev);
  }
}

int nd_demo_take_damage(nd_rect_t *out) {
  if (!g_demo_ready) {
    return 0;
  }
  return ng_take_damage(out);
}

void nd_demo_paint(nd_rect_t clip) {
  if (g_demo_ready) {
    ng_render(clip);
  }
}

void nd_demo_tick(void) {
  nd_rect_t  d;
  nd_event_t ev;

  if (!g_demo_ready) {
    return;
  }
  nd_input_poll();
  while (nd_input_pop(&ev) != 0) {
    if (ev.type == ND_EV_MOUSE_MOVE) {
      nd_cursor_set(ev.x, ev.y); /* restores the pixels under the old one */
    }
    nd_demo_dispatch(&ev);
  }
  if (nd_demo_take_damage(&d)) {
    nd_cursor_hide();
    nd_demo_paint(d);
  }
  /* Idempotent: draws only when the pointer was hidden by a move or by
   * the repaint above. */
  nd_cursor_draw();
}

#else /* !CONFIG_NOVUS_GUI */

void nd_selftest(void) {
  kputs("[ND] GUI disabled, no widget self test\n");
}

void nd_demo_init(void) {}

void nd_demo_tick(void) {}

int nd_demo_ready(void) { return 0; }

void nd_demo_dispatch(const nd_event_t *ev) { (void)ev; }

int nd_demo_take_damage(nd_rect_t *out) {
  (void)out;
  return 0;
}

void nd_demo_paint(nd_rect_t clip) { (void)clip; }

#endif /* CONFIG_NOVUS_GUI */
