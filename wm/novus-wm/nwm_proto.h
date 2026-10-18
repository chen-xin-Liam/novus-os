/**
 * Novus OS - Novus-WM IPC protocol, version 1.0 (M6, README part 10).
 *
 * One wire format for every GUI client: a 32-byte kernel-stamped header
 * (kernel/ipc.h) followed by one of the fixed payloads below. Requests go
 * to the well-known port "nwm"; the WM answers on the sender's inbox and
 * pushes events to the owner of the window they concern.
 *
 * Why fixed payloads and not a pixel buffer: the client and the WM are in
 * different address spaces, and M6 keeps the protocol copy-based - a
 * client describes what its window contains (fills, frames, lines) and
 * the WM replays that list into the compositor, clipped to the window.
 * Nothing is shared, nothing can be drawn outside a window, and a client
 * that dies leaves no pointer behind. A shared-memory path is the next
 * step and does not change these message types.
 *
 * All structs are packed and passed as raw payload bytes; both sides
 * compile this one header, so there is no marshalling layer to drift.
 */

#ifndef NOVUS_NWM_PROTO_H
#define NOVUS_NWM_PROTO_H

#include "uefi.h"

#define NWM_PROTO_VERSION 0x00010001u /* 1.1: window content lists */
#define NWM_PORT_NAME     "nwm"
#define NWM_PID_SHELL     0ull /* the desktop itself: no inbox, local call */

#define NWM_MAX_WINDOWS 8
#define NWM_MAX_CMDS    7  /* (IPC_PAYLOAD_MAX - 8) / 16 */
#define NWM_MAX_SHAPES  28 /* per-window display list, v1.1 */
#define NWM_MAX_TEXTS   40
#define NWM_TEXT_MAX    96
#define NWM_TITLE_MAX   24
#define NWM_TITLE_H     24

/* ---- message types ---------------------------------------------------- */

/* Requests: client -> WM. */
#define NWM_REQ_CREATE   0x0101u
#define NWM_REQ_DESTROY  0x0102u
#define NWM_REQ_MOVE     0x0103u
#define NWM_REQ_RESIZE   0x0104u
#define NWM_REQ_RAISE    0x0105u
#define NWM_REQ_MINIMIZE 0x0106u
#define NWM_REQ_MAXIMIZE 0x0107u
#define NWM_REQ_RESTORE  0x0108u
#define NWM_REQ_FOCUS    0x0109u
#define NWM_REQ_DRAW     0x010Au
#define NWM_REQ_TITLE    0x010Bu
#define NWM_REQ_EFFECT   0x010Cu
/* v1.1: a window's content is a list of shapes and text runs. CLEAR
 * empties it, SHAPE and TEXT append one item each. Small messages on
 * purpose: the payload limit is 128 bytes, and one item per message keeps
 * every field inside it without a shared buffer. */
#define NWM_REQ_CLEAR    0x010Du
#define NWM_REQ_SHAPE    0x010Eu
#define NWM_REQ_TEXT     0x010Fu

/* Replies: WM -> requester's inbox, flagged IPC_F_REPLY. */
#define NWM_RSP_OK  0x0001u
#define NWM_RSP_ERR 0x0002u

/* Events: WM -> window owner. */
#define NWM_EV_CREATED 0x0201u
#define NWM_EV_MOUSE   0x0202u
#define NWM_EV_KEY     0x0203u
#define NWM_EV_FOCUS   0x0204u
#define NWM_EV_RESIZE  0x0205u
#define NWM_EV_CLOSE   0x0206u

/* Request flags. */
#define NWM_F_SYNC 0x0001u /* "reply before you go on" - the WM always replies */

/* ---- request payloads ------------------------------------------------- */

typedef struct {
  UINT32 win;   /* 0 = assign one */
  INT32  x, y;  /* frame position */
  INT32  w, h;  /* frame size, title bar included */
  UINT16 flags;
  UINT16 pad;
  char   title[NWM_TITLE_MAX];
} nwm_create_t;

typedef struct {
  UINT32 win;
  INT32  x, y;
} nwm_move_t;

typedef struct {
  UINT32 win;
  INT32  w, h;
} nwm_resize_t;

typedef struct {
  UINT32 win;
} nwm_win_t;

typedef struct {
  UINT32 win;
  char   title[NWM_TITLE_MAX];
} nwm_title_t;

typedef struct {
  UINT32 win;
  UINT8  alpha; /* 0 = invisible, 255 = opaque (window fade effect) */
  UINT8  pad[3];
} nwm_effect_t;

/* One drawing command, 16 bytes. Coordinates are window-local, i.e.
 * relative to the top-left of the client area. */
typedef struct {
  UINT8  op; /* NWM_OP_* */
  UINT8  pad;
  INT16  x, y, w, h;
  UINT16 pad2;
  UINT32 color; /* 0x00RRGGBB */
} nwm_cmd_t;

#define NWM_OP_FILL   1u /* filled rectangle */
#define NWM_OP_FRAME  2u /* rectangle outline, `w` = thickness */
#define NWM_OP_HLINE  3u
#define NWM_OP_VLINE  4u
#define NWM_OP_CIRCLE 5u /* outline, radius = w */

/* One shape in the v1.1 display list. */
typedef struct {
  UINT32 win;
  UINT8  op; /* NWM_OP_* */
  UINT8  pad;
  INT16  x, y, w, h;
  UINT32 color;
} nwm_shape_t;

/* One text run. `scale` 1 is 5x7, 2 is double; coordinates are relative
 * to the client area, `len` is the number of bytes actually used. */
typedef struct {
  UINT32 win;
  INT16  x, y;
  UINT8  scale;
  UINT8  len;
  UINT32 color;
  char   text[NWM_TEXT_MAX];
} nwm_text_t;

typedef struct {
  UINT32    win;
  UINT16    count;
  UINT16    pad;
  nwm_cmd_t cmd[NWM_MAX_CMDS];
} nwm_draw_t;

/* ---- reply and event payloads ----------------------------------------- */

typedef struct {
  UINT32 win;
  INT32  status; /* 0 ok, negative = -NWM_E* */
} nwm_rsp_t;

typedef struct {
  UINT32 win;
  INT32  x, y;      /* client-area local */
  UINT8  type;      /* ND_EV_MOUSE_* */
  UINT8  button;
  UINT8  mods;
  UINT8  pad;
} nwm_ev_mouse_t;

typedef struct {
  UINT32 win;
  UINT32 key;
  UINT8  mods;
  UINT8  pad[3];
} nwm_ev_key_t;

typedef struct {
  UINT32 win;
  UINT32 focused;
} nwm_ev_focus_t;

typedef struct {
  UINT32 win;
  INT32  w, h;
} nwm_ev_resize_t;

/* ---- error codes carried in nwm_rsp_t.status -------------------------- */

#define NWM_OK         0
#define NWM_EBADWIN  (-1)
#define NWM_EBADREQ  (-2)
#define NWM_EFULL    (-3) /* window table full */
#define NWM_EDENIED  (-4) /* the window belongs to another process */
#define NWM_EBADPAY  (-5)

#endif /* NOVUS_NWM_PROTO_H */
