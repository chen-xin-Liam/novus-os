/**
 * Host-side reproduction of the in-guest TLS 1.3 client handshake, using the
 * exact kernel TLS sources compiled against a POSIX socket. Used only to
 * diagnose why the kernel-side HTTPS probe fails; not part of the OS image.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

#include "novu_format.h"
#include "tls13_client.h"

void novus_sha256_host(const UINT8 *data, UINT64 len, UINT8 *out);

/* The generated cabundle is a set of trust anchors: one flat DER blob plus
 * offset/length tables. This host repro checks a single connection against
 * one anchor (the QEMU test CA), so it uses the test anchor blob directly. */
extern const UINT8 novus_e2e_root_der[];
extern const UINT32 novus_e2e_root_len;

static int g_sock = -1;

static int io_send(void *ctx, const UINT8 *buf, UINT32 len) {
    (void)ctx;
    int total = 0;
    while (total < (int)len) {
        int r = (int)send(g_sock, buf + total, len - (UINT32)total, 0);
        if (r <= 0) return -1;
        total += r;
    }
    fprintf(stderr, "[io] send len=%u -> ok\n", len);
    return 0;
}
static int io_recv(void *ctx, UINT8 *buf, UINT32 max) {
    (void)ctx;
    int r = (int)recv(g_sock, buf, max, 0);
    fprintf(stderr, "[io] recv max=%u -> %d\n", max, r);
    return r;
}

static void my_sha256(const UINT8 *d, UINT64 n, UINT8 *out) {
    novus_sha256_host(d, n, out);
}
static void my_rng(UINT8 *out, UINT32 len) {
    int fd = open("/dev/urandom", 0);
    if (fd >= 0) { (void)read(fd, out, (size_t)len); close(fd); }
    else { for (UINT32 i = 0; i < len; i++) out[i] = (UINT8)(i * 31 + 7); }
}

int main(void) {
    struct sockaddr_in sa;
    tls13_client c;
    int rc;
    setvbuf(stderr, NULL, _IONBF, 0);
    const char *host = "10.0.2.2";
    UINT8 req[256];
    int rl, got;
    UINT8 resp[4096];

    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock < 0) { perror("socket"); return 2; }
    sa.sin_family = AF_INET;
    sa.sin_port = htons(8443);
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(g_sock, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        perror("connect");
        return 2;
    }
    fprintf(stderr, "[host] connected to 127.0.0.1:8443, root_len=%u\n",
            novus_e2e_root_len);

    rc = tls13_client_connect(&c, host, (tls_io){io_send, io_recv},
                             (void *)0, novus_e2e_root_der, novus_e2e_root_len,
                             (UINT64)time(NULL), my_sha256, my_rng);
    fprintf(stderr, "[host] tls13_client_connect rc=%d\n", rc);
    if (rc != 0) return 1;

    rl = snprintf((char *)req, sizeof(req),
                  "GET /v1/updates HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
                  host);
    rc = tls13_client_write(&c, req, (UINT32)rl);
    fprintf(stderr, "[host] write rc=%d\n", rc);
    got = tls13_client_read(&c, resp, sizeof(resp));
    fprintf(stderr, "[host] read rc=%d\n", got);
    if (got > 0) {
        fwrite(resp, 1, got, stdout);
    }
    return 0;
}
