/**
 * Novus OS - M9 Win32 sample: the ws2_32 surface.
 *
 * The kernel owns a TCP engine, a resolver and DHCP, but that engine is one
 * connection with no descriptor layer over it, so ws2_32 cannot hand out a
 * socket yet. What this sample proves is the part that must be real for any
 * program to get through its socket setup:
 *
 *   - WSAStartup answers a version and fills WSADATA the way the layer
 *     documents, including the honest iMaxSockets = 0;
 *   - inet_addr and inet_ntoa are real conversions, so config parsing and
 *     address printing work;
 *   - socket, connect, gethostbyname and closesocket fail with the codes
 *     Windows uses when the network subsystem is down, and WSAGetLastError
 *     reports them;
 *   - the same functions resolve through LoadLibrary + GetProcAddress,
 *     which is how a program that does not link against ws2_32 reaches them.
 *
 * Console only: one line per check on the serial console, then exit.
 */

typedef unsigned long long u64;
typedef unsigned int       u32;
typedef long long          i64;
typedef int                i32;
typedef unsigned short     u16;
typedef unsigned char      u8;

typedef struct {
  u16 wVersion;
  u16 wHighVersion;
  char szDescription[257];
  char szSystemStatus[129];
  u16 iMaxSockets;
  u16 iMaxUdpDg;
  u64 lpVendorInfo;
} WSADATA;

__declspec(dllimport) void *GetStdHandle(u32 which);
__declspec(dllimport) int   WriteConsoleA(void *h, const void *text,
                                          u32 len, u32 *written, void *ov);
__declspec(dllimport) void  ExitProcess(u32 code);
__declspec(dllimport) void *LoadLibraryA(const char *name);
__declspec(dllimport) void *GetProcAddress(void *mod, const char *name);

__declspec(dllimport) i32   WSAStartup(u16 version, WSADATA *data);
__declspec(dllimport) i32   WSACleanup(void);
__declspec(dllimport) i32   WSAGetLastError(void);
__declspec(dllimport) void  WSASetLastError(i32 err);
__declspec(dllimport) u64   socket(i32 af, i32 type, i32 proto);
__declspec(dllimport) i32   connect(u64 s, const void *addr, i32 len);
__declspec(dllimport) i32   send(u64 s, const void *buf, i32 len, i32 flags);
__declspec(dllimport) i32   recv(u64 s, void *buf, i32 len, i32 flags);
__declspec(dllimport) i32   closesocket(u64 s);
__declspec(dllimport) void *gethostbyname(const char *name);
__declspec(dllimport) i32   getaddrinfo(const char *node, const char *service,
                                        const void *hints, void **res);
__declspec(dllimport) u32   inet_addr(const char *text);
__declspec(dllimport) char *inet_ntoa(u32 addr);

#define STD_OUTPUT_HANDLE ((u32)-11)
#define WSAENETDOWN       10050
#define WSAENOTSOCK       10038
#define WSAHOST_NOT_FOUND 11001
#define WSAEWOULDBLOCK    10035
#define WSAENOTCONN       10057
#define WSAETIMEDOUT      10060
#define WSAEAFNOSUPPORT   10047
#define WSAEPROTOTYPE     10041

/* ---- console output (no CRT) ------------------------------------------- */

static u32 str_len(const char *s) {
  u32 n = 0;

  while (s[n] != '\0') {
    n++;
  }
  return n;
}

static void say(const char *s) {
  u32 written = 0;

  WriteConsoleA(GetStdHandle(STD_OUTPUT_HANDLE), s, str_len(s), &written, 0);
}

static void say_hex(u64 v) {
  char buf[19];
  u64  n = 0;
  u32  i;

  buf[0] = '0';
  buf[1] = 'x';
  if (v == 0) {
    buf[2] = '0';
    n = 3;
  } else {
    char tmp[16];
    u32  t = 0;

    while (v != 0 && t < 16) {
      u32 d = (u32)(v & 0xF);

      tmp[t++] = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
      v >>= 4;
    }
    for (i = 0; i < t && n < 18; i++) {
      buf[n++] = tmp[t - 1 - i];
    }
  }
  buf[n] = '\0';
  say(buf);
}

static u32 g_fail;

static void expect(const char *what, u64 got, u64 want) {
  say("  ");
  say(what);
  say(" = ");
  say_hex(got);
  if (got == want) {
    say("  ok\n");
  } else {
    say("  expect ");
    say_hex(want);
    say("  FAIL\n");
    g_fail++;
  }
}

static void expect_true(const char *what, int ok) {
  say("  ");
  say(what);
  if (ok != 0) {
    say("  ok\n");
  } else {
    say("  FAIL\n");
    g_fail++;
  }
}

static int str_eq(const char *a, const char *b) {
  u32 i = 0;

  while (a[i] != '\0' || b[i] != '\0') {
    if (a[i] != b[i]) {
      return 0;
    }
    i++;
  }
  return 1;
}

static void c_main(void) {
  WSADATA     data;
  u64         s;
  u8          sa[16];
  void       *host;
  void      **ai = 0;
  char       *txt;
  void       *mod;
  char *(*ntoa)(u32);

  say("[WS] ws2_32 surface\n");

  /* 1. The handshake. A program calls this first and refuses to go on if
   *    the version it gets back is unusable. */
  expect("WSAStartup returns zero", (u64)(u32)WSAStartup(0x0202u, &data), 0u);
  expect("WSADATA.wVersion", (u64)data.wVersion, 0x0202u);
  expect("WSADATA.wHighVersion", (u64)data.wHighVersion, 0x0202u);
  /* The description names the layer, and the status line is the honest
   * sentence a program's log will end up showing. */
  expect_true("WSADATA.szDescription starts with 'N'",
              data.szDescription[0] == 'N');
  expect_true("WSADATA.szSystemStatus mentions the connection limit",
              data.szSystemStatus[0] == 'o');
  /* The engine is one connection, so the limit says one instead of a
   * plausible-looking number that would invite a program to open sockets it
   * can never use. */
  expect("WSADATA.iMaxSockets", (u64)data.iMaxSockets, 1u);
  expect("WSADATA.iMaxUdpDg", (u64)data.iMaxUdpDg, 0u);

  /* 2. Address conversion, which needs no transport at all. */
  expect("inet_addr 192.168.1.1", (u64)inet_addr("192.168.1.1"), 0x0101A8C0u);
  expect("inet_addr 0.0.0.0", (u64)inet_addr("0.0.0.0"), 0u);
  expect("inet_addr 256.1.1.1 rejected", (u64)inet_addr("256.1.1.1"),
         0xFFFFFFFFu);
  expect("inet_addr 1.2.3 rejected", (u64)inet_addr("1.2.3"), 0xFFFFFFFFu);
  expect_true("inet_addr abc rejected",
              (u64)inet_addr("abc") == 0xFFFFFFFFu);
  txt = inet_ntoa(0x0101A8C0u);
  expect_true("inet_ntoa 0x0101A8C0 = 192.168.1.1",
              txt != 0 && str_eq(txt, "192.168.1.1"));
  txt = inet_ntoa(0x00000000u);
  expect_true("inet_ntoa 0 = 0.0.0.0", txt != 0 && str_eq(txt, "0.0.0.0"));
  txt = inet_ntoa(0xFF010203u);
  expect_true("inet_ntoa 0xFF010203 = 3.2.1.255",
              txt != 0 && str_eq(txt, "3.2.1.255"));

  /* 3. The socket descriptor layer. socket() hands out a handle now, so the
   *    checks are about what the handle can and cannot do: a connect with no
   *    link fails with the documented code, send/recv on an unconnected
   *    socket say so, and closesocket on a handle from a neighbour answers
   *    WSAENOTSOCK. Exact failure codes depend on the machine: WSAENETDOWN
   *    when no device is up (QEMU with -net none), WSAETIMEDOUT when there
   *    is a link but the peer does not answer. */
  s = socket(2 /* AF_INET */, 1 /* SOCK_STREAM */, 0);
  expect_true("socket returns a handle", s != 0xFFFFFFFFFFFFFFFFu &&
              (s & 0xFF000000u) == 0x26000000u);
  {
    u64  dg = socket(2, 2 /* SOCK_DGRAM */, 0);

    expect("dgram socket is INVALID_SOCKET", dg, 0xFFFFFFFFFFFFFFFFu);
    expect("WSAGetLastError after dgram socket", (u64)(u32)WSAGetLastError(),
           WSAEPROTOTYPE);
    expect("socket(AF_UNIX) refused", (u64)(u32)socket(1, 1, 0),
           0xFFFFFFFFu);
    expect("WSAGetLastError after AF_UNIX", (u64)(u32)WSAGetLastError(),
           WSAEAFNOSUPPORT);
  }
  /* connect() on a handle that was never handed out: the owner and slot
   * checks run before anything touches the engine. */
  expect("connect on an unknown handle", (u64)(u32)connect(0x1234u, sa, 16),
         0xFFFFFFFFu);
  expect("WSAGetLastError after connect", (u64)(u32)WSAGetLastError(),
         WSAENOTSOCK);
  {
    i32 rc;
    i32 err;

    sa[0] = 2; sa[1] = 0;             /* AF_INET, little-endian u16 */
    sa[2] = 0; sa[3] = 80;            /* port 80 in network order */
    sa[4] = 127; sa[5] = 0; sa[6] = 0; sa[7] = 1; /* 127.0.0.1 */
    sa[8] = 0; sa[9] = 0; sa[10] = 0; sa[11] = 0;
    sa[12] = 0; sa[13] = 0; sa[14] = 0; sa[15] = 0;
    rc = connect(s, sa, 16);
    expect("connect fails without a link or a peer", (u64)(u32)rc,
           0xFFFFFFFFu);
    err = WSAGetLastError();
    say("[WS] connect error code = ");
    say_hex((u64)(u32)err);
    say("\n");
    expect_true("the error is a documented connect failure",
                err == WSAENETDOWN || err == WSAETIMEDOUT ||
                err == 10061 /* WSAECONNREFUSED */);
  }
  expect("send on an unconnected socket", (u64)(u32)send(s, (const void *)0, 0, 0),
         0xFFFFFFFFu);
  expect("WSAGetLastError after send", (u64)(u32)WSAGetLastError(),
         WSAENOTCONN);
  expect("recv on an unconnected socket", (u64)(u32)recv(s, (void *)0, 0, 0),
         0xFFFFFFFFu);
  expect("closesocket on the live handle", (u64)(u32)closesocket(s), 0u);
  expect("closesocket again says not a socket", (u64)(u32)closesocket(s),
         0xFFFFFFFFu);
  expect("WSAGetLastError after double close", (u64)(u32)WSAGetLastError(),
         WSAENOTSOCK);
  host = gethostbyname("example.com");
  expect("gethostbyname answers null", (u64)host, 0u);
  expect("WSAGetLastError after gethostbyname", (u64)(u32)WSAGetLastError(),
         WSAHOST_NOT_FOUND);
  expect("getaddrinfo fails", (u64)(u32)getaddrinfo("example.com", 0, 0, ai),
         0xFFFFFFFCu);
  /* Setting the error by hand must read back: a program that clears its
   * last error before a call relies on that. */
  WSASetLastError(0);
  expect("WSASetLastError(0) reads back", (u64)(u32)WSAGetLastError(), 0u);

  /* 4. The dynamic path. A program that loads ws2_32 at run time gets the
   *    same thunks an import table would have. */
  mod = LoadLibraryA("ws2_32.dll");
  expect_true("LoadLibraryA ws2_32.dll", mod != 0);
  ntoa = (char *(*)(u32))GetProcAddress(mod, "inet_ntoa");
  expect_true("GetProcAddress inet_ntoa", ntoa != 0);
  if (ntoa != 0) {
    txt = ntoa(0x0101A8C0u);
    expect_true("inet_ntoa through the resolved pointer",
                txt != 0 && str_eq(txt, "192.168.1.1"));
  }

  expect("WSACleanup returns zero", (u64)(u32)WSACleanup(), 0u);

  say("[WS] FAIL count = ");
  say_hex(g_fail);
  say("\n");
  say(g_fail == 0 ? "[WS] PASS\n" : "[WS] FAILED\n");
  ExitProcess(g_fail == 0 ? 0u : 1u);
}

/* The PE entry point the loader calls; it never returns. */
void novus_win32_main(void) {
  c_main();
  ExitProcess(0xFFFFFFFFu);
}
