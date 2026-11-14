/**
 * Novus OS - update service state machine + journal (see novu_service.h).
 */
#include "novu_service.h"
#include "kernel.h"
#include "net.h"
#include "net_http.h"

static novu_service_t g_svc;

#if NOVUS_HTTPS_E2E_PROBE
/* One-shot kernel-side HTTPS end-to-end probe (gated by the E2E make flag).
 * Driven by the scheduler tick after DHCP binds and the cabundle is loaded,
 * so a real updatesrv over QEMU user-net can be joint-debugged from the
 * serial log. The probe performs no writes: it only fetches the update index
 * and reports the result. */
static UINT8 g_e2e_buf[NET_HTTP_MAX];
static int  g_e2e_done;
static UINT32 g_e2e_tries;
#define NOVU_E2E_MAX_TRIES 3u
static UINT32 put_dec(char *b, UINT32 cap, UINT32 pos, UINT32 v); /* defined below */

/* Returns 1 on success (probe is finished), 0 on a transient failure that
 * should be retried after the TCP session frees up. */
static int novu_e2e_probe(void) {
    int n = https_get(NET_IP(10, 0, 2, 2), 8443, "10.0.2.2", "/v1/updates",
                      0, g_e2e_buf, sizeof(g_e2e_buf));
    if (n > 0) {
        char msg[64];
        UINT32 pos = 0;
        const char *p = "[E2E] OK len=";
        while (*p != 0 && pos < sizeof(msg) - 1) msg[pos++] = *p++;
        pos = put_dec(msg, sizeof(msg), pos, (UINT32)n);
        msg[pos] = 0;
        kputs(msg);
        kputs("\n");
        {
            UINT32 i = 0, lp = 0;
            char line[128];
            while (i < (UINT32)n && lp < sizeof(line) - 1u &&
                   g_e2e_buf[i] != '\r' && g_e2e_buf[i] != '\n') {
                line[lp++] = (char)g_e2e_buf[i++];
            }
            line[lp] = 0;
            kputs(line);
            kputs("\n");
        }
    } else {
        kputs("[E2E] FAIL: https_get returned ");
        {
            char msg[16];
            UINT32 pos = 0;
            const char *p = "[E2E] FAIL: https_get returned ";
            while (*p != 0 && pos < sizeof(msg) - 1) msg[pos++] = *p++;
            pos = put_dec(msg, sizeof(msg), pos, (UINT32)(-n));
            msg[pos] = 0;
            kputs(msg);
            kputs("\n");
        }
        return 0;
    }
    g_e2e_done = 1;
    return 1;
}
#endif

void novu_service_init(UINT32 now) {
    __builtin_memset(&g_svc, 0, sizeof(g_svc));
    g_svc.state = NOVU_SVC_IDLE;
    g_svc.last_tick = now;
}

void novu_service_tick(UINT32 now) {
    if (now - g_svc.last_tick < NOVU_TICK_INTERVAL) {
        return;
    }
    g_svc.last_tick = now;

#if NOVUS_HTTPS_E2E_PROBE
    /* Kernel-side end-to-end probe. The kernel TCP stack has ONE session,
     * shared with the NCSI connectivity check. If the HTTPS probe starts
     * while NCSI is still in DNS/TCP/HTTP, the two jobs overwrite each
     * other's receive buffer and the certificate message arrives corrupt.
     * Wait for NCSI to latch a terminal verdict AND for the session to be
     * free, then run (with a bounded retry for transient failures). */
    if (!g_e2e_done && net_dhcp_state() == NET_DHCP_BOUND &&
        net_http_ready() && g_e2e_tries < NOVU_E2E_MAX_TRIES) {
        int ncsi = net_ncsi_status();
        int ncsi_done = (ncsi == NET_NCSI_OK ||
                         ncsi == NET_NCSI_FAIL_LINK ||
                         ncsi == NET_NCSI_FAIL_DNS ||
                         ncsi == NET_NCSI_FAIL_TCP ||
                         ncsi == NET_NCSI_FAIL_HTTP ||
                         ncsi == NET_NCSI_FAIL_TIMEOUT);
        int phase = net_tcp_phase();
        if (ncsi_done && (phase == NET_TCP_IDLE || phase == NET_TCP_CLOSED ||
                          phase == NET_TCP_FAIL)) {
            g_e2e_tries++;
            kputs("[E2E] link up, cabundle ready: starting HTTPS probe\n");
            (void)novu_e2e_probe();
        }
    }
#endif

    /* A real poll (local scan / HTTPS index refresh) is enqueued by the
     * transport layer; the state machine itself only bounds the rate. */
    if (g_svc.state == NOVU_SVC_IDLE) {
        /* idle: nothing to do this sample */
    } else if (g_svc.state == NOVU_SVC_INSTALL) {
        if (g_svc.progress < 1000u) {
            g_svc.progress += 100u;
        }
        if (g_svc.progress >= 1000u) {
            g_svc.state = NOVU_SVC_IDLE;
        }
    }
}

void novu_service_request_check(void) {
    if (g_svc.state == NOVU_SVC_IDLE) {
        g_svc.state = NOVU_SVC_CHECKING;
        g_svc.progress = 0;
    }
}

void novu_service_begin(const UINT8 *uuid, UINT8 level) {
    __builtin_memcpy(g_svc.uuid, uuid, 8);
    g_svc.level = level;
    g_svc.retry = 0;
    g_svc.step = 0;
    g_svc.progress = 0;
    g_svc.state = NOVU_SVC_DOWNLOAD;
}

void novu_service_set_state(UINT8 state) {
    g_svc.state = state;
    g_svc.progress = 0;
}

void novu_service_advance(UINT32 progress) {
    if (progress > 1000u) progress = 1000u;
    g_svc.progress = progress;
    if (progress >= 1000u && g_svc.state == NOVU_SVC_INSTALL) {
        g_svc.state = NOVU_SVC_IDLE;
    }
}

void novu_service_fail(void) {
    g_svc.retry++;
    if (g_svc.retry >= NOVU_MAX_RETRY) {
        g_svc.state = NOVU_SVC_ERROR;
    } else {
        g_svc.state = NOVU_SVC_IDLE;
    }
}

const novu_service_t *novu_service_get(void) {
    return &g_svc;
}

static char hexd(UINT8 x) {
    return (char)(x < 10 ? ('0' + x) : ('a' + (x - 10)));
}
static UINT8 hexval(char c) {
    if (c >= '0' && c <= '9') return (UINT8)(c - '0');
    if (c >= 'a' && c <= 'f') return (UINT8)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (UINT8)(c - 'A' + 10);
    return 0;
}
static UINT32 put_dec(char *b, UINT32 cap, UINT32 pos, UINT32 v) {
    char t[12];
    int k = 0;
    if (v == 0) { if (pos < cap) b[pos++] = '0'; return pos; }
    while (v > 0) { t[k++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (k > 0) { if (pos >= cap) break; b[pos++] = t[--k]; }
    return pos;
}
static UINT32 parse_dec(const char **pp) {
    UINT32 v = 0;
    while (**pp >= '0' && **pp <= '9') { v = v * 10u + (UINT32)(**pp - '0'); (*pp)++; }
    return v;
}

INT32 novu_service_journal_serialize(char *buf, UINT32 cap) {
    UINT32 pos = 0;
    UINT32 i;
    if (buf == (char *)0 || cap < 32) return 0;
    pos = put_dec(buf, cap, pos, g_svc.state);
    if (pos < cap) buf[pos++] = ' ';
    pos = put_dec(buf, cap, pos, g_svc.retry);
    if (pos < cap) buf[pos++] = ' ';
    for (i = 0; i < 8; i++) {
        if (pos + 1 >= cap) return 0;
        buf[pos++] = hexd((UINT8)(g_svc.uuid[i] >> 4));
        buf[pos++] = hexd((UINT8)(g_svc.uuid[i] & 0x0F));
    }
    if (pos < cap) buf[pos++] = ' ';
    pos = put_dec(buf, cap, pos, g_svc.level);
    if (pos < cap) buf[pos++] = ' ';
    pos = put_dec(buf, cap, pos, g_svc.step);
    if (pos < cap) buf[pos++] = ' ';
    pos = put_dec(buf, cap, pos, g_svc.progress);
    if (pos < cap) buf[pos++] = ' ';
    pos = put_dec(buf, cap, pos, g_svc.last_tick);
    if (pos < cap) buf[pos++] = '\n';
    if (pos < cap) buf[pos] = 0;
    return (INT32)pos;
}

INT32 novu_service_journal_parse(const char *buf, UINT32 len) {
    const char *p = buf;
    const char *end = buf + len;
    UINT32 i;
    __builtin_memset(&g_svc, 0, sizeof(g_svc));
    g_svc.state = (UINT8)parse_dec(&p);
    if (p < end && *p == ' ') p++;
    g_svc.retry = (UINT8)parse_dec(&p);
    if (p < end && *p == ' ') p++;
    for (i = 0; i < 8; i++) {
        if (p + 1 >= end) return -1;
        g_svc.uuid[i] = (UINT8)((hexval(p[0]) << 4) | hexval(p[1]));
        p += 2;
    }
    if (p < end && *p == ' ') p++;
    g_svc.level = (UINT8)parse_dec(&p);
    if (p < end && *p == ' ') p++;
    g_svc.step = parse_dec(&p);
    if (p < end && *p == ' ') p++;
    g_svc.progress = parse_dec(&p);
    if (p < end && *p == ' ') p++;
    g_svc.last_tick = parse_dec(&p);
    return 0;
}
