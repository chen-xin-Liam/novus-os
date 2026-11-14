/**
 * Novus OS - HTTPS client (see net_http.h).
 */
#include "kernel.h"
#include "sched.h"
#include "net.h"
#include "net_http.h"
#include "rtc.h"
#include "tls13_client.h"
#include "tls12_client.h"
#include "chain.h"
#include "csprng.h"
#include "../lib/novuslib_rs.h"

/* One connection at a time; the TLS clients and their scratch live in BSS.
 *
 * Trust anchors are described by an array of {der,len} pointing either into
 * the generated cabundle in .rodata (the real vendor roots, ~170 KB, far
 * too large to copy; see tools/cabundle/roots.pem for the current count) or
 * into a small bump buffer for roots installed at runtime (single-root
 * compatibility API and the isolated E2E test anchor). */
static tls13_client g_tls13;
static tls12_client g_tls12;
static x509_der g_roots[NET_HTTP_MAX_ROOTS];
static UINT32 g_nroots;
static UINT8 g_ca_store[8192];
static UINT32 g_ca_store_used;
static int g_rng_ready;

static void ca_reset(void) {
  g_nroots = 0u;
  g_ca_store_used = 0u;
}

static const UINT8 *ca_alloc(const UINT8 *der, UINT32 len) {
  UINT8 *dst;
  UINT32 i;
  if (der == (const UINT8 *)0 || len == 0u ||
      len > sizeof(g_ca_store) - g_ca_store_used) {
    return (const UINT8 *)0;
  }
  dst = g_ca_store + g_ca_store_used;
  for (i = 0; i < len; i++) dst[i] = der[i];
  g_ca_store_used += len;
  return dst;
}

int net_http_ready(void) { return g_nroots != 0u; }

UINT32 net_http_root_count(void) { return g_nroots; }

int net_http_set_ca(const UINT8 *der, UINT32 len) {
  const UINT8 *copy;
  ca_reset();
  copy = ca_alloc(der, len);
  if (copy == (const UINT8 *)0) return -1;
  g_roots[0].der = copy;
  g_roots[0].len = len;
  g_nroots = 1u;
  return 0;
}

int net_http_set_ca_set(const UINT8 *blob, const UINT32 *offs,
                        const UINT32 *lens, UINT32 count) {
  UINT32 i;
  if (blob == (const UINT8 *)0 || offs == (const UINT32 *)0 ||
      lens == (const UINT32 *)0 || count == 0u ||
      count > NET_HTTP_MAX_ROOTS) {
    return -1;
  }
  ca_reset();
  for (i = 0; i < count; i++) {
    if (lens[i] == 0u) return -1;
    g_roots[i].der = blob + offs[i];
    g_roots[i].len = lens[i];
  }
  g_nroots = count;
  return 0;
}

int net_http_add_ca(const UINT8 *der, UINT32 len) {
  const UINT8 *copy;
  if (g_nroots >= NET_HTTP_MAX_ROOTS) return -1;
  copy = ca_alloc(der, len);
  if (copy == (const UINT8 *)0) return -1;
  g_roots[g_nroots].der = copy;
  g_roots[g_nroots].len = len;
  g_nroots++;
  return 0;
}

static void rng_bridge(UINT8 *out, UINT32 len) {
  tls_rng_bytes(out, len);
}

/* Wall clock for certificate validity comes from the global rtc_unix() in
 * rtc.c (declared in rtc.h above). */

static int io_send(void *ctx, const UINT8 *buf, UINT32 len) {
  (void)ctx;
  /* Queue the segment; the network pump below actually transmits it. */
  int r = net_tcp_send(buf, len);
  net_tick(); /* flush queued TX so the peer receives our flight promptly */
  return r;
}

static int io_recv(void *ctx, UINT8 *buf, UINT32 max) {
  UINT32 spins = 0;

  (void)ctx;
  for (;;) {
    UINT32 avail = net_tcp_rx_len();
    if (avail > 0u) {
      const UINT8 *src = net_tcp_rx_data();
      UINT32 take = (avail < max) ? avail : max;
      UINT32 i;
      for (i = 0; i < take; i++) {
        buf[i] = src[i];
      }
      net_tcp_consume(take);
      return (int)take;
    }
    if (net_tcp_phase() == NET_TCP_FAIL || net_tcp_phase() == NET_TCP_IDLE ||
        net_tcp_phase() == NET_TCP_CLOSED) {
      return -1;
    }
    /* The HTTPS handshake is a blocking state machine driven from a scheduler
     * tick, so net_tick() is not otherwise re-invoked while we wait. Pump the
     * network here so queued TX is sent and incoming TCP segments are received
     * (the outer net_tick for this tick has already returned, so this is a
     * fresh, non-reentrant call). */
    net_tick();
    sched_yield();
    if (++spins > 2000000u) {
      return -1;
    }
  }
}

static int build_request(char *req, UINT32 cap, const char *host,
                         const char *path) {
  UINT32 n = 0;
  const char *p;

  for (p = "GET "; *p != 0 && n < cap; p++) req[n++] = *p;
  for (p = (path && *path) ? path : "/"; *p != 0 && n < cap; p++) req[n++] = *p;
  for (p = " HTTP/1.0\r\nHost: "; *p != 0 && n < cap; p++) req[n++] = *p;
  for (p = host; *p != 0 && n < cap; p++) req[n++] = *p;
  for (p = "\r\nConnection: close\r\n\r\n"; *p != 0 && n < cap; p++) req[n++] = *p;
  return (int)n;
}

int https_get(UINT32 ip, UINT16 port, const char *host, const char *path,
              UINT64 now_unix, UINT8 *out, UINT32 cap) {
  tls_io io;
  char req[256];
  UINT32 off = 0, spins = 0, rl;
  int rc, got, mode;

  if (out == (UINT8 *)0 || cap == 0u || g_nroots == 0u ||
      host == (const char *)0) {
    return -1; /* fail closed without a trust anchor */
  }
  if (!g_rng_ready) {
    UINT8 seed[32];
    UINT32 i;
    UINT64 t = sched_ticks();
    for (i = 0; i < 32u; i++) {
      seed[i] = (UINT8)(t >> ((i & 7u) * 8u)) ^ (UINT8)(i * 31u + 7u);
    }
    tls_rng_seed(novus_rs_sha256, seed, 32u);
    g_rng_ready = 1;
  }

  net_tcp_close();
  if (net_tcp_open(ip, port, (const UINT8 *)0, 0) != 0) {
    return -1;
  }
  /* Drive the network here too: the probe may run before the scheduler's tick
   * starts pumping net_tick(), and the 3-way handshake only completes once
   * net_tick() processes the peer's SYN-ACK. Without this the connection would
   * never reach OPEN and the TLS client would fail before sending anything. */
  while (net_tcp_phase() == NET_TCP_WAIT) {
    net_tick();
    sched_yield();
    if (++spins > 2000000u) {
      net_tcp_close();
      return -1;
    }
  }
  if (net_tcp_phase() != NET_TCP_OPEN) {
    net_tcp_close();
    return -1;
  }

  io.send = io_send;
  io.recv = io_recv;
  {
    UINT64 now = (now_unix != 0u) ? now_unix : rtc_unix();
    mode = 1;
    rc = tls13_client_connect_set(&g_tls13, host, io, (void *)0,
                                  g_roots, g_nroots,
                                  now, novus_rs_sha256, rng_bridge);
    if (rc != 0) {
      /* Fall back to TLS 1.2 on a fresh connection. */
      net_tcp_close();
      mode = 0;
      if (net_tcp_open(ip, port, (const UINT8 *)0, 0) != 0) {
        return -1;
      }
      spins = 0;
      while (net_tcp_phase() == NET_TCP_WAIT) {
        net_tick();
        sched_yield();
        if (++spins > 2000000u) {
          net_tcp_close();
          return -1;
        }
      }
      if (net_tcp_phase() != NET_TCP_OPEN) {
        net_tcp_close();
        return -1;
      }
      rc = tls12_client_connect_set(&g_tls12, host, io, (void *)0,
                                    g_roots, g_nroots,
                                    now, novus_rs_sha256, rng_bridge);
      if (rc != 0) {
        net_tcp_close();
        return -1;
      }
      mode = 2;
    }
  }

  rl = (UINT32)build_request(req, sizeof(req), host, path);
  if (mode == 1) {
    rc = tls13_client_write(&g_tls13, (const UINT8 *)req, rl);
  } else {
    rc = tls12_client_write(&g_tls12, (const UINT8 *)req, rl);
  }
  if (rc != 0) {
    net_tcp_close();
    return -1;
  }

  for (;;) {
    if (off >= cap) {
      break;
    }
    if (mode == 1) {
      got = tls13_client_read(&g_tls13, out + off, cap - off);
    } else {
      got = tls12_client_read(&g_tls12, out + off, cap - off);
    }
    if (got <= 0) {
      break;
    }
    off += (UINT32)got;
  }
  net_tcp_close();
  return (int)off;
}
