/**
 * Novus OS - DNS resolver (M10 follow-up).
 *
 * One A-record query at a time: build the question name, send it to the
 * server the DHCP lease named, retry three times, then answer with the first
 * A record in the response. Names are not compressed on the way out and only
 * the simple pointer-free form is accepted on the way back, which is all an
 * ordinary recursive resolver needs to say.
 */

#include "kernel.h"
#include "sched.h"
#include "net.h"

#if CONFIG_NOVUS_NET

static UINT32 g_server;
static UINT16 g_id;
static UINT8  g_state; /* 0 idle 1 running 2 ok 3 fail */
static UINT32 g_addr;
static UINT32 g_deadline;
static UINT8  g_tries;
static UINT8  g_query[160];
static UINT32 g_qlen;

void net_dns_set_server(UINT32 ip) { g_server = ip; }

UINT32 net_dns_server(void) { return g_server; }

int    net_dns_status(void) { return (int)g_state; }
UINT32 net_dns_addr(void) { return g_addr; }

static void put16w(UINT8 *p, UINT16 v) {
  p[0] = (UINT8)(v >> 8);
  p[1] = (UINT8)v;
}

static UINT16 get16w(const UINT8 *p) {
  return (UINT16)(((UINT16)p[0] << 8) | p[1]);
}

/* Wire bytes -> our address type, whose memory order is the wire order. */
static UINT32 ip_from_wire(const UINT8 *p) {
  return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) |
         ((UINT32)p[3] << 24);
}

/* "a.b.c" -> 3 a 1 b 1 c 0; returns the encoded length or 0 on a bad name. */
static UINT32 encode_name(const char *name, UINT8 *out, UINT32 cap) {
  UINT32 n = 0;
  UINT32 i = 0;

  while (name[i] != '\0') {
    UINT32 start = i;
    UINT32 lbl;

    while (name[i] != '\0' && name[i] != '.') {
      i++;
    }
    lbl = i - start;
    if (lbl == 0u || lbl > 63u || n + lbl + 2u > cap) {
      return 0;
    }
    out[n++] = (UINT8)lbl;
    while (start < i) {
      out[n++] = (UINT8)name[start++];
    }
    if (name[i] == '.') {
      i++;
    }
  }
  if (n + 1u > cap) {
    return 0;
  }
  out[n++] = 0u;
  return n;
}

static void dns_send(void) {
  if (g_qlen == 0u || g_server == 0u) {
    return;
  }
  /* A miss on the ARP cache is not fatal: the next tick retries. */
  (void)net_udp_send(g_server, NET_DNS_LOCAL_PORT, 53u, g_query, g_qlen);
}

int net_dns_resolve(const char *name) {
  UINT32 n;

  g_addr = 0;
  g_state = 3;
  g_qlen = 0;
  if (net_active() == 0 || net_cfg()->ip == 0u || g_server == 0u) {
    kputs("[DNS] no link or no server\n");
    return -1;
  }
  /* Header: id, flags (RD), qdcount 1, the rest zero. */
  g_id = (UINT16)(sched_ticks() * 37u + 1u);
  g_query[0] = (UINT8)(g_id >> 8);
  g_query[1] = (UINT8)g_id;
  g_query[2] = 0x01u;
  g_query[3] = 0x00u;
  g_query[4] = 0x00u;
  g_query[5] = 0x01u;
  g_query[6] = 0x00u;
  g_query[7] = 0x00u;
  g_query[8] = 0x00u;
  g_query[9] = 0x00u;
  g_query[10] = 0x00u;
  g_query[11] = 0x00u;
  n = 12u + encode_name(name, g_query + 12, (UINT32)sizeof(g_query) - 14u);
  if (n <= 12u) {
    kputs("[DNS] bad name\n");
    return -1;
  }
  put16w(g_query + n, 1u); /* QTYPE A */
  put16w(g_query + n + 2u, 1u); /* QCLASS IN */
  g_qlen = n + 4u;
  g_tries = 0;
  g_deadline = sched_ticks() + 100u;
  g_state = 1;
  dns_send();
  kputs("[DNS] query ");
  kputs(name);
  kputs("\n");
  return 0;
}

void net_dns_tick(void) {
  UINT32 now = sched_ticks();

  if (g_state != 1u || now < g_deadline) {
    return;
  }
  if (++g_tries > 3u) {
    g_state = 3;
    kputs("[DNS] timeout\n");
    return;
  }
  g_deadline = now + 100u;
  dns_send();
}

/* Dotted form for the log; the raw value is stored in wire byte order. */
static void dns_log_ip(UINT32 ip) {
  char   s[16];
  UINT32 n = 0;
  UINT32 i;

  for (i = 0; i < 4u; i++) {
    UINT32 v = (ip >> (i * 8u)) & 0xFFu;

    if (v >= 100u) {
      s[n++] = (char)('0' + v / 100u);
    }
    if (v >= 10u) {
      s[n++] = (char)('0' + (v / 10u) % 10u);
    }
    s[n++] = (char)('0' + v % 10u);
    if (i < 3u) {
      s[n++] = '.';
    }
  }
  s[n] = '\0';
  kputs(s);
}

/* Skip a (possibly compressed) name; returns the offset after it. */
static UINT32 skip_name(const UINT8 *p, UINT32 len, UINT32 off) {
  while (off < len) {
    UINT8 c = p[off];

    if (c == 0u) {
      return off + 1u;
    }
    if ((c & 0xC0u) == 0xC0u) {
      return off + 2u;
    }
    off += (UINT32)c + 1u;
  }
  return len;
}

void net_dns_input(const UINT8 *p, UINT32 len, UINT32 src) {
  UINT32 off;
  UINT32 ancount;
  UINT32 i;

  if (g_state != 1u || len < 12u || src != g_server) {
    return;
  }
  if (get16w(p) != g_id) {
    return;
  }
  if ((p[2] & 0x80u) == 0u) {
    return; /* not a response */
  }
  if ((p[3] & 0x0Fu) != 0u) {
    g_state = 3;
    kputs("[DNS] server error\n");
    return;
  }
  ancount = get16w(p + 6);
  off = 12u;
  for (i = 0; i < (UINT32)get16w(p + 4); i++) { /* questions */
    off = skip_name(p, len, off);
    if (off + 4u > len) {
      return;
    }
    off += 4u;
  }
  for (i = 0; i < ancount; i++) { /* answers */
    UINT16 type;
    UINT16 cls;
    UINT16 rdlen;

    off = skip_name(p, len, off);
    if (off + 10u > len) {
      return;
    }
    type = get16w(p + off);
    cls = get16w(p + off + 2u);
    rdlen = get16w(p + off + 8u);
    off += 10u;
    if (off + rdlen > len) {
      return;
    }
    if (type == 1u && cls == 1u && rdlen == 4u) {
      g_addr = ip_from_wire(p + off);
      g_state = 2;
      kputs("[DNS] answer ");
      dns_log_ip(g_addr);
      kputs("\n");
      return;
    }
    off += rdlen;
  }
  g_state = 3;
  kputs("[DNS] no A record\n");
}

#else

void   net_dns_set_server(UINT32 ip) { (void)ip; }
UINT32 net_dns_server(void) { return 0; }
int    net_dns_resolve(const char *name) { (void)name; return -1; }
int    net_dns_status(void) { return 0; }
UINT32 net_dns_addr(void) { return 0; }
void   net_dns_input(const UINT8 *p, UINT32 l, UINT32 s) { (void)p; (void)l; (void)s; }
void   net_dns_tick(void) {}

#endif
