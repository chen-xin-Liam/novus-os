/**
 * Novus OS - Novus-WM: windows, z-order, focus, input routing, compositor
 * and the IPC server side of the protocol (M6).
 *
 * Layering on screen, bottom to top:
 *
 *   desktop gradient + the M5 shell scene   (nvgui, drawn by nd_demo)
 *   WM windows                              (this file)
 *   mouse pointer                           (nd_hal save-under)
 *
 * Everything the WM draws goes through nd_surface_set_clip(), so a window
 * can never paint outside its own rectangle: that is README 10.4's
 * "窗口裁剪，避免窗口越界渲染" done at the compositor instead of trusting
 * clients.
 *
 * Windows are drawn from a command list the owner sent with NWM_REQ_DRAW,
 * not from client memory. A client that exits therefore leaves nothing
 * dangling; its window is closed by the owner-liveness sweep.
 */

#include "kernel.h"
#include "ipc.h"
#include "sched.h"
#include "nd.h"
#include "nd_theme.h"
#include "nd_input.h"
#include "nvgui.h"
#include "nwm.h"
#include "nwm_proto.h"

#if CONFIG_NOVUS_WM

#define NWM_SHADOW   14
#define NWM_MIN_W    120
#define NWM_MIN_H    48
/* Windows 11 caption buttons: 46x32 hit areas, flush with the top-right
 * corner, no gaps - the hover highlight is the only affordance. */
#define NWM_BTN      46
#define NWM_BTN_GAP  0

typedef struct {
  UINT8     used;
  UINT32    id;
  UINT64    owner;
  char      title[NWM_TITLE_MAX];
  nd_rect_t rect;  /* frame rect, title bar included */
  UINT8     state; /* 0 normal, 1 minimized, 2 maximized */
  UINT8     alpha; /* content effect: 255 opaque .. 0 invisible */
  UINT32    draw_n;
  nwm_cmd_t draw[NWM_MAX_CMDS];
  /* v1.1 content list: shapes and text runs a client appends one message
   * at a time (nwm_proto.h NWM_REQ_CLEAR/SHAPE/TEXT). Kept per window so
   * the compositor can replay it after any repaint, which is what makes
   * damage-driven redraw work at all. */
  UINT32      shape_n;
  nwm_shape_t shape[NWM_MAX_SHAPES];
  UINT32      text_n;
  nwm_text_t  text[NWM_MAX_TEXTS];
  nd_rect_t restore; /* geometry to come back to from maximized */
} nwm_window_t;

static nwm_window_t g_wins[NWM_MAX_WINDOWS];
static UINT8     g_z[NWM_MAX_WINDOWS]; /* window indices, bottom -> top */
static UINT32    g_zcount;
static UINT64    g_port;      /* the "nwm" port id */
static INT32     g_mouse_x;   /* last pointer position, for hover states */
static INT32     g_mouse_y;
static UINT32    g_next_id;
static int       g_ready;
static int       g_focus;   /* index, -1 = the shell */
static int       g_drag;    /* index being dragged by its title bar */
static int       g_capture; /* index that received the press */
static INT32     g_drag_dx;
static INT32     g_drag_dy;
static nd_rect_t g_damage;
static UINT32    g_requests;
static UINT32    g_events;
static UINT32    g_dropped;
static UINT32    g_liveness_tick;
static UINT32    g_next_stats;
static UINT32    g_full_log;

/* ---- small helpers ---------------------------------------------------- */

static nd_surface_t *screen(void) { return nd_screen(); }

/* Composite target. Painting straight into the framebuffer forces the
 * scene into two passes - the shell first, which covers any window in the
 * damage rectangle, then the window again - and the screen shows that
 * intermediate state for as long as the second pass takes. With a back
 * buffer the damage rectangle is composed off-screen and copied in one
 * blit, so the visible screen only ever holds finished frames. */
static nd_surface_t g_frame;
static int          g_frame_ok;

static nd_surface_t *target(void) {
  return g_frame_ok ? &g_frame : nd_screen();
}

static void damage(nd_rect_t r) { g_damage = nd_rect_union(g_damage, r); }

/* A window's shadow reaches NWM_SHADOW pixels past the frame, so the
 * pixels it owns are not the pixels it repaints (same rule as nvgui). */
static void damage_frame(nd_rect_t r) {
  damage(nd_rect(r.x - NWM_SHADOW, r.y - NWM_SHADOW + 2,
                 r.w + NWM_SHADOW * 2, r.h + NWM_SHADOW * 2));
}

static int win_index(UINT32 id) {
  UINT32 i;

  for (i = 0; i < NWM_MAX_WINDOWS; i++) {
    if (g_wins[i].used && g_wins[i].id == id) {
      return (int)i;
    }
  }
  return -1;
}

static nwm_window_t *win_get(UINT32 id) {
  int i = win_index(id);

  return (i >= 0) ? &g_wins[i] : 0;
}

static nd_rect_t client_rect(const nwm_window_t *w) {
  return nd_rect(w->rect.x + 1, w->rect.y + NWM_TITLE_H, w->rect.w - 2,
                 w->rect.h - NWM_TITLE_H - 1);
}

static void z_take_out(int idx) {
  UINT32 i, j = 0;

  for (i = 0; i < g_zcount; i++) {
    if ((int)g_z[i] != idx) {
      g_z[j++] = g_z[i];
    }
  }
  g_zcount = j;
}

static void z_raise(int idx) {
  z_take_out(idx);
  if (g_zcount < NWM_MAX_WINDOWS) {
    g_z[g_zcount++] = (UINT8)idx;
  }
}

/* Topmost window under the point, or -1. Minimized windows are out of
 * the picture entirely, which is what "minimized" means here. */
static int hit_test(INT32 x, INT32 y) {
  UINT32 i;

  for (i = g_zcount; i > 0u; i--) {
    nwm_window_t *w = &g_wins[g_z[i - 1u]];

    if (w->used && w->state != 1u && nd_rect_contains(w->rect, x, y)) {
      return (int)g_z[i - 1u];
    }
  }
  return -1;
}

static UINT32 g_evt_log;

static void deliver(UINT64 owner, UINT16 type, const void *payload, UINT32 len) {
  int rc;

  if (owner == NWM_PID_SHELL) {
    return; /* the shell is dispatched inline, in absolute coordinates */
  }
  rc = ipc_send(owner, NWM_PID_SHELL, type, IPC_F_REPLY, payload, len);
  if (rc == IPC_OK) {
    g_events++;
    if (g_evt_log < 12u) {
      g_evt_log++;
      kputs("[NWM] event ");
      kputs_hex(type);
      kputs(" -> pid ");
      kputs_hex(owner);
      kputs("\n");
    }
  } else {
    g_dropped++;
  }
}

static void reply(UINT64 to, UINT32 win, INT32 status) {
  nwm_rsp_t rsp;

  memset(&rsp, 0, sizeof(rsp));
  rsp.win = win;
  rsp.status = status;
  (void)ipc_send(to, NWM_PID_SHELL,
                 (status == NWM_OK) ? NWM_RSP_OK : NWM_RSP_ERR, IPC_F_REPLY,
                 &rsp, sizeof(rsp));
}

/* Asynchronous requests get no acknowledgement. A client that only reads
 * its inbox for events would otherwise find it full of replies to its own
 * MOVEs - the queue is bounded, and the events would be the messages that
 * get dropped. CREATE must answer (the window id is the result), and an
 * error is always reported so a buggy client can see it. */
static void ack(const ipc_msg_t *m, UINT32 win, INT32 status) {
  if (status != NWM_OK || m->hdr.type == NWM_REQ_CREATE ||
      (m->hdr.flags & IPC_F_SYNC) != 0u) {
    reply(m->hdr.sender, win, status);
  }
}

/* Focus is stored as a window-array index (the public id is only for events).
 * A transition sends focus events to eligible owners and updates the
 * scheduler's foreground weight; -1 selects the shell and clears that weight. */
static void focus_set(int idx) {
  nwm_ev_focus_t ev;
  int           old = g_focus;

  if (old == idx) {
    return;
  }
  if (old >= 0 && g_wins[old].used) {
    memset(&ev, 0, sizeof(ev));
    ev.win = g_wins[old].id;
    ev.focused = 0;
    deliver(g_wins[old].owner, NWM_EV_FOCUS, &ev, sizeof(ev));
    damage(g_wins[old].rect);
  }
  g_focus = idx;
  (void)sched_set_foreground(idx >= 0 && g_wins[idx].owner != NWM_PID_SHELL
                                 ? g_wins[idx].owner : SCHED_FOREGROUND_NONE);
  if (idx >= 0) {
    memset(&ev, 0, sizeof(ev));
    ev.win = g_wins[idx].id;
    ev.focused = 1;
    deliver(g_wins[idx].owner, NWM_EV_FOCUS, &ev, sizeof(ev));
    damage(g_wins[idx].rect);
  }
}

static void clamp_rect(nd_rect_t *r) {
  nd_surface_t *s = screen();
  INT32         sw = (s != 0) ? s->w : 1024;
  INT32         sh = (s != 0) ? s->h : 768;

  if (r->w < NWM_MIN_W) {
    r->w = NWM_MIN_W;
  }
  if (r->h < NWM_MIN_H) {
    r->h = NWM_MIN_H;
  }
  if (r->w > sw) {
    r->w = sw;
  }
  if (r->h > sh) {
    r->h = sh;
  }
  if (r->x < 0) {
    r->x = 0;
  }
  if (r->y < 0) {
    r->y = 0;
  }
  if (r->x + r->w > sw) {
    r->x = sw - r->w;
  }
  if (r->y + r->h > sh) {
    r->y = sh - r->h;
  }
}

/* ---- compositor ------------------------------------------------------- */

/* One drawing primitive. Both display-list formats (the M6 command
 * array and the v1.1 shape list) end up here, so a client of either
 * version draws identically and there is one place to add an op. */
static void paint_shape(nd_rect_t client, UINT8 op, INT32 x, INT32 y, INT32 w,
                        INT32 h, UINT32 color, INT32 thickness, INT32 radius) {
  nd_surface_t *s = target();
  nd_rect_t     r = nd_rect(client.x + x, client.y + y, w, h);

  switch (op) {
  case NWM_OP_FILL:
    nd_fill_rect(s, r, color);
    break;
  case NWM_OP_FRAME:
    nd_frame(s, r, (thickness > 0) ? thickness : 1, color);
    break;
  case NWM_OP_HLINE:
    nd_hline(s, r.x, r.x + r.w, r.y, color);
    break;
  case NWM_OP_VLINE:
    nd_vline(s, r.x, r.y, r.y + r.h, color);
    break;
  case NWM_OP_CIRCLE:
    nd_circle(s, r.x, r.y, (w > 0) ? w : 1, color);
    break;
  case NWM_OP_ROUND_FILL:
    nd_round_rect(s, r, (radius > 0) ? radius : 1, color);
    break;
  case NWM_OP_ROUND_FRAME:
    nd_round_frame(s, r, (radius > 0) ? radius : 1,
                   (thickness > 0) ? thickness : 1, color);
    break;
  default:
    break; /* forward compatible: unknown ops are ignored */
  }
}

static void paint_commands(nwm_window_t *w, nd_rect_t client) {
  UINT32 i;

  for (i = 0; i < w->draw_n; i++) {
    const nwm_cmd_t *c = &w->draw[i];

    paint_shape(client, c->op, c->x, c->y, c->w, c->h, c->color,
                (c->pad2 > 0u) ? (INT32)c->pad2 : 1, 0);
  }
}

static UINT8 title_button(const nwm_window_t *w, INT32 x, INT32 y);
static INT32 caption_hover(const nwm_window_t *w);

static void paint_window(nwm_window_t *w, nd_rect_t clip) {
  nd_surface_t     *s = target();
  const ng_theme_t *t;
  nd_rect_t         vis;
  nd_rect_t         client;
  nd_color_t        title_bg;
  UINT32            i;

  if (w->state == 1u) {
    return; /* minimized */
  }
  vis = nd_rect_intersect(w->rect, clip);
  if (nd_rect_empty(vis)) {
    return;
  }
  t = &ng_ctx_get()->theme;
  nd_surface_set_clip(s, vis);

  nd_shadow(s, w->rect, NWM_SHADOW, 42);
  nd_round_rect(s, w->rect, 10, t->win_bg);

  /* Title band clipped to the rounded top: fill the corner rows with the
   * curve inset, exactly like the shell's own windows. */
  title_bg = (g_focus >= 0 && &g_wins[g_focus] == w) ? t->win_title
                                                     : t->win_title_dim;
  {
    static const INT32 corner[10] = {10, 7, 5, 4, 3, 2, 2, 1, 1, 1};
    INT32              k;

    for (k = 0; k < NWM_TITLE_H && k < w->rect.h; k++) {
      INT32 inset = (k < 10) ? corner[k] : 0;

      nd_fill_rect(s, nd_rect(w->rect.x + inset, w->rect.y + k,
                              w->rect.w - inset * 2, 1),
                   title_bg);
    }
  }
  nd_hline(s, w->rect.x + 1, w->rect.x + w->rect.w - 2,
           w->rect.y + NWM_TITLE_H - 1, t->win_border);
  nd_round_frame(s, w->rect, 10, 1, t->win_border);
  nd_text(s, w->rect.x + 12,
          w->rect.y + (NWM_TITLE_H - (INT32)nd_text_height(w->title, 2)) / 2,
          w->title, t->text, 2);

  /* Caption buttons, drawn by the same helper the shell windows use. */
  ng_draw_caption(s, w->rect, t, caption_hover(w), 3);

  (void)i;

  /* Client area. The clip narrows to it, so no command can spill into the
   * decorations or the window next door. */
  client = client_rect(w);
  nd_surface_set_clip(s, nd_rect_intersect(clip, client));
  nd_fill_rect(s, client, t->win_bg);
  paint_commands(w, client);
  {
    UINT32 k;

    for (k = 0; k < w->shape_n; k++) {
      const nwm_shape_t *sh = &w->shape[k];

      paint_shape(client, sh->op, sh->x, sh->y, sh->w, sh->h, sh->color,
                  (sh->op == NWM_OP_ROUND_FRAME) ? 1 : 1, (INT32)sh->pad);
    }
    for (k = 0; k < w->text_n; k++) {
      const nwm_text_t *tx = &w->text[k];

      nd_text(s, client.x + tx->x, client.y + tx->y, tx->text, tx->color,
              (tx->scale != 0u) ? tx->scale : 1u);
    }
  }
  if (w->alpha < 255u) {
    /* Fade effect: the content is dimmed towards the background. */
    nd_blend_rect(s, client, ND_RGB(0, 0, 0), (UINT8)(255u - w->alpha));
  }
  nd_surface_set_clip(s, vis);
}

static void paint(nd_rect_t clip) {
  nd_surface_t *dst = target();
  nd_surface_t *scr = screen();
  UINT32        i;

  if (dst == 0 || scr == 0) {
    return;
  }
  /* Shell underlay first: it owns the desktop gradient and the M5 scene. */
  nd_demo_paint(clip);

  nd_surface_set_clip(dst, nd_rect_intersect(clip, nd_rect(0, 0, dst->w, dst->h)));
  for (i = 0; i < g_zcount; i++) {
    paint_window(&g_wins[g_z[i]], clip);
  }
  /* The shell's overlay layer (start menu, IME popup) goes above the
   * client windows, the way a start menu does on a real desktop. */
  {
    nd_surface_t *ov = target();

    if (ov != 0) {
      ng_render_overlay(clip);
    }
  }
  nd_surface_reset_clip(dst);

  if (g_frame_ok) {
    nd_surface_set_clip(scr, clip);
    nd_blit(scr, clip.x, clip.y, dst, clip);
    nd_surface_reset_clip(scr);
  }
  /* A second display shows the same picture: copy what was just drawn. */
  nd_mirror_rect(clip);
}

/* ---- protocol --------------------------------------------------------- */

static void log_request(const char *what, UINT32 win, UINT64 from) {
  if (g_requests > 8u) {
    return; /* one line per request would flood COM1 once a client polls */
  }
  kputs("[NWM] ");
  kputs(what);
  kputs(" win=");
  kputs_hex(win);
  kputs(" from pid=");
  kputs_hex(from);
  kputs("\n");
}

static void req_create(const ipc_msg_t *m) {
  nwm_create_t c;
  nwm_window_t   *w = 0;
  UINT32       i;

  if (m->hdr.len < sizeof(c)) {
    ack(m, 0, NWM_EBADPAY);
    return;
  }
  memcpy(&c, m->payload, sizeof(c));
  for (i = 0; i < NWM_MAX_WINDOWS; i++) {
    if (!g_wins[i].used) {
      w = &g_wins[i];
      break;
    }
  }
  if (w == 0) {
    ack(m, 0, NWM_EFULL);
    return;
  }
  memset(w, 0, sizeof(*w));
  w->used = 1;
  w->id = (c.win != 0u) ? c.win : g_next_id++;
  if (w->id >= g_next_id) {
    g_next_id = w->id + 1u;
  }
  w->owner = m->hdr.sender;
  w->alpha = 255u;
  w->rect = nd_rect(c.x, c.y, c.w, c.h);
  clamp_rect(&w->rect);
  for (i = 0; i + 1u < NWM_TITLE_MAX && c.title[i] != '\0'; i++) {
    w->title[i] = c.title[i];
  }
  w->title[i] = '\0';
  z_raise((int)(w - g_wins));
  /* A new window takes focus: the client asked to be seen, and the shell
   * keeping the keyboard would make the first keystroke disappear. */
  focus_set((int)(w - g_wins));
  damage_frame(w->rect);
  log_request("create", w->id, m->hdr.sender);
  ack(m, w->id, NWM_OK);
}

static int owned_by(nwm_window_t *w, UINT64 pid, int *status) {
  /* The shell may steer any window (it is the compositor's own client);
   * everybody else only their own. That is the CAP check of README part 8
   * at the granularity M6 has: object ownership. */
  if (w == 0) {
    *status = NWM_EBADWIN;
    return 0;
  }
  if (pid != NWM_PID_SHELL && w->owner != pid) {
    *status = NWM_EDENIED;
    return 0;
  }
  return 1;
}

static void req_destroy(const ipc_msg_t *m) {
  nwm_window_t *w;
  int        st = NWM_OK;
  UINT32     id = 0;

  if (m->hdr.len < 4u) {
    ack(m, 0, NWM_EBADPAY);
    return;
  }
  memcpy(&id, m->payload, 4);
  w = win_get(id);
  if (!owned_by(w, m->hdr.sender, &st)) {
    ack(m, id, st);
    return;
  }
  if (g_focus == (int)(w - g_wins)) {
    focus_set(-1);
  }
  if (g_drag == (int)(w - g_wins)) {
    g_drag = -1;
  }
  if (g_capture == (int)(w - g_wins)) {
    g_capture = -1;
  }
  z_take_out((int)(w - g_wins));
  damage_frame(w->rect);
  memset(w, 0, sizeof(*w));
  log_request("destroy", id, m->hdr.sender);
  ack(m, id, NWM_OK);
}

static void req_move(const ipc_msg_t *m) {
  nwm_move_t mv;
  nwm_window_t *w;
  int        st = NWM_OK;

  if (m->hdr.len < sizeof(mv)) {
    ack(m, 0, NWM_EBADPAY);
    return;
  }
  memcpy(&mv, m->payload, sizeof(mv));
  w = win_get(mv.win);
  if (!owned_by(w, m->hdr.sender, &st)) {
    ack(m, mv.win, st);
    return;
  }
  damage_frame(w->rect);
  w->rect.x = mv.x;
  w->rect.y = mv.y;
  clamp_rect(&w->rect);
  damage_frame(w->rect);
  ack(m, w->id, NWM_OK);
}

static void req_resize(const ipc_msg_t *m) {
  nwm_resize_t rs;
  nwm_window_t   *w;
  nwm_ev_resize_t ev;
  int          st = NWM_OK;

  if (m->hdr.len < sizeof(rs)) {
    ack(m, 0, NWM_EBADPAY);
    return;
  }
  memcpy(&rs, m->payload, sizeof(rs));
  w = win_get(rs.win);
  if (!owned_by(w, m->hdr.sender, &st)) {
    ack(m, rs.win, st);
    return;
  }
  damage_frame(w->rect);
  w->rect.w = rs.w;
  w->rect.h = rs.h;
  clamp_rect(&w->rect);
  damage_frame(w->rect);
  memset(&ev, 0, sizeof(ev));
  ev.win = w->id;
  ev.w = client_rect(w).w;
  ev.h = client_rect(w).h;
  deliver(w->owner, NWM_EV_RESIZE, &ev, sizeof(ev));
  ack(m, w->id, NWM_OK);
}

static void req_raise(const ipc_msg_t *m) {
  UINT32     id = 0;
  nwm_window_t *w;
  int        st = NWM_OK;

  if (m->hdr.len < 4u) {
    ack(m, 0, NWM_EBADPAY);
    return;
  }
  memcpy(&id, m->payload, 4);
  w = win_get(id);
  if (!owned_by(w, m->hdr.sender, &st)) {
    ack(m, id, st);
    return;
  }
  z_raise((int)(w - g_wins));
  damage_frame(w->rect);
  ack(m, id, NWM_OK);
}

/* Minimize / maximize / restore share their body; `mode` picks one. */
static void req_state(const ipc_msg_t *m, UINT8 mode) {
  UINT32     id = 0;
  nwm_window_t *w;
  int        st = NWM_OK;

  if (m->hdr.len < 4u) {
    ack(m, 0, NWM_EBADPAY);
    return;
  }
  memcpy(&id, m->payload, 4);
  w = win_get(id);
  if (!owned_by(w, m->hdr.sender, &st)) {
    ack(m, id, st);
    return;
  }
  damage_frame(w->rect);
  if (mode == 1u) {
    w->state = (w->state == 1u) ? 0u : 1u;
    if (g_focus == (int)(w - g_wins)) {
      focus_set(-1);
    }
  } else if (mode == 2u) {
    if (w->state == 2u) {
      w->rect = w->restore;
      w->state = 0;
    } else {
      nd_surface_t *s = screen();

      w->restore = w->rect;
      w->state = 2;
      w->rect = nd_rect(0, 0, (s != 0) ? s->w : 1024, (s != 0) ? s->h : 768);
    }
  } else {
    w->state = 0;
  }
  damage_frame(w->rect);
  ack(m, id, NWM_OK);
}

static void req_focus(const ipc_msg_t *m) {
  UINT32     id = 0;
  nwm_window_t *w;
  int        st = NWM_OK;

  if (m->hdr.len < 4u) {
    ack(m, 0, NWM_EBADPAY);
    return;
  }
  memcpy(&id, m->payload, 4);
  w = win_get(id);
  if (!owned_by(w, m->hdr.sender, &st)) {
    ack(m, id, st);
    return;
  }
  z_raise((int)(w - g_wins));
  if (w->state == 1u) {
    w->state = 0;
    damage_frame(w->rect);
  }
  focus_set((int)(w - g_wins));
  ack(m, id, NWM_OK);
}

static void req_draw(const ipc_msg_t *m) {
  nwm_draw_t d;
  nwm_window_t *w;
  int        st = NWM_OK;
  UINT32     n;

  if (m->hdr.len < 8u) {
    ack(m, 0, NWM_EBADPAY);
    return;
  }
  memcpy(&d, m->payload, sizeof(d));
  w = win_get(d.win);
  if (!owned_by(w, m->hdr.sender, &st)) {
    ack(m, d.win, st);
    return;
  }
  n = d.count;
  if (n > NWM_MAX_CMDS) {
    n = NWM_MAX_CMDS;
  }
  if (m->hdr.len < 8u + n * sizeof(nwm_cmd_t)) {
    ack(m, d.win, NWM_EBADPAY);
    return;
  }
  memcpy(d.cmd, m->payload + 8, n * sizeof(nwm_cmd_t));
  w->draw_n = n;
  memcpy(w->draw, d.cmd, n * sizeof(nwm_cmd_t));
  damage(client_rect(w));
  ack(m, w->id, NWM_OK);
}

/* ---- v1.1 content list ------------------------------------------------ */

static void req_clear(const ipc_msg_t *m) {
  nwm_win_t     r;
  nwm_window_t *w;
  int           st = NWM_OK;

  if (m->hdr.len < sizeof(r)) {
    ack(m, 0, NWM_EBADPAY);
    return;
  }
  memcpy(&r, m->payload, sizeof(r));
  w = win_get(r.win);
  if (!owned_by(w, m->hdr.sender, &st)) {
    ack(m, r.win, st);
    return;
  }
  w->draw_n = 0;
  w->shape_n = 0;
  w->text_n = 0;
  /* No damage here: the list is empty until the client finishes the frame,
   * and repainting now would show a blank window. INVALIDATE is the end of
   * a frame and does the damage. */
  ack(m, w->id, NWM_OK);
}

static void req_shape(const ipc_msg_t *m) {
  nwm_shape_t   s;
  nwm_window_t *w;
  int           st = NWM_OK;

  if (m->hdr.len < sizeof(s)) {
    ack(m, 0, NWM_EBADPAY);
    return;
  }
  memcpy(&s, m->payload, sizeof(s));
  w = win_get(s.win);
  if (!owned_by(w, m->hdr.sender, &st)) {
    ack(m, s.win, st);
    return;
  }
  if (w->shape_n >= NWM_MAX_SHAPES) {
    ack(m, w->id, NWM_EFULL);
    return;
  }
  w->shape[w->shape_n++] = s;
  /* Stored, not painted: a frame is painted as a whole when the client
   * says INVALIDATE, so a half-delivered list never reaches the screen. */
  ack(m, w->id, NWM_OK);
}

static void req_text(const ipc_msg_t *m) {
  nwm_text_t    t;
  nwm_window_t *w;
  int           st = NWM_OK;
  UINT32        len;

  if (m->hdr.len < sizeof(t)) {
    ack(m, 0, NWM_EBADPAY);
    return;
  }
  memcpy(&t, m->payload, sizeof(t));
  w = win_get(t.win);
  if (!owned_by(w, m->hdr.sender, &st)) {
    ack(m, t.win, st);
    return;
  }
  if (w->text_n >= NWM_MAX_TEXTS) {
    if (g_full_log < 8u) {
      g_full_log++;
      kputs("[NWM] WARN display list full (text) win=");
      kputs_hex(w->id);
      kputs("\n");
    }
    ack(m, w->id, NWM_EFULL);
    return;
  }
  /* The run is a byte string, not a C string: trust the length, cap it,
   * and terminate the copy so a client cannot run off the array. */
  len = t.len;
  if (len >= NWM_TEXT_MAX) {
    len = NWM_TEXT_MAX - 1u;
  }
  t.text[len] = '\0';
  t.len = (UINT8)len;
  w->text[w->text_n++] = t;
  /* Stored, not painted: see req_shape. */
  ack(m, w->id, NWM_OK);
}

/* Repaint the whole client from the list. The owner check is the same as
 * every other op: a window can only invalidate itself. */
static void req_invalidate(const ipc_msg_t *m) {
  nwm_win_t     r;
  nwm_window_t *w;
  int           st = NWM_OK;

  if (m->hdr.len < sizeof(r)) {
    ack(m, 0, NWM_EBADPAY);
    return;
  }
  memcpy(&r, m->payload, sizeof(r));
  w = win_get(r.win);
  if (!owned_by(w, m->hdr.sender, &st)) {
    ack(m, r.win, st);
    return;
  }
  damage(client_rect(w));
  ack(m, w->id, NWM_OK);
}

static void req_title(const ipc_msg_t *m) {
  nwm_title_t t;
  nwm_window_t  *w;
  int         st = NWM_OK;

  if (m->hdr.len < sizeof(t)) {
    ack(m, 0, NWM_EBADPAY);
    return;
  }
  memcpy(&t, m->payload, sizeof(t));
  w = win_get(t.win);
  if (!owned_by(w, m->hdr.sender, &st)) {
    ack(m, t.win, st);
    return;
  }
  memset(w->title, 0, sizeof(w->title));
  memcpy(w->title, t.title, NWM_TITLE_MAX - 1u);
  damage_frame(nd_rect(w->rect.x, w->rect.y, w->rect.w, NWM_TITLE_H));
  ack(m, w->id, NWM_OK);
}

static void req_effect(const ipc_msg_t *m) {
  nwm_effect_t e;
  nwm_window_t   *w;
  int          st = NWM_OK;

  if (m->hdr.len < sizeof(e)) {
    ack(m, 0, NWM_EBADPAY);
    return;
  }
  memcpy(&e, m->payload, sizeof(e));
  w = win_get(e.win);
  if (!owned_by(w, m->hdr.sender, &st)) {
    ack(m, e.win, st);
    return;
  }
  w->alpha = e.alpha;
  damage(client_rect(w));
  ack(m, w->id, NWM_OK);
}

static void handle_request(const ipc_msg_t *m) {
  g_requests++;

  switch (m->hdr.type) {
  case NWM_REQ_CREATE:
    req_create(m);
    break;
  case NWM_REQ_DESTROY:
    req_destroy(m);
    break;
  case NWM_REQ_MOVE:
    req_move(m);
    break;
  case NWM_REQ_RESIZE:
    req_resize(m);
    break;
  case NWM_REQ_RAISE:
    req_raise(m);
    break;
  case NWM_REQ_MINIMIZE:
  case NWM_REQ_MAXIMIZE:
  case NWM_REQ_RESTORE:
    req_state(m, (m->hdr.type == NWM_REQ_MINIMIZE) ? 1u
                   : (m->hdr.type == NWM_REQ_MAXIMIZE) ? 2u
                                                       : 0u);
    break;
  case NWM_REQ_FOCUS:
    req_focus(m);
    break;
  case NWM_REQ_DRAW:
    req_draw(m);
    break;
  case NWM_REQ_TITLE:
    req_title(m);
    break;
  case NWM_REQ_EFFECT:
    req_effect(m);
    break;
  case NWM_REQ_CLEAR:
    req_clear(m);
    break;
  case NWM_REQ_SHAPE:
    req_shape(m);
    break;
  case NWM_REQ_TEXT:
    req_text(m);
    break;
  case NWM_REQ_INVALIDATE:
    req_invalidate(m);
    break;
  default:
    ack(m, 0, NWM_EBADREQ);
    break;
  }
}

/* ---- input routing ---------------------------------------------------- */

static void deliver_mouse(nwm_window_t *w, const nd_event_t *ev) {
  nwm_ev_mouse_t m;
  nd_rect_t      c = client_rect(w);

  memset(&m, 0, sizeof(m));
  m.win = w->id;
  m.x = ev->x - c.x;
  m.y = ev->y - c.y;
  m.type = ev->type;
  m.button = ev->button;
  m.mods = ev->mods;
  deliver(w->owner, NWM_EV_MOUSE, &m, sizeof(m));
}

static void deliver_key(nwm_window_t *w, const nd_event_t *ev) {
  nwm_ev_key_t m;

  memset(&m, 0, sizeof(m));
  m.win = w->id;
  m.key = ev->key;
  m.mods = ev->mods;
  deliver(w->owner, NWM_EV_KEY, &m, sizeof(m));
}

/* 1..3 when the pointer is over one of this window's caption buttons. */
static INT32 caption_hover(const nwm_window_t *w) {
  UINT8 b = title_button(w, g_mouse_x, g_mouse_y);

  return (b == 0u) ? 0 : (INT32)b;
}

/* Which decoration did the press land on? 0 none, 1 close, 2 maximize,
 * 3 minimize. Geometry matches paint_window(): three 46x32 boxes flush
 * with the right edge. */
static UINT8 title_button(const nwm_window_t *w, INT32 x, INT32 y) {
  UINT32 i;

  for (i = 0; i < 3u; i++) {
    nd_rect_t b = nd_rect(w->rect.x + w->rect.w - (INT32)(i + 1u) * NWM_BTN,
                          w->rect.y, NWM_BTN, NWM_TITLE_H);

    if (nd_rect_contains(b, x, y)) {
      return (UINT8)(i == 0u ? 1u : (i == 1u ? 2u : 3u));
    }
  }
  return 0;
}

/* Hardware mouse coordinates remain absolute for hit testing and shell
 * dispatch; deliver_mouse translates them into the selected client's space.
 * Press capture keeps release paired with its original window, while keys
 * follow focus rather than whichever window is merely under the pointer. */
static void route(const nd_event_t *ev) {
  int idx;

  g_mouse_x = ev->x;
  g_mouse_y = ev->y;

  /* An open start menu (or any overlay card) takes the pointer before the
   * windows do: it is drawn above them, so it must be clicked above them
   * too. Dragging a window keeps priority so a drag cannot be swallowed. */
  if (g_drag < 0 &&
      (ev->type == ND_EV_MOUSE_MOVE || ev->type == ND_EV_MOUSE_DOWN ||
       ev->type == ND_EV_MOUSE_UP || ev->type == ND_EV_WHEEL) &&
      ng_overlay_hit(ev->x, ev->y)) {
    nd_demo_dispatch(ev);
    return;
  }

  switch (ev->type) {
  case ND_EV_MOUSE_MOVE:
    nd_cursor_set(ev->x, ev->y);
    if (g_drag >= 0 && g_wins[g_drag].used) {
      nwm_window_t *w = &g_wins[g_drag];

      damage_frame(w->rect);
      w->rect.x = ev->x - g_drag_dx;
      w->rect.y = ev->y - g_drag_dy;
      if (w->rect.y < 0) {
        w->rect.y = 0;
      }
      damage_frame(w->rect);
      break;
    }
    idx = hit_test(ev->x, ev->y);
    if (idx >= 0) {
      deliver_mouse(&g_wins[idx], ev);
    } else {
      nd_demo_dispatch(ev); /* the shell's own widgets */
    }
    break;

  case ND_EV_MOUSE_DOWN:
    idx = hit_test(ev->x, ev->y);
    if (idx < 0) {
      focus_set(-1);
      nd_demo_dispatch(ev);
      break;
    }
    z_raise(idx);
    focus_set(idx);
    damage_frame(g_wins[idx].rect);
    g_capture = idx;
    if (ev->y < g_wins[idx].rect.y + NWM_TITLE_H) {
      UINT8 b = title_button(&g_wins[idx], ev->x, ev->y);

      if (b == 1u) {
        UINT32    id = g_wins[idx].id;
        ipc_msg_t self;

        /* Tell the owner, then close: the WM does not keep an orphan
         * around waiting for the client to agree. */
        deliver(g_wins[idx].owner, NWM_EV_CLOSE, &id, 4);
        memset(&self, 0, sizeof(self));
        self.hdr.type = NWM_REQ_DESTROY;
        self.hdr.len = 4;
        self.hdr.sender = NWM_PID_SHELL;
        memcpy(self.payload, &id, 4);
        handle_request(&self);
      } else if (b == 2u) {
        ipc_msg_t self;

        memset(&self, 0, sizeof(self));
        self.hdr.type = NWM_REQ_MAXIMIZE;
        self.hdr.len = 4;
        self.hdr.sender = NWM_PID_SHELL;
        memcpy(self.payload, &g_wins[idx].id, 4);
        handle_request(&self);
      } else if (b == 3u) {
        ipc_msg_t self;

        memset(&self, 0, sizeof(self));
        self.hdr.type = NWM_REQ_MINIMIZE;
        self.hdr.len = 4;
        self.hdr.sender = NWM_PID_SHELL;
        memcpy(self.payload, &g_wins[idx].id, 4);
        handle_request(&self);
      } else {
        g_drag = idx;
        g_drag_dx = ev->x - g_wins[idx].rect.x;
        g_drag_dy = ev->y - g_wins[idx].rect.y;
      }
    } else {
      deliver_mouse(&g_wins[idx], ev);
    }
    break;

  case ND_EV_MOUSE_UP:
    if (g_drag >= 0) {
      if (g_wins[g_drag].used) {
        damage_frame(g_wins[g_drag].rect);
      }
      g_drag = -1;
      g_capture = -1;
      break;
    }
    idx = (g_capture >= 0 && g_wins[g_capture].used) ? g_capture
                                                     : hit_test(ev->x, ev->y);
    g_capture = -1;
    if (idx >= 0) {
      deliver_mouse(&g_wins[idx], ev);
    } else {
      nd_demo_dispatch(ev);
    }
    break;

  case ND_EV_KEY:
    if (g_focus >= 0 && g_wins[g_focus].used && g_wins[g_focus].state != 1u) {
      deliver_key(&g_wins[g_focus], ev);
    } else {
      nd_demo_dispatch(ev);
    }
    break;

  default:
    nd_demo_dispatch(ev);
    break;
  }
}

/* ---- tick ------------------------------------------------------------- */

static void pump(void) {
  ipc_msg_t msg;
  UINT32    n = 0;

  /* Bounded work per tick: a client cannot make the scheduler spend the
   * whole slice inside the WM. */
  while (n < 8u && ipc_try_recv(g_port, &msg) >= 0) {
    handle_request(&msg);
    n++;
  }
}

/* A window whose process is gone must not linger. Only the ring-3 case
 * needs this; the shell (pid 0) is always there. */
static void sweep_owners(void) {
  UINT32 i;

  if ((sched_ticks() - g_liveness_tick) < 40u) {
    return;
  }
  g_liveness_tick = (UINT32)sched_ticks();

  /* Health line every ~10 s, the same cadence the scheduler uses for its
   * own counters: requests served, events delivered, events dropped
   * because a client's inbox was full. */
  if (g_next_stats == 0u) {
    g_next_stats = (UINT32)sched_ticks() + 1000u;
  } else if ((UINT32)sched_ticks() >= g_next_stats) {
    g_next_stats = (UINT32)sched_ticks() + 1000u;
    kputs("[NWM] stats req=");
    kputs_hex(g_requests);
    kputs(" ev=");
    kputs_hex(g_events);
    kputs(" drop=");
    kputs_hex(g_dropped);
    kputs("\n");
  }

  for (i = 0; i < NWM_MAX_WINDOWS; i++) {
    nwm_window_t *w = &g_wins[i];

    if (!w->used || w->owner == NWM_PID_SHELL) {
      continue;
    }
    if (sched_find_pid(w->owner) == 0) {
      kputs("[NWM] owner gone, closing win=");
      kputs_hex(w->id);
      kputs("\n");
      if (g_focus == (int)i) {
        focus_set(-1);
      }
      z_take_out((int)i);
      damage_frame(w->rect);
      memset(w, 0, sizeof(*w));
    }
  }
}

/* Accent and wallpaper live in the design-token layer now: both modes keep
 * their own wallpapers, and the accent override picks a readable caption
 * from its luminance. The settings panel is still the only caller. */
UINT32 nwm_accent_get(void) { return nd_theme_accent(); }

void nwm_accent_set(UINT32 rgb) {
  if (ng_ctx_get() == 0) {
    return;
  }
  nd_theme_set_accent(rgb);
  ng_theme_apply();
}

UINT32 nwm_wallpaper_get(void) { return nd_theme_wallpaper(); }

void nwm_wallpaper_set(UINT32 idx) {
  if (ng_ctx_get() == 0) {
    return;
  }
  nd_theme_set_wallpaper(idx);
  ng_theme_apply();
}

/* Mode changes come from two places (the settings panel and the shell's
 * quick settings). Both end here so the widget theme and the shell scene
 * change in the same tick. */
void nwm_theme_set_mode(UINT32 mode) {
  nd_theme_mode_t m = (mode == ND_THEME_DARK) ? ND_THEME_DARK
                     : (mode == ND_THEME_LIGHT) ? ND_THEME_LIGHT
                                                : ND_THEME_AUTO;

  nd_theme_set_mode(m);
  ng_theme_apply();
  /* The shell rebuilds on its next idle pass (theme version changed).
   * Rebuilding from inside a widget callback would free the widget that
   * is still unwinding its own press. */
}

UINT32 nwm_theme_mode(void) { return (UINT32)nd_theme_mode(); }

UINT32 nwm_window_count(void) {
  UINT32 i;
  UINT32 n = 0;

  for (i = 0; i < NWM_MAX_WINDOWS; i++) {
    if (g_wins[i].used) {
      n++;
    }
  }
  return n;
}

void nwm_tick(void) {
  nd_event_t ev;
  nd_rect_t  d;

  if (!g_ready) {
    return;
  }
  nd_input_poll();
  while (nd_input_pop(&ev) != 0) {
    route(&ev);
  }
  pump();
  sweep_owners();
  /* The shell owns the taskbar clock; the WM owns the tick. */
  nd_demo_idle();

  if (nd_demo_take_damage(&d)) {
    damage(d);
  }
  if (!nd_rect_empty(g_damage)) {
    nd_surface_t *s = screen();
    nd_rect_t     clip = g_damage;

    g_damage = nd_rect(0, 0, 0, 0);
    if (s != 0) {
      clip = nd_rect_intersect(clip, nd_rect(0, 0, s->w, s->h));
    }
    if (!nd_rect_empty(clip)) {
      nd_cursor_hide();
      paint(clip);
    }
  }
  nd_cursor_draw();
}

/* ---- boot ------------------------------------------------------------- */

void nwm_init(void) {
  memset(g_wins, 0, sizeof(g_wins));
  memset(g_z, 0, sizeof(g_z));
  g_zcount = 0;
  g_next_id = 1;
  g_focus = -1;
  g_drag = -1;
  g_capture = -1;
  g_damage = nd_rect(0, 0, 0, 0);
  g_requests = 0;
  g_events = 0;
  g_dropped = 0;
  g_liveness_tick = 0;

  if (ipc_port_create(NWM_PORT_NAME, NWM_PID_SHELL, 0, &g_port) != IPC_OK) {
    kputs("[NWM] ERROR: no IPC port for the window manager\n");
    return;
  }

  /* Back buffer the size of the screen, and point the widget library at
   * it: from here on the shell composes off-screen like the windows do. */
  {
    nd_surface_t *scr = screen();

    g_frame_ok = 0;
    if (scr != 0 && nd_surface_create(&g_frame, scr->w, scr->h) == 0) {
      ng_ctx_t *ctx = ng_ctx_get();

      if (ctx != 0) {
        ctx->surf = &g_frame;
        g_frame_ok = 1;
      }
      /* The scene was painted straight to the screen before the buffer
       * existed; repaint everything into it on the first tick. */
      g_damage = nd_rect(0, 0, scr->w, scr->h);
    }
    kputs(g_frame_ok ? "[NWM] compositor: double buffered\n"
                     : "[NWM] compositor: direct to framebuffer\n");
  }

  g_ready = 1;
  kputs("[NWM] Novus-WM up: protocol ");
  kputs_hex(NWM_PROTO_VERSION);
  kputs(", port ");
  kputs_hex(g_port);
  kputs(", max ");
  kputs_hex(NWM_MAX_WINDOWS);
  kputs(" windows\n");
  (void)nwm_selftest();
}

/* ---- self test -------------------------------------------------------- */

/* Drive the real request handlers with synthetic messages, the way a
 * client would, and check the state they leave behind. Runs before ring 3
 * so a protocol bug shows up in the boot log instead of in the desktop. */
static void fake_msg(ipc_msg_t *m, UINT16 type, UINT64 sender,
                     const void *payload, UINT32 len) {
  memset(m, 0, sizeof(*m));
  m->hdr.magic = IPC_MAGIC;
  m->hdr.type = type;
  m->hdr.sender = sender;
  m->hdr.target = g_port;
  m->hdr.len = len;
  if (payload != 0 && len != 0u && len <= IPC_PAYLOAD_MAX) {
    memcpy(m->payload, payload, len);
  }
}

static int expect(int cond, const char *what) {
  if (!cond) {
    kputs("[NWM] FAIL: ");
    kputs(what);
    kputs("\n");
  }
  return cond;
}

static UINT32 id_of_owner(UINT64 owner) {
  UINT32 i;

  for (i = 0; i < NWM_MAX_WINDOWS; i++) {
    if (g_wins[i].used && g_wins[i].owner == owner) {
      return g_wins[i].id;
    }
  }
  return 0;
}

int nwm_selftest(void) {
  ipc_msg_t    m;
  nwm_create_t c;
  nwm_move_t   mv;
  nwm_draw_t   dr;
  nwm_effect_t ef;
  UINT32       saved_req = g_requests;
  UINT32       a, b;
  int          pass = 1;

  if (!g_ready) {
    return 0;
  }

  /* Two windows from two fake processes, overlapping on purpose. */
  memset(&c, 0, sizeof(c));
  c.x = 120;
  c.y = 120;
  c.w = 320;
  c.h = 200;
  memcpy(c.title, "selftest A", 10);
  fake_msg(&m, NWM_REQ_CREATE, 0xF001ull, &c, sizeof(c));
  handle_request(&m);
  c.x = 200;
  c.y = 160;
  memcpy(c.title, "selftest B", 10);
  fake_msg(&m, NWM_REQ_CREATE, 0xF002ull, &c, sizeof(c));
  handle_request(&m);

  a = id_of_owner(0xF001ull);
  b = id_of_owner(0xF002ull);
  pass &= expect(a != 0u && b != 0u, "create");
  pass &= expect(g_zcount == 2u, "z-order depth");
  pass &= expect(g_z[1] == (UINT8)win_index(b), "newest is on top");
  pass &= expect(hit_test(250, 200) == win_index(b), "hit test picks top");

  {
    task_t *front = sched_find_name("spin");
    task_t *back = sched_find_name("selftest");

    if (front != 0 && back != 0) {
      win_get(a)->owner = front->pid;
      win_get(b)->owner = front->pid;
      fake_msg(&m, NWM_REQ_FOCUS, front->pid, &a, 4);
      handle_request(&m);
      pass &= expect(front->foreground && front->slice == 8u, "focus boosts owner");
      front->slice = 7u;
      fake_msg(&m, NWM_REQ_FOCUS, front->pid, &b, 4);
      handle_request(&m);
      pass &= expect(front->slice == 7u, "same owner keeps remaining slice");
      win_get(b)->owner = back->pid;
      fake_msg(&m, NWM_REQ_FOCUS, front->pid, &a, 4);
      handle_request(&m);
      fake_msg(&m, NWM_REQ_FOCUS, back->pid, &b, 4);
      handle_request(&m);
      pass &= expect(!front->foreground && front->slice == 4u &&
                         back->foreground && back->slice == 8u,
                     "focus transfers weight");
      fake_msg(&m, NWM_REQ_MINIMIZE, back->pid, &b, 4);
      handle_request(&m);
      pass &= expect(!back->foreground && back->slice == 4u && g_focus == -1,
                     "minimize restores background slice");
      fake_msg(&m, NWM_REQ_FOCUS, back->pid, &b, 4);
      handle_request(&m);
      pass &= expect(win_get(b)->state == 0u && back->foreground,
                     "focus restores minimized window");
      focus_set(-1);
      win_get(a)->owner = 0xF001ull;
      win_get(b)->owner = 0xF002ull;
    }
  }

  /* Move, draw, effect. */
  memset(&mv, 0, sizeof(mv));
  mv.win = b;
  mv.x = 600;
  mv.y = 400;
  fake_msg(&m, NWM_REQ_MOVE, 0xF002ull, &mv, sizeof(mv));
  handle_request(&m);
  pass &= expect(win_get(b) != 0 && win_get(b)->rect.x == 600, "move");

  memset(&dr, 0, sizeof(dr));
  dr.win = a;
  dr.count = 2;
  dr.cmd[0].op = NWM_OP_FILL;
  dr.cmd[0].w = 40;
  dr.cmd[0].h = 20;
  dr.cmd[0].color = ND_RGB(0x20, 0x80, 0xC0);
  dr.cmd[1].op = NWM_OP_FRAME;
  dr.cmd[1].w = 100;
  dr.cmd[1].h = 50;
  dr.cmd[1].h = 50;
  fake_msg(&m, NWM_REQ_DRAW, 0xF001ull, &dr, sizeof(dr));
  handle_request(&m);
  pass &= expect(win_get(a) != 0 && win_get(a)->draw_n == 2u, "draw list");

  memset(&ef, 0, sizeof(ef));
  ef.win = a;
  ef.alpha = 100;
  fake_msg(&m, NWM_REQ_EFFECT, 0xF001ull, &ef, sizeof(ef));
  handle_request(&m);
  pass &= expect(win_get(a) != 0 && win_get(a)->alpha == 100u, "effect");

  /* Clamping: a window cannot be smaller than the decorations allow. */
  {
    nwm_resize_t rs;

    memset(&rs, 0, sizeof(rs));
    rs.win = a;
    rs.w = 10;
    rs.h = 10;
    fake_msg(&m, NWM_REQ_RESIZE, 0xF001ull, &rs, sizeof(rs));
    handle_request(&m);
    pass &= expect(win_get(a) != 0 && win_get(a)->rect.w == NWM_MIN_W &&
                       win_get(a)->rect.h == NWM_MIN_H,
                   "minimum size");
  }

  /* Ownership: a third process may not steer A. */
  {
    INT32 before = (win_get(a) != 0) ? win_get(a)->rect.x : -1;

    memset(&mv, 0, sizeof(mv));
    mv.win = a;
    mv.x = 700;
    mv.y = 700;
    fake_msg(&m, NWM_REQ_MOVE, 0xF003ull, &mv, sizeof(mv));
    handle_request(&m);
    pass &= expect(win_get(a) != 0 && win_get(a)->rect.x == before, "denied");
  }

  /* Tear down in the same order a client would. */
  fake_msg(&m, NWM_REQ_DESTROY, 0xF001ull, &a, 4);
  handle_request(&m);
  fake_msg(&m, NWM_REQ_DESTROY, 0xF002ull, &b, 4);
  handle_request(&m);
  pass &= expect(g_zcount == 0u, "destroy");

  /* An unknown request must be refused, not crash and not create. */
  fake_msg(&m, 0x7FFFu, 0xF001ull, 0, 0);
  handle_request(&m);
  pass &= expect(g_zcount == 0u, "unknown request ignored");

  g_requests = saved_req; /* the boot log budget belongs to real clients */
  kputs("[NWM] selftest: ");
  kputs(pass ? "PASS" : "FAIL");
  kputs("\n");
  return pass;
}

#else /* !CONFIG_NOVUS_WM */

void nwm_init(void) {}
void nwm_tick(void) {}
int nwm_selftest(void) { return 1; }

#endif /* CONFIG_NOVUS_WM */
