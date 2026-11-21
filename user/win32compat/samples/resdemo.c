/**
 * Novus OS - M9 Win32 sample: resources.
 *
 * Windows programs read their strings, menus and dialogs out of the .rsrc
 * section of their own image. This sample asks for one RCDATA blob and two
 * string-table entries, which exercises the resource-directory walk on the
 * compatibility side.
 */

typedef unsigned long long u64;
typedef unsigned int       u32;
typedef long long          i64;
typedef int                i32;
typedef char               i8;

__declspec(dllimport) void *GetStdHandle(u32 which);
__declspec(dllimport) int   WriteConsoleA(void *h, const void *text,
                                           u32 len, u32 *written, void *ov);
__declspec(dllimport) void  ExitProcess(u32 code);
__declspec(dllimport) u64   GetModuleHandleA(const char *name);
__declspec(dllimport) u64   FindResourceA(u64 module, const char *name,
                                          const char *type);
__declspec(dllimport) u64   LoadResource(u64 module, u64 res);
__declspec(dllimport) void *LockResource(u64 res);
__declspec(dllimport) u32   SizeofResource(u64 module, u64 res);
__declspec(dllimport) int   LoadStringA(u64 module, u32 id, char *buf,
                                        i32 max);

#define RT_RCDATA 10

#define STD_OUTPUT_HANDLE ((u32)-11)

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

void novus_win32_main(void) {
  u64   module;
  u64   blob;
  u64   loaded;
  void *locked;
  u32   size;
  char  text[64];
  i32   got;

  say("[RES] resources in the image\n");
  module = GetModuleHandleA(0);
  expect_true("GetModuleHandleA(NULL)", module != 0u);

  /* MAKEINTRESOURCE(1) is the pointer value 1, which is how a program asks
   * for a resource by number. */
  blob = FindResourceA(module, (const char *)1, (const char *)RT_RCDATA);
  expect_true("FindResourceA(RCDATA, 1)", blob != 0u);
  size = SizeofResource(module, blob);
  /* windres stores the payload without its terminating NUL, so the blob is
   * exactly the 17 characters in the .rc. */
  expect_true("SizeofResource is the payload length", size == 17u);
  loaded = LoadResource(module, blob);
  expect("LoadResource", loaded, blob);
  locked = LockResource(blob);
  expect_true("LockResource", locked != 0);
  if (locked != 0 && size >= 17u) {
    const char *p = (const char *)locked;

    expect_true("the payload is the one in the .rc",
                p[0] == 'n' && p[1] == 'o' && p[2] == 'v' && p[3] == 'u' &&
                p[4] == 's');
  }

  text[0] = '\0';
  got = LoadStringA(module, 101u, text, (i32)sizeof(text));
  expect("LoadStringA(101) length",
         (u64)(u32)got, (u64)str_len("console says hello"));
  expect_true("LoadStringA(101) text", str_eq(text, "console says hello"));

  text[0] = '\0';
  got = LoadStringA(module, 202u, text, (i32)sizeof(text));
  expect_true("LoadStringA(202) text",
              str_eq(text, "second line from the table"));

  expect("LoadStringA on a missing id", (u64)(u32)LoadStringA(module, 999u,
         text, (i32)sizeof(text)), 0u);
  expect("FindResourceA on a missing type",
         FindResourceA(module, (const char *)1, (const char *)2), 0u);

  say("[RES] FAIL count = ");
  say_hex(g_fail);
  say("\n");
  say(g_fail == 0 ? "[RES] PASS\n" : "[RES] FAILED\n");
  ExitProcess(g_fail == 0 ? 0u : 1u);
}
