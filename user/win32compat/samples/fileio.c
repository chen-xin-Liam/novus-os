/**
 * Novus OS - M9 Win32 sample: files and libraries.
 *
 * A window proves the layer can draw; this proves it can work. Every check
 * goes through the same Win32 entry points a real program would call - 
 * CreateFileA, WriteFile, ReadFile, GetFileSize, GetFileAttributesA,
 * CreateDirectoryA, FindFirstFileA/FindNextFileA/FindClose, DeleteFileA - 
 * and then confirms the effect through a second path (the bytes read back,
 * the enumeration that finds the name) rather than trusting a return value
 * alone.
 *
 * Console only: one line per check on the serial console, then exit.
 */

typedef unsigned long long u64;
typedef unsigned int       u32;
typedef long long          i64;
typedef int                i32;

__declspec(dllimport) void *GetStdHandle(u32 which);
__declspec(dllimport) int   WriteConsoleA(void *h, const void *text,
                                          u32 len, u32 *written, void *ov);
__declspec(dllimport) int   WriteFile(void *h, const void *text, u32 len,
                                      u32 *written, void *ov);
__declspec(dllimport) int   ReadFile(void *h, void *buf, u32 len, u32 *read,
                                     void *ov);
__declspec(dllimport) void  ExitProcess(u32 code);
__declspec(dllimport) void  Sleep(u32 ms);
__declspec(dllimport) void *CreateFileA(const char *name, u32 access, u32 share,
                                        void *sec, u32 creation, u32 flags,
                                        void *templ);
__declspec(dllimport) int   CloseHandle(void *h);
__declspec(dllimport) u32   GetFileSize(void *h, u32 *high);
__declspec(dllimport) u32   GetFileAttributesA(const char *name);
__declspec(dllimport) int   DeleteFileA(const char *name);
__declspec(dllimport) int   CreateDirectoryA(const char *name, void *sec);
__declspec(dllimport) void *FindFirstFileA(const char *name, void *data);
__declspec(dllimport) int   FindNextFileA(void *h, void *data);
__declspec(dllimport) int   FindClose(void *h);
__declspec(dllimport) void *LoadLibraryA(const char *name);
__declspec(dllimport) void *GetProcAddress(void *module, const char *name);
__declspec(dllimport) int   FreeLibrary(void *module);

#define STD_OUTPUT_HANDLE ((u32)-11)

#define GENERIC_READ    0x80000000u
#define GENERIC_WRITE   0x40000000u
#define CREATE_NEW      1u
#define CREATE_ALWAYS   2u
#define OPEN_EXISTING   3u
#define OPEN_ALWAYS     4u
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define FIND_DATA_LEN   320u

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

static void expect_true(const char *what, int bad) {
  say("  ");
  say(what);
  if (bad == 0) {
    say("  ok\n");
  } else {
    say("  FAIL\n");
    g_fail++;
  }
}

/* Make one string in the image so the path literals stay in .rdata and no
 * CRT is needed. */
static void make(char *out, const char *src) {
  u32 i;

  for (i = 0; src[i] != '\0'; i++) {
    out[i] = src[i];
  }
  out[i] = '\0';
}

void novus_win32_main(void) {
  char  dir[16];
  char  file[24];
  char  glob[16];
  char  gone[24];
  char  text[32];
  char  back[32];
  u32   findbuf[FIND_DATA_LEN / 4u];
  void *fh;
  void *dll;
  void *dll_bad;
  void *proc;
  void *proc_bad;
  void *find;
  u32   written;
  u32   high;
  u32   read;
  u32   size;
  u32   i;
  int   found;

  make(dir, "/fio");
  make(file, "/fio/note.txt");
  make(glob, "/fio/*");
  make(gone, "/fio/note.txt");
  make(text, "novus file io");
  for (i = 0; i < sizeof(back); i++) {
    back[i] = 0;
  }

  say("\n[FIO] files, directories, enumeration, libraries\n");

  /* ---- a directory to work in ----------------------------------------- */

  /* CreateDirectoryA returns 0 for "already there", which is not a failure
   * of this layer, so the check accepts either path and then reads the
   * attribute back: 0x10 is what a directory must report. */
  expect_true("CreateDirectoryA(/fio)",
              (GetFileAttributesA(dir) == 0x10u)
                  ? 0
                  : ((CreateDirectoryA(dir, 0) == 1) ? 0 : 1));
  expect("GetFileAttributesA(dir) = DIRECTORY",
         (u64)GetFileAttributesA(dir), 0x10u);

  /* ---- write a file, then prove it through GetFileSize and a re-read --- */

  fh = CreateFileA(file, GENERIC_WRITE, 0u, 0, CREATE_ALWAYS, 0u, 0);
  say("  CreateFileA(/fio/note.txt) h = ");
  say_hex((u64)fh);
  say("\n");
  if (fh == 0) {
    say("[FIO] FAIL: CreateFileA returned NULL\n");
    ExitProcess(2u);
  }
  expect_true("handle is tagged as a file",
              ((u32)(u64)fh & 0xFF000000u) != 0x22000000u);

  written = 0;
  expect("WriteFile", (u64)WriteFile(fh, text, 13u, &written, 0), 1u);
  expect("  bytes written = 13", (u64)written, 13u);
  high = 0;
  size = GetFileSize(fh, &high);
  expect("GetFileSize = 13", (u64)size, 13u);
  expect("CloseHandle", (u64)CloseHandle(fh), 1u);
  expect("CloseHandle again fails", (u64)CloseHandle(fh), 0u);

  fh = CreateFileA(file, GENERIC_READ, 0u, 0, OPEN_EXISTING, 0u, 0);
  expect_true("OPEN_EXISTING on the new file", fh == 0 ? 1 : 0);
  read = 0;
  expect("ReadFile", (u64)ReadFile(fh, back, 13u, &read, 0), 1u);
  expect("  bytes read = 13", (u64)read, 13u);
  found = 0;
  for (i = 0; i < 13u; i++) {
    if (back[i] != text[i]) {
      found = 1;
    }
  }
  expect_true("payload survived the round trip", found);
  expect("CloseHandle(read handle)", (u64)CloseHandle(fh), 1u);

  /* CREATE_NEW on an existing name must refuse; the mode is not a hint. */
  fh = CreateFileA(file, GENERIC_WRITE, 0u, 0, CREATE_NEW, 0u, 0);
  expect("CREATE_NEW on an existing file refused", (u64)fh, 0u);

  /* ---- attributes ----------------------------------------------------- */

  expect("GetFileAttributesA(file)", (u64)GetFileAttributesA(file), 0x80u);
  expect("GetFileAttributesA(missing) = INVALID",
         (u64)GetFileAttributesA("/fio/nothing.here"),
         (u64)INVALID_FILE_ATTRIBUTES);

  /* ---- enumeration ---------------------------------------------------- */

  /* WIN32_FIND_DATAA is 320 bytes; handing the layer a 32-byte buffer here
   * was the sample's own bug (and it clobbered the stack), so the buffer
   * is spelled out at the size the layer fills. */
  find = FindFirstFileA(glob, findbuf);
  say("  FindFirstFileA(/fio/*) h = ");
  say_hex((u64)find);
  say("\n");
  if (find == 0) {
    say("[FIO] FAIL: nothing enumerated\n");
    ExitProcess(3u);
  }
  /* The find handle shares the 0x23000000 tag, so a program cannot mix it
   * up with a file handle. */
  expect_true("find handle tag", ((u32)(u64)find & 0xFFFF0000u) != 0x23000000u);
  i = 0;
  found = 0;
  do {
    /* The name is at 0x2C in WIN32_FIND_DATAA. */
    char *name = (char *)findbuf + 0x2C;

    say("    ");
    say(name);
    say("\n");
    if (name[0] == 'n' && name[1] == 'o' && name[2] == 't' && name[3] == 'e' &&
        name[4] == '.' && name[5] == 't' && name[6] == 'x' && name[7] == 't' &&
        name[8] == '\0') {
      found = 1;
    }
    i++;
    findbuf[0] = 0;
  } while (FindNextFileA(find, findbuf) != 0 && i < 8u);
  expect_true("enumeration found note.txt", found == 0 ? 1 : 0);
  expect("FindClose", (u64)FindClose(find), 1u);

  /* ---- delete --------------------------------------------------------- */

  expect("DeleteFileA", (u64)DeleteFileA(gone), 1u);
  expect("GetFileAttributesA after delete = INVALID",
         (u64)GetFileAttributesA(gone), (u64)INVALID_FILE_ATTRIBUTES);
  expect("DeleteFileA again refused", (u64)DeleteFileA(gone), 0u);

  /* ---- libraries ------------------------------------------------------ */

  dll = LoadLibraryA("gdi32.dll");
  expect_true("LoadLibraryA(gdi32.dll)", dll == 0 ? 1 : 0);
  proc = GetProcAddress(dll, "TextOutA");
  expect_true("GetProcAddress(TextOutA)", proc == 0 ? 1 : 0);
  expect_true("the entry point is in the thunk page",
              ((u32)(u64)proc & 0xFFF00000u) != 0x60000000u);
  dll_bad = LoadLibraryA("ws2_32.dll");
  expect("LoadLibraryA(ws2_32.dll) refused", (u64)dll_bad, 0u);
  proc_bad = GetProcAddress(dll, "NoSuchEntry");
  expect("GetProcAddress(NoSuchEntry) refused", (u64)proc_bad, 0u);
  expect("FreeLibrary", (u64)FreeLibrary(dll), 1u);
  expect("FreeLibrary(NULL) refused", (u64)FreeLibrary(0), 0u);

  if (g_fail == 0u) {
    say("[FIO] PASS\n");
    ExitProcess(0u);
  }
  say("[FIO] FAIL count = ");
  say_hex((u64)g_fail);
  say("\n");
  ExitProcess(1u);
}
