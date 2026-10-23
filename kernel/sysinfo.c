/**
 * Novus OS - sysinfo service (M8).
 *
 * Runs in the timer tick next to the WM: drain a few requests, answer
 * them from kernel state, drop nothing. Every answer is one IPC message;
 * lists are paged so the payload stays under the 128-byte limit.
 */

#include "kernel.h"
#include "ipc.h"
#include "sched.h"
#include "pmm.h"
#include "apps.h"
#include "nwm.h"
#include "sched.h"
#include "sysinfo.h"
#include "net.h"
#include "blkdev.h"
#include "nd_theme.h"

#if CONFIG_NOVUS_IPC && CONFIG_NOVUS_WM

static UINT64 g_port;
static UINT32 g_t2_log;

/* ---- built-in file tree ----------------------------------------------- */

typedef struct {
  const char *name;
  UINT8       kind;
  const char *what; /* device / virtual entries: a short description */
} sysinfo_entry_t;

static const sysinfo_entry_t g_dev[] = {
    {"fb0", SYSINFO_KIND_DEV, "framebuffer"},
    {"ps2", SYSINFO_KIND_DEV, "keyboard+mouse"},
    {"serial", SYSINFO_KIND_DEV, "com1"},
};

static UINT32 dev_count(void) {
  return (UINT32)(sizeof(g_dev) / sizeof(g_dev[0])) +
         (UINT32)blkdev_count();
}

static const sysinfo_entry_t g_proc[] = {
    {"tasks", SYSINFO_KIND_VIRT, "scheduler"},
    {"ipc", SYSINFO_KIND_VIRT, "ports"},
    {"mem", SYSINFO_KIND_VIRT, "free pages"},
};

static UINT32 dir_count(UINT8 dir) {
  switch (dir) {
  case SYSINFO_DIR_APPS:
    return apps_count();
  case SYSINFO_DIR_DEV:
    return dev_count();
  case SYSINFO_DIR_PROC:
    return (UINT32)(sizeof(g_proc) / sizeof(g_proc[0]));
  default:
    return 0;
  }
}

static void copy_str(char *dst, UINT32 cap, const char *src) {
  UINT32 i;

  for (i = 0; i + 1u < cap && src[i] != '\0'; i++) {
    dst[i] = src[i];
  }
  dst[i] = '\0';
}

/* ---- replies ---------------------------------------------------------- */

static void answer(const ipc_msg_t *m, UINT16 type, const void *payload,
                   UINT32 len) {
  ipc_send(m->hdr.sender, 0, type, IPC_F_REPLY, payload, len);
}

static void rsp_err(const ipc_msg_t *m) {
  ipc_send(m->hdr.sender, 0, SYSINFO_RSP_ERR, IPC_F_REPLY, 0, 0);
}

/* ---- handlers --------------------------------------------------------- */

static void req_tasks(const ipc_msg_t *m) {
  sysinfo_tasks_rsp_t r;
  sysinfo_list_req_t  q;
  UINT32 i;
  UINT32 live = 0;
  UINT32 slot = sched_task_count();

  if (m->hdr.len < sizeof(q)) {
    rsp_err(m);
    return;
  }
  memcpy(&q, m->payload, sizeof(q));
  memset(&r, 0, sizeof(r));

  /* Count first: the client needs the total to know when to stop paging,
   * and the slot index is what makes a page stable across calls. */
  for (i = 0; i < slot; i++) {
    sched_info_t t;

    if (sched_task_info(i, &t) == 0 && t.state != 3u) {
      live++; /* state 3 = EXITED: still listed, but not alive */
    }
  }
  r.h.total = (UINT16)live;

  {
    UINT32 seen = 0;
    UINT32 out = 0;

    for (i = 0; i < slot && out < q.max && out < SYSINFO_RECS; i++) {
      sched_info_t t;

      if (sched_task_info(i, &t) != 0) {
        continue;
      }
      if (seen++ < q.index) {
        continue;
      }
      r.rec[out].pid = t.pid;
      r.rec[out].ticks = t.ticks;
      r.rec[out].exit_code = t.exit_code;
      r.rec[out].state = t.state;
      r.rec[out].slice = t.slice;
      copy_str(r.rec[out].name, sizeof(r.rec[out].name), t.name);
      out++;
    }
    r.h.n = (UINT8)out;
  }
  answer(m, SYSINFO_RSP_TASKS, &r, sizeof(r));
}

static void req_files(const ipc_msg_t *m) {
  sysinfo_files_rsp_t r;
  sysinfo_list_req_t  q;
  UINT32 n;
  UINT32 i;

  if (m->hdr.len < sizeof(q)) {
    rsp_err(m);
    return;
  }
  memcpy(&q, m->payload, sizeof(q));
  memset(&r, 0, sizeof(r));

  n = dir_count(q.dir);
  r.h.total = (UINT16)n;
  for (i = q.index; i < n && (i - q.index) < q.max && (i - q.index) < SYSINFO_RECS;
       i++) {
    UINT32 k = i - q.index;

    switch (q.dir) {
    case SYSINFO_DIR_APPS:
      copy_str(r.rec[k].name, sizeof(r.rec[k].name), apps_name(i));
      r.rec[k].size = apps_size(i);
      r.rec[k].kind = SYSINFO_KIND_APP;
      break;
    case SYSINFO_DIR_DEV:
      if (i < sizeof(g_dev) / sizeof(g_dev[0])) {
        copy_str(r.rec[k].name, sizeof(r.rec[k].name), g_dev[i].name);
        r.rec[k].kind = g_dev[i].kind;
      } else {
        blkdev_t *dev = blkdev_get_at(i -
                         (UINT32)(sizeof(g_dev) / sizeof(g_dev[0])));
        if (dev != 0) {
          copy_str(r.rec[k].name, sizeof(r.rec[k].name), dev->name);
          r.rec[k].kind = SYSINFO_KIND_DEV;
        }
      }
      break;
    case SYSINFO_DIR_PROC:
      copy_str(r.rec[k].name, sizeof(r.rec[k].name), g_proc[i].name);
      r.rec[k].kind = g_proc[i].kind;
      break;
    default:
      break;
    }
    r.h.n = (UINT8)(k + 1u);
  }
  answer(m, SYSINFO_RSP_FILES, &r, sizeof(r));
}

static void req_stats(const ipc_msg_t *m) {
  sysinfo_stats_t st;
  UINT32          i;
  UINT32          live = 0;

  memset(&st, 0, sizeof(st));
  st.uptime = sched_ticks();
  st.free_kb = pmm_free_count() * 4u;  /* pages are 4 KiB */
  /* Managed RAM = free + reserved. pmm_total_pages() counts up to the top
   * of the physical address space, which on a PC includes the PCI hole and
   * is not "memory"; free + reserved is exactly what the bitmap tracks. */
  st.total_kb = pmm_usable_pages() * 4u;
  for (i = 0; i < sched_task_count(); i++) {
    sched_info_t t;

    if (sched_task_info(i, &t) == 0 && t.state != 3u) {
      live++; /* state 3 = EXITED: still listed, but not alive */
    }
  }
  st.tasks = live;
  st.windows = nwm_window_count();
  {
    const ipc_stats_t *ip = ipc_stats();

    if (ip != 0) {
      st.ipc_sent = ip->delivered;
      st.ipc_drop = ip->dropped;
    }
  }
  st.accent = nwm_accent_get();
  st.wallpaper = nwm_wallpaper_get();
  answer(m, SYSINFO_RSP_STATS, &st, sizeof(st));
}

static void req_accent(const ipc_msg_t *m) {
  sysinfo_accent_t q;
  UINT32           rgb;

  if (m->hdr.len >= sizeof(q)) {
    memcpy(&q, m->payload, sizeof(q));
    if (q.set != 0u) {
      nwm_accent_set(q.rgb);
    }
  }
  rgb = nwm_accent_get();
  answer(m, SYSINFO_RSP_ACCENT, &rgb, sizeof(rgb));
}

/* Theme: one struct, two ids. The reply always carries both values so a
 * client can refresh its swatches without a second round trip. */
static void req_theme(const ipc_msg_t *m) {
  sysinfo_theme_t     q;
  sysinfo_theme_rsp_t r;

  if (m->hdr.len >= sizeof(q)) {
    memcpy(&q, m->payload, sizeof(q));
    if (q.id == 0u && q.value != 0u) {
      nwm_accent_set(q.value);
    } else if (q.id == 1u) {
      nwm_wallpaper_set(q.value);
    } else if (q.id == 2u) {
      nwm_theme_set_mode(q.value);
    }
  }
  memset(&r, 0, sizeof(r));
  r.accent = nwm_accent_get();
  r.wallpaper = nwm_wallpaper_get();
  r.mode = nwm_theme_mode();
  r.resolved = (UINT32)(nd_theme_is_dark() ? 1 : 0);
  r.version = nd_theme_version();
  answer(m, SYSINFO_RSP_THEME, &r, sizeof(r));
}

/* Kill: the reply says what happened, the client shows it. */
static void req_kill(const ipc_msg_t *m) {
  sysinfo_kill_t q;
  INT32          rc;

  if (m->hdr.len < sizeof(q)) {
    rsp_err(m);
    return;
  }
  memcpy(&q, m->payload, sizeof(q));
  rc = sched_kill(q.pid);
  answer(m, SYSINFO_RSP_OK, &rc, sizeof(rc));
}

/* ---- network ---------------------------------------------------------- */

static void req_net(const ipc_msg_t *m) {
  sysinfo_net_t n;
  netdev_t     *d = net_active();
  const net_cfg_t *c = net_cfg();
  UINT32 i;

  for (i = 0; i < sizeof(n); i++) {
    ((UINT8 *)&n)[i] = 0;
  }
  n.ndev = (UINT8)net_count();
  n.dhcp = net_dhcp_state();
  n.ping_state = (UINT8)net_ping_status();
  n.tcp_state = (UINT8)net_tcp_status();
  n.ip = c->ip;
  n.mask = c->mask;
  n.gw = c->gw;
  n.rtt_ms = net_ping_rtt_ms();
  n.tcp_bytes = net_tcp_bytes();
  if (d != 0) {
    n.link = d->link;
    n.state = d->state;
    n.speed_mbps = (UINT16)d->speed_mbps;
    n.rx_pkts = d->rx_pkts;
    n.tx_pkts = d->tx_pkts;
    n.rx_err = d->rx_err;
    n.tx_err = d->tx_err;
    for (i = 0; i < 6u; i++) {
      n.mac[i] = d->mac[i];
    }
  }
  answer(m, SYSINFO_RSP_NET, &n, sizeof(n));
}

static void req_netdevs(const ipc_msg_t *m) {
  sysinfo_netdevs_t out;
  sysinfo_list_req_t q;
  UINT32 i;

  if (m->hdr.len < sizeof(q)) {
    rsp_err(m);
    return;
  }
  memcpy(&q, m->payload, sizeof(q));
  for (i = 0; i < sizeof(out); i++) {
    ((UINT8 *)&out)[i] = 0;
  }
  out.n = 0;
  for (i = 0; i < 2u; i++) {
    netdev_t *d = net_dev((int)(q.index + i));

    if (d == 0) {
      break;
    }
    copy_str(out.dev[i].name, sizeof(out.dev[i].name), d->name);
    out.dev[i].link = d->link;
    out.dev[i].state = d->state;
    {
      UINT32 k;

      for (k = 0; k < 6u; k++) {
        out.dev[i].mac[k] = d->mac[k];
      }
    }
    out.n++;
  }
  answer(m, SYSINFO_RSP_NET, &out, sizeof(out));
}

static void req_ping(const ipc_msg_t *m) {
  UINT32 ip = 0;
  INT32  rc;

  if (m->hdr.len >= 4u) {
    memcpy(&ip, m->payload, 4);
  }
  rc = (INT32)net_ping(ip);
  answer(m, SYSINFO_RSP_OK, &rc, sizeof(rc));
}

static void req_tcp_test(const ipc_msg_t *m) {
  UINT32 ip = 0;
  UINT16 port = 0;
  INT32  rc;

  if (m->hdr.len >= 6u) {
    memcpy(&ip, m->payload, 4);
    memcpy(&port, m->payload + 4, 2);
  }
  rc = (INT32)net_tcp_test(ip, port);
  answer(m, SYSINFO_RSP_OK, &rc, sizeof(rc));
}

/* ---- task manager: rich records and actions --------------------------- */

static void req_tasks2(const ipc_msg_t *m) {
  sysinfo_list_req_t q;
  sysinfo_tasks2_t   r;
  UINT32             i;
  UINT32             live = 0;
  UINT32             seen = 0;
  UINT32             out = 0;

  if (m->hdr.len < sizeof(q)) {
    rsp_err(m);
    return;
  }
  memcpy(&q, m->payload, sizeof(q));
  if (q.max == 0u || q.max > SYSINFO_RECS) {
    q.max = SYSINFO_RECS;
  }
  for (i = 0; i < sizeof(r); i++) {
    ((UINT8 *)&r)[i] = 0;
  }
  for (i = 0; i < SCHED_MAX_TASKS; i++) {
    sched_info2_t t;

    if (sched_task_info2(i, &t) == 0 && t.state != 3u) {
      live++;
    }
  }
  r.total = (UINT16)live;
  for (i = 0; i < SCHED_MAX_TASKS && out < q.max; i++) {
    sched_info2_t t;

    if (sched_task_info2(i, &t) != 0) {
      continue;
    }
    if (t.state == 3u) {
      continue; /* EXITED: not shown in the task manager */
    }
    if (seen++ < q.index) {
      continue;
    }
    memcpy(r.rec + out * sizeof(sched_info2_t), &t, sizeof(t));
    out++;
  }
  r.n = (UINT8)out;
  answer(m, SYSINFO_RSP_TASKS2, &r, sizeof(r));
}

static void req_task_action(const ipc_msg_t *m) {
  sysinfo_tact_t a;
  INT32          rc;

  if (m->hdr.len < sizeof(a)) {
    rsp_err(m);
    return;
  }
  memcpy(&a, m->payload, sizeof(a));
  switch (a.action) {
  case 0:
    rc = (INT32)sched_kill(a.pid);
    break;
  case 1:
    rc = (INT32)sched_suspend(a.pid);
    break;
  case 2:
    rc = (INT32)sched_resume(a.pid);
    break;
  default:
    rc = -1;
    break;
  }
  answer(m, SYSINFO_RSP_OK, &rc, sizeof(rc));
}

static void req_spawn(const ipc_msg_t *m) {
  UINT8 which = 0xFFu;
  INT32 rc;

  if (m->hdr.len >= 1u) {
    which = m->payload[0];
  }
  rc = apps_launch(which);
  if (rc == 0 || rc == APPS_ERUNNING) {
    /* The result travels back so the caller can say "already running"
     * instead of pretending it started a second copy. */
    answer(m, SYSINFO_RSP_OK, &rc, sizeof(rc));
  } else {
    rsp_err(m);
  }
}

/* Name-based launch: the wire format is the task name, zero padded. Kept
 * alongside the M8 index request so adding an app cannot change what an
 * old caller meant. */
static void req_spawn_name(const ipc_msg_t *m) {
  char  name[16];
  UINT32 i;
  INT32 rc;

  if (m->hdr.len == 0u) {
    rsp_err(m);
    return;
  }
  memset(name, 0, sizeof(name));
  for (i = 0; i + 1u < sizeof(name) && i < m->hdr.len; i++) {
    name[i] = (char)m->payload[i];
  }
  rc = apps_launch_name(name);
  if (rc == 0 || rc == APPS_ERUNNING) {
    answer(m, SYSINFO_RSP_OK, &rc, sizeof(rc));
  } else {
    rsp_err(m);
  }
}

static void handle(const ipc_msg_t *m) {
  switch (m->hdr.type) {
  case SYSINFO_REQ_TASKS:
    req_tasks(m);
    break;
  case SYSINFO_REQ_FILES:
    req_files(m);
    break;
  case SYSINFO_REQ_STATS:
    req_stats(m);
    break;
  case SYSINFO_REQ_ACCENT:
    req_accent(m);
    break;
  case SYSINFO_REQ_NET:
    req_net(m);
    break;
  case SYSINFO_REQ_NETDEVS:
    req_netdevs(m);
    break;
  case SYSINFO_REQ_PING:
    req_ping(m);
    break;
  case SYSINFO_REQ_TCP:
    req_tcp_test(m);
    break;
  case SYSINFO_REQ_TASKS2:
    req_tasks2(m);
    break;
  case SYSINFO_REQ_TACT:
    req_task_action(m);
    break;
  case SYSINFO_REQ_SPAWN:
    req_spawn(m);
    break;
  case SYSINFO_REQ_SPAWN_NAME:
    req_spawn_name(m);
    break;
  case SYSINFO_REQ_THEME:
    req_theme(m);
    break;
  case SYSINFO_REQ_KILL:
    req_kill(m);
    break;
  default:
    rsp_err(m);
    break;
  }
}

/* ---- service ---------------------------------------------------------- */

void sysinfo_init(void) {
  if (ipc_port_create(SYSINFO_PORT_NAME, 0, 0, &g_port) != IPC_OK) {
    kputs("[SYS] ERROR: no port for the sysinfo service\n");
    return;
  }
  kputs("[SYS] sysinfo up: files, tasks, stats on port ");
  kputs_hex(g_port);
  kputs("\n");
}

void sysinfo_tick(void) {
  ipc_msg_t msg;
  UINT32    n = 0;

  if (g_port == 0) {
    return;
  }
  /* ipc_recv() returns the payload length on success, not zero: only a
   * zero-length message would look like IPC_OK. The M6 WM loop tests
   * >= 0 for the same reason. */
  while (n < 8u && ipc_recv(g_port, &msg, 0) >= 0) {
    handle(&msg);
    n++;
  }
}

#else /* IPC or WM disabled */

void sysinfo_init(void) {}
void sysinfo_tick(void) {}

#endif
