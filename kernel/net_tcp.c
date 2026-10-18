/**
 * Novus OS - minimal TCP client (M10).
 *
 * One connection, used by the network app's echo test: resolve the server
 * with ARP, three-way handshake, one data segment each way, then FIN.
 * No window scaling, no options, no congestion control - a fixed 4 KB
 * window, 1 s retransmit, three tries. That is enough to prove the stack
 * talks to a real peer and is honest about being a client.
 */

#include "kernel.h"
#include "sched.h"
#include "net.h"

#if CONFIG_NOVUS_NET

#define TF_FIN 0x01u
#define TF_SYN 0x02u /* SYN */
#define TF_RST 0x04u
#define TF_PSH 0x08u
#define TF_ACK 0x10u

#define ST_IDLE   0u
#define ST_ARP    1u
#define ST_SYN    2u
#define ST_ESTAB  3u
#define ST_OK     4u
#define ST_FAIL   5u

static UINT8  g_state;
static UINT32 g_dst;
static UINT16 g_dport;
static UINT16 g_sport;
static UINT32 g_snd;
static UINT32 g_rcv;
static UINT32 g_deadline;
static UINT8  g_tries;
static UINT32 g_bytes;

int net_tcp_status(void) { return g_state; }
UINT32 net_tcp_bytes(void) { return g_bytes; }

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

static UINT16 csum_tcp(UINT32 src, UINT32 dst, const UINT8 *seg, UINT32 len) {
  UINT32 sum = 0;
  UINT32 i;

  /* Pseudo header: src, dst, zero, protocol, length. */
  sum += (src & 0xFFFFu) + ((src >> 8) & 0xFF00u);
  sum += (dst & 0xFFFFu) + ((dst >> 8) & 0xFF00u);
  sum += 6u;
  sum += len;
  for (i = 0; i + 1u < len; i += 2u) {
    sum += ((UINT32)seg[i] << 8) | seg[i + 1u];
  }
  if ((len & 1u) != 0u) {
    sum += (UINT32)seg[len - 1u] << 8;
  }
  while ((sum >> 16) != 0u) {
    sum = (sum & 0xFFFFu) + (sum >> 16);
  }
  return (UINT16)(~sum & 0xFFFFu);
}

static void tcp_send_flags(UINT8 flags, const UINT8 *data, UINT32 len) {
  UINT8  seg[256];
  UINT32 i;

  put16w(seg + 0, g_sport);
  put16w(seg + 2, g_dport);
  put32w(seg + 4, g_snd);
  put32w(seg + 8, g_rcv);
  seg[12] = 0x50u; /* data offset 5 words */
  seg[13] = flags;
  put16w(seg + 14, 4096u); /* window */
  put16w(seg + 16, 0);
  put16w(seg + 18, 0);
  for (i = 0; i < len; i++) {
    seg[20u + i] = data[i];
  }
  put16w(seg + 16, csum_tcp(net_cfg()->ip, g_dst, seg, 20u + len));
  if (net_send_ip(g_dst, 6u, seg, 20u + len) == 0) {
    g_snd += len;
    if ((flags & TF_SYN) != 0u || (flags & TF_FIN) != 0u) {
      g_snd++; /* SYN and FIN consume a sequence number */
    }
  }
}

int net_tcp_test(UINT32 ip, UINT16 port) {
  if (net_active() == 0 || net_cfg()->ip == 0u) {
    return -1;
  }
  g_dst = ip;
  g_dport = port;
  g_sport = (UINT16)(40000u + (sched_ticks() & 0x3FFu));
  g_snd = 0x10000u + (sched_ticks() << 8);
  g_rcv = 0;
  g_bytes = 0;
  g_tries = 0;
  g_state = ST_ARP;
  g_deadline = sched_ticks() + 100u;
  net_arp_request(ip);
  kputs("[NET] tcp connect\n");
  return 0;
}

void net_tcp_tick(void) {
  UINT32 now = sched_ticks();

  if (g_state == ST_IDLE || g_state == ST_OK || g_state == ST_FAIL) {
    return;
  }
  if (now < g_deadline) {
    return;
  }
  if (++g_tries > 3u) {
    g_state = ST_FAIL;
    kputs("[NET] tcp timeout\n");
    return;
  }
  g_deadline = now + 100u;
  if (g_state == ST_ARP) {
    net_arp_request(g_dst);
    g_state = ST_SYN;
    tcp_send_flags(TF_SYN, 0, 0);
  } else if (g_state == ST_SYN) {
    tcp_send_flags(TF_SYN, 0, 0);
  } else if (g_state == ST_ESTAB) {
    static const char hello[] = "hello from novus\n";

    tcp_send_flags(TF_ACK | TF_PSH, (const UINT8 *)hello,
                   sizeof(hello) - 1u);
  }
}

void net_tcp_input(const UINT8 *p, UINT32 len, UINT32 src) {
  UINT16 sport;
  UINT16 dport;
  UINT32 seq;
  UINT32 ack;
  UINT8  flags;
  UINT32 hdr;
  UINT32 plen;
  UINT32 i;

  if (len < 20u || src != g_dst) {
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
    g_state = ST_FAIL;
    kputs("[NET] tcp reset\n");
    return;
  }
  if ((flags & TF_SYN) != 0u && (flags & TF_ACK) != 0u && g_state == ST_SYN) {
    g_rcv = seq + 1u;
    g_snd = ack;
    g_state = ST_ESTAB;
    g_tries = 0;
    g_deadline = sched_ticks() + 100u;
    tcp_send_flags(TF_ACK, 0, 0);
    {
      static const char hello[] = "hello from novus\n";

      tcp_send_flags(TF_ACK | TF_PSH, (const UINT8 *)hello,
                     sizeof(hello) - 1u);
    }
    kputs("[NET] tcp established\n");
    return;
  }
  if (plen > 0u && g_state == ST_ESTAB) {
    g_bytes += plen;
    g_rcv += plen;
    tcp_send_flags(TF_ACK, 0, 0);
    g_state = ST_OK;
    kputs("[NET] tcp echo ");
    kputs_hex(g_bytes);
    kputs(" bytes\n");
    /* Close politely; the peer may or may not answer the FIN. */
    tcp_send_flags(TF_ACK | TF_FIN, 0, 0);
    return;
  }
  if ((flags & TF_FIN) != 0u) {
    g_rcv++;
    tcp_send_flags(TF_ACK, 0, 0);
    g_state = (g_bytes != 0u) ? ST_OK : ST_FAIL;
  }
  (void)i;
}

#else

int net_tcp_test(UINT32 ip, UINT16 port) { (void)ip; (void)port; return -1; }
int net_tcp_status(void) { return 0; }
UINT32 net_tcp_bytes(void) { return 0; }
void net_tcp_input(const UINT8 *p, UINT32 l, UINT32 s) { (void)p; (void)l; (void)s; }
void net_tcp_tick(void) {}

#endif
