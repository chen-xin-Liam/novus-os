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

/* Second block: what a program calls before it draws anything. */
typedef struct {
  unsigned short wYear, wMonth, wDayOfWeek, wDay;
  unsigned short wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME;

typedef struct {
  unsigned short wProcessorArchitecture, wReserved;
  unsigned int   dwPageSize;
  unsigned long long lpMinimumApplicationAddress;
  unsigned long long lpMaximumApplicationAddress;
  unsigned long long dwActiveProcessorMask;
  unsigned int   dwNumberOfProcessors, dwProcessorType, dwAllocationGranularity;
  unsigned short wProcessorLevel, wProcessorRevision;
} SYSTEMINFO;

__declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void *mod, char *buf,
                                                              unsigned long size);
__declspec(dllimport) void *__stdcall VirtualAlloc(void *addr, unsigned long long size,
                                                  unsigned long type, unsigned long prot);
__declspec(dllimport) int __stdcall VirtualFree(void *addr, unsigned long long size,
                                                unsigned long type);
__declspec(dllimport) void *__stdcall GetProcessHeap(void);
__declspec(dllimport) void *__stdcall HeapAlloc(void *heap, unsigned long flags,
                                                unsigned long long bytes);
__declspec(dllimport) int __stdcall HeapFree(void *heap, unsigned long flags, void *mem);
__declspec(dllimport) void __stdcall GetSystemTime(SYSTEMTIME *st);
__declspec(dllimport) int __stdcall QueryPerformanceCounter(unsigned long long *out);
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(unsigned long long *out);
__declspec(dllimport) unsigned long __stdcall GetCurrentProcessId(void);
__declspec(dllimport) void __stdcall GetSystemInfo(SYSTEMINFO *si);

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

/* Decimal, because the log reads better than hex for dates and counts. */
static void append_num(char *dst, DWORD *n, unsigned long long v, int width) {
  char digits[24];
  int  i = 0;
  int  pad;

  if (v == 0ull) {
    digits[i++] = '0';
  }
  while (v != 0ull && i < 23) {
    digits[i++] = (char)('0' + (char)(v % 10ull));
    v /= 10ull;
  }
  for (pad = width - i; pad > 0; pad--) {
    if (*n + 1u >= 64u) {
      return;
    }
    dst[(*n)++] = '0';
  }
  while (i > 0 && *n + 1u < 64u) {
    dst[(*n)++] = digits[--i];
  }
  dst[*n] = '\0';
}

static void append_str(char *dst, DWORD *n, const char *src) {
  while (*src != '\0' && *n + 1u < 63u) {
    dst[(*n)++] = *src++;
  }
  dst[*n] = '\0';
}

/* One scratch line for every message; the layer prints 64 bytes at a time,
 * so the buffer stays just under that. */
static void line(const char *a, const char *b, unsigned long long v, int width) {
  char  buf[64];
  DWORD n = 0;

  append_str(buf, &n, a);
  if (b != 0) {
    append_str(buf, &n, b);
  }
  if (width >= 0) {
    append_num(buf, &n, v, width);
  }
  append_str(buf, &n, "\n");
  out(buf);
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

  /* ---- second block: memory, clock, machine ---- */
  {
    char        name[48];
    void       *block;
    void       *heap_mem;
    SYSTEMTIME  st;
    SYSTEMINFO  si;
    unsigned long long qpc = 0ull;
    unsigned long long qpf = 0ull;
    unsigned long       i;

    GetModuleFileNameA(0, name, (unsigned long)sizeof(name));
    line("image: ", name, 0ull, -1);

    block = VirtualAlloc(0, 65536ull, 0x3000ul, 0x04ul);
    if (block != 0) {
      unsigned char *p = (unsigned char *)block;
      unsigned long  sum = 0ul;

      /* Touching every page proves the whole range is mapped, not just the
       * first page the allocator happened to make reachable. */
      for (i = 0; i < 65536ul; i += 4096ul) {
        p[i] = (unsigned char)(i >> 12);
        sum += p[i];
      }
      p[0] = 0xA5u;
      line("virtual: block ok, pagesum=", 0, (unsigned long long)sum, 0);
      VirtualFree(block, 0, 0x8000ul);
    } else {
      out("virtual: alloc failed\n");
    }

    heap_mem = HeapAlloc(GetProcessHeap(), 0, 4096ull);
    if (heap_mem != 0) {
      ((unsigned char *)heap_mem)[0] = 0x5Au;
      HeapFree(GetProcessHeap(), 0, heap_mem);
      out("heap: alloc/free ok\n");
    } else {
      out("heap: alloc failed\n");
    }

    GetSystemTime(&st);
    line("date: ", 0, st.wYear, 4);
    line("  month ", 0, st.wMonth, 0);
    line("  day ", 0, st.wDay, 0);
    line("  hour ", 0, st.wHour, 0);
    line("  minute ", 0, st.wMinute, 0);

    if (QueryPerformanceFrequency(&qpf) != 0) {
      line("qpf: ", 0, qpf, 0);
    }
    if (QueryPerformanceCounter(&qpc) != 0) {
      line("qpc: ", 0, qpc, 0);
    }

    GetSystemInfo(&si);
    line("page: ", 0, si.dwPageSize, 0);
    line("cpus: ", 0, si.dwNumberOfProcessors, 0);

    line("pid: ", 0, GetCurrentProcessId(), 0);
  }

  ExitProcess(0);
}
