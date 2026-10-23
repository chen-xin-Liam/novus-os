/**
 * Novus OS - network core: devices, framing, ARP, IPv4, ICMP (M10).
 */

#include "kernel.h"
#include "pmm.h"
#include "sched.h"
#include "pci.h"
#include "net.h"

#if CONFIG_NOVUS_NET

#define ETH_HDR   14u
#define ETH_ARP   0x0608u /* 0x0806 on the wire, read little endian */
#define ETH_IPV4  0x0008u /* 0x0800 */
#define IP_ICMP   1u
#define IP_UDP    17u
#define IP_TCP    6u

#define NET_FRAME_MAX 1560u

static netdev_t  g_devs[NET_MAX_DEVS];
static int       g_ndev;
static netdev_t *g_active;
static net_cfg_t g_cfg;
static UINT8     g_dhcp_state;
static UINT32    g_dhcp_since;
static UINT8     g_rx[NET_FRAME_MAX];
static UINT8     g_tx[NET_FRAME_MAX];

/* ---- helpers ---------------------------------------------------------- */

static void put16(UINT8 *p, UINT16 v) {
  p[0] = (UINT8)(v >> 8);
  p[1] = (UINT8)v;
}

static UINT16 get16(const UINT8 *p) {
  return (UINT16)(((UINT16)p[0] << 8) | p[1]);
}

/* Wire order <-> UINT32 (our address type is "bytes in memory order"). */
static UINT32 ip_from_wire(const UINT8 *p) {
  return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) |
         ((UINT32)p[3] << 24);
}

static void ip_to_wire(UINT8 *p, UINT32 ip) {
  p[0] = (UINT8)ip;
  p[1] = (UINT8)(ip >> 8);
  p[2] = (UINT8)(ip >> 16);
  p[3] = (UINT8)(ip >> 24);
}

static UINT16 csum(const UINT8 *p, UINT32 len) {
  UINT32 sum = 0;
  UINT32 i;

  for (i = 0; i + 1u < len; i += 2u) {
    sum += ((UINT32)p[i] << 8) | p[i + 1u];
  }
  if ((len & 1u) != 0u) {
    sum += (UINT32)p[len - 1u] << 8;
  }
  while ((sum >> 16) != 0u) {
    sum = (sum & 0xFFFFu) + (sum >> 16);
  }
  return (UINT16)(~sum & 0xFFFFu);
}

/* ---- device registry -------------------------------------------------- */

int net_count(void) { return g_ndev; }

netdev_t *net_dev(int i) {
  return (i >= 0 && i < g_ndev) ? &g_devs[i] : 0;
}

netdev_t *net_active(void) { return g_active; }

const net_cfg_t *net_cfg(void) { return &g_cfg; }

UINT8 net_dhcp_state(void) { return g_dhcp_state; }

UINT32 net_dhcp_leased_ms(void) {
  return (g_dhcp_state == NET_DHCP_BOUND) ? (sched_ticks() - g_dhcp_since) * 10u
                                          : 0u;
}

const char *net_link_name(UINT8 link) {
  switch (link) {
  case NET_LINK_ETHER: return "以太网";
  case NET_LINK_FIBER: return "光纤/SFP";
  case NET_LINK_WIFI:  return "无线";
  default:             return "未知";
  }
}

const char *net_state_name(UINT8 state) {
  switch (state) {
  case NET_STATE_UP:    return "已连接";
  case NET_STATE_DOWN:  return "未连接";
  case NET_STATE_NODRV: return "无驱动";
  default:              return "未知";
  }
}

void net_register(netdev_t *d) {
  if (d == 0 || g_ndev >= NET_MAX_DEVS) {
    return;
  }
  g_devs[g_ndev++] = *d;
  if (g_active == 0 && d->state == NET_STATE_UP) {
    g_active = &g_devs[g_ndev - 1];
  }
}

int net_send_frame(const UINT8 *frame, UINT32 len) {
  if (g_active == 0 || g_active->send == 0) {
    return -1;
  }
  if (g_active->send(g_active, frame, len) != 0) {
    g_active->tx_err++;
    return -1;
  }
  g_active->tx_pkts++;
  return 0;
}

/*
 * Device selection is intentionally centralized: protocol code always sends
 * through the first registered UP device rather than choosing a NIC itself.
 * The driver callback owns hardware submission; this layer accounts only
 * successful submissions and reports a failed callback as a TX error.
 */

/* ---- ARP -------------------------------------------------------------- */

typedef struct {
  UINT32 ip;
  UINT8  mac[6];
  UINT8  valid;
  UINT32 age;
} arp_entry_t;

static arp_entry_t g_arp[8];
static UINT32      g_arp_log;

int net_arp_lookup(UINT32 ip, UINT8 *mac) {
  int i;

  for (i = 0; i < 8; i++) {
    if (g_arp[i].valid && g_arp[i].ip == ip) {
      if (mac != 0) {
        UINT32 k;
        for (k = 0; k < 6u; k++) {
          mac[k] = g_arp[i].mac[k];
        }
      }
      g_arp[i].age = sched_ticks();
      return 1;
    }
  }
  return 0;
}

void net_arp_request(UINT32 ip) {
  UINT8 *f = g_tx;
  UINT32 i;

  for (i = 0; i < 6u; i++) {
    f[i] = 0xFFu;  /* broadcast */
    f[6u + i] = g_active->mac[i];
  }
  put16(f + 12, 0x0806u);
  put16(f + 14, 1u);           /* ethernet */
  put16(f + 16, 0x0800u);      /* IPv4 */
  f[18] = 6u;
  f[19] = 4u;
  put16(f + 20, 1u);           /* request */
  for (i = 0; i < 6u; i++) {
    f[22u + i] = g_active->mac[i];
    f[32u + i] = 0u;
  }
  ip_to_wire(f + 28, g_cfg.ip);
  ip_to_wire(f + 38, ip);
  (void)net_send_frame(f, 42u);
}

void net_arp_input(const UINT8 *p, UINT32 len) {
  UINT32 ip;
  UINT8  mac[6];
  int    i;
  int    slot = -1;

  /* `p` points at the ARP payload, not the frame: 28 bytes for IPv4 over
   * Ethernet. Operation is at 6, sender MAC at 8, sender IP at 14. */
  if (len < 28u || ((UINT16)p[6] << 8 | p[7]) != 2u) {
    return; /* only replies teach us anything here */
  }
  for (i = 0; i < 6; i++) {
    mac[i] = p[8u + i];
  }
  ip = (UINT32)p[14] | ((UINT32)p[15] << 8) | ((UINT32)p[16] << 16) |
       ((UINT32)p[17] << 24);
  for (i = 0; i < 8; i++) {
    if (g_arp[i].valid && g_arp[i].ip == ip) {
      slot = i;
      break;
    }
    if (slot < 0 && !g_arp[i].valid) {
      slot = i;
    }
  }
  if (slot < 0) {
    slot = 0;
  }
  if (g_arp_log < 6u) {
    g_arp_log++;
    kputs("[NET] arp reply for ");
    kputs_hex(ip);
    kputs("\n");
  }
  g_arp[slot].ip = ip;
  g_arp[slot].valid = 1;
  g_arp[slot].age = sched_ticks();
  for (i = 0; i < 6; i++) {
    g_arp[slot].mac[i] = mac[i];
  }
}

void net_arp_tick(void) {
  UINT32 now = sched_ticks();
  int    i;

  for (i = 0; i < 8; i++) {
    if (g_arp[i].valid && (now - g_arp[i].age) > 3000u) { /* 30 s */
      g_arp[i].valid = 0;
    }
  }
}

/* ---- IPv4 ------------------------------------------------------------- */

static UINT16 g_ip_id;

int net_send_ip(UINT32 dst, UINT8 proto, const UINT8 *payload, UINT32 len) {
  UINT8 *f = g_tx;
  UINT8 *ip = f + ETH_HDR;
  UINT32 i;
  UINT8  dstmac[6];
  UINT32 total = 20u + len;

  if (g_active == 0 || len > 1400u) {
    return -1;
  }
  if (dst == 0xFFFFFFFFu) {
    for (i = 0; i < 6u; i++) {
      dstmac[i] = 0xFFu;
    }
  } else if (!net_arp_lookup(dst, dstmac)) {
    net_arp_request(dst);
    return -2; /* caller retries once the ARP reply lands */
  }
  for (i = 0; i < 6u; i++) {
    f[i] = dstmac[i];
    f[6u + i] = g_active->mac[i];
  }
  put16(f + 12, 0x0800u);

  ip[0] = 0x45u;
  ip[1] = 0;
  put16(ip + 2, (UINT16)total);
  put16(ip + 4, ++g_ip_id);
  put16(ip + 6, 0x4000u); /* don't fragment */
  ip[8] = 64u;
  ip[9] = proto;
  put16(ip + 10, 0);
  ip_to_wire(ip + 12, g_cfg.ip);
  ip_to_wire(ip + 16, dst);
  put16(ip + 10, csum(ip, 20u));
  for (i = 0; i < len; i++) {
    ip[20u + i] = payload[i];
  }
  return net_send_frame(f, ETH_HDR + total);
}

/*
 * IPv4 output does not queue a packet while neighbor resolution is pending.
 * A cache miss emits ARP and returns a distinct retry result, so the caller
 * must decide whether and when to submit the original payload again.
 * Fragmentation is also absent: outbound payloads are bounded before framing.
 */

void net_ip_input(const UINT8 *p, UINT32 len) {
  UINT32 ihl;
  UINT32 src;
  UINT32 dst;
  UINT16 c;
  UINT8  proto;
  const UINT8 *payload;
  UINT32 plen;

  if (len < 20u || (p[0] >> 4) != 4u) {
    return;
  }
  ihl = (UINT32)(p[0] & 0x0Fu) * 4u;
  if (ihl < 20u || len < ihl) {
    return;
  }
  c = csum(p, ihl);
  if (c != 0u) {
    return;
  }
  if ((get16(p + 6) & 0x1FFFu) != 0u) {
    return; /* no fragment reassembly */
  }
  dst = ip_from_wire(p + 16);
  if (dst != g_cfg.ip && dst != 0xFFFFFFFFu) {
    return;
  }
  src = ip_from_wire(p + 12);
  proto = p[9];
  payload = p + ihl;
  plen = get16(p + 2);
  if (plen < ihl) {
    return;
  }
  plen -= ihl;
  if (plen > len - ihl) {
    plen = len - ihl;
  }
  switch (proto) {
  case IP_ICMP:
    net_icmp_input(payload, plen, src);
    break;
  case IP_UDP:
    net_udp_input(payload, plen, src);
    break;
  case IP_TCP:
    net_tcp_input(payload, plen, src);
    break;
  default:
    break;
  }
}

/* ---- ICMP: echo, which is what "ping" needs --------------------------- */

static UINT8  g_ping_state;   /* 0 idle 1 arp 2 sent 3 ok 4 timeout */
static UINT32 g_ping_target;
static UINT32 g_ping_sent;
static UINT32 g_ping_rtt;
static UINT32 g_ping_deadline;
static UINT8  g_ping_tries;

int net_ping_status(void) { return g_ping_state; }
UINT32 net_ping_rtt_ms(void) { return g_ping_rtt * 10u; }
UINT32 net_ping_target(void) { return g_ping_target; }

static void ping_send_echo(void) {
  UINT8  buf[32];
  UINT32 i;
  UINT16 c;

  for (i = 0; i < sizeof(buf); i++) {
    buf[i] = (UINT8)('a' + (i % 26u));
  }
  buf[0] = 8u; /* echo request */
  buf[1] = 0u;
  put16(buf + 2, 0);
  put16(buf + 4, 0x1234u);
  put16(buf + 6, (UINT16)(g_ping_tries + 1u));
  for (i = 8; i < sizeof(buf); i++) {
    buf[i] = (UINT8)(i * 7u);
  }
  c = csum(buf, sizeof(buf));
  put16(buf + 2, c);
  g_ping_sent = sched_ticks();
  if (net_send_ip(g_ping_target, IP_ICMP, buf, sizeof(buf)) != 0) {
    kputs("[NET] ping send failed (no route/arp)\n");
  }
}

int net_ping(UINT32 ip) {
  if (g_active == 0 || g_cfg.ip == 0u) {
    return -1;
  }
  g_ping_target = ip;
  g_ping_rtt = 0;
  g_ping_tries = 0;
  g_ping_state = 1;
  g_ping_deadline = sched_ticks() + 100u; /* 1 s per stage */
  net_arp_request(ip);
  return 0;
}

void net_icmp_tick(void) {
  UINT32 now = sched_ticks();

  if (g_ping_state != 1u && g_ping_state != 2u) {
    return;
  }
  if (now < g_ping_deadline) {
    return;
  }
  if (g_ping_tries >= 3u) {
    g_ping_state = 4;
    kputs("[NET] ping timeout\n");
    return;
  }
  g_ping_tries++;
  g_ping_deadline = now + 100u;
  kputs("[NET] ping retry ");
  kputs_hex(g_ping_tries);
  kputs("\n");
  ping_send_echo();
  g_ping_state = 2;
}

void net_icmp_input(const UINT8 *p, UINT32 len, UINT32 src) {
  if (len < 8u || p[0] != 0u || p[1] != 0u) {
    return; /* not an echo reply */
  }
  if (csum(p, len) != 0u) {
    return;
  }
  if (src != g_ping_target || g_ping_state != 2u) {
    return;
  }
  g_ping_rtt = sched_ticks() - g_ping_sent;
  g_ping_state = 3;
  kputs("[NET] ping ");
  kputs_hex(g_ping_target);
  kputs(" rtt=");
  kputs_hex(g_ping_rtt * 10u);
  kputs("ms\n");
}

/* ---- UDP -------------------------------------------------------------- */

int net_udp_send(UINT32 dst, UINT16 sport, UINT16 dport, const UINT8 *data,
                 UINT32 len) {
  UINT8  buf[1500];
  UINT16 c;

  if (len > sizeof(buf) - 8u) {
    return -1;
  }
  put16(buf + 0, sport);
  put16(buf + 2, dport);
  put16(buf + 4, (UINT16)(len + 8u));
  put16(buf + 6, 0); /* checksum optional for IPv4 */
  {
    UINT32 i;

    for (i = 0; i < len; i++) {
      buf[8u + i] = data[i];
    }
  }
  (void)c;
  return net_send_ip(dst, IP_UDP, buf, len + 8u);
}

void net_udp_input(const UINT8 *p, UINT32 len, UINT32 src) {
  UINT16 dport;

  if (len < 8u) {
    return;
  }
  dport = get16(p + 2);
  if (dport == 68u) { /* DHCP client port */
    net_dhcp_input(p + 8, len - 8u);
    return;
  }
  (void)src;
}

/* ---- input path ------------------------------------------------------- */

static void net_eth_input(const UINT8 *f, UINT32 len) {
  UINT16 type;

  if (len < ETH_HDR) {
    return;
  }
  type = get16(f + 12);
  if (type == 0x0806u) {
    net_arp_input(f + ETH_HDR, len - ETH_HDR);
  } else if (type == 0x0800u) {
    net_ip_input(f + ETH_HDR, len - ETH_HDR);
  }
}

/*
 * The network tick drains at most eight frames from the driver's poll
 * callback, then advances protocol timers once. This bounds receive work per
 * scheduler tick; a still-nonempty driver queue is left for the next tick.
 * Ethernet dispatch only hands payloads upward after the minimum header.
 */
void net_tick(void) {
  UINT32 i;

  if (g_active == 0) {
    return;
  }
  for (i = 0; i < 8u; i++) { /* bounded work per tick */
    UINT32 len = 0;

    if (g_active->poll == 0 || g_active->poll(g_active, g_rx, sizeof(g_rx),
                                              &len) == 0) {
      break;
    }
    if (len > ETH_HDR) {
      g_active->rx_pkts++;
      net_eth_input(g_rx, len);
    } else {
      g_active->rx_drop++;
    }
  }
  net_arp_tick();
  net_dhcp_tick();
  net_icmp_tick();
  net_tcp_tick();
}

void net_dhcp_apply(UINT32 ip, UINT32 mask, UINT32 gw) {
  g_cfg.ip = ip;
  g_cfg.mask = mask;
  g_cfg.gw = gw;
  g_dhcp_state = NET_DHCP_BOUND;
  g_dhcp_since = sched_ticks();
  /* One ping straight after the lease: it is the cheapest end-to-end proof
   * that ARP, IPv4 and ICMP work, and it lands in the boot log. */
  if (g_ping_state == 0u && gw != 0u) {
    (void)net_ping(gw);
  }
  kputs("[NET] ip ");
  kputs_hex((UINT64)(ip & 0xFFu));
  kputs(".");
  kputs_hex((UINT64)((ip >> 8) & 0xFFu));
  kputs(".");
  kputs_hex((UINT64)((ip >> 16) & 0xFFu));
  kputs(".");
  kputs_hex((UINT64)((ip >> 24) & 0xFFu));
  kputs(" gw ");
  kputs_hex((UINT64)(gw & 0xFFu));
  kputs(".");
  kputs_hex((UINT64)((gw >> 8) & 0xFFu));
  kputs(".");
  kputs_hex((UINT64)((gw >> 16) & 0xFFu));
  kputs(".");
  kputs_hex((UINT64)((gw >> 24) & 0xFFu));
  kputs("\n");
}

/* ---- boot ------------------------------------------------------------- */

void net_log(void) {
  int i;

  for (i = 0; i < g_ndev; i++) {
    kputs("[NET] ");
    kputs(g_devs[i].name);
    kputs(" link=");
    kputs(net_link_name(g_devs[i].link));
    kputs(" state=");
    kputs(net_state_name(g_devs[i].state));
    kputs(" mac=");
    kputs_hex((UINT64)g_devs[i].mac[0]);
    kputs(":");
    kputs_hex((UINT64)g_devs[i].mac[1]);
    kputs(":");
    kputs_hex((UINT64)g_devs[i].mac[5]);
    kputs("\n");
  }
  if (g_active != 0) {
    kputs("[NET] active ");
    kputs(g_active->name);
    kputs("\n");
  } else {
    kputs("[NET] no usable network device\n");
  }
}

int net_init(void) {
  g_ndev = 0;
  g_active = 0;
  g_cfg.ip = 0;
  g_cfg.mask = NET_IP(255, 255, 255, 0);
  g_cfg.gw = 0;
  g_dhcp_state = NET_DHCP_IDLE;

  pci_log();
  (void)net_e1000_probe();
  (void)net_igb_probe();
  (void)net_wifi_probe();
  net_log();
  if (g_active != 0) {
    net_dhcp_start();
  }
  return (g_active != 0) ? 0 : -1;
}

#else /* !CONFIG_NOVUS_NET */

int net_init(void) { return -1; }
void net_tick(void) {}
int net_count(void) { return 0; }
netdev_t *net_dev(int i) { (void)i; return 0; }
netdev_t *net_active(void) { return 0; }
const net_cfg_t *net_cfg(void) { static net_cfg_t c; return &c; }
UINT8 net_dhcp_state(void) { return 0; }
UINT32 net_dhcp_leased_ms(void) { return 0; }
const char *net_link_name(UINT8 l) { (void)l; return "-"; }
const char *net_state_name(UINT8 s) { (void)s; return "-"; }
int net_send_frame(const UINT8 *f, UINT32 l) { (void)f; (void)l; return -1; }
int net_send_ip(UINT32 d, UINT8 p, const UINT8 *b, UINT32 l) { (void)d; (void)p; (void)b; (void)l; return -1; }
int net_ping(UINT32 ip) { (void)ip; return -1; }
int net_ping_status(void) { return 0; }
UINT32 net_ping_rtt_ms(void) { return 0; }
UINT32 net_ping_target(void) { return 0; }
int net_tcp_test(UINT32 ip, UINT16 port) { (void)ip; (void)port; return -1; }
int net_tcp_status(void) { return 0; }
UINT32 net_tcp_bytes(void) { return 0; }
void net_log(void) {}
void net_arp_input(const UINT8 *p, UINT32 l) { (void)p; (void)l; }
int net_arp_lookup(UINT32 ip, UINT8 *m) { (void)ip; (void)m; return 0; }
void net_arp_request(UINT32 ip) { (void)ip; }
void net_arp_tick(void) {}
void net_icmp_input(const UINT8 *p, UINT32 l, UINT32 s) { (void)p; (void)l; (void)s; }
void net_icmp_tick(void) {}
void net_udp_input(const UINT8 *p, UINT32 l, UINT32 s) { (void)p; (void)l; (void)s; }
void net_dhcp_input(const UINT8 *p, UINT32 l) { (void)p; (void)l; }
void net_dhcp_tick(void) {}
void net_dhcp_start(void) {}
void net_tcp_input(const UINT8 *p, UINT32 l, UINT32 s) { (void)p; (void)l; (void)s; }
void net_tcp_tick(void) {}
void net_ip_input(const UINT8 *p, UINT32 l) { (void)p; (void)l; }
int net_udp_send(UINT32 d, UINT16 s, UINT16 p, const UINT8 *b, UINT32 l) { (void)d; (void)s; (void)p; (void)b; (void)l; return -1; }
void net_register(netdev_t *d) { (void)d; }
int net_e1000_probe(void) { return -1; }
int net_igb_probe(void) { return -1; }
int net_wifi_probe(void) { return -1; }

#endif
