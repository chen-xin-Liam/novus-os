/**
 * Novus OS - DHCP client (M10).
 *
 * Four messages, one lease: DISCOVER -> OFFER -> REQUEST -> ACK, all
 * broadcast from 0.0.0.0:68 to 255.255.255.255:67, retried once a second
 * for eight seconds. If the server never answers (QEMU can be started
 * without its user-mode network) the client falls back to the same static
 * address the emulated network uses, and says so in the log.
 */

#include "kernel.h"
#include "sched.h"
#include "net.h"

#if CONFIG_NOVUS_NET

#define DHCP_MAGIC 0x63825363u

static UINT32 g_xid;
static UINT8  g_tries;
static UINT32 g_deadline;
static UINT32 g_offered;
static UINT32 g_server;
static UINT8  g_phase; /* 1 = wait offer, 2 = wait ack */

static UINT32 get32be(const UINT8 *p) {
  return ((UINT32)p[0] << 24) | ((UINT32)p[1] << 16) | ((UINT32)p[2] << 8) |
         p[3];
}

/* Wire bytes -> our address type (little endian, like NET_IP). */
static UINT32 ip_from_wire_le(const UINT8 *p) {
  return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) |
         ((UINT32)p[3] << 24);
}

static UINT8 *opt_put(UINT8 *p, UINT8 code, UINT8 len, const UINT8 *val) {
  UINT32 i;

  *p++ = code;
  *p++ = len;
  for (i = 0; i < len; i++) {
    *p++ = val[i];
  }
  return p;
}

static void dhcp_send(UINT8 type) {
  UINT8  msg[300];
  UINT8 *p = msg;
  UINT32 i;
  UINT32 zero = 0;
  UINT8  opt;

  for (i = 0; i < sizeof(msg); i++) {
    msg[i] = 0;
  }
  msg[0] = 1;    /* BOOTREQUEST */
  msg[1] = 1;    /* ethernet */
  msg[2] = 6;    /* hw length */
  for (i = 0; i < 4u; i++) {
    msg[4u + i] = (UINT8)(g_xid >> (24u - i * 8u));
  }
  p = msg + 28;  /* ciaddr .. giaddr stay zero */
  (void)p;
  (void)zero;
  {
    netdev_t *d = net_active();

    for (i = 0; i < 6u && d != 0; i++) {
      msg[28u + i] = d->mac[i]; /* chaddr */
    }
  }
  msg[236] = 0x63;
  msg[237] = 0x82;
  msg[238] = 0x53;
  msg[239] = 0x63;
  p = msg + 240;
  opt = type;
  p = opt_put(p, 53, 1, &opt);
  if (type == 3u) { /* REQUEST carries what we were offered */
    p = opt_put(p, 50, 4, (const UINT8 *)&g_offered);
    p = opt_put(p, 54, 4, (const UINT8 *)&g_server);
  }
  {
    UINT8 params[3] = {1, 3, 51}; /* mask, router, lease */

    p = opt_put(p, 55, 3, params);
  }
  *p++ = 255;
  (void)net_udp_send(0xFFFFFFFFu, 68u, 67u, msg, (UINT32)(p - msg));
}

void net_dhcp_start(void) {
  g_xid = sched_ticks() ^ 0x5A5A0000u;
  g_tries = 0;
  g_phase = 1;
  g_deadline = sched_ticks() + 100u;
  dhcp_send(1u); /* DISCOVER */
  kputs("[NET] dhcp discover\n");
}

void net_dhcp_tick(void) {
  if (g_phase == 0u) {
    return;
  }
  if (sched_ticks() < g_deadline) {
    return;
  }
  if (++g_tries > 8u) {
    kputs("[NET] dhcp timeout, using the emulated-network defaults\n");
    net_dhcp_apply(NET_IP(10, 0, 2, 15), NET_IP(255, 255, 255, 0),
                   NET_IP(10, 0, 2, 2));
    return;
  }
  g_deadline = sched_ticks() + 100u;
  dhcp_send((g_phase == 1u) ? 1u : 3u);
}

void net_dhcp_input(const UINT8 *p, UINT32 len) {
  UINT32 off;
  UINT8  type = 0;
  UINT32 mask = NET_IP(255, 255, 255, 0);
  UINT32 gw = 0;
  UINT32 yiaddr;

  if (len < 240u || p[0] != 2u) {
    return;
  }
  if (get32be(p + 4) != g_xid) {
    return;
  }
  if (get32be(p + 236) != DHCP_MAGIC) {
    return;
  }
  yiaddr = ip_from_wire_le(p + 16);
  off = 240;
  while (off + 2u < len && p[off] != 255u) {
    UINT8 code = p[off];
    UINT8 olen = p[off + 1u];

    if (off + 2u + olen > len) {
      break;
    }
    if (code == 53 && olen >= 1u) {
      type = p[off + 2u];
    } else if (code == 1 && olen >= 4u) {
      mask = ip_from_wire_le(p + off + 2u);
    } else if (code == 3 && olen >= 4u) {
      gw = ip_from_wire_le(p + off + 2u);
    } else if (code == 54 && olen >= 4u) {
      g_server = ip_from_wire_le(p + off + 2u);
    }
    off += (UINT32)olen + 2u;
  }

  if (type == 2u && g_phase == 1u) { /* OFFER */
    g_offered = yiaddr;
    g_phase = 2;
    g_tries = 0;
    g_deadline = sched_ticks() + 100u;
    kputs("[NET] dhcp offer\n");
    dhcp_send(3u); /* REQUEST */
  } else if (type == 5u && g_phase == 2u) { /* ACK */
    g_phase = 0; /* stop retransmitting; the lease is ours */
    net_dhcp_apply(yiaddr, mask, gw);
  }
}

#else

void net_dhcp_start(void) {}
void net_dhcp_tick(void) {}
void net_dhcp_input(const UINT8 *p, UINT32 l) { (void)p; (void)l; }

#endif
