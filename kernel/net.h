/**
 * Novus OS - network core (M10).
 *
 * One device abstraction for every link type, one protocol stack on top of
 * it, and a tiny polled I/O model: `net_tick()` runs from the timer tick,
 * asks the active device for a bounded number of frames and feeds them to
 * ARP / IPv4 / ICMP / UDP / DHCP / TCP. Nothing blocks and nothing sleeps,
 * so a slow or missing link can never stall the desktop.
 *
 * Addresses are stored in wire byte order inside a UINT32 (x86 is little
 * endian, so NET_IP(10,0,2,15) lays out as 0A 00 02 0F on the wire).
 */

#ifndef NOVUS_NET_H
#define NOVUS_NET_H

#include "uefi.h"

#define NET_MAX_DEVS 8

#define NET_LINK_ETHER 1u
#define NET_LINK_FIBER 2u
#define NET_LINK_WIFI  3u

#define NET_STATE_DOWN  0u
#define NET_STATE_UP    1u
#define NET_STATE_NODRV 2u

/* Driver family a PCI network device belongs to; see net_ids.c. */
#define NET_FAMILY_NONE   0u
#define NET_FAMILY_E1000  1u
#define NET_FAMILY_IGB    2u
#define NET_FAMILY_VIRTIO 3u
#define NET_FAMILY_WIFI   4u
#define NET_FAMILY_RTL8139 5u

#define NET_IP(a, b, c, d)                                                    \
  ((UINT32)(a) | ((UINT32)(b) << 8) | ((UINT32)(c) << 16) |                   \
   ((UINT32)(d) << 24))

typedef struct netdev netdev_t;

struct netdev {
  char     name[12];
  /* Human-readable controller name from the PCI ID table ("Intel Wi-Fi 6E
   * AX211 160MHz"); empty when the ID is unknown. */
  char     model[32];
  UINT8    link;  /* NET_LINK_* */
  UINT8    state; /* NET_STATE_* */
  UINT8    mac[6];
  UINT16   mtu;
  UINT32   speed_mbps;
  UINT64   rx_pkts;
  UINT64   tx_pkts;
  UINT64   rx_err;
  UINT64   tx_err;
  UINT64   rx_drop;
  /* Send one Ethernet frame. Returns 0 on success. */
  int (*send)(netdev_t *d, const UINT8 *frame, UINT32 len);
  /* Next received frame, or 0 when the ring is empty. */
  int (*poll)(netdev_t *d, UINT8 *buf, UINT32 cap, UINT32 *out_len);
  void *priv;
};

typedef struct {
  UINT32 ip;
  UINT32 mask;
  UINT32 gw;
} net_cfg_t;

/* DHCP state machine states, also reported to the UI. */
#define NET_DHCP_IDLE    0u
#define NET_DHCP_WAIT    1u
#define NET_DHCP_BOUND   2u
#define NET_DHCP_FALLBACK 3u

/* Drivers register themselves here; the first one that reports UP becomes
 * the active link for the protocol stack. */
void net_register(netdev_t *d);

/* Bring up PCI, probe the drivers, start DHCP (asynchronous). */
int net_init(void);
/* Timer tick: poll the active device and run the protocol timers. */
void net_tick(void);

int       net_count(void);
netdev_t *net_dev(int i);
netdev_t *net_active(void);
const net_cfg_t *net_cfg(void);
UINT8     net_dhcp_state(void);
UINT32    net_dhcp_leased_ms(void);
const char *net_link_name(UINT8 link);
const char *net_state_name(UINT8 state);

/* ICMP echo: starts a request; result is latched for the UI. */
int  net_ping(UINT32 ip);
int  net_ping_status(void); /* 0 idle, 1 running, 2 ok, 3 timeout */
UINT32 net_ping_rtt_ms(void);
UINT32 net_ping_target(void);

/* Minimal TCP client used by the "network" app's echo test. */
int  net_tcp_test(UINT32 ip, UINT16 port);
int  net_tcp_status(void);  /* 0 idle, 1 connecting, 2 ok, 3 failed */
UINT32 net_tcp_bytes(void);

/* The same client, generalised: connect, push one request, collect the reply
 * into a fixed buffer. One session at a time, no window scaling. */
#define NET_TCP_RX_CAP 2048u
#define NET_TCP_TX_CAP 192u

#define NET_TCP_IDLE   0u
#define NET_TCP_WAIT   1u /* ARP / SYN */
#define NET_TCP_OPEN   2u
#define NET_TCP_CLOSED 3u /* peer sent FIN, reply is complete */
#define NET_TCP_FAIL   4u

int          net_tcp_open(UINT32 ip, UINT16 port, const UINT8 *tx, UINT32 txlen);
void         net_tcp_close(void);
int          net_tcp_phase(void);
UINT32       net_tcp_rx_len(void);
const UINT8 *net_tcp_rx_data(void);

/* ---- DNS: one A query over UDP ---------------------------------------- */

/* Fixed local port for the single outstanding query; UDP replies are matched
 * on it in net_udp_input(). */
#define NET_DNS_LOCAL_PORT 5353u

void   net_dns_set_server(UINT32 ip);
UINT32 net_dns_server(void);
/* 0 = query started, -1 = no link or no server configured. */
int    net_dns_resolve(const char *name);
int    net_dns_status(void); /* 0 idle, 1 running, 2 ok, 3 failed */
UINT32 net_dns_addr(void);
void   net_dns_input(const UINT8 *p, UINT32 len, UINT32 src);
void   net_dns_tick(void);

/* ---- NCSI: is the machine really on the internet? --------------------- */

#define NET_NCSI_IDLE         0u
#define NET_NCSI_DNS          1u
#define NET_NCSI_TCP          2u
#define NET_NCSI_HTTP         3u
#define NET_NCSI_OK           4u
#define NET_NCSI_FAIL_LINK    5u
#define NET_NCSI_FAIL_DNS     6u
#define NET_NCSI_FAIL_TCP     7u
#define NET_NCSI_FAIL_HTTP    8u
#define NET_NCSI_FAIL_TIMEOUT 9u

/* GET http://ncsi.novus-os.ryzn.pro/connecttest.txt, then latch the verdict
 * (NET_NCSI_OK or a NET_NCSI_FAIL_*). Re-entrant calls are refused while a
 * check is in flight. */
int         net_ncsi_check(void);
int         net_ncsi_status(void);
UINT16      net_ncsi_http_code(void);
UINT32      net_ncsi_elapsed_ms(void);
const char *net_ncsi_status_name(void);
void        net_ncsi_tick(void);

/* Used by the protocol modules. */
int net_send_frame(const UINT8 *frame, UINT32 len);
int net_send_ip(UINT32 dst, UINT8 proto, const UINT8 *payload, UINT32 len);
void net_log(void);

/* Protocol entry points, called from net.c's dispatcher. */
void net_arp_input(const UINT8 *p, UINT32 len);
int  net_arp_lookup(UINT32 ip, UINT8 *mac);
void net_arp_request(UINT32 ip);
void net_arp_tick(void);
void net_icmp_input(const UINT8 *p, UINT32 len, UINT32 src);
void net_icmp_tick(void);
void net_udp_input(const UINT8 *p, UINT32 len, UINT32 src);
void net_dhcp_input(const UINT8 *p, UINT32 len);
void net_dhcp_tick(void);
void net_dhcp_start(void);
void net_dhcp_apply(UINT32 ip, UINT32 mask, UINT32 gw);
void net_tcp_input(const UINT8 *p, UINT32 len, UINT32 src);
void net_tcp_tick(void);
int  net_udp_send(UINT32 dst, UINT16 sport, UINT16 dport, const UINT8 *data,
                  UINT32 len);

/* Driver probes. Each returns 0 when it took the device. */
int net_e1000_probe(void);
int net_igb_probe(void);
int net_virtio_probe(void);
int net_rtl8139_probe(void);
int net_wifi_probe(void);

/* ---- PCI identity table (net_ids.c) ----------------------------------- */

/* Which driver family owns this vendor/device pair, or NET_FAMILY_NONE. */
UINT8 net_id_family(UINT16 vendor, UINT16 device);
/* Marketing name of the controller, or 0 when the ID is not in the table. */
const char *net_id_name(UINT16 vendor, UINT16 device);
/* A short fallback name derived from the vendor ID, for unknown devices. */
const char *net_vendor_name(UINT16 vendor);
/* Copy at most `cap` bytes of `s` into dst and always terminate it. */
void net_strcpy(char *dst, UINT32 cap, const char *s);

#endif /* NOVUS_NET_H */
