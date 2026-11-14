/**
 * Novus OS - HTTPS client over the minimal TCP stack (TLS 1.3 / 1.2).
 *
 * The TLS clients (security/tls/*) drive net_tcp through a tls_io adapter.
 * Trust anchors are installed at boot from the generated cabundle (a set of
 * real vendor roots). Until at least one anchor is installed https_get fails
 * closed rather than trusting everything.
 */
#ifndef NOVUS_NET_HTTP_H
#define NOVUS_NET_HTTP_H

#include "uefi.h"

#define NET_HTTP_MAX 16384u
/* 148 production vendor roots + a few spare slots (E2E test anchor). */
#define NET_HTTP_MAX_ROOTS 192u

/* Install a single trusted root (DER, copied into a bounded buffer). Kept for
 * compatibility and for the isolated QEMU test anchor. */
int net_http_set_ca(const UINT8 *der, UINT32 len);

/* Install a set of trust anchors described by a flat DER blob plus an
 * offset/length table (see security/novu/cabundle.c). The DER bytes are NOT
 * copied: they stay in the image's read-only data. */
int net_http_set_ca_set(const UINT8 *blob, const UINT32 *offs,
                        const UINT32 *lens, UINT32 count);

/* Append one more anchor (DER copied into a bounded slot). Used only by the
 * E2E build to add the QEMU test CA; returns -1 when the set is full. */
int net_http_add_ca(const UINT8 *der, UINT32 len);

/* Number of installed trust anchors. */
UINT32 net_http_root_count(void);

/* Perform a GET and write the response body into out[cap]. Returns the body
 * length, or -1. `now_unix` feeds certificate validity checking. */
int https_get(UINT32 ip, UINT16 port, const char *host, const char *path,
              UINT64 now_unix, UINT8 *out, UINT32 cap);

int net_http_ready(void);

#endif /* NOVUS_NET_HTTP_H */
