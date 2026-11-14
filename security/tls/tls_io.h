/**
 * Novus OS - transport abstraction shared by the TLS clients.
 *
 * Two function pointers: the same TLS code then runs over the kernel net_tcp
 * or a host POSIX socket in tests.
 */
#ifndef NOVUS_TLS_IO_H
#define NOVUS_TLS_IO_H

#include "../novu/novu_format.h"

typedef struct {
    int (*send)(void *ctx, const UINT8 *buf, UINT32 len);
    int (*recv)(void *ctx, UINT8 *buf, UINT32 max);
} tls_io;

#endif /* NOVUS_TLS_IO_H */
