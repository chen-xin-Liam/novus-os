/**
 * Novus OS - NCSI connectivity probe.
 *
 * "Is this machine really on the internet?" is answered by the same test
 * Windows uses: fetch a small text file over plain HTTP from a host we
 * control. The probe is three stages - resolve the name with DNS, open a TCP
 * connection to port 80, read the response - and it latches one verdict:
 * NET_NCSI_OK when the server answered 200 with a body, or the stage that
 * failed. Nothing here blocks: each tick advances whatever stage is current.
 */

#include "kernel.h"
#include "sched.h"
#include "net.h"

#if CONFIG_NOVUS_NET

#define NCSI_HOST    "ncsi.novus-os.ryzn.pro"
#define NCSI_PORT    80u
#define NCSI_TIMEOUT 1800u /* 18 s for the whole probe */

static const char k_req[] = "GET /connecttest.txt HTTP/1.1\r\n"
                            "Host: " NCSI_HOST "\r\n"
                            "User-Agent: NovusOS-NCSI\r\n"
                            "Accept: */*\r\n"
                            "Connection: close\r\n"
                            "\r\n";

static UINT8  g_stage;
static UINT16 g_http;
static UINT32 g_started;
static UINT32 g_ms;
static UINT32 g_host;

int net_ncsi_status(void) { return (int)g_stage; }
UINT16 net_ncsi_http_code(void) { return g_http; }
UINT32 net_ncsi_elapsed_ms(void) {
  return (g_stage == NET_NCSI_DNS || g_stage == NET_NCSI_TCP ||
          g_stage == NET_NCSI_HTTP)
             ? (sched_ticks() - g_started) * 10u
             : g_ms;
}

const char *net_ncsi_status_name(void) {
  switch (g_stage) {
  case NET_NCSI_OK:           return "已连接";
  case NET_NCSI_DNS:          return "解析域名";
  case NET_NCSI_TCP:          return "建立连接";
  case NET_NCSI_HTTP:         return "读取响应";
  case NET_NCSI_FAIL_LINK:    return "无网络设备";
  case NET_NCSI_FAIL_DNS:     return "DNS 失败";
  case NET_NCSI_FAIL_TCP:     return "TCP 失败";
  case NET_NCSI_FAIL_HTTP:    return "HTTP 失败";
  case NET_NCSI_FAIL_TIMEOUT: return "检测超时";
  default:                    return "未检测";
  }
}

static void ncsi_dec(UINT32 v) {
  char   buf[11];
  UINT32 n = 0;
  UINT32 i;

  if (v == 0u) {
    kputs("0");
    return;
  }
  while (v != 0u && n < sizeof(buf)) {
    buf[n++] = (char)('0' + (v % 10u));
    v /= 10u;
  }
  for (i = n; i > 0u; i--) {
    char c[2];

    c[0] = buf[i - 1u];
    c[1] = '\0';
    kputs(c);
  }
}

static void ncsi_fail(UINT8 stage) {
  g_ms = (sched_ticks() - g_started) * 10u;
  g_stage = stage;
  net_tcp_close();
  kputs("[NCSI] ");
  kputs(net_ncsi_status_name());
  kputs(" - not connected\n");
}

/*
 * Body check: the endpoint is ours, so a 200 with a non-empty file is the
 * proof we want. The first bytes go into the log for the case where a captive
 * portal answered instead of the real server.
 */
static void ncsi_body_check(const UINT8 *d, UINT32 len, UINT32 body) {
  UINT32 i = body;
  char   preview[33];
  UINT32 n = 0;

  while (i < len && (d[i] == ' ' || d[i] == '\r' || d[i] == '\n' ||
                     d[i] == '\t')) {
    i++;
  }
  if (i >= len) {
    return; /* empty body: treat as failure */
  }
  for (; i < len && n + 1u < sizeof(preview); i++) {
    UINT8 c = d[i];

    if (c < 0x20u || c > 0x7Eu) {
      break;
    }
    preview[n++] = (char)c;
  }
  preview[n] = '\0';
  g_ms = (sched_ticks() - g_started) * 10u;
  g_stage = NET_NCSI_OK;
  kputs("[NCSI] connected: ");
  kputs(preview);
  kputs("\n");
}

static void ncsi_finish(void) {
  UINT32 len = net_tcp_rx_len();
  const UINT8 *d = net_tcp_rx_data();
  UINT32 body = 0;
  UINT16 code;
  UINT32 i;
  UINT32 n;

  /* Status parsing lives in http_status(); the body offset is only used when
   * the header block was actually seen. */
  code = 0;
  body = 0;
  if (len >= 12u && d[0] == 'H' && d[1] == 'T' && d[2] == 'T' && d[3] == 'P') {
    n = 0;
    while (n < len && d[n] != ' ') {
      n++;
    }
    n++;
    if (n + 3u <= len && d[n] >= '0' && d[n] <= '9' && d[n + 1u] >= '0' &&
        d[n + 1u] <= '9' && d[n + 2u] >= '0' && d[n + 2u] <= '9') {
      code = (UINT16)((UINT32)(d[n] - '0') * 100u +
                      (UINT32)(d[n + 1u] - '0') * 10u +
                      (UINT32)(d[n + 2u] - '0'));
      for (i = 0; i + 3u < len; i++) {
        if (d[i] == '\r' && d[i + 1u] == '\n' && d[i + 2u] == '\r' &&
            d[i + 3u] == '\n') {
          body = i + 4u;
          break;
        }
      }
    }
  }
  g_http = code;
  if (code == 200u && body != 0u) {
    ncsi_body_check(d, len, body);
    if (g_stage == NET_NCSI_OK) {
      return;
    }
  }
  g_ms = (sched_ticks() - g_started) * 10u;
  g_stage = NET_NCSI_FAIL_HTTP;
  net_tcp_close();
  kputs("[NCSI] http ");
  ncsi_dec((UINT32)code);
  kputs(" - not connected\n");
}

int net_ncsi_check(void) {
  if (g_stage == NET_NCSI_DNS || g_stage == NET_NCSI_TCP ||
      g_stage == NET_NCSI_HTTP) {
    return -1; /* one probe at a time */
  }
  g_http = 0;
  g_ms = 0;
  g_host = 0;
  g_started = sched_ticks();
  if (net_active() == 0 || net_cfg()->ip == 0u) {
    g_stage = NET_NCSI_FAIL_LINK;
    kputs("[NCSI] no link - not connected\n");
    return -1;
  }
  if (net_tcp_phase() == NET_TCP_WAIT || net_tcp_phase() == NET_TCP_OPEN) {
    g_stage = NET_NCSI_IDLE; /* the echo test is using the client */
    return -1;
  }
  net_tcp_close();
  g_stage = NET_NCSI_DNS;
  if (net_dns_resolve(NCSI_HOST) != 0) {
    g_stage = NET_NCSI_FAIL_DNS;
    kputs("[NCSI] dns unavailable - not connected\n");
    return -1;
  }
  kputs("[NCSI] probe http://" NCSI_HOST "/connecttest.txt\n");
  return 0;
}

void net_ncsi_tick(void) {
  UINT32 now = sched_ticks();
  int    st;
  int    ph;

  if (g_stage != NET_NCSI_DNS && g_stage != NET_NCSI_TCP &&
      g_stage != NET_NCSI_HTTP) {
    return;
  }
  if (now - g_started > NCSI_TIMEOUT) {
    ncsi_fail(NET_NCSI_FAIL_TIMEOUT);
    return;
  }
  switch (g_stage) {
  case NET_NCSI_DNS:
    st = net_dns_status();
    if (st == 2) {
      UINT32 ip = net_dns_addr();

      if (ip == 0u) {
        ncsi_fail(NET_NCSI_FAIL_DNS);
        break;
      }
      g_host = ip;
      g_stage = NET_NCSI_TCP;
      if (net_tcp_open(ip, NCSI_PORT, (const UINT8 *)k_req,
                       (UINT32)(sizeof(k_req) - 1u)) != 0) {
        ncsi_fail(NET_NCSI_FAIL_TCP);
      }
    } else if (st != 1) {
      ncsi_fail(NET_NCSI_FAIL_DNS);
    }
    break;
  case NET_NCSI_TCP:
    ph = net_tcp_phase();
    if (ph == NET_TCP_CLOSED || ph == NET_TCP_FAIL) {
      ncsi_finish();
    } else if (ph == NET_TCP_IDLE) {
      ncsi_fail(NET_NCSI_FAIL_TCP);
    } else if (net_tcp_rx_len() + 1u >= NET_TCP_RX_CAP) {
      ncsi_finish(); /* buffer full: enough to judge the reply */
    }
    break;
  default:
    break;
  }
}

#else

int         net_ncsi_check(void) { return -1; }
int         net_ncsi_status(void) { return 0; }
UINT16      net_ncsi_http_code(void) { return 0; }
UINT32      net_ncsi_elapsed_ms(void) { return 0; }
const char *net_ncsi_status_name(void) { return "-"; }
void        net_ncsi_tick(void) {}

#endif
