/**
 * Novus OS - IPC message queues (M6, README part 10.2-10.3).
 *
 * Static everything: a port table, a bounded ring per port, no allocation
 * on the send path, so a misbehaving client can only fill its own queue
 * and is then told IPC_EBUSY.
 */

#include "kernel.h"
#include "ipc.h"
#include "sched.h"

typedef struct {
  UINT8     used;
  char      name[IPC_NAME_MAX];
  UINT64    id;
  UINT64    owner;
  UINT32    head;
  UINT32    tail;
  UINT32    count;
  ipc_msg_t q[IPC_QUEUE_LEN];
} ipc_port_t;

static ipc_port_t  g_ports[IPC_MAX_PORTS];
static ipc_stats_t g_stats;
static UINT64      g_next_id = IPC_PORT_BASE;

/* FNV-1a over the fields the kernel owns plus the payload. Cheap, and it
 * exists to catch a torn queue entry, not to stop a forger - a forger
 * never gets to write the header. */
static UINT32 ipc_checksum(const ipc_hdr_t *h, const UINT8 *payload) {
  UINT32        sum = 0x811C9DC5u;
  UINT32        i;
  const UINT8  *b = (const UINT8 *)h;

  for (i = 0; i < IPC_HDR_SIZE - 4u; i++) { /* everything but the checksum */
    sum = (sum ^ b[i]) * 0x01000193u;
  }
  for (i = 0; i < h->len && i < IPC_PAYLOAD_MAX; i++) {
    sum = (sum ^ payload[i]) * 0x01000193u;
  }
  return sum;
}

static ipc_port_t *port_by_id(UINT64 id) {
  UINT32 i;

  for (i = 0; i < IPC_MAX_PORTS; i++) {
    if (g_ports[i].used && g_ports[i].id == id) {
      return &g_ports[i];
    }
  }
  return 0;
}

void ipc_init(void) {
  memset(g_ports, 0, sizeof(g_ports));
  memset(&g_stats, 0, sizeof(g_stats));
  g_next_id = IPC_PORT_BASE;
  kputs("[IPC] message ports ready: queue ");
  kputs_hex(IPC_QUEUE_LEN);
  kputs(", payload ");
  kputs_hex(IPC_PAYLOAD_MAX);
  kputs(" bytes\n");
}

int ipc_port_create(const char *name, UINT64 owner, UINT64 id_hint,
                    UINT64 *out_id) {
  ipc_port_t *p = 0;
  UINT32      i;

  for (i = 0; i < IPC_MAX_PORTS; i++) {
    if (!g_ports[i].used) {
      p = &g_ports[i];
      break;
    }
  }
  if (p == 0) {
    return IPC_EBUSY;
  }
  memset(p, 0, sizeof(*p));
  p->used = 1;
  p->owner = owner;
  p->id = (id_hint != 0) ? id_hint : g_next_id++;
  if (name != 0) {
    for (i = 0; i + 1u < IPC_NAME_MAX && name[i] != '\0'; i++) {
      p->name[i] = name[i];
    }
    p->name[i] = '\0';
  }
  if (out_id != 0) {
    *out_id = p->id;
  }
  return IPC_OK;
}

int ipc_port_lookup(const char *name, UINT64 *out_id) {
  UINT32 i;

  if (name == 0) {
    return IPC_ENOENT;
  }
  for (i = 0; i < IPC_MAX_PORTS; i++) {
    const char *a = g_ports[i].name;
    const char *b = name;

    if (!g_ports[i].used || g_ports[i].name[0] == '\0') {
      continue;
    }
    while (*a != '\0' && *a == *b) {
      a++;
      b++;
    }
    if (*a == '\0' && *b == '\0') {
      if (out_id != 0) {
        *out_id = g_ports[i].id;
      }
      return IPC_OK;
    }
  }
  return IPC_ENOENT;
}

int ipc_task_attach(UINT64 pid) {
  UINT64 id = 0;
  /* Port id 0 is the recv ABI's "my inbox" sentinel, so pid 0 (the idle
   * task) gets a different one. Nothing ever answers it: the shell is a
   * client of the WM inside the kernel, never over IPC. */
  int    rc = ipc_port_create(0, pid, (pid != 0u) ? pid : IPC_PORT_BASE - 1u,
                              &id);

  if (rc != IPC_OK) {
    kputs("[IPC] warning: no inbox for pid ");
    kputs_hex(pid);
    kputs("\n");
  }
  return rc;
}

void ipc_owner_exit(UINT64 pid) {
  UINT32 i;

  for (i = 0; i < IPC_MAX_PORTS; i++) {
    if (g_ports[i].used && g_ports[i].owner == pid) {
      memset(&g_ports[i], 0, sizeof(g_ports[i]));
    }
  }
}

int ipc_send(UINT64 dst_port, UINT64 sender, UINT16 type, UINT16 flags,
             const void *payload, UINT32 len) {
  ipc_port_t *p = port_by_id(dst_port);
  ipc_msg_t  *m;
  UINT32      next;

  if (p == 0) {
    return IPC_ENOENT;
  }
  if (len > IPC_PAYLOAD_MAX) {
    g_stats.bad++;
    return IPC_EMSGSIZE;
  }
  /* One slot is always kept free so head == tail can never mean both
   * "empty" and "full". */
  next = (p->head + 1u) % IPC_QUEUE_LEN;
  if (next == p->tail) {
    g_stats.dropped++;
    return IPC_EBUSY;
  }
  m = &p->q[p->head];
  memset(m, 0, sizeof(*m));
  m->hdr.magic = IPC_MAGIC;
  m->hdr.type = type;
  m->hdr.flags = flags;
  m->hdr.sender = sender;
  m->hdr.target = dst_port;
  m->hdr.len = len;
  if (payload != 0 && len > 0u) {
    memcpy(m->payload, payload, len);
  }
  m->hdr.checksum = ipc_checksum(&m->hdr, m->payload);
  p->head = next;
  p->count++;
  g_stats.sent++;
  sched_wake_port(dst_port);
  return IPC_OK;
}

int ipc_try_recv(UINT64 port, ipc_msg_t *out) {
  ipc_port_t *p = port_by_id(port);
  ipc_msg_t  *m;

  if (p == 0) {
    return IPC_EBADF;
  }
  while (p->tail != p->head) {
    UINT32 want;

    m = &p->q[p->tail];
    /* Skip a torn entry rather than deliver it, then keep looking: one
     * corrupted message must not wedge the queue. */
    want = ipc_checksum(&m->hdr, m->payload);
    p->tail = (p->tail + 1u) % IPC_QUEUE_LEN;
    if (p->count > 0u) {
      p->count--;
    }
    if (m->hdr.magic != IPC_MAGIC || m->hdr.len > IPC_PAYLOAD_MAX ||
        want != m->hdr.checksum) {
      g_stats.bad++;
      continue;
    }
    if (out != 0) {
      *out = *m;
    }
    g_stats.delivered++;
    return (int)m->hdr.len;
  }
  return IPC_EAGAIN;
}

int ipc_recv(UINT64 port, ipc_msg_t *out, int block) {
  int rc = ipc_try_recv(port, out);

  if (rc != IPC_EAGAIN) {
    return rc;
  }
  if (!block) {
    return IPC_EAGAIN;
  }
  if (sched_current() == 0) {
    return IPC_EAGAIN; /* no task to park (boot-time caller) */
  }
  g_stats.blocked++;
  sched_park(port, 500u); /* 5 s at 100 Hz, then ETIMEDOUT via wait_err */
  return IPC_ERETRY;
}

const ipc_stats_t *ipc_stats(void) { return &g_stats; }

/* ---- self test --------------------------------------------------------- */

static int check(int cond, const char *what) {
  if (!cond) {
    kputs("[IPC] FAIL: ");
    kputs(what);
    kputs("\n");
  }
  return cond;
}

void ipc_selftest(void) {
  UINT64     a = 0;
  UINT64     b = 0;
  UINT64     got = 0;
  ipc_msg_t  msg;
  UINT8      payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  UINT32     i;
  ipc_port_t *pa;
  int        pass = 1;
  int        rc;

  pass &= check(ipc_port_create("selftest.a", 0, 0, &a) == IPC_OK, "port a");
  pass &= check(ipc_port_create("selftest.b", 0, 0, &b) == IPC_OK, "port b");
  pass &= check(a != b, "distinct ids");
  pass &= check(ipc_port_lookup("selftest.a", &got) == IPC_OK && got == a,
                "lookup by name");
  pass &= check(ipc_port_lookup("nope", &got) == IPC_ENOENT, "lookup miss");

  /* Round trip, and the header the receiver sees. */
  memset(&msg, 0, sizeof(msg));
  rc = ipc_send(a, 42u, 0x1234u, IPC_F_SYNC, payload, 8u);
  pass &= check(rc == IPC_OK, "send");
  rc = ipc_recv(a, &msg, 0);
  pass &= check(rc == 8, "recv length");
  pass &= check(msg.hdr.magic == IPC_MAGIC, "magic");
  pass &= check(msg.hdr.type == 0x1234u && msg.hdr.flags == IPC_F_SYNC,
                "type/flags");
  pass &= check(msg.hdr.sender == 42u && msg.hdr.target == a, "identity");
  pass &= check(memcmp(msg.payload, payload, 8) == 0, "payload");

  /* Length limit. */
  pass &= check(ipc_send(a, 0, 1u, 0, payload,
                         IPC_PAYLOAD_MAX + 1u) == IPC_EMSGSIZE,
                "oversize refused");

  /* Flood protection: the queue stops at its documented depth. */
  for (i = 0; i < IPC_QUEUE_LEN; i++) {
    (void)ipc_send(a, 0, 2u, 0, payload, 4u);
  }
  pass &= check(ipc_send(a, 0, 2u, 0, payload, 4u) == IPC_EBUSY,
                "queue full refuses");
  for (i = 0; i < IPC_QUEUE_LEN; i++) {
    (void)ipc_try_recv(a, &msg);
  }

  /* Corruption is detected, not delivered. */
  (void)ipc_send(b, 0, 3u, 0, payload, 4u);
  pa = port_by_id(b);
  if (pa != 0) {
    pa->q[pa->tail].payload[0] ^= 0xFFu;
  }
  pass &= check(ipc_recv(b, &msg, 0) == IPC_EAGAIN, "corrupt entry dropped");

  pass &= check(ipc_send(0xDEADu, 0, 1u, 0, 0, 0) == IPC_ENOENT,
                "unknown port");
  pass &= check(ipc_recv(0xDEADu, &msg, 0) == IPC_EBADF, "unknown recv");

  /* Give the table slots back: the boot-time self test must not eat into
   * the ports real clients need. */
  {
    ipc_port_t *p;

    p = port_by_id(a);
    if (p != 0) {
      memset(p, 0, sizeof(*p));
    }
    p = port_by_id(b);
    if (p != 0) {
      memset(p, 0, sizeof(*p));
    }
  }

  kputs("[IPC] selftest: ");
  kputs(pass ? "PASS" : "FAIL");
  kputs("\n");
}
