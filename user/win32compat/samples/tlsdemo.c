/**
 * Novus OS - M9 Win32 sample: the TEB, thread-local storage, and the screen.
 *
 * Every Windows program is handed a thread information block before its
 * first instruction, and reaches it through %fs. This sample checks the
 * block the loader built byte by byte: the self pointer at %fs:0x30, the
 * client id at %fs:0x40, the stack bounds, and the inline TLS slots at
 * %fs:0x1480 that TlsAlloc/TlsSetValue write and a compiler's thread-local
 * variable reads. It then asks user32 for the screen size.
 *
 * Console only: it writes one line per check to the serial console, then
 * exits. It is also the cheapest regression test to run after touching the
 * loader or the segment-base switch.
 */

typedef unsigned long long u64;
typedef unsigned int       u32;
typedef long long          i64;
typedef int                i32;

__declspec(dllimport) void *GetStdHandle(u32 which);
__declspec(dllimport) int   WriteConsoleA(void *h, const void *text,
                                          u32 len, u32 *written, void *ov);
__declspec(dllimport) u32   TlsAlloc(void);
__declspec(dllimport) int   TlsFree(u32 index);
__declspec(dllimport) u64   TlsGetValue(u32 index);
__declspec(dllimport) int   TlsSetValue(u32 index, u64 value);
__declspec(dllimport) i32   GetSystemMetrics(i32 index);
__declspec(dllimport) u32   GetCurrentProcessId(void);
__declspec(dllimport) u32   GetCurrentThreadId(void);
__declspec(dllimport) void  ExitProcess(u32 code);

#define STD_OUTPUT_HANDLE ((u32)-11)
#define SM_CXSCREEN       0
#define SM_CYSCREEN       1

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
    /* tmp holds the digits least-significant first; reverse them. */
    for (i = 0; i < t && n < 18; i++) {
      buf[n++] = tmp[t - 1 - i];
    }
  }
  buf[n] = '\0';
  say(buf);
}

static void say_dec(u64 v) {
  char buf[21];
  u32  n = 0;
  u32  i;

  if (v == 0) {
    buf[n++] = '0';
  } else {
    char tmp[20];
    u32  t = 0;
    while (v != 0 && t < 20) {
      tmp[t++] = (char)('0' + (u32)(v % 10));
      v /= 10;
    }
    for (i = 0; i < t; i++) {
      buf[n++] = tmp[t - 1 - i];
    }
  }
  buf[n] = '\0';
  say(buf);
}

/* ---- the checks -------------------------------------------------------- */

/* Reading %fs here is the point of the test: the value comes from the
 * thread information block, not from a kernel query. */
static u64 fs_read(u64 off) {
  u64 v;
  __asm__ volatile("movq %%fs:(%1), %0" : "=r"(v) : "r"(off));
  return v;
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

void novus_win32_main(void) {
  u64  teb_self;
  u64  peb_ptr;
  u64  pid_teb;
  u64  tid_teb;
  u64  stack_base;
  u64  slot0;
  u64  via_fs;
  u32  idx1;
  u32  idx2;
  u64  got;
  i32  scr_w;
  i32  scr_h;

  say("\n[TLS] TEB checks on %fs\n");

  teb_self = fs_read(0x30);
  say("  %fs:0x30 self      = ");
  say_hex(teb_self);
  say("\n");
  expect("%fs:0x30 self", teb_self, 0x42000000ULL);

  /* Self must point back at the block itself, not at a copy. */
  peb_ptr = fs_read(0x60); /* PEB pointer: the layer leaves it clear */
  expect("%fs:0x60 PEB (clear)", peb_ptr, 0);

  pid_teb = fs_read(0x40) & 0xFFFFFFFFULL;
  tid_teb = fs_read(0x48) & 0xFFFFFFFFULL;
  expect("ClientId.ProcessId vs API", pid_teb,
         (u64)GetCurrentProcessId());
  expect("ClientId.ThreadId vs API", tid_teb, (u64)GetCurrentThreadId());

  stack_base = fs_read(0x08);
  expect("StackBase", stack_base, 0xA0000000ULL);

  /* The stack limit sits 64 KiB below the base: two pages of room for the
   * first call, and a fault instead of a silent walk into the TEB. */
  expect("StackLimit", fs_read(0x10), 0xA0000000ULL - 0x10000ULL);

  say("\n[TLS] TlsAlloc / TlsSetValue / TlsGetValue\n");
  idx1 = TlsAlloc();
  say("  TlsAlloc           = ");
  say_dec(idx1);
  say("\n");
  expect("first index", idx1, 1); /* slot 0 is reserved */

  idx2 = TlsAlloc();
  say("  TlsAlloc again     = ");
  say_dec(idx2);
  say("\n");
  expect("second index", idx2, 2);

  expect("TlsSetValue(1)", (u64)TlsSetValue(idx1, 0xC0FFEEu), 1);
  got = TlsGetValue(idx1);
  expect("TlsGetValue(1)", got, 0xC0FFEEULL);

  /* Now the same value through %fs, at the offset a compiler uses. */
  via_fs = fs_read(0x1480ULL + (u64)idx1 * 8ULL);
  expect("%fs TLS slot 1", via_fs, 0xC0FFEEULL);

  /* A second index must not alias the first. */
  expect("TlsSetValue(2)", (u64)TlsSetValue(idx2, 0xBEEF), 1);
  expect("%fs TLS slot 2", fs_read(0x1480ULL + (u64)idx2 * 8ULL), 0xBEEFULL);
  expect("slot 1 unchanged", TlsGetValue(idx1), 0xC0FFEEULL);

  slot0 = fs_read(0x1480);
  expect("slot 0 reserved", slot0, 0);

  say("\n[TLS] TlsFree\n");
  expect("TlsFree(1)", (u64)TlsFree(idx1), 1);
  expect("free index reads 0", TlsGetValue(idx1), 0);
  expect("%fs slot cleared", fs_read(0x1480ULL + (u64)idx1 * 8ULL), 0);
  expect("TlsFree(0) refused", (u64)TlsFree(0), 0);
  expect("TlsSetValue(1) refused", (u64)TlsSetValue(idx1, 1), 0);

  say("\n[SM] GetSystemMetrics\n");
  scr_w = GetSystemMetrics(SM_CXSCREEN);
  scr_h = GetSystemMetrics(SM_CYSCREEN);
  say("  screen             = ");
  say_dec((u64)scr_w);
  say(" x ");
  say_dec((u64)scr_h);
  say("\n");
  if (scr_w <= 0 || scr_h <= 0) {
    say("  screen size FAIL\n");
    g_fail++;
  } else {
    say("  screen size ok\n");
  }

  if (g_fail == 0) {
    say("\n[TLS] PASS\n");
  } else {
    say("\n[TLS] FAIL: ");
    say_dec(g_fail);
    say(" check(s)\n");
  }
  ExitProcess(g_fail == 0 ? 0u : 1u);
}
