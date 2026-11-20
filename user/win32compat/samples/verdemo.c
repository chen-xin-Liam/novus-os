/**
 * Novus OS - M9 Win32 sample: the version the layer reports.
 *
 * Installers and older programs branch on GetVersion / GetVersionExA. The
 * default answer is the generation this layer behaves like, and a
 * compatibility mode overrides it through two registry values, the same
 * way the compatibility tab does on Windows:
 *
 *   HKLM\Software\Novus\Compat  WinVersion  SZ     "6.1"
 *   HKLM\Software\Novus\Compat  WinBuild    DWORD  7601
 *
 * This sample reads the default, switches to Windows 7 and then Windows XP,
 * checks that GetVersion and GetVersionExA agree in every mode, and finally
 * puts the default back so the next program is not left in XP mode.
 *
 * Console only, no CRT: one line per check on the serial console, exit code =
 * number of failures.
 */

typedef unsigned int  u32;
typedef unsigned long u32l;
typedef unsigned long long u64;
typedef unsigned char u8;

typedef struct {
  u32 dwOSVersionInfoSize;
  u32 dwMajorVersion;
  u32 dwMinorVersion;
  u32 dwBuildNumber;
  u32 dwPlatformId;
  char szCSDVersion[128];
} OSVERSIONINFOA;

typedef void *HKEY;

__declspec(dllimport) void *GetStdHandle(u32 which);
__declspec(dllimport) int   WriteConsoleA(void *h, const void *text,
                                          u32 len, u32 *written, void *ov);
__declspec(dllimport) void  ExitProcess(u32 code);
__declspec(dllimport) void *GetCurrentProcess(void);

__declspec(dllimport) u32   GetVersion(void);
__declspec(dllimport) int   GetVersionExA(OSVERSIONINFOA *vi);
__declspec(dllimport) int   IsWow64Process(void *process, int *wow64);

__declspec(dllimport) long  RegOpenKeyExA(HKEY key, const char *sub,
                                          u32 reserved, u32 access,
                                          HKEY *out);
__declspec(dllimport) long  RegSetValueExA(HKEY key, const char *name,
                                           u32 reserved, u32 type,
                                           const void *data, u32 size);
__declspec(dllimport) long  RegCloseKey(HKEY key);

#define STD_OUTPUT_HANDLE ((u32)-11)
#define HKEY_LOCAL_MACHINE ((HKEY)0x80000002u)
#define REG_SZ            1u
#define REG_DWORD         4u
#define ERROR_SUCCESS     0u
#define VER_PLATFORM_NT   2u
#define KEY_READ          0x20019u
#define KEY_WRITE         0x20006u

static u32 g_fail;

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

static void say_dec(u32 v) {
  char buf[12];
  u32  n = 0;
  u32  i;

  if (v == 0) {
    buf[n++] = '0';
  } else {
    while (v != 0 && n < 10) {
      buf[n++] = (char)('0' + (v % 10u));
      v /= 10u;
    }
  }
  for (i = 0; i < n / 2; i++) {
    char t = buf[i];

    buf[i] = buf[n - 1 - i];
    buf[n - 1 - i] = t;
  }
  buf[n] = '\0';
  say(buf);
}

static void say_hex(u64 v) {
  char buf[19];
  char tmp[16];
  u32  n = 0;
  u32  t = 0;
  u32  i;

  buf[n++] = '0';
  buf[n++] = 'x';
  if (v == 0) {
    buf[n++] = '0';
  } else {
    while (v != 0 && t < 16) {
      u32 d = (u32)(v & 0xF);

      tmp[t++] = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
      v >>= 4;
    }
    for (i = 0; i < t; i++) {
      buf[n++] = tmp[t - 1 - i];
    }
  }
  buf[n] = '\0';
  say(buf);
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

/* ---- registry ----------------------------------------------------------- */

/* RegSetValueExA writes into an open key, so the compat key is opened once
 * and reused; on Windows the key already exists, and the layer creates the
 * name on open for exactly this case. */
static HKEY open_compat(void) {
  HKEY h = 0;

  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\Novus\\Compat", (u32)0,
                    KEY_READ | KEY_WRITE, &h) != ERROR_SUCCESS) {
    return 0;
  }
  return h;
}

static int set_value(HKEY key, const char *name, u32 type, const void *data,
                     u32 len) {
  return RegSetValueExA(key, name, (u32)0, type, data, len) == ERROR_SUCCESS;
}

/* ---- the check --------------------------------------------------------- */

/* Both calls have to agree: a program reading GetVersion and another reading
 * GetVersionExA are looking at the same machine. */
static void expect(u32 maj, u32 min, u32 build, const char *what) {
  OSVERSIONINFOA vi;
  u32            v;

  vi.dwOSVersionInfoSize = (u32)sizeof(vi);
  vi.dwMajorVersion = 0xFFFFFFFFu;
  vi.dwMinorVersion = 0xFFFFFFFFu;
  vi.dwBuildNumber = 0xFFFFFFFFu;
  vi.dwPlatformId = 0xFFFFFFFFu;
  expect_true("GetVersionExA succeeds", GetVersionExA(&vi) == 1);
  expect_true("major/minor match",
              vi.dwMajorVersion == maj && vi.dwMinorVersion == min);
  expect_true("build matches", vi.dwBuildNumber == build);
  expect_true("platform is NT", vi.dwPlatformId == VER_PLATFORM_NT);
  v = GetVersion();
  expect_true("GetVersion words match",
              (v & 0xFFFFu) == maj && ((v >> 16) & 0xFFFFu) == min);
  say("[VER] ");
  say(what);
  say(": ");
  say_dec(vi.dwMajorVersion);
  say(".");
  say_dec(vi.dwMinorVersion);
  say(" build ");
  say_dec(vi.dwBuildNumber);
  say(" platform ");
  say_dec(vi.dwPlatformId);
  say(" GetVersion=");
  say_hex((u64)v);
  say("\n");
}

void novus_win32_main(void) {
  int   wow = 1;
  HKEY  key;
  u32   win11_build = 22631u;
  u32   xp_build = 2600u;

  /* Default: this layer behaves like Windows 11. */
  expect(10u, 0u, 22631u, "default");
  expect_true("IsWow64Process succeeds",
              IsWow64Process(GetCurrentProcess(), &wow) == 1);
  expect_true("not a WOW64 process", wow == 0);
  say("[VER] IsWow64Process = ");
  say_dec((u32)wow);
  say("\n");

  /* Windows 7 mode, the one most old programs want. */
  key = open_compat();
  expect_true("open compat key", key != 0);
  expect_true("set WinVersion 6.1",
              set_value(key, "WinVersion", REG_SZ, "6.1", 4u));
  expect_true("set WinBuild",
              set_value(key, "WinBuild", REG_DWORD, &win11_build, 4u));
  expect(6u, 1u, 22631u, "win7 mode");

  /* Windows XP, which some installers insist on. */
  expect_true("set WinVersion 5.1",
              set_value(key, "WinVersion", REG_SZ, "5.1", 4u));
  expect_true("set XP build",
              set_value(key, "WinBuild", REG_DWORD, &xp_build, 4u));
  expect(5u, 1u, 2600u, "xp mode");

  /* Back to the default. */
  expect_true("restore WinVersion",
              set_value(key, "WinVersion", REG_SZ, "10.0", 5u));
  expect_true("restore WinBuild",
              set_value(key, "WinBuild", REG_DWORD, &win11_build, 4u));
  expect(10u, 0u, 22631u, "restored");
  if (key != 0) {
    RegCloseKey(key);
  }

  if (g_fail == 0) {
    say("[VER] PASS\n");
  }
  ExitProcess(g_fail);
}
