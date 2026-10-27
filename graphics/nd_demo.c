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
#include "nd_theme.h"
#include "nd_wallpaper.h"
#include "nd_input.h"
#include "nd_ime.h"
#include "rtc.h"
#include "apps.h"
#include "session.h"
#include "nd_lang.h"
#include "sched.h"
#include "nwm.h"
#include "power.h"
#include "install.h"

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

/* The self-test deliberately uses a heap-backed surface and synthetic input,
 * so its pixel and widget assertions do not depend on firmware graphics or
 * a physical keyboard. It exercises the same renderer and dispatcher used
 * by the visible desktop. */
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

  /* Mirroring copies the picture between framebuffers that may disagree
   * about the byte order, so the conversion has to round-trip exactly. */
  {
    nd_surface_t rgb;
    nd_surface_t bgr;
    UINT32        words[4];
    nd_color_t    c = ND_RGB(0x12, 0x34, 0x56);
    UINT32        w_bgr;
    UINT32        w_rgb;
    UINT32        mirrored;

    memset(&rgb, 0, sizeof(rgb));
    memset(&bgr, 0, sizeof(bgr));
    rgb.px = words; rgb.w = 2; rgb.h = 2; rgb.stride = 2;
    rgb.fb = 1; rgb.bgr = 0; rgb.clip = nd_rect(0, 0, 2, 2);
    bgr.px = words; bgr.w = 2; bgr.h = 2; bgr.stride = 2;
    bgr.fb = 1; bgr.bgr = 1; bgr.clip = nd_rect(0, 0, 2, 2);
    w_bgr = nd_hw_pixel(&bgr, c);
    w_rgb = nd_hw_pixel(&rgb, c);
    mirrored = (w_rgb & 0xFF00FF00u) | ((w_rgb & 0x000000FFu) << 16) |
               ((w_rgb & 0x00FF0000u) >> 16);
    CHECK(w_bgr == 0xFF123456u, "bgr keeps the canonical word");
    CHECK(w_rgb == 0xFF563412u, "rgb swaps red and blue");
    CHECK(mirrored == w_bgr, "mirror conversion round-trips");
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
  /* The edit tests exercise plain Latin input; run them in Latin
   * passthrough mode so the pinyin IME does not start a composition. */
  nd_ime_set_en(1);
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
  nd_ime_set_en(0); /* restore the default Chinese mode for the desktop */

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
  set_status("已应用设置");
  kputs("[ND] GUI: Apply pressed\n");
}

static void on_cancel(ng_ctx_t *ctx, ng_widget_t *w) {
  (void)ctx;
  (void)w;
  set_status("已取消");
}

static void on_nx(ng_ctx_t *ctx, ng_widget_t *w) {
  (void)ctx;
  set_status(w->u.check.checked != 0u ? "已启用 NX 保护"
                                     : "已关闭 NX 保护");
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
  set_status("窗口已关闭");
  kputs("[ND] GUI: window closed\n");
}

static ng_widget_t *add_label(ng_widget_t *p, INT32 x, INT32 y, const char *text,
                              nd_color_t c) {
  return ng_label(ng_create(p, NG_LABEL, x, y, 400, 16), text, c,
                  NG_TEXT_SCALE);
}

/* ---- Novus Fluent shell ----------------------------------------------- */

static ng_widget_t *g_win_demo;
static ng_widget_t *g_win_widgets;
static ng_widget_t *g_start_menu;
static ng_widget_t *g_quick;
static ng_widget_t *g_clock_label;
static ng_widget_t *g_run_dot[5];
static UINT32       g_theme_seen;
static UINT32       g_lang_seen;
static char         g_clock[8] = "00:00";
static char         g_search[48];

/* Taskbar running indicators follow the scheduler, not a private list:
 * an app started from the start menu lights its dot, one that exits
 * clears it. Names, not table indices - the installer moved the table. */
static const char *const g_app_names[5] = {"fileman", "taskman", "settings",
                                           "netman", "nsh"};

/* The RTC is optional: without one the taskbar just keeps the placeholder. */
static void clock_refresh(void) {
  UINT8 h = 0;
  UINT8 m = 0;
  char  buf[8];
  UINT32 i;

  if (!rtc_now(&h, &m)) {
    return;
  }
  buf[0] = (char)('0' + (h / 10u) % 10u);
  buf[1] = (char)('0' + h % 10u);
  buf[2] = ':';
  buf[3] = (char)('0' + (m / 10u) % 10u);
  buf[4] = (char)('0' + m % 10u);
  buf[5] = '\0';
  for (i = 0; i < sizeof(g_clock); i++) {
    if (g_clock[i] == buf[i]) {
      if (buf[i] == '\0') {
        return; /* same minute: nothing to repaint */
      }
      continue;
    }
    break;
  }
  for (i = 0; i < sizeof(g_clock); i++) {
    g_clock[i] = buf[i];
    if (buf[i] == '\0') {
      break;
    }
  }
  if (g_clock_label != 0) {
    g_clock_label->u.label.text = g_clock;
    ng_damage_widget(g_clock_label);
  }
}

static void running_refresh(void) {
  UINT32 i;

  for (i = 0; i < 5u; i++) {
    if (g_run_dot[i] != 0) {
      ng_show(g_run_dot[i], sched_find_name(g_app_names[i]) != 0);
    }
  }
}

/* Called from the window manager's tick: the shell owns the clock, but the
 * WM owns the timer path once it is running. */
void nd_demo_idle(void) {
  clock_refresh();
  running_refresh();
  /* Auto mode flips at 19:00 and 07:00; a manual change bumps the version
   * from the settings panel or the quick-settings card. Either way the
   * static widget tree is rebuilt once, not repainted colour by colour. */
  (void)nd_theme_resolve();
  if (nd_theme_version() != g_theme_seen) {
    nd_demo_theme_sync();
  }
  /* Language needs no rebuild (the renderer resolves each label), only a
   * full repaint so every cached pixel is replaced. */
  if (nd_lang_version() != g_lang_seen) {
    nd_surface_t *s = nd_screen();

    g_lang_seen = nd_lang_version();
    if (s != 0) {
      ng_damage(nd_rect(0, 0, s->w, s->h));
    }
  }
}

static void overlays_close(void) {
  if (g_start_menu != 0) {
    ng_show(g_start_menu, 0);
  }
  if (g_quick != 0) {
    ng_show(g_quick, 0);
  }
}

static void on_start(ng_ctx_t *ctx, ng_widget_t *w) {
  (void)ctx;
  (void)w;
  if (g_quick != 0) {
    ng_show(g_quick, 0);
  }
  if (g_start_menu != 0) {
    ng_show(g_start_menu, (g_start_menu->flags & NG_WF_VISIBLE) == 0u);
  }
}

static void on_quick(ng_ctx_t *ctx, ng_widget_t *w) {
  (void)ctx;
  (void)w;
  if (g_start_menu != 0) {
    ng_show(g_start_menu, 0);
  }
  if (g_quick != 0) {
    ng_show(g_quick, (g_quick->flags & NG_WF_VISIBLE) == 0u);
  }
}

static void on_open_demo(ng_ctx_t *ctx, ng_widget_t *w) {
  (void)ctx;
  (void)w;
  overlays_close();
  if (g_win_demo != 0) {
    ng_show(g_win_demo, 1);
    ng_raise(g_win_demo);
  }
}

static void on_open_widgets(ng_ctx_t *ctx, ng_widget_t *w) {
  (void)ctx;
  (void)w;
  overlays_close();
  if (g_win_widgets != 0) {
    ng_show(g_win_widgets, 1);
    ng_raise(g_win_widgets);
  }
}

static void launch(const char *name) {
  overlays_close();
  (void)apps_launch_name(name);
}

static void on_launch_files(ng_ctx_t *ctx, ng_widget_t *w) {
  (void)ctx;
  (void)w;
  launch("fileman");
}

static void on_launch_tasks(ng_ctx_t *ctx, ng_widget_t *w) {
  (void)ctx;
  (void)w;
  launch("taskman");
}

static void on_launch_settings(ng_ctx_t *ctx, ng_widget_t *w) {
  (void)ctx;
  (void)w;
  launch("settings");
}

static void on_launch_net(ng_ctx_t *ctx, ng_widget_t *w) {
  (void)ctx;
  (void)w;
  launch("netman");
}

static void on_launch_install(ng_ctx_t *ctx, ng_widget_t *w) {
  (void)ctx;
  (void)w;
  launch("setup");
}

static void on_launch_terminal(ng_ctx_t *ctx, ng_widget_t *w) {
  (void)ctx;
  (void)w;
  launch("nsh");
}

static void on_about(ng_ctx_t *ctx, ng_widget_t *w) {
  set_status("Novus OS：内核 C + 用户态 Rust，Novus Fluent 桌面");
  on_open_demo(ctx, w);
}

static void on_mode_light(ng_ctx_t *ctx, ng_widget_t *w) {
  (void)ctx;
  (void)w;
  nwm_theme_set_mode(ND_THEME_LIGHT);
}

static void on_mode_dark(ng_ctx_t *ctx, ng_widget_t *w) {
  (void)ctx;
  (void)w;
  nwm_theme_set_mode(ND_THEME_DARK);
}

static void on_mode_auto(ng_ctx_t *ctx, ng_widget_t *w) {
  (void)ctx;
  (void)w;
  nwm_theme_set_mode(ND_THEME_AUTO);
}

/* One callback per preset: the widget id is a pool slot, not a colour
 * index, so the mapping lives here where it is visible. */
static const UINT32 g_accents[8] = {
    0x000067C0u, 0x00107C10u, 0x00B45309u, 0x007C3AEDu,
    0x00C42B1Cu, 0x000E7490u, 0x00BE185Du, 0x004B5563u,
};

static void on_accent0(ng_ctx_t *c, ng_widget_t *w) { (void)c; (void)w; nwm_accent_set(g_accents[0]); }
static void on_accent1(ng_ctx_t *c, ng_widget_t *w) { (void)c; (void)w; nwm_accent_set(g_accents[1]); }
static void on_accent2(ng_ctx_t *c, ng_widget_t *w) { (void)c; (void)w; nwm_accent_set(g_accents[2]); }
static void on_accent3(ng_ctx_t *c, ng_widget_t *w) { (void)c; (void)w; nwm_accent_set(g_accents[3]); }
static void on_accent4(ng_ctx_t *c, ng_widget_t *w) { (void)c; (void)w; nwm_accent_set(g_accents[4]); }
static void on_accent5(ng_ctx_t *c, ng_widget_t *w) { (void)c; (void)w; nwm_accent_set(g_accents[5]); }
static void on_accent6(ng_ctx_t *c, ng_widget_t *w) { (void)c; (void)w; nwm_accent_set(g_accents[6]); }
static void on_accent7(ng_ctx_t *c, ng_widget_t *w) { (void)c; (void)w; nwm_accent_set(g_accents[7]); }

static void on_paper0(ng_ctx_t *c, ng_widget_t *w) { (void)c; (void)w; nwm_wallpaper_set(0); }
static void on_paper1(ng_ctx_t *c, ng_widget_t *w) { (void)c; (void)w; nwm_wallpaper_set(1); }
static void on_paper2(ng_ctx_t *c, ng_widget_t *w) { (void)c; (void)w; nwm_wallpaper_set(2); }
static void on_paper3(ng_ctx_t *c, ng_widget_t *w) { (void)c; (void)w; nwm_wallpaper_set(3); }
static void on_paper4(ng_ctx_t *c, ng_widget_t *w) { (void)c; (void)w; nwm_wallpaper_set(4); }
static void on_paper5(ng_ctx_t *c, ng_widget_t *w) { (void)c; (void)w; nwm_wallpaper_set(5); }
static void on_paper6(ng_ctx_t *c, ng_widget_t *w) { (void)c; (void)w; nwm_wallpaper_set(6); }

static void on_shutdown(ng_ctx_t *c, ng_widget_t *w) {
  (void)c;
  (void)w;
  power_off();
}

static void on_reboot(ng_ctx_t *c, ng_widget_t *w) {
  (void)c;
  (void)w;
  power_reboot();
}

static void build_desktop(nd_surface_t *screen, ng_ctx_t *ctx) {
  ng_widget_t *win;
  static const char *items[5] = {"创建进程", "映射页面", "检查 NX 保护",
                                 "切换 CR3", "退出码 0"};
  static ng_tnode_t nodes[7];
  INT32 W = screen->w;
  INT32 H = screen->h;
  INT32 tw = 660;
  INT32 th = 56;
  INT32 tx = (W - tw) / 2;
  INT32 ty = H - th - 10;
  INT32 mx = tx;
  INT32 my = ty - 8 - 560;
  INT32 qx = tx + tw - 340;
  INT32 qy = ty - 8 - 330;
  INT32 i;

  if (my < 8) {
    my = 8;
  }
  if (qy < 8) {
    qy = 8;
  }

  /* Window 1: renderer + settings. Hidden until the start menu opens it. */
  win = ng_create(ctx->root, NG_WINDOW, 40, 44, 520, 340);
  g_win_demo = win;
  ng_window(win, "显示与控件", 0);
  ng_label(ng_create(win, NG_LABEL, 16, 10, 480, 20), "2D 渲染与控件演示",
           ctx->theme.accent, NG_TEXT_SCALE);
  add_label(win, 16, 40, "内置点阵字体、裁剪与透明混合", ctx->theme.text_dim);
  ng_progress(ng_create(win, NG_PROGRESS, 16, 72, 488, 26), 720, 0);
  ng_button(ng_create(win, NG_BUTTON, 16, 112, 110, 34), "应用", on_apply);
  ng_button(ng_create(win, NG_BUTTON, 138, 112, 110, 34), "取消", on_cancel);
  ng_checkbox(ng_create(win, NG_CHECKBOX, 16, 160, 440, 26),
              "使用 NX 保护数据页", 1, on_nx);
  add_label(win, 16, 198, "滑块", ctx->theme.text_dim);
  ng_slider(ng_create(win, NG_SLIDER, 16, 218, 300, 30), 0, 100, 65,
            on_slider);
  g_slider_label = add_label(win, 330, 224, "65", ctx->theme.text);
  ng_label(ng_create(win, NG_LABEL, 16, 264, 480, 16),
           "拖动标题栏、点击控件、Tab 切换焦点", ctx->theme.text_dim, 1);

  /* Window 2: the text/list/tree half of the widget set. */
  win = ng_create(ctx->root, NG_WINDOW, 600, 64, 380, 440);
  g_win_widgets = win;
  ng_window(win, "控件", 1);
  win->on_action = on_close;
  add_label(win, 16, 8, "文本框", ctx->theme.text_dim);
  g_input[0] = 'h';
  g_input[1] = 'e';
  g_input[2] = 'l';
  g_input[3] = 'l';
  g_input[4] = 'o';
  g_input[5] = '\0';
  ng_textbox(ng_create(win, NG_TEXTBOX, 16, 32, 348, 32), g_input,
             sizeof(g_input), "在此输入");
  add_label(win, 16, 76, "列表", ctx->theme.text_dim);
  ng_list(ng_create(win, NG_LIST, 16, 100, 348, 132), items, 5, on_list);
  add_label(win, 16, 244, "树", ctx->theme.text_dim);

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

  /* Desktop: quiet watermark bottom-left, tile column on the right. */
  ng_label(ng_create(ctx->root, NG_LABEL, 28, H - 56, 260, 24), "Novus OS",
           ctx->theme.text_dim, 2);
  ng_label(ng_create(ctx->root, NG_LABEL, 30, H - 30, 260, 16),
           "Novus Fluent", ctx->theme.text_dim, 1);

  {
    static const char *names[6] = {"文件管理器", "任务管理器", "设置", "网络",
                                   "终端", "安装系统"};
    static const ng_icon_kind_t kinds[6] = {
        NG_ICON_FILES, NG_ICON_TASKS, NG_ICON_SETTINGS, NG_ICON_NET,
        NG_ICON_TERM, NG_ICON_INSTALL};
    static ng_action_fn acts[6] = {on_launch_files, on_launch_tasks,
                                   on_launch_settings, on_launch_net,
                                   on_launch_terminal, on_launch_install};
    UINT32 n = install_available() ? 6u : 5u;

    for (i = 0; i < (INT32)n; i++) {
      ng_button_icon(ng_create(ctx->root, NG_BUTTON, W - 112,
                               24 + i * 96, 96, 88),
                     names[i], kinds[i], acts[i]);
    }
  }

  /* Start menu: a floating card above the taskbar, hidden until asked. */
  g_start_menu = ng_panel(ng_create(ctx->root, NG_PANEL, mx, my, 480, 560),
                          ctx->theme.win_bg);
  ng_panel_frame(g_start_menu, 1);
  ng_set_overlay(g_start_menu, 1);
  ng_textbox(ng_create(g_start_menu, NG_TEXTBOX, 20, 18, 440, 34), g_search,
             sizeof(g_search), "搜索应用、设置与文件");
  ng_label(ng_create(g_start_menu, NG_LABEL, 20, 64, 200, 20), "已固定",
           ctx->theme.text, NG_TEXT_SCALE);
  ng_button_icon(ng_create(g_start_menu, NG_BUTTON, 20, 92, 136, 100),
                 "文件管理器", NG_ICON_FILES, on_launch_files);
  ng_button_icon(ng_create(g_start_menu, NG_BUTTON, 168, 92, 136, 100),
                 "任务管理器", NG_ICON_TASKS, on_launch_tasks);
  ng_button_icon(ng_create(g_start_menu, NG_BUTTON, 316, 92, 136, 100),
                 "设置", NG_ICON_SETTINGS, on_launch_settings);
  ng_button_icon(ng_create(g_start_menu, NG_BUTTON, 20, 202, 136, 100),
                 "网络", NG_ICON_NET, on_launch_net);
  ng_button_icon(ng_create(g_start_menu, NG_BUTTON, 168, 202, 136, 100),
                 "控件演示", NG_ICON_AUTO, on_open_demo);
  if (install_available()) {
    ng_button_icon(ng_create(g_start_menu, NG_BUTTON, 316, 202, 136, 100),
                   "安装系统", NG_ICON_INSTALL, on_launch_install);
  }

  ng_label(ng_create(g_start_menu, NG_LABEL, 20, 316, 200, 20), "推荐",
           ctx->theme.text, NG_TEXT_SCALE);
  ng_button(ng_create(g_start_menu, NG_BUTTON, 20, 344, 440, 42), "控件与列表",
            on_open_widgets);
  ng_button(ng_create(g_start_menu, NG_BUTTON, 20, 392, 440, 42), "控件演示",
            on_open_demo);
  ng_button(ng_create(g_start_menu, NG_BUTTON, 20, 440, 440, 42), "关于本系统",
            on_about);
  ng_button(ng_create(g_start_menu, NG_BUTTON, 20, 488, 210, 42), "终端",
            on_launch_terminal);

  {
    ng_widget_t *line = ng_panel(
        ng_create(g_start_menu, NG_PANEL, 20, 498, 440, 1), ctx->theme.win_border);
    (void)line;
  }
  ng_label(ng_create(g_start_menu, NG_LABEL, 20, 516, 220, 20),
           session_live() ? ND_L("Live 会话", "Live session") : "Novus 用户",
           ctx->theme.text_dim, 1);
  ng_icon(ng_create(g_start_menu, NG_ICON, 372, 508, 36, 36), NG_ICON_SETTINGS,
          18, on_launch_settings);
  ng_icon(ng_create(g_start_menu, NG_ICON, 416, 508, 36, 36), NG_ICON_POWER, 18,
          on_shutdown);

  /* Quick settings: theme, accent, wallpaper, network, power. */
  g_quick = ng_panel(ng_create(ctx->root, NG_PANEL, qx, qy, 340, 330),
                     ctx->theme.win_bg);
  ng_panel_frame(g_quick, 1);
  ng_set_overlay(g_quick, 1);
  ng_label(ng_create(g_quick, NG_LABEL, 20, 16, 240, 20), "快速设置",
           ctx->theme.text, NG_TEXT_SCALE);
  ng_label(ng_create(g_quick, NG_LABEL, 20, 54, 120, 16), "主题",
           ctx->theme.text_dim, 1);
  ng_button(ng_create(g_quick, NG_BUTTON, 20, 74, 92, 32), "浅色",
            on_mode_light);
  ng_button(ng_create(g_quick, NG_BUTTON, 120, 74, 92, 32), "深色",
            on_mode_dark);
  ng_button(ng_create(g_quick, NG_BUTTON, 220, 74, 100, 32), "跟随时间",
            on_mode_auto);
  ng_label(ng_create(g_quick, NG_LABEL, 20, 118, 120, 16), "强调色",
           ctx->theme.text_dim, 1);
  {
    ng_action_fn accent_acts[8] = {on_accent0, on_accent1, on_accent2, on_accent3,
                                   on_accent4, on_accent5, on_accent6, on_accent7};

    for (i = 0; i < 8; i++) {
      ng_swatch(ng_create(g_quick, NG_SWATCH, 20 + i * 38, 140, 32, 32),
                g_accents[i], nd_theme_accent() == g_accents[i],
                accent_acts[i]);
    }
  }
  ng_label(ng_create(g_quick, NG_LABEL, 20, 182, 120, 16), "壁纸",
           ctx->theme.text_dim, 1);
  {
    /* One swatch per embedded photo; the theme's paper colour stands in
     * for the image until it is decoded. */
    static ng_action_fn paper_acts[ND_WALLPAPER_COUNT] = {
      on_paper0, on_paper1, on_paper2, on_paper3,
      on_paper4, on_paper5, on_paper6
    };
    int pi;

    for (pi = 0; pi < ND_WALLPAPER_COUNT; pi++) {
      ng_swatch(ng_create(g_quick, NG_SWATCH, 20 + pi * 38, 204, 32, 32),
                nd_theme_paper_top(pi), (int)nd_theme_wallpaper() == pi,
                paper_acts[pi]);
    }
  }
  ng_label(ng_create(g_quick, NG_LABEL, 20, 252, 300, 16), "网络已连接",
           ctx->theme.text_dim, 1);
  ng_button(ng_create(g_quick, NG_BUTTON, 20, 282, 92, 32), "重启", on_reboot);
  ng_button(ng_create(g_quick, NG_BUTTON, 120, 282, 92, 32), "关机",
            on_shutdown);
  ng_icon(ng_create(g_quick, NG_ICON, 272, 278, 36, 36), NG_ICON_POWER, 18,
          on_shutdown);

  /* Taskbar: one floating pill. Centre apps, clock and quick settings on
   * the right, search on the left. Running dots sit under their app. */
  {
    ng_widget_t *bar = ng_panel(ng_create(ctx->root, NG_PANEL, tx, ty, tw, th),
                                ctx->theme.win_bg);

    ng_panel_frame(bar, 1);
    ng_icon(ng_create(ctx->root, NG_ICON, tx + 8, ty + 6, 44, 44),
            NG_ICON_START, 22, on_start);
    ng_icon(ng_create(ctx->root, NG_ICON, tx + 56, ty + 6, 44, 44),
            NG_ICON_SEARCH, 18, on_start);

    {
      static const ng_icon_kind_t kinds[5] = {
          NG_ICON_FILES, NG_ICON_TASKS, NG_ICON_SETTINGS, NG_ICON_NET,
          NG_ICON_TERM};
      ng_action_fn acts[5] = {on_launch_files, on_launch_tasks,
                              on_launch_settings, on_launch_net,
                              on_launch_terminal};

      for (i = 0; i < 5; i++) {
        ng_icon(ng_create(ctx->root, NG_ICON, tx + 116 + i * 46, ty + 6, 44, 44),
                kinds[i], 20, acts[i]);
        if (i < 5) {
          g_run_dot[i] = ng_panel(
              ng_create(ctx->root, NG_PANEL, tx + 116 + i * 46 + 14, ty + th - 7,
                        16, 3),
              nd_theme_accent());
        }
      }
    }

    g_clock_label = ng_label(
        ng_create(ctx->root, NG_LABEL, tx + tw - 178, ty + 10, 120, 36),
        g_clock, ctx->theme.text, 2);
    g_clock_label->u.label.align = 2; /* right aligned */
    ng_icon(ng_create(ctx->root, NG_ICON, tx + tw - 50, ty + 6, 44, 44),
            NG_ICON_AUTO, 20, on_quick);
  }

  /* Status line, bottom left, small and quiet. */
  set_status("就绪");
  g_status_label = ng_label(
      ng_create(ctx->root, NG_LABEL, 28, H - 92, 620, 20), g_status,
      ctx->theme.text_dim, 1);

  ng_layout();
  clock_refresh();
  running_refresh();
  ng_show(g_win_demo, 0);
  ng_show(g_win_widgets, 0);
  ng_show(g_start_menu, 0);
  ng_show(g_quick, 0);
}

void nd_demo_init(void) {
  nd_surface_t *screen = nd_screen();
  nd_surface_t *frame = nd_canvas();
  ng_ctx_t     *ctx;

  g_demo_ready = 0;
  if (screen == 0) {
    return;
  }
  /* Decode the wallpaper for this screen size before the first frame; a
   * failure just leaves the gradient in place. */
  nd_wallpaper_init(screen->w, screen->h);
  nd_input_init();
  /* The desktop is composed into the off-screen buffer, not painted onto
   * the glass: the boot splash stays visible until the first frame is
   * complete. */
  ctx = ng_init(frame);
  if (ctx == 0) {
    kputs("[ND] demo: GUI init failed\n");
    return;
  }
  nd_ime_init();
  build_desktop(screen, ctx);
  ng_render_all();
  nd_cursor_set(screen->w / 2, screen->h / 3);
  nd_cursor_draw();
  g_demo_ready = 1;
  g_theme_seen = nd_theme_version();
  g_lang_seen = nd_lang_version();

  /* One present for the whole desktop: the boot splash is replaced by a
   * finished picture, never by a half-built one. */
  nd_boot_splash_end();
  nd_present(nd_rect(0, 0, screen->w, screen->h));
  kputs("[ND] demo desktop painted\n");
}

/* Rebuild the shell after a light/dark or accent change. Client windows
 * belong to the WM and survive; the shell's demo windows are recreated
 * hidden. The widget pool has no per-widget repaint hook, so rebuilding is
 * both simpler and more predictable than walking 80 widgets to recolour
 * them. */
void nd_demo_theme_sync(void) {
  nd_surface_t *canvas = nd_canvas();
  ng_ctx_t     *ctx;

  if (!g_demo_ready || canvas == 0) {
    return;
  }
  g_win_demo = 0;
  g_win_widgets = 0;
  g_start_menu = 0;
  g_quick = 0;
  g_clock_label = 0;
  g_status_label = 0;
  memset(g_run_dot, 0, sizeof(g_run_dot));

  /* Rebuild into the compositor buffer, never onto the framebuffer: a
   * theme change repaints every widget, and doing that on the glass is
   * precisely the flicker the off-screen path exists to prevent. */
  ctx = ng_init(canvas);
  if (ctx == 0) {
    g_demo_ready = 0;
    return;
  }
  nd_ime_init();
  build_desktop(canvas, ctx);
  ng_render_all();
  g_theme_seen = nd_theme_version();
  g_lang_seen = nd_lang_version();
  kputs("[ND] theme applied: ");
  kputs(nd_theme_is_dark() ? "dark\n" : "light\n");
}

int nd_demo_ready(void) { return g_demo_ready; }

void nd_demo_dispatch(const nd_event_t *ev) {
  /* A press anywhere outside an open card closes it, the way it does on
   * Windows - including a press on a window the card was covering. */
  if (ev->type == ND_EV_MOUSE_DOWN) {
    if (g_start_menu != 0 &&
        (g_start_menu->flags & NG_WF_VISIBLE) != 0u &&
        !nd_rect_contains(g_start_menu->abs, ev->x, ev->y)) {
      ng_show(g_start_menu, 0);
    }
    if (g_quick != 0 && (g_quick->flags & NG_WF_VISIBLE) != 0u &&
        !nd_rect_contains(g_quick->abs, ev->x, ev->y)) {
      ng_show(g_quick, 0);
    }
  }
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

/* Input bytes are decoded into events before this loop; damage is composed
 * and presented only after queued events have updated widget state.
 *
 * The pointer contributes damage of its own (the rectangle it left and the
 * one it moved into). Merging that into the widget damage is what erases
 * the old arrow: it is repainted from the scene, not patched over. */
void nd_demo_tick(void) {
  nd_rect_t  d;
  nd_rect_t  cd;
  int        have = 0;
  nd_event_t ev;

  if (!g_demo_ready) {
    return;
  }
  nd_input_poll();
  while (nd_input_pop(&ev) != 0) {
    if (ev.type == ND_EV_MOUSE_MOVE) {
      nd_cursor_set(ev.x, ev.y);
    }
    nd_demo_dispatch(&ev);
  }
  if (nd_demo_take_damage(&d)) {
    have = 1;
  }
  if (nd_cursor_take_damage(&cd)) {
    d = have ? nd_rect_union(d, cd) : cd;
    have = 1;
  }
  if (have) {
    d = nd_rect_intersect(d, nd_rect(0, 0, nd_canvas()->w, nd_canvas()->h));
    if (!nd_rect_empty(d)) {
      nd_demo_paint(d);
      nd_present(d);
    }
  }
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
