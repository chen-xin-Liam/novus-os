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
#include "sysinfo.h"

#if CONFIG_NOVUS_IPC && CONFIG_NOVUS_WM

static UINT64 g_port;

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
    return (UINT32)(sizeof(g_dev) / sizeof(g_dev[0]));
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

    if (sched_task_info(i, &t) == 0) {
      live++;
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
      copy_str(r.rec[k].name, sizeof(r.rec[k].name), g_dev[i].name);
      r.rec[k].kind = g_dev[i].kind;
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
  st.free_kb = pmm_free_count() * 4u; /* pages are 4 KiB */
  for (i = 0; i < sched_task_count(); i++) {
    sched_info_t t;

    if (sched_task_info(i, &t) == 0) {
      live++;
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
  case SYSINFO_REQ_SPAWN:
    req_spawn(m);
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
