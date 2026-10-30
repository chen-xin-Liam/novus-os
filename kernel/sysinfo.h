/**
 * Novus OS - sysinfo service (M8).
 *
 * One port that answers questions about the machine: the scheduler's task
 * table, the built-in file tree, memory and IPC counters, the desktop
 * accent. The three M8 applications are pure ring-3 clients; keeping the
 * answers behind IPC means they need no special syscall and no header
 * from the kernel - only this protocol.
 *
 * Transport is the M6 IPC format (kernel/ipc.h): requests carry the
 * structs below as payload, replies come back on the caller's inbox with
 * one of the RSP_* types. Lists are paged (three records per reply) so
 * every answer fits in IPC_PAYLOAD_MAX without a second message format.
 */

#ifndef NOVUS_SYSINFO_H
#define NOVUS_SYSINFO_H

#include "uefi.h"

#define SYSINFO_PORT_NAME "sys"

/* Requests: client -> service. */
#define SYSINFO_REQ_TASKS  0x0301u
#define SYSINFO_REQ_FILES  0x0302u
#define SYSINFO_REQ_STATS  0x0303u
#define SYSINFO_REQ_ACCENT 0x0304u
#define SYSINFO_REQ_SPAWN  0x0305u
#define SYSINFO_REQ_THEME  0x0306u /* accent / wallpaper, query or set */
#define SYSINFO_REQ_KILL   0x0307u
#define SYSINFO_REQ_NET    0x0308u
#define SYSINFO_REQ_PING   0x0309u
#define SYSINFO_REQ_TCP    0x030Au
#define SYSINFO_REQ_TASKS2 0x030Bu
#define SYSINFO_REQ_TACT   0x030Cu
#define SYSINFO_REQ_NETDEVS 0x030Du /* end a task by pid */
#define SYSINFO_REQ_SPAWN_NAME 0x030Eu /* start a built-in app by name */
#define SYSINFO_REQ_LANG      0x030Fu /* get / set the UI language */
#define SYSINFO_REQ_DISPLAY   0x0318u /* the graphics outputs of the machine */
#define SYSINFO_REQ_NCSI      0x031Au /* start an NCSI connectivity probe */
#define SYSINFO_REQ_DEVLIST   0x031Bu /* the device manager's device tree */
#define SYSINFO_REQ_DEVFIELD  0x031Du /* one page of a device's properties */

typedef struct {
  UINT32 lang;    /* 0 zh, 1 en */
  UINT32 version; /* increments on every change */
} sysinfo_lang_rsp_t;

typedef struct {
  UINT32 set;   /* 0 = query only */
  UINT32 value; /* ND_LANG_* */
} sysinfo_lang_t;

/* Replies. Distinct types per query so a client can match its answer
 * without a correlation id. */
#define SYSINFO_RSP_OK     0x0310u
#define SYSINFO_RSP_TASKS  0x0311u
#define SYSINFO_RSP_FILES  0x0312u
#define SYSINFO_RSP_STATS  0x0313u
#define SYSINFO_RSP_ACCENT 0x0314u
#define SYSINFO_RSP_THEME  0x0315u
#define SYSINFO_RSP_NET    0x0316u
#define SYSINFO_RSP_TASKS2 0x0317u
#define SYSINFO_RSP_DISPLAY 0x0319u
#define SYSINFO_RSP_DEVLIST  0x031Cu
#define SYSINFO_RSP_DEVFIELD 0x031Eu
#define SYSINFO_RSP_ERR    0x03FFu

/* Directories the file manager can browse. */
#define SYSINFO_DIR_APPS 0u
#define SYSINFO_DIR_DEV  1u
#define SYSINFO_DIR_PROC 2u
#define SYSINFO_DIR_EXE  3u

/* Entry kinds. */
#define SYSINFO_KIND_APP  1u
#define SYSINFO_KIND_DEV  2u
#define SYSINFO_KIND_VIRT 3u
#define SYSINFO_KIND_EXE  4u

#define SYSINFO_RECS 3u

typedef struct {
  UINT16 total; /* entries in this directory */
  UINT8  n;     /* records in this reply */
  UINT8  pad;
} sysinfo_list_t;

typedef struct {
  UINT64 pid;
  UINT32 ticks;
  INT32  exit_code;
  UINT8  state;
  UINT8  slice;
  char   name[14];
} sysinfo_task_t;

typedef struct {
  sysinfo_list_t h;
  sysinfo_task_t rec[SYSINFO_RECS];
} sysinfo_tasks_rsp_t;

typedef struct {
  UINT32 size;
  UINT8  kind;
  UINT8  pad[3];
  char   name[24];
} sysinfo_file_t;

typedef struct {
  sysinfo_list_t h;
  sysinfo_file_t rec[SYSINFO_RECS];
} sysinfo_files_rsp_t;

typedef struct {
  UINT64 uptime;   /* ticks since boot */
  UINT64 free_kb;
  UINT64 total_kb; /* usable RAM, for the settings memory bar */
  UINT32 tasks;    /* live tasks */
  UINT32 windows;  /* windows the WM holds */
  UINT32 ipc_sent;
  UINT32 ipc_drop;
  UINT32 accent;
  UINT32 wallpaper;
} sysinfo_stats_t;

/* Theme query/set. id 0 = accent (rgb), 1 = wallpaper index (0..6, one per
 * embedded photo wallpaper). A query ignores `value`. */
typedef struct {
  UINT8  id;
  UINT8  pad[3];
  UINT32 value;
} sysinfo_theme_t;

/* mode: ND_THEME_*; resolved: 0 light / 1 dark; version increments on
 * every change so a client can poll cheaply. Appended to the M8 reply, so
 * an old client still finds accent and wallpaper at the same offsets. */
typedef struct {
  UINT32 accent;
  UINT32 wallpaper;
  UINT32 mode;
  UINT32 resolved;
  UINT32 version;
} sysinfo_theme_rsp_t;

/* The graphics outputs the firmware offered: one entry per display, with
 * the mode in use, the mode the display would prefer, and whether the
 * link is digital (DP/eDP/HDMI) or analog (VGA). */
#define SYSINFO_DISPLAY_MAX 4u

typedef struct {
  UINT32 width;
  UINT32 height;
  UINT32 pref_width;
  UINT32 pref_height;
  UINT32 flags; /* bit 0: digital link, bit 1: the primary output */
} sysinfo_display_out_t;

typedef struct {
  UINT32                  count;
  sysinfo_display_out_t   out[SYSINFO_DISPLAY_MAX];
} sysinfo_display_rsp_t;

/* Kill: the pid to end. Reply is an INT32 status through RSP_OK. */
typedef struct {
  UINT64 pid;
} sysinfo_kill_t;

typedef struct {
  UINT16 index;
  UINT8  max;
  UINT8  dir;
  UINT32 pad;
} sysinfo_list_req_t;

typedef struct {
  UINT32 rgb;
  UINT8  set; /* 0 = query, 1 = apply */
  UINT8  pad[3];
} sysinfo_accent_t;

/* Network snapshot: link, addresses, DHCP state and counters. */
typedef struct {
  UINT8  ndev;
  UINT8  dhcp;      /* NET_DHCP_* */
  UINT8  ping_state;/* 0 idle 1 running 2 ok 3 timeout */
  UINT8  tcp_state; /* 0 idle 1 connecting 2 ok 3 failed */
  UINT32 ip;
  UINT32 mask;
  UINT32 gw;
  UINT32 rtt_ms;
  UINT32 tcp_bytes;
  UINT64 rx_pkts;
  UINT64 tx_pkts;
  UINT64 rx_err;
  UINT64 tx_err;
  UINT8  link;
  UINT8  state;
  UINT8  mac[6];
  UINT16 speed_mbps;
  UINT16 pad;
  UINT8  ncsi;      /* NET_NCSI_* stage or verdict */
  UINT8  ncsi_pad;
  UINT16 http_code; /* status code of the last NCSI response, 0 = none */
  UINT32 ncsi_ms;   /* probe duration so far / total */
  /* Controller name of the active device, e.g. "Intel Wi-Fi 6E AX211";
   * empty when there is no active device or the ID is unknown. */
  char   model[32];
} sysinfo_net_t;

typedef struct {
  char  name[10];
  UINT8 link;
  UINT8 state;
  UINT8 mac[6];
} sysinfo_netdev_t;

typedef struct {
  UINT8            n;
  UINT8            pad[3];
  sysinfo_netdev_t dev[2];
} sysinfo_netdevs_t;

typedef struct {
  UINT16 total;
  UINT8  n;
  UINT8  pad;
  UINT32 pad0; /* keeps rec 8-byte aligned, matching the Rust layout */
  /* sched_info2_t rec[3] follows; declared as raw bytes so this header
   * does not depend on sched.h. */
  UINT8  rec[3 * 32];
} sysinfo_tasks2_t;

typedef struct {
  UINT64 pid;
  UINT8  action; /* 0 kill, 1 suspend, 2 resume */
  UINT8  pad[3];
} sysinfo_tact_t;

/* ---- device manager (M12) --------------------------------------------- */

/* A flat device list: `parent` links a node to its category, so the client
 * builds the tree without a second query. DEVTAB_NONE means "root". The
 * ids of class/kind/status/flags are the devtab.h ABI; the words are the
 * client's. */
#define SYSINFO_DEV_RECS 3u
#define SYSINFO_DEV_NAME_MAX 32u

typedef struct {
  UINT16 parent;
  UINT8  class_id; /* DEV_CAT_* */
  UINT8  status;   /* DEV_STATUS_* */
  UINT8  kind;     /* DEV_KIND_* */
  UINT8  idx;      /* index in the backing subsystem table */
  UINT8  flags;    /* DEV_FLAG_* */
  UINT8  pad;
  char   name[SYSINFO_DEV_NAME_MAX];
} sysinfo_dev_t;

typedef struct {
  sysinfo_list_t h;
  sysinfo_dev_t  rec[SYSINFO_DEV_RECS];
} sysinfo_devs_rsp_t;

/* Property fields: key is DEV_F_*, and `group` is the event row or BAR
 * index. Values are longer than a whole reply would allow, hence paging. */
#define SYSINFO_DEVF_RECS 2u
#define SYSINFO_DEVF_VALUE_MAX 56u

typedef struct {
  UINT16 dev;
  UINT8  tab;   /* DEV_TAB_* */
  UINT8  index; /* first field to return */
  UINT32 pad;
} sysinfo_devfield_req_t;

typedef struct {
  UINT8  key;
  UINT8  group;
  UINT16 pad;
  char   value[SYSINFO_DEVF_VALUE_MAX];
} sysinfo_devfield_t;

typedef struct {
  sysinfo_list_t     h; /* total = fields in this tab */
  sysinfo_devfield_t rec[SYSINFO_DEVF_RECS];
} sysinfo_devfields_rsp_t;

/* Create the port. Call once, after the scheduler owns a task table. */
void sysinfo_init(void);

/* Timer tick: answer queued requests (bounded work per tick). */
void sysinfo_tick(void);

#endif /* NOVUS_SYSINFO_H */
