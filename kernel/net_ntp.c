/**
 * Novus OS - automatic SNTP client.
 *
 * A single outstanding request at a time. After DHCP has a lease the client
 * resolves ntp.aliyun.com with the existing DNS resolver, sends one RFC 4330
 * client packet to UDP/123, and corrects the software clock by the measured
 * offset. The server address is cached, so later hourly re-syncs need no DNS.
 * All work happens in net_tick(); nothing blocks the boot path.
 */

#include "kernel.h"
#include "sched.h"
#include "net.h"
#include "rtc.h"

#include <string.h>

#if CONFIG_NOVUS_NET && CONFIG_NOVUS_NTP

#define NTP_PORT          123u
#define NTP_PKT_LEN       48u
#define NTP_EPOCH_DIFF    2208988800ULL
#define NTP_MAX_SKEW      946080000LL  /* reject corrections over 30 years */

/* ntp.aliyun.com when DNS is unavailable (wire byte order: 203.107.6.88). */
#define NTP_FALLBACK_IP   (203u | (107u << 8) | (6u << 16) | (88u << 24))

#define ST_OFF   0u
#define ST_LINK  1u
#define ST_DNS   2u
#define ST_SEND  3u
#define ST_WAIT  4u
#define ST_DONE  5u

static UINT8  g_state;
static UINT32 g_server;
static UINT32 g_deadline;
static UINT32 g_next_sync;
static UINT8  g_tries;
static UINT8  g_resolved;
static UINT64 g_tx;

int    net_ntp_status(void) { return (int)g_state; }
UINT32 net_ntp_server(void) { return g_server; }

static void ntp_put64(UINT8 *p, UINT64 v) {
  UINT32 i;

  for (i = 0u; i < 8u; i++) {
    p[i] = (UINT8)(v >> (56u - i * 8u));
  }
}

static UINT64 ntp_get64(const UINT8 *p) {
  UINT64 v = 0u;
  UINT32 i;

  for (i = 0u; i < 8u; i++) {
    v = (v << 8) | (UINT64)p[i];
  }
  return v;
}

static void ntp_log_ip(UINT32 ip) {
  char   s[16];
  UINT32 n = 0u;
  UINT32 i;

  for (i = 0u; i < 4u; i++) {
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

static void ntp_use_fallback(void) {
  g_server = NTP_FALLBACK_IP;
  g_resolved = 1u;
  g_state = ST_SEND;
  g_deadline = (UINT32)sched_ticks();
  kputs("[NTP] DNS unavailable, using 203.107.6.88\n");
}

static int ntp_send_now(void) {
  UINT8 pkt[NTP_PKT_LEN];
  UINT64 local = rtc_unix();

  if (local == 0u || g_server == 0u) {
    return -1;
  }
  memset(pkt, 0, sizeof(pkt));
  pkt[0] = 0x23u; /* LI=0, VN=4, mode=3 (client) */
  pkt[1] = 4u;    /* poll interval hint */
  g_tx = (local + NTP_EPOCH_DIFF) << 32;
  ntp_put64(pkt + 40u, g_tx);
  return net_udp_send(g_server, NET_NTP_LOCAL_PORT, NTP_PORT, pkt,
                      (UINT32)sizeof(pkt));
}

void net_ntp_init(void) {
  g_state = ST_LINK;
  g_server = 0u;
  g_tries = 0u;
  g_resolved = 0u;
  g_tx = 0u;
  g_deadline = (UINT32)sched_ticks() + 100u;
  g_next_sync = 0u;
  kputs("[NTP] auto-sync enabled: ntp.aliyun.com\n");
}

void net_ntp_tick(void) {
  UINT32 now;
  UINT8  dhcp;

  if (g_state == ST_OFF) {
    return;
  }
  now = (UINT32)sched_ticks();

  if (g_state == ST_DONE) {
    if (now >= g_next_sync) {
      g_state = ST_SEND;
      g_tries = 0u;
      g_deadline = now;
    }
    return;
  }

  if (g_state == ST_LINK) {
    if (now < g_deadline) {
      return;
    }
    if (net_active() == 0 || net_cfg()->ip == 0u) {
      return;
    }
    dhcp = net_dhcp_state();
    if (dhcp != NET_DHCP_BOUND && dhcp != NET_DHCP_FALLBACK) {
      return;
    }
    if (g_server == 0u && !g_resolved) {
      if (net_dns_server() == 0u) {
        ntp_use_fallback();
        return;
      }
      /* The DNS slot is shared; only a query still in flight blocks us.
       * A completed query (ok or failed) already released the slot. */
      if (net_dns_status() == 1) {
        g_deadline = now + 50u;
        return;
      }
      if (net_dns_resolve("ntp.aliyun.com") != 0) {
        ntp_use_fallback();
        return;
      }
      g_state = ST_DNS;
      g_deadline = now + 500u;
      kputs("[NTP] resolving ntp.aliyun.com\n");
      return;
    }
    if (g_server == 0u) {
      ntp_use_fallback();
      return;
    }
    g_state = ST_SEND;
    g_deadline = now;
    return;
  }

  if (g_state == ST_DNS) {
    int status = net_dns_status();

    if (status == 2) {
      UINT32 ip = net_dns_addr();

      g_server = (ip != 0u) ? ip : NTP_FALLBACK_IP;
      g_resolved = 1u;
      g_state = ST_SEND;
      g_deadline = now;
      kputs("[NTP] server ");
      ntp_log_ip(g_server);
      kputs("\n");
    } else if (status == 3 || now >= g_deadline) {
      ntp_use_fallback();
    }
    return;
  }

  if (g_state == ST_SEND) {
    int sent;

    if (now < g_deadline) {
      return;
    }
    sent = ntp_send_now();
    if (sent == -2) { /* ARP miss: next tick retries */
      g_deadline = now + 20u;
      return;
    }
    if (sent != 0) {
      g_tries++;
      if (g_tries >= 3u) {
        kputs("[NTP] send failed, retrying later\n");
        g_state = ST_LINK;
        g_deadline = now + 3000u;
      } else {
        g_deadline = now + 50u;
      }
      return;
    }
    g_state = ST_WAIT;
    g_deadline = now + 300u;
    return;
  }

  if (g_state == ST_WAIT && now >= g_deadline) {
    if (++g_tries < 3u) {
      g_state = ST_SEND;
      g_deadline = now + 20u;
    } else {
      kputs("[NTP] no reply, retrying later\n");
      g_state = ST_LINK;
      g_deadline = now + 3000u;
    }
  }
}

void net_ntp_input(const UINT8 *p, UINT32 len, UINT32 src) {
  UINT64 t1, t2, t3, t4;
  UINT64 corrected;
  INT64  delta;
  UINT8  version;
  UINT8  stratum;

  if (g_state != ST_WAIT || g_server == 0u || src != g_server) {
    return;
  }
  if (len < NTP_PKT_LEN || (p[0] & 0x07u) != 4u) {
    return;
  }
  version = (UINT8)((p[0] >> 3) & 0x07u);
  stratum = p[1];
  if ((version != 3u && version != 4u) || stratum == 0u || stratum > 15u) {
    return;
  }
  /* The server must echo our transmit timestamp: this rejects stray UDP
   * traffic that happens to land on the local port. */
  if (ntp_get64(p + 24u) != g_tx) {
    return;
  }
  t1 = g_tx;
  t2 = ntp_get64(p + 32u);
  t3 = ntp_get64(p + 40u);
  t4 = ((UINT64)rtc_unix() + NTP_EPOCH_DIFF) << 32;
  if (t2 == 0u || t3 == 0u) {
    return;
  }
  /* RFC 4330: offset = ((t2 - t1) + (t3 - t4)) / 2, in NTP fixed point.
   * The kernel clock has one-second resolution, so drop the fraction. */
  delta = (INT64)(t2 - t1) + (INT64)(t3 - t4);
  delta /= 2;
  delta /= 4294967296LL;
  if (delta > NTP_MAX_SKEW || delta < -NTP_MAX_SKEW) {
    kputs("[NTP] implausible offset: t1=");
    kputs_hex(t1);
    kputs(" t2=");
    kputs_hex(t2);
    kputs(" t3=");
    kputs_hex(t3);
    kputs(" t4=");
    kputs_hex(t4);
    kputs(" delta=");
    kputs_hex((UINT64)delta);
    kputs("\n");
    return;
  }
  corrected = rtc_unix();
  if (delta >= 0) {
    corrected += (UINT64)delta;
  } else {
    corrected -= (UINT64)(-delta);
  }
  rtc_set_unix(corrected);

  g_state = ST_DONE;
  g_tries = 0u;
  g_next_sync = (UINT32)sched_ticks() + 360000u; /* one hour at 10 ms/tick */
  kputs("[NTP] synced via ntp.aliyun.com, offset ");
  if (delta < 0) {
    kputs("-");
  }
  kputs_hex((UINT64)(delta < 0 ? -delta : delta));
  kputs("s\n");
}

#else /* !CONFIG_NOVUS_NET || !CONFIG_NOVUS_NTP */

void net_ntp_init(void) {}
void net_ntp_tick(void) {}
void net_ntp_input(const UINT8 *p, UINT32 len, UINT32 src) {
  (void)p;
  (void)len;
  (void)src;
}
int net_ntp_status(void) { return 0; }
UINT32 net_ntp_server(void) { return 0u; }

#endif
