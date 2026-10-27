/**
 * Novus OS - minimal TCP client (M10).
 *
 * One connection, used by the network app's echo test and by the NCSI probe:
 * resolve the peer with ARP (through the gateway when the peer is off-link),
 * three-way handshake, one request segment, then collect whatever the peer
 * sends until it closes. No window scaling, no options, no congestion
 * control - a fixed receive buffer, 1 s retransmit, three tries. That is
 * enough to prove the stack talks to a real peer and is honest about being a
 * client.
 */

#include "kernel.h"
#include "sched.h"
#include "net.h"

#if CONFIG_NOVUS_NET

#define TF_FIN 0x01u
#define TF_SYN 0x02u
#define TF_RST 0x04u
#define TF_PSH 0x08u
#define TF_ACK 0x10u

static UINT8  g_phase;
static UINT32 g_dst;
static UINT16 g_dport;
static UINT16 g_sport;
static UINT32 g_iss;     /* initial send sequence */
static UINT32 g_snd;     /* next sequence we would send */
static UINT32 g_snd_una; /* oldest unacknowledged sequence */
static UINT32 g_rcv;     /* next sequence we expect from the peer */
static UINT32 g_deadline;
static UINT8  g_tries;
static UINT8  g_sent;    /* the request segment has been pushed once */
static UINT32 g_txlen;
static UINT32 g_rxlen;
static UINT8  g_tx[NET_TCP_TX_CAP];
static UINT8  g_rx[NET_TCP_RX_CAP];

int          net_tcp_phase(void) { return (int)g_phase; }
UINT32       net_tcp_rx_len(void) { return g_rxlen; }
const UINT8 *net_tcp_rx_data(void) { return g_rx; }
UINT32       net_tcp_bytes(void) { return g_rxlen; }

/* Legacy view for the network app: 0 idle, 1 connecting, 2 replied, 3 failed. */
int net_tcp_status(void) {
  switch (g_phase) {
  case NET_TCP_IDLE:   return 0;
  case NET_TCP_WAIT:   return 1;
  case NET_TCP_OPEN:   return 1;
  case NET_TCP_CLOSED: return (g_rxlen != 0u) ? 2 : 3;
  default:             return 3;
  }
}

static void put16w(UINT8 *p, UINT16 v) {
  p[0] = (UINT8)(v >> 8);
  p[1] = (UINT8)v;
}

static void put32w(UINT8 *p, UINT32 v) {
  p[0] = (UINT8)(v >> 24);
  p[1] = (UINT8)(v >> 16);
  p[2] = (UINT8)(v >> 8);
  p[3] = (UINT8)v;
}

static UINT32 sum_be(UINT32 sum, const UINT8 *p, UINT32 len) {
  UINT32 i;

  for (i = 0; i + 1u < len; i += 2u) {
    sum += ((UINT32)p[i] << 8) | p[i + 1u];
  }
  if ((len & 1u) != 0u) {
    sum += (UINT32)p[len - 1u] << 8;
  }
  return sum;
}

/*
 * Addresses live in memory in wire order, so the four bytes of a UINT32 are
 * already the four pseudo-header bytes. The checksum is computed over that
 * pseudo header plus the segment, both in network byte order.
 */
static UINT16 csum_tcp(UINT32 src, UINT32 dst, const UINT8 *seg, UINT32 len) {
  UINT8  ph[12];
  const UINT8 *sb = (const UINT8 *)&src;
  const UINT8 *db = (const UINT8 *)&dst;
  UINT32 sum = 0;
  UINT32 i;

  for (i = 0; i < 4u; i++) {
    ph[i] = sb[i];
    ph[4u + i] = db[i];
  }
  ph[8] = 0u;
  ph[9] = 6u; /* TCP */
  put16w(ph + 10, (UINT16)len);
  sum = sum_be(sum, ph, sizeof(ph));
  sum = sum_be(sum, seg, len);
  while ((sum >> 16) != 0u) {
    sum = (sum & 0xFFFFu) + (sum >> 16);
  }
  return (UINT16)(~sum & 0xFFFFu);
}

/* Advertised window follows the free space in the receive buffer. */
static UINT16 tcp_window(void) {
  UINT32 free = NET_TCP_RX_CAP - g_rxlen;

  return (UINT16)((free > 4096u) ? 4096u : free);
}

/* Returns 0 when the frame went out, or the net_send_ip result. */
static int tcp_emit(UINT32 seq, UINT8 flags, const UINT8 *data, UINT32 len) {
  UINT8  seg[20u + NET_TCP_TX_CAP];
  UINT32 i;
  int    rc;

  put16w(seg + 0, g_sport);
  put16w(seg + 2, g_dport);
  put32w(seg + 4, seq);
  put32w(seg + 8, g_rcv);
  seg[12] = 0x50u; /* data offset 5 words */
  seg[13] = flags;
  put16w(seg + 14, tcp_window());
  put16w(seg + 16, 0);
  put16w(seg + 18, 0);
  for (i = 0; i < len; i++) {
    seg[20u + i] = data[i];
  }
  put16w(seg + 16, csum_tcp(net_cfg()->ip, g_dst, seg, 20u + len));
  rc = net_send_ip(g_dst, 6u, seg, 20u + len);
  return rc;
}

/*
 * The sequence space is tracked explicitly: SYN and FIN consume one number,
 * payload consumes its length, and only the ACK from the peer advances the
 * unacknowledged edge. The request is a single segment; there is no
 * out-of-order queue, so a gap in the reply is answered with the last
 * contiguous ACK and left to the peer to retransmit.
 */
int net_tcp_open(UINT32 ip, UINT16 port, const UINT8 *tx, UINT32 txlen) {
  UINT32 i;

  if (net_active() == 0 || net_cfg()->ip == 0u || txlen > NET_TCP_TX_CAP) {
    return -1;
  }
  for (i = 0; i < txlen; i++) {
    g_tx[i] = tx[i];
  }
  g_txlen = txlen;
  g_dst = ip;
  g_dport = port;
  g_sport = (UINT16)(40000u + (sched_ticks() & 0x3FFu));
  g_iss = 0x10000u + (sched_ticks() << 8);
  g_snd = g_iss + 1u;
  g_snd_una = g_iss + 1u;
  g_rcv = 0;
  g_rxlen = 0;
  g_sent = 0;
  g_tries = 0;
  g_phase = NET_TCP_WAIT;
  g_deadline = sched_ticks() + 100u;
  (void)tcp_emit(g_iss, TF_SYN, 0, 0); /* ARP miss is retried by the tick */
  kputs("[NET] tcp connect\n");
  return 0;
}

void net_tcp_close(void) {
  g_phase = NET_TCP_IDLE;
  g_rxlen = 0;
  g_txlen = 0;
  g_sent = 0;
}

/* Echo probe kept for the network app: push a short line to a known peer. */
int net_tcp_test(UINT32 ip, UINT16 port) {
  static const char hello[] = "hello from novus\n";

  return net_tcp_open(ip, port, (const UINT8 *)hello, sizeof(hello) - 1u);
}

void net_tcp_tick(void) {
  UINT32 now = sched_ticks();

  if (g_phase != NET_TCP_WAIT && g_phase != NET_TCP_OPEN) {
    return;
  }
  if (now < g_deadline) {
    return;
  }
  if (++g_tries > 3u) {
    g_phase = NET_TCP_FAIL;
    kputs("[NET] tcp timeout\n");
    return;
  }
  g_deadline = now + 100u;
  if (g_phase == NET_TCP_WAIT) {
    (void)tcp_emit(g_iss, TF_SYN, 0, 0);
  } else if (!g_sent) {
    if (g_txlen != 0u) {
      (void)tcp_emit(g_snd, TF_ACK | TF_PSH, g_tx, g_txlen);
    } else {
      (void)tcp_emit(g_snd, TF_ACK, 0, 0);
    }
  } else if (g_snd_una != g_snd) {
    (void)tcp_emit(g_snd_una, TF_ACK | TF_PSH, g_tx, g_txlen);
  }
  /* Otherwise the request is acknowledged and we are only waiting for the
   * reply, which may legitimately take longer than one retransmit period. */
}

/*
 * The timer retries the action associated with the current coarse state and
 * fails after three expirations. It does not implement congestion control or
 * selective acknowledgement, so this is a bounded probe, not general TCP
 * recovery.
 */
void net_tcp_input(const UINT8 *p, UINT32 len, UINT32 src) {
  UINT16 sport;
  UINT16 dport;
  UINT32 seq;
  UINT32 ack;
  UINT8  flags;
  UINT32 hdr;
  UINT32 plen;
  UINT32 i;

  if (len < 20u || g_phase == NET_TCP_IDLE || g_phase == NET_TCP_FAIL ||
      g_phase == NET_TCP_CLOSED) {
    return;
  }
  if (src != g_dst) {
    return;
  }
  sport = (UINT16)(((UINT16)p[0] << 8) | p[1]);
  dport = (UINT16)(((UINT16)p[2] << 8) | p[3]);
  if (dport != g_sport || sport != g_dport) {
    return;
  }
  seq = ((UINT32)p[4] << 24) | ((UINT32)p[5] << 16) | ((UINT32)p[6] << 8) | p[7];
  ack = ((UINT32)p[8] << 24) | ((UINT32)p[9] << 16) | ((UINT32)p[10] << 8) | p[11];
  flags = p[13];
  hdr = (UINT32)(p[12] >> 4) * 4u;
  if (hdr < 20u || len < hdr) {
    return;
  }
  plen = len - hdr;

  if ((flags & TF_RST) != 0u) {
    g_phase = NET_TCP_FAIL;
    kputs("[NET] tcp reset\n");
    return;
  }
  if ((flags & TF_SYN) != 0u && (flags & TF_ACK) != 0u &&
      g_phase == NET_TCP_WAIT) {
    g_rcv = seq + 1u;
    g_snd = ack;
    g_snd_una = ack;
    g_phase = NET_TCP_OPEN;
    g_tries = 0;
    g_deadline = sched_ticks() + 100u;
    (void)tcp_emit(g_snd, TF_ACK, 0, 0);
    if (g_txlen != 0u) {
      (void)tcp_emit(g_snd, TF_ACK | TF_PSH, g_tx, g_txlen);
      g_snd += g_txlen;
    }
    g_sent = 1;
    kputs("[NET] tcp established\n");
    return;
  }
  if ((flags & TF_ACK) != 0u && ack > g_snd_una && ack <= g_snd) {
    g_snd_una = ack;
    g_tries = 0;
    g_deadline = sched_ticks() + 100u;
  }
  if (plen > 0u) {
    if (seq == g_rcv) {
      UINT32 free = NET_TCP_RX_CAP - g_rxlen;
      UINT32 take = (plen < free) ? plen : free;

      for (i = 0; i < take; i++) {
        g_rx[g_rxlen + i] = p[hdr + i];
      }
      g_rxlen += take;
      g_rcv += plen;
      kputs("[NET] tcp rx ");
      kputs_hex((UINT64)g_rxlen);
      kputs(" bytes\n");
    }
    (void)tcp_emit(g_snd, TF_ACK, 0, 0);
  }
  if ((flags & TF_FIN) != 0u) {
    g_rcv++;
    (void)tcp_emit(g_snd, TF_ACK, 0, 0);
    g_phase = NET_TCP_CLOSED;
    kputs("[NET] tcp closed\n");
  }
}

#else

int          net_tcp_phase(void) { return 0; }
UINT32       net_tcp_rx_len(void) { return 0; }
const UINT8 *net_tcp_rx_data(void) { return 0; }
int          net_tcp_open(UINT32 ip, UINT16 port, const UINT8 *tx, UINT32 l) {
  (void)ip; (void)port; (void)tx; (void)l;
  return -1;
}
void net_tcp_close(void) {}
int net_tcp_test(UINT32 ip, UINT16 port) { (void)ip; (void)port; return -1; }
int net_tcp_status(void) { return 0; }
UINT32 net_tcp_bytes(void) { return 0; }
void net_tcp_input(const UINT8 *p, UINT32 l, UINT32 s) { (void)p; (void)l; (void)s; }
void net_tcp_tick(void) {}

#endif
