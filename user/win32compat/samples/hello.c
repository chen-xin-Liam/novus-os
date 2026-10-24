/*
 * Novus OS - M9 Win32 compatibility sample.
 *
 * Built by the top-level Makefile with the mingw cross compiler, no CRT and
 * no <windows.h>: everything is declared here so the import table contains
 * exactly the API subset kernel/win32.c implements. The entry point is
 * novus_win32_main, chosen with -Wl,-e.
 */

typedef unsigned int  DWORD;
typedef long          LONG;
typedef unsigned char BYTE;
typedef void         *HANDLE;
typedef void         *HKEY;

#define HKEY_CURRENT_USER ((HKEY)0x80000001u)

#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define REG_SZ            1u
#define REG_DWORD         4u

__declspec(dllimport) HANDLE __stdcall GetStdHandle(DWORD which);
__declspec(dllimport) int __stdcall WriteFile(HANDLE file, const void *buf,
                                               DWORD len, DWORD *written,
                                               void *overlapped);
__declspec(dllimport) void __stdcall Sleep(DWORD ms);
__declspec(dllimport) DWORD __stdcall GetTickCount(void);
__declspec(dllimport) void __stdcall ExitProcess(DWORD code);
__declspec(dllimport) LONG __stdcall RegOpenKeyExA(HKEY key, const char *sub,
                                                    DWORD reserved, DWORD access,
                                                    HKEY *out);
__declspec(dllimport) LONG __stdcall RegQueryValueExA(HKEY key, const char *name,
                                                       DWORD *reserved, DWORD *type,
                                                       BYTE *data, DWORD *size);
__declspec(dllimport) LONG __stdcall RegSetValueExA(HKEY key, const char *name,
                                                     DWORD reserved, DWORD type,
                                                     const BYTE *data, DWORD size);
__declspec(dllimport) LONG __stdcall RegCloseKey(HKEY key);

static HANDLE g_out;

/* Messages live in a pointer table that must be kept even when the
 * compiler folds the lookups, so the image carries a real DIR64 base
 * relocation: the loader maps it at a base other than its preferred one. */
static const char *const g_lines[] __attribute__((used)) = {
    "Hello from Win32 on Novus\n",
    "win32 layer done\n",
};

static DWORD text_len(const char *s) {
  DWORD n = 0;

  while (s[n] != '\0') {
    n++;
  }
  return n;
}

/* One call per line: the layer prints at most 64 bytes per WriteFile. */
static void out(const char *s) {
  DWORD written = 0;

  WriteFile(g_out, s, text_len(s), &written, 0);
}

void novus_win32_main(void) {
  HKEY key = 0;
  BYTE value[64];
  DWORD size = sizeof(value);
  DWORD type = 0;
  DWORD runs;

  g_out = GetStdHandle(STD_OUTPUT_HANDLE);
  out(g_lines[0]);

  if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Novus", 0, 0, &key) == 0) {
    size = sizeof(value);
    if (RegQueryValueExA(key, "Greeting", 0, &type, value, &size) == 0) {
      char line[64];
      DWORD n = 0;
      DWORD i = 0;

      while (n < sizeof(line) - 1u && "Greeting="[n] != '\0') {
        line[n] = "Greeting="[n];
        n++;
      }
      /* The stored string includes its terminator; drop it. */
      while (i < size && n + 2u < sizeof(line) && value[i] != 0) {
        line[n++] = (char)value[i++];
      }
      line[n++] = '\n';
      line[n] = '\0';
      out(line);
    }
    runs = GetTickCount();
    RegSetValueExA(key, "Runs", 0, REG_DWORD, (const BYTE *)&runs,
                   (DWORD)sizeof(runs));
    RegCloseKey(key);
  }

  Sleep(200);
  out(g_lines[1]);
  ExitProcess(0);
}
