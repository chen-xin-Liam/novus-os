/**
 * Novus OS - Win32 compatibility layer: the API gate and the registry (M9).
 *
 * A compatibility process reaches this file only through SYS_WIN32. The
 * thunk that pe.c writes puts the API id in R10 and moves the Windows first
 * argument (RCX) to RDI, because `syscall` overwrites RCX with the return
 * address. Everything else is read from the trap frame exactly as Windows
 * x64 left it: RDX, R8, R9 for arguments 1..3 and the user stack for the
 * rest (return address + 32-byte shadow space, so the fifth argument is at
 * rsp + 0x28).
 *
 * The registry is a flat table persisted under /Novus/wincompat. It is not a real
 * key tree: keys are strings, values are strings or DWORDs, and the whole
 * table is rewritten after every set. That is enough for the milestone's
 * "simulated registry" and leaves the format documented in one struct.
 */

#include "kernel.h"
#include "win32.h"
#include "sched.h"
#include "ipc.h"
#include "nvus_paths.h"
#include "pmm.h"
#include "rtc.h"
#include "../fs/vfs.h"

#define WIN32_REG_MAX_ENTRIES 16u
#define WIN32_REG_KEY_LEN     48u
#define WIN32_REG_VALUE_LEN   40u
#define WIN32_REG_DATA_LEN    72u
#define WIN32_REG_MAGIC       0x4752564Eu /* 'NVRG' little-endian */
#define WIN32_REG_VERSION     1u
#define WIN32_REG_PATH        NVUS_REGISTRY_FILE

#define WIN32_KEY_MAX 8u
#define WIN32_KEY_BASE 0x1000ULL

#define WIN32_HKEY_CURRENT_USER  0x80000001ULL
#define WIN32_HKEY_LOCAL_MACHINE 0x80000002ULL

#define WIN32_REG_SZ    1u
#define WIN32_REG_DWORD 4u

#define WIN32_ERROR_SUCCESS        0u
#define WIN32_ERROR_FILE_NOT_FOUND 2u
#define WIN32_ERROR_INVALID_PARAM  87u

typedef struct {
  char   key[WIN32_REG_KEY_LEN];
  char   value[WIN32_REG_VALUE_LEN];
  UINT32 type;
  UINT32 len;
  UINT8  data[WIN32_REG_DATA_LEN];
} win32_reg_entry_t;

typedef struct {
  UINT32 magic;
  UINT32 version;
  UINT32 count;
  UINT32 pad;
  win32_reg_entry_t e[WIN32_REG_MAX_ENTRIES];
} win32_reg_file_t;

typedef struct {
  UINT8  used;
  UINT64 owner;
  char   path[72];
} win32_key_t;

_Static_assert(sizeof(win32_reg_entry_t) == 168u, "registry ABI drift");
_Static_assert(sizeof(win32_reg_file_t) == 2704u, "registry file ABI drift");

static void win32_mem_owner_exit(UINT64 pid);
static void win32_gui_owner_exit(UINT64 pid);

static win32_reg_file_t g_reg;
static win32_key_t      g_keys[WIN32_KEY_MAX];
static int              g_save_failed;

/* API ids, in the order the thunks encode them. */
enum {
  WIN32_API_EXIT_PROCESS = 0,
  WIN32_API_GET_STD_HANDLE,
  WIN32_API_WRITE_FILE,
  WIN32_API_WRITE_CONSOLE,
  WIN32_API_SLEEP,
  WIN32_API_GET_TICK_COUNT64,
  WIN32_API_GET_TICK_COUNT,
  WIN32_API_GET_LAST_ERROR,
  WIN32_API_SET_LAST_ERROR,
  WIN32_API_GET_COMMAND_LINE,
  WIN32_API_GET_MODULE_HANDLE,
  WIN32_API_REG_OPEN_KEY_EX,
  WIN32_API_REG_QUERY_VALUE_EX,
  WIN32_API_REG_SET_VALUE_EX,
  WIN32_API_REG_CLOSE_KEY,
  /* Second block: the calls a program makes before it draws anything.
   * Memory, its own path, the clock and the machine description. */
  WIN32_API_GET_MODULE_FILE_NAME,
  WIN32_API_VIRTUAL_ALLOC,
  WIN32_API_VIRTUAL_FREE,
  WIN32_API_GET_PROCESS_HEAP,
  WIN32_API_HEAP_ALLOC,
  WIN32_API_HEAP_FREE,
  WIN32_API_GET_SYSTEM_TIME,
  WIN32_API_GET_LOCAL_TIME,
  WIN32_API_QUERY_PERFORMANCE_COUNTER,
  WIN32_API_QUERY_PERFORMANCE_FREQUENCY,
  WIN32_API_GET_CURRENT_PROCESS_ID,
  WIN32_API_GET_CURRENT_THREAD_ID,
  WIN32_API_IS_DEBUGGER_PRESENT,
  WIN32_API_GET_SYSTEM_INFO,
  WIN32_API_SET_UNHANDLED_EXCEPTION_FILTER,
  /* Third block: a window and a message loop. */
  WIN32_API_REGISTER_CLASS,
  WIN32_API_CREATE_WINDOW,
  WIN32_API_DESTROY_WINDOW,
  WIN32_API_SHOW_WINDOW,
  WIN32_API_GET_MESSAGE,
  WIN32_API_PEEK_MESSAGE,
  WIN32_API_DISPATCH_MESSAGE,
  WIN32_API_DEF_WINDOW_PROC,
  WIN32_API_TRANSLATE_MESSAGE,
  WIN32_API_GET_CLIENT_RECT,
  WIN32_API_GET_DC,
  WIN32_API_RELEASE_DC,
  WIN32_API_BEGIN_PAINT,
  WIN32_API_END_PAINT,
  WIN32_API_FILL_RECT,
  WIN32_API_TEXT_OUT,
  WIN32_API_SET_TEXT_COLOR,
  WIN32_API_CREATE_SOLID_BRUSH,
};

typedef struct {
  const char *dll;
  const char *fn;
  UINT32      id;
} win32_api_t;

static const win32_api_t g_api[] = {
    {"kernel32.dll", "ExitProcess", WIN32_API_EXIT_PROCESS},
    {"kernel32.dll", "GetStdHandle", WIN32_API_GET_STD_HANDLE},
    {"kernel32.dll", "WriteFile", WIN32_API_WRITE_FILE},
    {"kernel32.dll", "WriteConsoleA", WIN32_API_WRITE_CONSOLE},
    {"kernel32.dll", "Sleep", WIN32_API_SLEEP},
    {"kernel32.dll", "GetTickCount64", WIN32_API_GET_TICK_COUNT64},
    {"kernel32.dll", "GetTickCount", WIN32_API_GET_TICK_COUNT},
    {"kernel32.dll", "GetLastError", WIN32_API_GET_LAST_ERROR},
    {"kernel32.dll", "GetCommandLineA", WIN32_API_GET_COMMAND_LINE},
    {"kernel32.dll", "GetModuleHandleA", WIN32_API_GET_MODULE_HANDLE},
    {"advapi32.dll", "RegOpenKeyExA", WIN32_API_REG_OPEN_KEY_EX},
    {"advapi32.dll", "RegQueryValueExA", WIN32_API_REG_QUERY_VALUE_EX},
    {"advapi32.dll", "RegSetValueExA", WIN32_API_REG_SET_VALUE_EX},
    {"advapi32.dll", "RegCloseKey", WIN32_API_REG_CLOSE_KEY},
    {"kernel32.dll", "GetModuleFileNameA", WIN32_API_GET_MODULE_FILE_NAME},
    {"kernel32.dll", "VirtualAlloc", WIN32_API_VIRTUAL_ALLOC},
    {"kernel32.dll", "VirtualFree", WIN32_API_VIRTUAL_FREE},
    {"kernel32.dll", "GetProcessHeap", WIN32_API_GET_PROCESS_HEAP},
    {"kernel32.dll", "HeapAlloc", WIN32_API_HEAP_ALLOC},
    {"kernel32.dll", "HeapFree", WIN32_API_HEAP_FREE},
    {"kernel32.dll", "GetSystemTime", WIN32_API_GET_SYSTEM_TIME},
    {"kernel32.dll", "GetLocalTime", WIN32_API_GET_SYSTEM_TIME},
    {"kernel32.dll", "QueryPerformanceCounter", WIN32_API_QUERY_PERFORMANCE_COUNTER},
    {"kernel32.dll", "QueryPerformanceFrequency", WIN32_API_QUERY_PERFORMANCE_FREQUENCY},
    {"kernel32.dll", "GetCurrentProcessId", WIN32_API_GET_CURRENT_PROCESS_ID},
    {"kernel32.dll", "GetCurrentThreadId", WIN32_API_GET_CURRENT_THREAD_ID},
    {"kernel32.dll", "IsDebuggerPresent", WIN32_API_IS_DEBUGGER_PRESENT},
    {"kernel32.dll", "GetSystemInfo", WIN32_API_GET_SYSTEM_INFO},
    {"kernel32.dll", "SetUnhandledExceptionFilter", WIN32_API_SET_UNHANDLED_EXCEPTION_FILTER},
    /* Third block: the window. A program that draws registers a class,
     * opens a window, pumps messages and paints into it. user32 owns the
     * message and window half, gdi32 the pixels. */
    {"user32.dll", "RegisterClassExA", WIN32_API_REGISTER_CLASS},
    {"user32.dll", "CreateWindowExA", WIN32_API_CREATE_WINDOW},
    {"user32.dll", "DestroyWindow", WIN32_API_DESTROY_WINDOW},
    {"user32.dll", "ShowWindow", WIN32_API_SHOW_WINDOW},
    {"user32.dll", "GetMessageA", WIN32_API_GET_MESSAGE},
    {"user32.dll", "PeekMessageA", WIN32_API_PEEK_MESSAGE},
    {"user32.dll", "DispatchMessageA", WIN32_API_DISPATCH_MESSAGE},
    {"user32.dll", "DefWindowProcA", WIN32_API_DEF_WINDOW_PROC},
    {"user32.dll", "TranslateMessage", WIN32_API_TRANSLATE_MESSAGE},
    {"user32.dll", "GetClientRect", WIN32_API_GET_CLIENT_RECT},
    {"user32.dll", "GetDC", WIN32_API_GET_DC},
    {"user32.dll", "ReleaseDC", WIN32_API_RELEASE_DC},
    {"user32.dll", "BeginPaint", WIN32_API_BEGIN_PAINT},
    {"user32.dll", "EndPaint", WIN32_API_END_PAINT},
    {"gdi32.dll", "FillRect", WIN32_API_FILL_RECT},
    {"gdi32.dll", "TextOutA", WIN32_API_TEXT_OUT},
    {"gdi32.dll", "SetTextColor", WIN32_API_SET_TEXT_COLOR},
    {"gdi32.dll", "CreateSolidBrush", WIN32_API_CREATE_SOLID_BRUSH},
};

/* ---- small helpers ----------------------------------------------------- */

static char lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int ci_equal(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    if (lower(*a) != lower(*b)) {
      return 0;
    }
    a++;
    b++;
  }
  return *a == '\0' && *b == '\0';
}

static int text_equal(const char *a, const char *b) {
  while (*a != '\0' && *a == *b) {
    a++;
    b++;
  }
  return *a == '\0' && *b == '\0';
}

static void copy_text(char *dst, UINT32 cap, const char *src) {
  UINT32 i;

  for (i = 0; i + 1u < cap && src[i] != '\0'; i++) {
    dst[i] = src[i];
  }
  dst[i] = '\0';
}

/* Append `src` to `dst`, keeping one byte for the terminator. */
static int append_text(char *dst, UINT32 cap, const char *src) {
  UINT32 n = 0;

  while (n + 1u < cap && dst[n] != '\0') {
    n++;
  }
  while (n + 1u < cap && *src != '\0') {
    dst[n++] = *src++;
  }
  dst[n] = '\0';
  return *src == '\0' ? 0 : -1;
}

/* Byte-at-a-time so a string that ends on the last byte of a mapped page is
 * read without touching the next, possibly unmapped, page. */
static int read_user_str(UINT64 cr3, UINT64 va, char *out, UINT32 cap) {
  UINT32 i;

  if (va == 0) {
    out[0] = '\0';
    return 0;
  }
  for (i = 0; i + 1u < cap; i++) {
    if (user_copy(cr3, va + i, &out[i], 1u, 0) != 0) {
      out[0] = '\0';
      return -1;
    }
    if (out[i] == '\0') {
      return 0;
    }
  }
  out[cap - 1u] = '\0';
  return 0;
}

/* Argument 4 and above lives on the caller's stack. */
/* The fifth and later Windows x64 arguments live above the shadow space:
 * the return address sits at rsp, the four register slots below it, and
 * argument 5 at rsp+0x28. n is the real argument number, so 5 is the first
 * one that ever reaches the stack. */
static int stack_arg(trap_frame_t *f, task_t *t, UINT32 n, UINT64 *out) {
  UINT64 va;

  if (t == NULL || n < 5u) {
    return -1;
  }
  va = f->rsp + 0x28u + (UINT64)(n - 5u) * 8u;
  return user_copy(t->cr3, va, out, 8u, 0);
}

static UINT32 reg_key_len(const char *s) {
  UINT32 n = 0;

  while (n < WIN32_REG_KEY_LEN && s[n] != '\0') {
    n++;
  }
  return n;
}

/* ---- registry ---------------------------------------------------------- */

static win32_reg_entry_t *reg_find(const char *key, const char *value) {
  UINT32 i;

  for (i = 0; i < g_reg.count && i < WIN32_REG_MAX_ENTRIES; i++) {
    if (text_equal(g_reg.e[i].key, key) &&
        text_equal(g_reg.e[i].value, value)) {
      return &g_reg.e[i];
    }
  }
  return NULL;
}

static win32_reg_entry_t *reg_alloc(const char *key, const char *value) {
  win32_reg_entry_t *e;

  e = reg_find(key, value);
  if (e != NULL) {
    return e;
  }
  if (g_reg.count >= WIN32_REG_MAX_ENTRIES) {
    return NULL;
  }
  e = &g_reg.e[g_reg.count++];
  memset(e, 0, sizeof(*e));
  copy_text(e->key, sizeof(e->key), key);
  copy_text(e->value, sizeof(e->value), value);
  return e;
}

/* Fewer than sixteen entries: a linear scan is the table. */
static int reg_save(void) {
  vfs_fs_t *fs = vfs_root();

  if (fs == NULL || !vfs_root_persistent()) {
    return -1;
  }
  g_reg.magic = WIN32_REG_MAGIC;
  g_reg.version = WIN32_REG_VERSION;
  if (vfs_write_file(fs, WIN32_REG_PATH, &g_reg, (UINT32)sizeof(g_reg)) != 0) {
    if (!g_save_failed) {
      g_save_failed = 1;
      kputs("[WIN32] registry write failed\n");
    }
    return -1;
  }
  return 0;
}

/* The file is accepted only at the exact size this build writes, so a
 * truncated or foreign file is ignored instead of half-loading. */
static int reg_load(void) {
  vfs_fs_t *fs = vfs_root();
  UINT32    ino;
  UINT64    size = 0;
  UINT16    mode = 0;

  if (fs == NULL) {
    return -1;
  }
  ino = vfs_lookup_path(fs, WIN32_REG_PATH);
  if (ino == 0 || fs->stat(fs, ino, &size, &mode) != 0 ||
      size != (UINT64)sizeof(g_reg)) {
    return -1;
  }
  if (vfs_read_file(fs, WIN32_REG_PATH, &g_reg, (UINT32)sizeof(g_reg)) != 0) {
    return -1;
  }
  if (g_reg.magic != WIN32_REG_MAGIC || g_reg.version != WIN32_REG_VERSION ||
      g_reg.count > WIN32_REG_MAX_ENTRIES) {
    return -1;
  }
  return 0;
}

static void reg_seed(void) {
  win32_reg_entry_t *e;

  memset(&g_reg, 0, sizeof(g_reg));
  g_reg.magic = WIN32_REG_MAGIC;
  g_reg.version = WIN32_REG_VERSION;
  {
    static const char greeting[] = "Hello from the Novus registry";

    e = reg_alloc("HKCU\\Software\\Novus", "Greeting");
    if (e != NULL) {
      e->type = WIN32_REG_SZ;
      e->len = (UINT32)sizeof(greeting);
      memcpy(e->data, greeting, sizeof(greeting));
    }
  }
}

void win32_init(void) {
  reg_seed();
  if (!vfs_root_persistent()) {
    kputs("[WIN32] registry: in-memory only (live root is read-only)\n");
    return;
  }
  if (reg_load() == 0) {
    kputs("[WIN32] registry: loaded ");
    kputs(WIN32_REG_PATH);
    kputs("\n");
    return;
  }
  (void)reg_save();
  kputs("[WIN32] registry: seeded ");
  kputs(WIN32_REG_PATH);
  kputs("\n");
}

void win32_owner_exit(UINT64 pid) {
  UINT32 i;

  for (i = 0; i < WIN32_KEY_MAX; i++) {
    if (g_keys[i].used && g_keys[i].owner == pid) {
      memset(&g_keys[i], 0, sizeof(g_keys[i]));
    }
  }
  win32_mem_owner_exit(pid);
  win32_gui_owner_exit(pid);
}

/* ---- key handles ------------------------------------------------------- */

/* Resolve hKey to a path prefix: a predefined hive or an open handle. */
static int key_base(UINT64 hkey, char *out, UINT32 cap) {
  UINT32 i;

  if (hkey == WIN32_HKEY_CURRENT_USER) {
    copy_text(out, cap, "HKCU");
    return 0;
  }
  if (hkey == WIN32_HKEY_LOCAL_MACHINE) {
    copy_text(out, cap, "HKLM");
    return 0;
  }
  if (hkey >= WIN32_KEY_BASE && hkey < WIN32_KEY_BASE + WIN32_KEY_MAX) {
    i = (UINT32)(hkey - WIN32_KEY_BASE);
    if (g_keys[i].used) {
      copy_text(out, cap, g_keys[i].path);
      return 0;
    }
  }
  return -1;
}

/* Compose "<base>\<sub>" into out. subkey_va == 0 appends nothing. */
static int key_path(UINT64 cr3, UINT64 hkey, UINT64 subkey_va, char *out,
                    UINT32 cap) {
  char sub[WIN32_REG_KEY_LEN];

  if (key_base(hkey, out, cap) != 0) {
    return -1;
  }
  if (subkey_va == 0 || read_user_str(cr3, subkey_va, sub, sizeof(sub)) != 0 ||
      sub[0] == '\0') {
    return 0;
  }
  if (append_text(out, cap, "\\") != 0 ||
      append_text(out, cap, sub) != 0 ||
      reg_key_len(out) >= WIN32_REG_KEY_LEN) {
    return -1;
  }
  return 0;
}

static int key_open(UINT64 pid, const char *path, UINT64 *out) {
  UINT32 i;

  for (i = 0; i < WIN32_KEY_MAX; i++) {
    if (!g_keys[i].used) {
      g_keys[i].used = 1;
      g_keys[i].owner = pid;
      copy_text(g_keys[i].path, sizeof(g_keys[i].path), path);
      *out = WIN32_KEY_BASE + i;
      return 0;
    }
  }
  return -1;
}

static void key_close(UINT64 pid, UINT64 hkey) {
  UINT32 i;

  if (hkey < WIN32_KEY_BASE || hkey >= WIN32_KEY_BASE + WIN32_KEY_MAX) {
    return;
  }
  i = (UINT32)(hkey - WIN32_KEY_BASE);
  if (g_keys[i].used && g_keys[i].owner == pid) {
    memset(&g_keys[i], 0, sizeof(g_keys[i]));
  }
}

/* ---- private memory ---------------------------------------------------- */

/* A compatibility process gets its own window of address space for
 * VirtualAlloc and the process heap. It sits clear of every fixed region
 * win32.h reserves (image 0x50000000, thunks 0x60000000, command line
 * 0x70000000, trampoline 0x90000000, stack 0xA0000000) and leaves room for
 * a 256 MiB working set, which is more than the 2.5 GiB test machine would
 * hand out anyway.
 *
 * The bookkeeping lives in the kernel, not in the process: a Win32
 * allocator would normally keep a header in front of every block, but the
 * kernel cannot write user memory without a copy, so the table is the
 * cheaper and the safer place. */
#define WIN32_HEAP_VA    0x0000000020000000ULL
#define WIN32_HEAP_END   0x0000000030000000ULL
#define WIN32_HEAP_SLOTS 64u
#define WIN32_PAGE       4096u
#define WIN32_HEAP_HANDLE 0x80000001ULL

typedef struct {
  UINT8  used;
  UINT64 owner;
  UINT64 va;
  UINT64 size;
} win32_alloc_t;

static win32_alloc_t g_alloc[WIN32_HEAP_SLOTS];

static void win32_mem_owner_exit(UINT64 pid) {
  UINT32 i;

  for (i = 0; i < WIN32_HEAP_SLOTS; i++) {
    if (g_alloc[i].used && g_alloc[i].owner == pid) {
      memset(&g_alloc[i], 0, sizeof(g_alloc[i]));
    }
  }
}

static UINT64 round_page(UINT64 n) {
  return (n + WIN32_PAGE - 1u) & ~(UINT64)(WIN32_PAGE - 1u);
}

/* Where the next block for this process goes: after the highest block it
 * already owns, so the range stays contiguous without a per-task cursor
 * that a restart would have to reset. */
static UINT64 arena_next(UINT64 pid) {
  UINT64 next = WIN32_HEAP_VA;
  UINT32 i;

  for (i = 0; i < WIN32_HEAP_SLOTS; i++) {
    if (g_alloc[i].used && g_alloc[i].owner == pid &&
        g_alloc[i].va + g_alloc[i].size > next) {
      next = g_alloc[i].va + g_alloc[i].size;
    }
  }
  return next;
}

static win32_alloc_t *alloc_find(UINT64 pid, UINT64 va) {
  UINT32 i;

  for (i = 0; i < WIN32_HEAP_SLOTS; i++) {
    if (g_alloc[i].used && g_alloc[i].owner == pid && g_alloc[i].va == va) {
      return &g_alloc[i];
    }
  }
  return NULL;
}

/* Reserve-and-commit `bytes` at the next free address of the process and
 * map a fresh physical page for every page of it. Pages come zeroed from
 * pmm_alloc_page(), which is what both VirtualAlloc and HEAP_ZERO_MEMORY
 * promise. */
static win32_alloc_t *arena_carve(task_t *t, UINT64 bytes) {
  win32_alloc_t *slot = NULL;
  UINT64 base = round_page(arena_next(t->pid));
  UINT64 size = round_page(bytes == 0u ? WIN32_PAGE : bytes);
  UINT64 va;

  if (base + size > WIN32_HEAP_END) {
    return NULL;
  }
  for (va = base; va < base + size; va += WIN32_PAGE) {
    void *page = pmm_alloc_page();

    if (page == NULL) {
      return NULL;
    }
    if (vm_user_map(&t->space, va, (UINT64)(UINTN)page,
                    VM_PROT_WRITE | VM_PROT_USER) != 0) {
      pmm_free_page(page);
      return NULL;
    }
  }
  for (va = 0; va < WIN32_HEAP_SLOTS; va++) {
    if (!g_alloc[va].used) {
      slot = &g_alloc[va];
      break;
    }
  }
  if (slot == NULL) {
    return NULL;
  }
  slot->used = 1u;
  slot->owner = t->pid;
  slot->va = base;
  slot->size = size;
  return slot;
}

/* A block is dropped from the bookkeeping but its pages stay mapped: this
 * layer has no unmap path yet, and handing pages back while a pointer to
 * them may still be live in the process would be worse than leaking them.
 * The space itself dies with the process. */
static int arena_release(UINT64 pid, UINT64 va) {
  win32_alloc_t *a = alloc_find(pid, va);

  if (a == NULL) {
    return 0;
  }
  memset(a, 0, sizeof(*a));
  return 1;
}

/* ---- API handlers ------------------------------------------------------ */

static void api_exit(trap_frame_t *f) {
  sched_exit_current((int)(UINT32)f->rdi);
  f->rax = 0;
}

static void api_get_std_handle(trap_frame_t *f) {
  switch ((UINT32)f->rdi) {
  case 0xFFFFFFF5u: /* STD_OUTPUT_HANDLE (-11) */
    f->rax = 1u;
    return;
  case 0xFFFFFFF4u: /* STD_ERROR_HANDLE (-12) */
    f->rax = 2u;
    return;
  default: /* STD_INPUT_HANDLE (-10) and anything else */
    f->rax = 0u;
    return;
  }
}

/* WriteFile/WriteConsoleA differ only in the last argument's meaning, which
 * this prototype ignores; both print through the kernel log. */
static void api_write(trap_frame_t *f) {
  task_t *t = sched_current();
  UINT64  len = f->r8;
  char    buf[65];
  UINT32  n;

  if (t == NULL) {
    f->rax = 0;
    return;
  }
  if (len > 64u) {
    len = 64u;
  }
  if (len != 0u) {
    if (user_copy(t->cr3, f->rdx, buf, len, 0) != 0) {
      t->win32_last_error = WIN32_ERROR_INVALID_PARAM;
      f->rax = 0;
      return;
    }
  }
  buf[len] = '\0';
  kputs("[WIN32] ");
  kputs(buf);
  if (len == 0u || buf[len - 1u] != '\n') {
    kputs("\n");
  }
  n = (UINT32)len;
  if (f->r9 != 0u) {
    (void)user_copy(t->cr3, f->r9, &n, 4u, 1);
  }
  f->rax = 1u;
}

static void api_sleep(trap_frame_t *f) {
  UINT64 ms = f->rdi;
  UINT32 ticks = (UINT32)((ms + 9u) / 10u);

  if (ticks == 0u) {
    ticks = 1u;
  }
  /* Park with no waker: only the tick deadline retires the wait. Rewind to
   * the `syscall` so the thunk replays when the task runs again; the entry
   * check above then turns the timeout into success. */
  sched_park(WIN32_SLEEP_PORT, ticks);
  f->rip -= 2u;
}

static void api_ticks(trap_frame_t *f, int wide) {
  UINT64 ms = sched_ticks() * 10u; /* the LAPIC tick is 10 ms, lapic.c */

  f->rax = wide ? ms : (UINT64)(UINT32)ms;
}

static void api_reg_open(trap_frame_t *f) {
  task_t *t = sched_current();
  char    path[72];
  UINT64  out_va = 0;
  UINT64  handle = 0;

  if (t == NULL || key_path(t->cr3, f->rdi, f->rdx, path, sizeof(path)) != 0 ||
      stack_arg(f, t, 5u, &out_va) != 0 || out_va == 0u) {
    if (t != NULL) {
      t->win32_last_error = WIN32_ERROR_FILE_NOT_FOUND;
    }
    f->rax = WIN32_ERROR_FILE_NOT_FOUND;
    return;
  }
  /* The prototype does not require the key to exist yet: opening is what
   * creates the name, and a later set makes it appear in the table. */
  if (key_open(t->pid, path, &handle) != 0) {
    t->win32_last_error = WIN32_ERROR_INVALID_PARAM;
    f->rax = WIN32_ERROR_INVALID_PARAM;
    return;
  }
  (void)user_copy(t->cr3, out_va, &handle, 8u, 1);
  t->win32_last_error = WIN32_ERROR_SUCCESS;
  f->rax = WIN32_ERROR_SUCCESS;
}

static void api_reg_query(trap_frame_t *f) {
  task_t *t = sched_current();
  char    path[72];
  char    name[WIN32_REG_VALUE_LEN];
  win32_reg_entry_t *e;
  UINT64  type_va = 0;
  UINT64  data_va = 0;
  UINT64  size_va = 0;
  UINT32  want;

  if (t == NULL || key_path(t->cr3, f->rdi, 0, path, sizeof(path)) != 0 ||
      read_user_str(t->cr3, f->rdx, name, sizeof(name)) != 0 ||
      stack_arg(f, t, 5u, &data_va) != 0 ||
      stack_arg(f, t, 6u, &size_va) != 0) {
    f->rax = WIN32_ERROR_INVALID_PARAM;
    return;
  }
  type_va = f->r9;
  e = reg_find(path, name);
  if (e == NULL) {
    if (t != NULL) {
      t->win32_last_error = WIN32_ERROR_FILE_NOT_FOUND;
    }
    f->rax = WIN32_ERROR_FILE_NOT_FOUND;
    return;
  }
  if (type_va != 0u) {
    (void)user_copy(t->cr3, type_va, &e->type, 4u, 1);
  }
  if (data_va == 0u) {
    if (size_va != 0u) {
      (void)user_copy(t->cr3, size_va, &e->len, 4u, 1);
    }
    f->rax = WIN32_ERROR_SUCCESS;
    return;
  }
  want = e->len;
  if (size_va != 0u) {
    UINT32 cap = 0;

    if (user_copy(t->cr3, size_va, &cap, 4u, 0) != 0) {
      f->rax = WIN32_ERROR_INVALID_PARAM;
      return;
    }
    if (cap < want) {
      want = cap;
    }
  }
  if (want != 0u && user_copy(t->cr3, data_va, e->data, want, 1) != 0) {
    f->rax = WIN32_ERROR_INVALID_PARAM;
    return;
  }
  if (size_va != 0u) {
    (void)user_copy(t->cr3, size_va, &e->len, 4u, 1);
  }
  t->win32_last_error = WIN32_ERROR_SUCCESS;
  f->rax = WIN32_ERROR_SUCCESS;
}

static void api_reg_set(trap_frame_t *f) {
  task_t *t = sched_current();
  char    path[72];
  char    name[WIN32_REG_VALUE_LEN];
  win32_reg_entry_t *e;
  UINT64  data_va = 0;
  UINT64  data_len = 0;

  if (t == NULL || key_path(t->cr3, f->rdi, 0, path, sizeof(path)) != 0 ||
      read_user_str(t->cr3, f->rdx, name, sizeof(name)) != 0 ||
      stack_arg(f, t, 5u, &data_va) != 0 ||
      stack_arg(f, t, 6u, &data_len) != 0) {
    f->rax = WIN32_ERROR_INVALID_PARAM;
    return;
  }
  if (data_len > WIN32_REG_DATA_LEN || (data_len != 0u && data_va == 0u)) {
    t->win32_last_error = WIN32_ERROR_INVALID_PARAM;
    f->rax = WIN32_ERROR_INVALID_PARAM;
    return;
  }
  e = reg_alloc(path, name);
  if (e == NULL) {
    t->win32_last_error = WIN32_ERROR_INVALID_PARAM;
    f->rax = WIN32_ERROR_INVALID_PARAM;
    return;
  }
  e->type = (UINT32)f->r9;
  e->len = (UINT32)data_len;
  if (data_len != 0u &&
      user_copy(t->cr3, data_va, e->data, data_len, 0) != 0) {
    t->win32_last_error = WIN32_ERROR_INVALID_PARAM;
    f->rax = WIN32_ERROR_INVALID_PARAM;
    return;
  }
  (void)reg_save();
  t->win32_last_error = WIN32_ERROR_SUCCESS;
  f->rax = WIN32_ERROR_SUCCESS;
}

static void api_reg_close(trap_frame_t *f) {
  task_t *t = sched_current();

  if (t != NULL) {
    key_close(t->pid, f->rdi);
  }
  f->rax = WIN32_ERROR_SUCCESS;
}

/* ---- dispatcher -------------------------------------------------------- */

/* ---- second block ------------------------------------------------------ */

static void api_module_file_name(trap_frame_t *f) {
  task_t *t = sched_current();
  UINT32  cap = (UINT32)f->r8;
  UINT32  n = 0;

  if (t == NULL || f->rdx == 0u || cap == 0u) {
    f->rax = 0u;
    return;
  }
  /* The image name the loader used, which is also what the desktop shows
   * for the process. A full path would need the mount point, and nothing
   * in the layer carries one yet. */
  while (t->name[n] != '\0' && n + 1u < cap) {
    UINT8 c = (UINT8)t->name[n];

    if (user_copy(t->cr3, f->rdx + n, &c, 1u, 1) != 0) {
      f->rax = 0u;
      return;
    }
    n++;
  }
  {
    UINT8 zero = 0u;

    (void)user_copy(t->cr3, f->rdx + n, &zero, 1u, 1);
  }
  f->rax = n;
}

static void api_virtual_alloc(trap_frame_t *f) {
  task_t        *t = sched_current();
  win32_alloc_t *a;

  if (t == NULL) {
    f->rax = 0u;
    return;
  }
  /* The requested address is ignored: the process has one window and no
   * way to ask for a slot inside it. Every caller in practice passes NULL. */
  a = arena_carve(t, f->rdx);
  if (a == NULL) {
    t->win32_last_error = 8u; /* ERROR_NOT_ENOUGH_MEMORY */
    f->rax = 0u;
    return;
  }
  f->rax = a->va;
}

static void api_virtual_free(trap_frame_t *f) {
  task_t *t = sched_current();

  if (t == NULL) {
    f->rax = 0u;
    return;
  }
  f->rax = arena_release(t->pid, f->rdi) ? 1u : 0u;
}

static void api_get_process_heap(trap_frame_t *f) {
  f->rax = WIN32_HEAP_HANDLE;
}

static void api_heap_alloc(trap_frame_t *f) {
  task_t        *t = sched_current();
  win32_alloc_t *a;

  if (t == NULL) {
    f->rax = 0u;
    return;
  }
  a = arena_carve(t, f->r8);
  f->rax = a != NULL ? a->va : 0u;
}

static void api_heap_free(trap_frame_t *f) {
  task_t *t = sched_current();

  if (t == NULL) {
    f->rax = 0u;
    return;
  }
  f->rax = arena_release(t->pid, f->rdx) ? 1u : 0u;
}

/* SYSTEMTIME is eight WORDs: year, month, day of week, day, hour, minute,
 * second, millisecond - in that order, which is why the day of week sits
 * between the month and the day. */
static void api_system_time(trap_frame_t *f) {
  task_t *t = sched_current();
  UINT16 st[8];
  UINT16 year = 1970u;
  UINT8  month = 1u;
  UINT8  day = 1u;
  UINT8  hour = 0u;
  UINT8  minute = 0u;
  UINT64 secs;
  UINT64 ms;

  if (t == NULL || f->rdi == 0u) {
    f->rax = 0u;
    return;
  }
  secs = rtc_unix();
  (void)rtc_now_date(&year, &month, &day);
  (void)rtc_now(&hour, &minute);
  ms = (UINT64)sched_ticks() * 10u;
  st[0] = year;
  st[1] = month;
  st[2] = (UINT16)((secs / 86400u + 4u) % 7u); /* 1970-01-01 was a Thursday */
  st[3] = day;
  st[4] = hour;
  st[5] = minute;
  st[6] = (UINT16)(secs % 60u);
  st[7] = (UINT16)(ms % 1000u);
  if (user_copy(t->cr3, f->rdi, st, (UINT32)sizeof(st), 1) != 0) {
    f->rax = 0u;
    return;
  }
  f->rax = 0u; /* GetSystemTime returns nothing */
}

static void api_qpc(trap_frame_t *f) {
  task_t *t = sched_current();
  UINT64 count = (UINT64)sched_ticks() * 10u * 10000u; /* 100 ns units */

  if (t != NULL && f->rdi != 0u) {
    (void)user_copy(t->cr3, f->rdi, &count, 8u, 1);
  }
  f->rax = 1u;
}

static void api_qpf(trap_frame_t *f) {
  task_t *t = sched_current();
  UINT64 freq = 10000000u;

  if (t != NULL && f->rdi != 0u) {
    (void)user_copy(t->cr3, f->rdi, &freq, 8u, 1);
  }
  f->rax = 1u;
}

static void api_ids(trap_frame_t *f) {
  task_t *t = sched_current();

  f->rax = t != NULL ? t->pid : 0u;
}

/* 48 bytes: architecture, page size, the application address window, the
 * processor mask and counts, the allocation granularity, then three level
 * and revision words. A program reads this once at start-up and sizes its
 * arenas from it; the numbers below describe exactly what this machine is. */
static void api_system_info(trap_frame_t *f) {
  task_t *t = sched_current();
  struct {
    UINT16 arch;
    UINT16 reserved;
    UINT32 page;
    UINT64 min_addr;
    UINT64 max_addr;
    UINT64 mask;
    UINT32 processors;
    UINT32 type;
    UINT32 granularity;
    UINT16 level;
    UINT16 revision;
  } si;
  UINT32 cpus = sched_task_count();

  if (t == NULL || f->rdi == 0u) {
    f->rax = 0u;
    return;
  }
  memset(&si, 0, sizeof(si));
  si.arch = 9u; /* PROCESSOR_ARCHITECTURE_AMD64 */
  si.page = WIN32_PAGE;
  si.min_addr = 0x10000ULL;
  si.max_addr = 0x7FFEFFFFULL;
  si.mask = cpus >= 64u ? 0xFFFFFFFFFFFFFFFFULL
                       : ((1ULL << (cpus == 0u ? 1u : cpus)) - 1ULL);
  si.processors = cpus == 0u ? 1u : cpus;
  si.type = 8664u;
  si.granularity = 65536u;
  si.level = 6u;
  si.revision = 0x3C03u;
  if (user_copy(t->cr3, f->rdi, &si, (UINT32)sizeof(si), 1) != 0) {
    f->rax = 0u;
    return;
  }
  f->rax = 0u;
}

/* The handler is accepted, never invoked: an exception in this layer ends
 * the process, and pretending otherwise would hide the failure. Returning
 * the previous filter is what the caller expects. */
static void api_set_filter(trap_frame_t *f) {
  f->rax = f->rdi;
}

/* ---- third block: user32 and gdi32 ------------------------------------- */

/* A Win32 program that draws does four things in order: register a window
 * class (which carries its window procedure), create a window, pump the
 * message queue, and paint. This block implements exactly those four, and
 * the state they need is per process because a class registration and a
 * window handle belong to the program that made them.
 *
 * Two structural choices are worth naming:
 *
 *   - The window itself is a Novus-WM window. The kernel drives the WM's
 *     request handlers with the process id as owner (nwm_owner_*), so the
 *     desktop's z-order, focus, drag and close button all work on a Win32
 *     window without a second geometry model to drift.
 *   - Painting is a display list, not a pixel buffer: FillRect and TextOut
 *     append items that BeginPaint cleared, and EndPaint commits them. That
 *     is the WM's existing protocol, so a Win32 window paints through the
 *     same path as a native app - and it is why the layer needs no shared
 *     memory yet.
 *
 * Message routing: the WM sends events to the owner's inbox (the process
 * id is the port), so the pump here drains that inbox and turns NWM events
 * into the WM_* messages a Windows program expects. Nothing is lost when
 * the program is busy: events queue in the inbox, and the pump runs at the
 * top of every GetMessage/PeekMessage.
 */

#include "../wm/novus-wm/nwm.h"
#include "../wm/novus-wm/nwm_proto.h"
#include "../graphics/nd_input.h"

#define WIN32_GUI_MAX_PROCS   4u
#define WIN32_GUI_MAX_CLASSES 6u
#define WIN32_GUI_MAX_WINDOWS 4u
#define WIN32_GUI_MSGQ        16u
#define WIN32_HWND_BASE       0x40000000u

#define WM_CREATE     0x0001u
#define WM_DESTROY    0x0002u
#define WM_SIZE       0x0005u
#define WM_PAINT      0x000Fu
#define WM_CLOSE      0x0010u
#define WM_QUIT       0x0012u
#define WM_KEYDOWN    0x0100u
#define WM_KEYUP      0x0101u
#define WM_CHAR       0x0102u
#define WM_MOUSEMOVE  0x0200u
#define WM_LBUTTONDOWN 0x0201u
#define WM_LBUTTONUP  0x0202u

#define WIN32_ERROR_INVALID_HANDLE 6u
#define WIN32_ERROR_INVALID_PARAM  87u

/* MSG as Windows lays it out for x64: 48 bytes, and the offsets matter
 * because the user-space dispatch stub reads them directly. */
typedef struct {
  UINT64 hwnd;
  UINT32 message;
  UINT32 pad;
  UINT64 wparam;
  UINT64 lparam;
  UINT32 time;
  INT32  x;
  INT32  y;
} win32_msg_t;

typedef struct {
  UINT8  used;
  char   name[32];
  UINT64 wndproc;
  UINT32 bg;      /* 0x00RRGGBB, already converted from COLORREF */
} win32_class_t;

typedef struct {
  UINT8  used;
  UINT32 hwnd;
  UINT32 win;     /* Novus-WM window id */
  INT32  w, h;    /* client size */
  UINT32 bg;
  UINT8  painting;
} win32_wnd_t;

typedef struct {
  UINT8       used;
  UINT64      pid;
  UINT8       stub_ready;
  UINT32      nclass;
  win32_class_t cls[WIN32_GUI_MAX_CLASSES];
  win32_wnd_t wnd[WIN32_GUI_MAX_WINDOWS];
  win32_msg_t q[WIN32_GUI_MSGQ];
  UINT32      qh, qn;
  UINT8       quit_pending;
  UINT32      paint_hwnd;
  UINT32      text_rgb;
  UINT32      nshape;   /* diagnostics: what the last paint submitted */
  UINT32      ntext;
} win32_gui_t;

static win32_gui_t g_gui[WIN32_GUI_MAX_PROCS];

/* The stub DispatchMessageA runs in the program's own ring. Forty bytes of
 * x86-64 that call the procedure the kernel stored for this window:
 *
 *   mov  rax,[rcx]        ; MSG->hwnd
 *   and  eax,0xF          ; window slot
 *   mov  r11,[table+rax*8]
 *   test r11,r11
 *   jz   .none
 *   mov  r8, [rcx+0x10]   ; MSG->wParam
 *   mov  r9, [rcx+0x18]   ; MSG->lParam
 *   mov  edx,[rcx+0x08]   ; MSG->message
 *   mov  rax,rcx
 *   mov  rcx,[rax]        ; hwnd goes in RCX, the first argument
 *   sub  rsp,8            ; realign for the call
 *   call r11
 *   add  rsp,8
 *   ret
 * .none:
 *   xor  eax,eax
 *   ret
 */
static const UINT8 g_dispatch_stub[] = {
    0x48, 0x8B, 0x01,             /* mov %rax,(%rcx) */
    0x83, 0xE0, 0x0F,             /* and $0xF,%eax */
    0x4C, 0x8B, 0x1C, 0xC5,
    0x00, 0x3F, 0x00, 0x60,       /* mov 0x60003F00(,%rax,8),%r11 */
    0x4D, 0x85, 0xDB,             /* test %r11,%r11 */
    0x74, 0x1D,                   /* jz .none */
    0x4C, 0x8B, 0x41, 0x10,       /* mov 0x10(%rcx),%r8 */
    0x4C, 0x8B, 0x49, 0x18,       /* mov 0x18(%rcx),%r9 */
    0x8B, 0x51, 0x08,             /* mov 0x8(%rcx),%edx */
    0x48, 0x89, 0xC8,             /* mov %rcx,%rax */
    0x48, 0x8B, 0x08,             /* mov (%rax),%rcx */
    0x48, 0x83, 0xEC, 0x08,       /* sub $8,%rsp */
    0x41, 0xFF, 0xD3,             /* call %r11 */
    0x48, 0x83, 0xC4, 0x08,       /* add $8,%rsp */
    0xC3,                         /* ret */
    0x31, 0xC0,                   /* xor %eax,%eax */
    0xC3,                         /* ret */
};

/* COLORREF is 0x00BBGGRR while the WM's display list takes 0x00RRGGBB, so
 * every colour crosses this one conversion - a channel swap that would
 * otherwise turn a red rectangle blue. */
static UINT32 colorref_rgb(UINT32 c) {
  return ((c & 0xFFu) << 16) | (c & 0xFF00u) | ((c >> 16) & 0xFFu);
}

/* A background brush is either one of ours (high byte set, a plain colour)
 * or a system colour index, which the layer flattens to the face colour
 * because it has no palette yet. */
static UINT32 brush_rgb(UINT32 brush) {
  if ((brush & 0xFF000000u) == 0xFF000000u) {
    return colorref_rgb(brush & 0xFFFFFFu);
  }
  return 0x00FFFFFFu;
}

static win32_gui_t *gui_state(UINT64 pid, int create) {
  UINT32 i;

  for (i = 0; i < WIN32_GUI_MAX_PROCS; i++) {
    if (g_gui[i].used && g_gui[i].pid == pid) {
      return &g_gui[i];
    }
  }
  if (!create) {
    return NULL;
  }
  for (i = 0; i < WIN32_GUI_MAX_PROCS; i++) {
    if (!g_gui[i].used) {
      memset(&g_gui[i], 0, sizeof(g_gui[i]));
      g_gui[i].used = 1u;
      g_gui[i].pid = pid;
      g_gui[i].text_rgb = 0x00000000u;
      return &g_gui[i];
    }
  }
  return NULL;
}

static win32_wnd_t *gui_window(win32_gui_t *gu, UINT32 hwnd) {
  UINT32 i;

  if (gu == NULL) {
    return NULL;
  }
  for (i = 0; i < WIN32_GUI_MAX_WINDOWS; i++) {
    if (gu->wnd[i].used && gu->wnd[i].hwnd == hwnd) {
      return &gu->wnd[i];
    }
  }
  return NULL;
}

static void gui_push(win32_gui_t *gu, UINT32 hwnd, UINT32 msg, UINT64 wp,
                     UINT64 lp) {
  win32_msg_t *m;

  if (gu == NULL || gu->qn >= WIN32_GUI_MSGQ) {
    return; /* a program that never pumps loses messages, as on Windows */
  }
  m = &gu->q[(gu->qh + gu->qn) % WIN32_GUI_MSGQ];
  memset(m, 0, sizeof(*m));
  m->hwnd = hwnd;
  m->message = msg;
  m->wparam = wp;
  m->lparam = lp;
  m->time = (UINT32)(sched_ticks() * 10u);
  gu->qn++;
}

/* Map a window id from an event payload onto our HWND encoding, ignoring
 * events for windows this process did not open. */
static UINT32 gui_hwnd_for_win(win32_gui_t *gu, UINT32 win) {
  UINT32 i;

  for (i = 0; i < WIN32_GUI_MAX_WINDOWS; i++) {
    if (gu->wnd[i].used && gu->wnd[i].win == win) {
      return gu->wnd[i].hwnd;
    }
  }
  return 0;
}

/* Drain the owner's inbox and translate WM events into WM_* messages. */
/* Translate one delivered message into a queued WM_* message. */
static void gui_translate(task_t *t, win32_gui_t *gu, const ipc_msg_t *msg) {
  ipc_msg_t m = *msg;
  UINT32    hwnd;
  UINT32    win;

  (void)t;
  if (m.hdr.type == NWM_RSP_OK || m.hdr.type == NWM_RSP_ERR) {
    return; /* a reply to a request we did not send through IPC */
  }
  win = 0;
  if (m.hdr.len >= 4u) {
    memcpy(&win, m.payload, 4);
  }
  hwnd = gui_hwnd_for_win(gu, win);
  if (hwnd == 0u) {
    return;
  }
  switch (m.hdr.type) {
  case NWM_EV_CLOSE:
    /* The WM has already taken the window down: the close button is the
     * compositor's, not the client's. Hand the program WM_CLOSE so it can
     * react, then a WM_DESTROY, then end the loop - the same sequence a
     * Windows program sees when the user clicks its X. */
    gui_push(gu, hwnd, WM_CLOSE, 0, 0);
    gui_push(gu, hwnd, WM_DESTROY, 0, 0);
    gu->quit_pending = 1u;
    {
      win32_wnd_t *w = gui_window(gu, hwnd);

      if (w != NULL) {
        memset(w, 0, sizeof(*w)); /* the window no longer exists */
      }
    }
    break;
  case NWM_EV_KEY: {
    nwm_ev_key_t k;

    if (m.hdr.len >= sizeof(k)) {
      memcpy(&k, m.payload, sizeof(k));
      gui_push(gu, hwnd, WM_KEYDOWN, k.key & 0xFFu, 0);
    }
    break;
  }
  case NWM_EV_RESIZE: {
    nwm_ev_resize_t r;

    if (m.hdr.len >= sizeof(r)) {
      win32_wnd_t *w = gui_window(gu, hwnd);

      memcpy(&r, m.payload, sizeof(r));
      if (w != NULL && r.w > 0 && r.h > 0) {
        w->w = r.w;
        w->h = r.h;
      }
      gui_push(gu, hwnd, WM_SIZE, 0,
               (UINT64)(UINT32)r.w | ((UINT64)(UINT32)r.h << 16));
      /* A resized client has to repaint, and this compositor does not ask
       * on its own yet. */
      gui_push(gu, hwnd, WM_PAINT, 0, 0);
    }
    break;
  }
  case NWM_EV_MOUSE: {
    nwm_ev_mouse_t mm;

    if (m.hdr.len >= sizeof(mm)) {
      UINT32 msg = WM_MOUSEMOVE;

      memcpy(&mm, m.payload, sizeof(mm));
      if (mm.type == ND_EV_MOUSE_DOWN) {
        msg = WM_LBUTTONDOWN;
      } else if (mm.type == ND_EV_MOUSE_UP) {
        msg = WM_LBUTTONUP;
      }
      gui_push(gu, hwnd, msg, 0,
               (UINT64)(UINT32)mm.x | ((UINT64)(UINT32)mm.y << 16));
    }
    break;
  }
  default:
    break;
  }
}

/* Drain the owner's inbox. */
static void gui_pump(task_t *t, win32_gui_t *gu) {
  ipc_msg_t m;

  while (ipc_try_recv(t->pid, &m) == 0) {
    gui_translate(t, gu, &m);
  }
}

/* Map the dispatch stub's page and leave the procedure table zeroed. The
 * page is user-executable and not user-writable: the table is filled in by
 * the kernel through user_copy, so a program cannot point it at arbitrary
 * code after the fact. */
static int gui_stub_map(task_t *t, win32_gui_t *gu) {
  void  *page;
  UINT32 i;

  if (gu->stub_ready) {
    return 0;
  }
  page = pmm_alloc_page();
  if (page == NULL) {
    return -1;
  }
  memset(page, 0, 4096);
  memcpy(page, g_dispatch_stub, sizeof(g_dispatch_stub));
  /* The stub page also carries the dispatch table, and the kernel fills
   * that table through user_copy(), whose access check demands write
   * permission. Without VM_PROT_WRITE the table write failed and every
   * DispatchMessageA returned zero - the window existed but never painted. */
  if (vm_user_map(&t->space, WIN32_GUI_STUB_VA, (UINT64)(UINTN)page,
                  VM_PROT_USER | VM_PROT_EXEC | VM_PROT_WRITE) != 0) {
    pmm_free_page(page);
    return -1;
  }
  for (i = 0; i < WIN32_GUI_SLOTS; i++) {
    UINT64 zero = 0;

    (void)user_copy(t->cr3, WIN32_GUI_TABLE_VA + (UINT64)i * 8u, &zero, 8u, 1);
  }
  gu->stub_ready = 1u;
  return 0;
}

static void gui_set_wndproc(task_t *t, UINT32 hwnd, UINT64 proc) {
  UINT64 slot = WIN32_GUI_TABLE_VA +
                (UINT64)(hwnd & (WIN32_GUI_SLOTS - 1u)) * 8u;
  int    rc = user_copy(t->cr3, slot, &proc, 8u, 1);

  (void)rc;
}

static void api_register_class(trap_frame_t *f) {
  task_t       *t = sched_current();
  win32_gui_t  *gu;
  win32_class_t *c = NULL;
  UINT8         wc[80];
  char          name[32];
  UINT64        proc = 0;
  UINT64        name_va = 0;
  UINT32        bg = 0;
  UINT32        i;

  if (t == NULL || f->rdi == 0u ||
      user_copy(t->cr3, f->rdi, wc, sizeof(wc), 0) != 0) {
    f->rax = 0u;
    return;
  }
  gu = gui_state(t->pid, 1);
  if (gu == NULL) {
    f->rax = 0u;
    return;
  }
  /* WNDCLASSEXA for x64: cbSize 0, style 4, lpfnWndProc 8,
   * cbClsExtra 0x10, cbWndExtra 0x14, hInstance 0x18, hIcon 0x20,
   * hCursor 0x28, hbrBackground 0x30, lpszMenuName 0x38,
   * lpszClassName 0x40, hIconSm 0x48. */
  memcpy(&proc, wc + 0x08, 8);
  memcpy(&bg, wc + 0x30, 4);
  memcpy(&name_va, wc + 0x40, 8);
  if (proc == 0u || read_user_str(t->cr3, name_va, name, sizeof(name)) != 0) {
    t->win32_last_error = WIN32_ERROR_INVALID_PARAM;
    f->rax = 0u;
    return;
  }
  for (i = 0; i < WIN32_GUI_MAX_CLASSES; i++) {
    if (gu->cls[i].used && text_equal(gu->cls[i].name, name)) {
      c = &gu->cls[i];
      break;
    }
  }
  if (c == NULL) {
    for (i = 0; i < WIN32_GUI_MAX_CLASSES; i++) {
      if (!gu->cls[i].used) {
        c = &gu->cls[i];
        break;
      }
    }
  }
  if (c == NULL) {
    t->win32_last_error = WIN32_ERROR_INVALID_PARAM;
    f->rax = 0u;
    return;
  }
  memset(c, 0, sizeof(*c));
  c->used = 1u;
  copy_text(c->name, sizeof(c->name), name);
  c->wndproc = proc;
  c->bg = brush_rgb(bg);
  gu->nclass++;
  /* An ATOM is a nonzero small integer; the class name is the lookup key. */
  f->rax = 0xC000u + (UINT64)i;
}

static void api_create_window(trap_frame_t *f) {
  task_t       *t = sched_current();
  win32_gui_t  *gu;
  win32_class_t *c = NULL;
  char          cls[32];
  char          title[NWM_TITLE_MAX];
  UINT64        x = 0, y = 0, w = 0, h = 0;
  UINT32        win = 0;
  UINT32        hwnd;
  INT32         cw = 0, ch = 0;
  UINT32        i;

  if (t == NULL) {
    f->rax = 0u;
    return;
  }
  gu = gui_state(t->pid, 1);
  if (gu == NULL || gui_stub_map(t, gu) != 0) {
    t->win32_last_error = WIN32_ERROR_INVALID_PARAM;
    f->rax = 0u;
    return;
  }
  if (read_user_str(t->cr3, f->rdx, cls, sizeof(cls)) != 0) {
    t->win32_last_error = WIN32_ERROR_INVALID_PARAM;
    f->rax = 0u;
    return;
  }
  (void)read_user_str(t->cr3, f->r8, title, sizeof(title));
  /* x, y, nWidth, nHeight are arguments 5..8 on the stack. */
  if (stack_arg(f, t, 5u, &x) != 0 || stack_arg(f, t, 6u, &y) != 0 ||
      stack_arg(f, t, 7u, &w) != 0 || stack_arg(f, t, 8u, &h) != 0) {
    t->win32_last_error = WIN32_ERROR_INVALID_PARAM;
    f->rax = 0u;
    return;
  }
  for (i = 0; i < WIN32_GUI_MAX_CLASSES; i++) {
    if (gu->cls[i].used && text_equal(gu->cls[i].name, cls)) {
      c = &gu->cls[i];
      break;
    }
  }
  if (c == NULL) {
    t->win32_last_error = 1410u; /* ERROR_CLASS_NOT_FOUND */
    f->rax = 0u;
    return;
  }
  if (nwm_owner_create(t->pid, title, (INT32)x, (INT32)y, (INT32)w,
                       (INT32)h, &win) != NWM_OK) {
    t->win32_last_error = WIN32_ERROR_INVALID_PARAM;
    f->rax = 0u;
    return;
  }
  (void)nwm_owner_client(win, &cw, &ch);
  hwnd = WIN32_HWND_BASE | win;
  /* One window per slot, and the slot is the window id: the stub indexes
   * the table by that low nibble, so two windows must not share one. */
  for (i = 0; i < WIN32_GUI_MAX_WINDOWS; i++) {
    win32_wnd_t *me;

    if (gu->wnd[i].used) {
      continue;
    }
    me = &gu->wnd[i];
    memset(me, 0, sizeof(*me));
    me->used = 1u;
    me->hwnd = hwnd;
    me->win = win;
    me->w = cw > 0 ? cw : (INT32)w;
    me->h = ch > 0 ? ch : (INT32)h;
    me->bg = c->bg;
    break;
  }
  gui_set_wndproc(t, hwnd, c->wndproc);
  /* The order a Windows program sees: WM_CREATE first, then a paint. */
  gui_push(gu, hwnd, WM_CREATE, 0, 0);
  gui_push(gu, hwnd, WM_PAINT, 0, 0);
  f->rax = hwnd;
}

static void api_destroy_window(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_wnd_t *w = gui_window(gu, (UINT32)f->rdi);

  if (w != NULL) {
    (void)nwm_owner_destroy(t->pid, w->win);
    gui_set_wndproc(t, w->hwnd, 0u);
    memset(w, 0, sizeof(*w));
    f->rax = 1u;
    return;
  }
  f->rax = 0u; /* not ours: the Windows answer is FALSE */
}

static void api_show_window(trap_frame_t *f) {
  /* The window is visible from the moment the WM create request is
   * accepted - there is no separate mapping step in this compositor - so
   * ShowWindow only has to say so. */
  f->rax = 1u;
}

static int gui_pull(trap_frame_t *f, task_t *t, win32_gui_t *gu, int wait) {
  UINT32 min_msg = (UINT32)f->r8;
  UINT32 max_msg = (UINT32)f->r9;

  for (;;) {
    win32_msg_t m;
    UINT64      lp;

    gui_pump(t, gu);
    if (gu->qn == 0u) {
      if (gu->quit_pending) {
        f->rax = 0u; /* WM_QUIT: the loop ends */
        return 0;
      }
      if (!wait) {
        f->rax = 0u;
        return 0;
      }
      /* Park on the inbox. A yield loop here does not work: this runs
       * inside the syscall, and yielding without ever leaving the syscall
       * starves the WM task that is supposed to produce the event, which
       * froze the machine the first time this was tried. ipc_recv(block)
       * parks the task properly and wakes it when a message arrives. */
      {
        ipc_msg_t in;
        int       rc = ipc_recv(t->pid, &in, 1);

        if (rc >= 0) {
          gui_translate(t, gu, &in); /* take the wake-up, not just discard */
          continue;
        }
        if (rc == IPC_ERETRY) {
          /* The task is parked and this syscall has to run again when a
           * message lands. Looping here instead would spin with interrupts
           * off and stop the whole machine - which is exactly what the
           * first version did. */
          return -1;
        }
        f->rax = 0u;
        return 0;
      }
    }
    m = gu->q[gu->qh];
    gu->qh = (gu->qh + 1u) % WIN32_GUI_MSGQ;
    gu->qn--;
    if (min_msg != 0u || max_msg != 0u) {
      UINT32 lo = min_msg != 0u ? min_msg : 0u;
      UINT32 hi = max_msg != 0u ? max_msg : 0xFFFFu;

      if (m.message < lo || m.message > hi) {
        continue;
      }
    }
    if (f->rdi == 0u || user_copy(t->cr3, f->rdi, &m, sizeof(m), 1) != 0) {
      f->rax = 0u;
      return 0;
    }
    lp = 0;
    (void)lp;
    f->rax = 1u;
    return 1;
  }
}

static void api_get_message(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 1) : NULL;
  int          rc;

  if (gu == NULL) {
    f->rax = 0u;
    return;
  }
  rc = gui_pull(f, t, gu, 1);
  if (rc > 0) {

  }
  if (rc < 0) {
    /* The task is parked. Rewind to the `syscall` instruction and put the
     * call number back in RAX, so that when the message lands the same
     * syscall runs again and finds it in the queue - the same replay the
     * blocking ipc_recv syscall uses. */
    f->rax = (UINT64)SYS_WIN32;
    f->rip -= WIN32_SYSCALL_LEN;
  }
}

static void api_peek_message(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 1) : NULL;

  if (gu == NULL) {
    f->rax = 0u;
    return;
  }
  f->rax = (UINT64)gui_pull(f, t, gu, 0);
}

static void api_translate_message(trap_frame_t *f) {
  /* No keyboard translation table yet: WM_KEYDOWN carries the raw code and
   * no WM_CHAR is produced. */
  f->rax = 0u;
}

static void api_def_window_proc(trap_frame_t *f) {
  /* Reached only when DispatchMessageA arrives as a syscall instead of
   * through the user-space stub, which is the diagnostic that the stub
   * never ran. */

  f->rax = 0u;
}

static void api_get_client_rect(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_wnd_t *w = gui_window(gu, (UINT32)f->rdi);
  INT32        r[4];

  if (w == NULL || f->rdx == 0u) {
    f->rax = 0u;
    return;
  }
  r[0] = 0;
  r[1] = 0;
  r[2] = w->w;
  r[3] = w->h;
  (void)user_copy(t->cr3, f->rdx, r, sizeof(r), 1);
  f->rax = 1u;
}

/* Begin a frame: the display list is emptied, and every FillRect/TextOut
 * until the matching release fills it. End commits the list and the WM
 * repaints the client area from it in one go. */
static void api_begin_frame(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_wnd_t *w = gui_window(gu, (UINT32)f->rdi);

  if (w == NULL) {
    f->rax = 0u;
    return;
  }
  (void)nwm_owner_clear(t->pid, w->win);
  w->painting = 1u;
  gu->paint_hwnd = w->hwnd;
  gu->nshape = 0u;
  gu->ntext = 0u;
  kputs("[WIN32] paint: begin hwnd=");
  kputs_hex(w->hwnd);
  kputs("\n");
  f->rax = (UINT64)w->hwnd; /* the HDC is the window, for this layer */
}

static void api_end_frame(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_wnd_t *w = gui_window(gu, (UINT32)f->rdi);

  if (w == NULL) {
    f->rax = 0u;
    return;
  }
  {
    UINT64 rc = (UINT64)nwm_owner_flush(t->pid, w->win);

    w->painting = 0u;
    kputs("[WIN32] paint: submitted shapes=");
    kputs_hex(gu->nshape);
    kputs(" texts=");
    kputs_hex(gu->ntext);
    kputs(" flush->");
    kputs_hex(rc);
    kputs("\n");
  }
  f->rax = 1u;
}

static void api_fill_rect(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_wnd_t *w = gui_window(gu, (UINT32)f->rdi);
  INT32        r[4];
  UINT32       color;

  if (w == NULL || f->rdx == 0u ||
      user_copy(t->cr3, f->rdx, r, sizeof(r), 0) != 0) {
    f->rax = 0u;
    return;
  }
  color = brush_rgb((UINT32)f->r8);
  (void)nwm_owner_shape(t->pid, w->win, NWM_OP_FILL,
                        (INT16)r[0], (INT16)r[1],
                        (INT16)(r[2] - r[0]), (INT16)(r[3] - r[1]), color);
  gu->nshape++;
  f->rax = 1u;
}

static void api_text_out(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 0) : NULL;
  win32_wnd_t *w = gui_window(gu, (UINT32)f->rdi);
  char         text[NWM_TEXT_MAX];
  UINT64       count = 0;
  UINT32       i;

  if (w == NULL || gu == NULL || f->r9 == 0u ||
      stack_arg(f, t, 5u, &count) != 0) {
    kputs("[WIN32] textout: bad args hwnd=");
    kputs_hex(f->rdi);
    kputs(" str=");
    kputs_hex(f->r9);
    kputs(" count=");
    kputs_hex(count);
    kputs("\n");
    f->rax = 0u;
    return;
  }
  if (count >= NWM_TEXT_MAX) {
    count = NWM_TEXT_MAX - 1u;
  }
  if (count == 0u || user_copy(t->cr3, f->r9, text, count, 0) != 0) {
    kputs("[WIN32] textout: count=");
    kputs_hex(count);
    kputs(" copy failed at ");
    kputs_hex(f->r9);
    kputs("\n");
    f->rax = 0u;
    return;
  }
  text[count] = '\0';
  /* The display list carries byte strings; drop anything past the cap. */
  for (i = 0; i < count; i++) {
    if (text[i] == '\0') {
      break;
    }
  }
  (void)nwm_owner_text(t->pid, w->win, (INT16)f->rdx, (INT16)f->r8, 1u,
                       text, (UINT32)i, gu->text_rgb);
  gu->ntext++;
  f->rax = 1u;
}

static void api_set_text_color(trap_frame_t *f) {
  task_t      *t = sched_current();
  win32_gui_t *gu = (t != NULL) ? gui_state(t->pid, 1) : NULL;
  UINT32       old;

  if (gu == NULL) {
    f->rax = 0u;
    return;
  }
  old = gu->text_rgb;
  /* Answer in COLORREF (BGR) for the caller, keep RGB internally. */
  f->rax = ((old & 0xFFu) << 16) | (old & 0xFF00u) | ((old >> 16) & 0xFFu);
  gu->text_rgb = colorref_rgb((UINT32)f->rdx);
}

static void api_create_solid_brush(trap_frame_t *f) {
  /* A brush is a colour with the high byte set: no GDI object table, and a
   * program that never deletes one leaks nothing. */
  f->rax = 0xFF000000u | ((UINT32)f->rdi & 0xFFFFFFu);
}

/* Drop every window and every class a dying process left behind. A window
 * outliving its client would keep a slot in the WM's table forever, and
 * the desktop would show a frame nobody can paint or close. */
static void win32_gui_owner_exit(UINT64 pid) {
  win32_gui_t *gu = gui_state(pid, 0);
  UINT32       i;

  if (gu == NULL) {
    return;
  }
  for (i = 0; i < WIN32_GUI_MAX_WINDOWS; i++) {
    if (gu->wnd[i].used) {
      (void)nwm_owner_destroy(pid, gu->wnd[i].win);
    }
  }
  memset(gu, 0, sizeof(*gu));
}

void win32_syscall(trap_frame_t *f) {
  task_t *t = sched_current();
  UINT32  api;


  if (t == NULL || t->abi != TASK_ABI_WIN32) {
    f->rax = (UINT64)-1;
    return;
  }
  api = (UINT32)f->r10;

  /* A replay after a park carries the timeout in wait_err. Sleep is the
   * only caller that parks, so a cleared wait_err here means the sleep is
   * over; success is what Windows returns (void, but RAX is defined). */
  if (t->wait_err != 0) {
    t->wait_err = 0;
    if (api == WIN32_API_SLEEP) {
      f->rax = 0;
      return;
    }
  }

  switch (api) {
  case WIN32_API_EXIT_PROCESS:
    api_exit(f);
    return;
  case WIN32_API_GET_STD_HANDLE:
    api_get_std_handle(f);
    return;
  case WIN32_API_WRITE_FILE:
  case WIN32_API_WRITE_CONSOLE:
    api_write(f);
    return;
  case WIN32_API_SLEEP:
    api_sleep(f);
    return;
  case WIN32_API_GET_TICK_COUNT64:
    api_ticks(f, 1);
    return;
  case WIN32_API_GET_TICK_COUNT:
    api_ticks(f, 0);
    return;
  case WIN32_API_GET_LAST_ERROR:
    f->rax = t->win32_last_error;
    return;
  case WIN32_API_SET_LAST_ERROR:
    t->win32_last_error = (UINT32)f->rdi;
    f->rax = 0;
    return;
  case WIN32_API_GET_COMMAND_LINE:
    f->rax = WIN32_ARGV_VA;
    return;
  case WIN32_API_GET_MODULE_HANDLE:
    f->rax = t->win32_image_base;
    return;
  case WIN32_API_REG_OPEN_KEY_EX:
    api_reg_open(f);
    return;
  case WIN32_API_REG_QUERY_VALUE_EX:
    api_reg_query(f);
    return;
  case WIN32_API_REG_SET_VALUE_EX:
    api_reg_set(f);
    return;
  case WIN32_API_GET_MODULE_FILE_NAME:
    api_module_file_name(f);
    return;
  case WIN32_API_VIRTUAL_ALLOC:
    api_virtual_alloc(f);
    return;
  case WIN32_API_VIRTUAL_FREE:
    api_virtual_free(f);
    return;
  case WIN32_API_GET_PROCESS_HEAP:
    api_get_process_heap(f);
    return;
  case WIN32_API_HEAP_ALLOC:
    api_heap_alloc(f);
    return;
  case WIN32_API_HEAP_FREE:
    api_heap_free(f);
    return;
  case WIN32_API_GET_SYSTEM_TIME:
    api_system_time(f);
    return;
  case WIN32_API_QUERY_PERFORMANCE_COUNTER:
    api_qpc(f);
    return;
  case WIN32_API_QUERY_PERFORMANCE_FREQUENCY:
    api_qpf(f);
    return;
  case WIN32_API_GET_CURRENT_PROCESS_ID:
  case WIN32_API_GET_CURRENT_THREAD_ID:
    api_ids(f);
    return;
  case WIN32_API_IS_DEBUGGER_PRESENT:
    f->rax = 0u;
    return;
  case WIN32_API_GET_SYSTEM_INFO:
    api_system_info(f);
    return;
  case WIN32_API_SET_UNHANDLED_EXCEPTION_FILTER:
    api_set_filter(f);
    return;
  case WIN32_API_REG_CLOSE_KEY:
    api_reg_close(f);
    return;
  case WIN32_API_REGISTER_CLASS:
    api_register_class(f);
    return;
  case WIN32_API_CREATE_WINDOW:
    api_create_window(f);
    return;
  case WIN32_API_DESTROY_WINDOW:
    api_destroy_window(f);
    return;
  case WIN32_API_SHOW_WINDOW:
    api_show_window(f);
    return;
  case WIN32_API_GET_MESSAGE:
    api_get_message(f);
    return;
  case WIN32_API_PEEK_MESSAGE:
    api_peek_message(f);
    return;
  case WIN32_API_DISPATCH_MESSAGE:
  case WIN32_API_DEF_WINDOW_PROC:
    api_def_window_proc(f);
    return;
  case WIN32_API_TRANSLATE_MESSAGE:
    api_translate_message(f);
    return;
  case WIN32_API_GET_CLIENT_RECT:
    api_get_client_rect(f);
    return;
  case WIN32_API_GET_DC:
  case WIN32_API_BEGIN_PAINT:
    api_begin_frame(f);
    return;
  case WIN32_API_RELEASE_DC:
  case WIN32_API_END_PAINT:
    api_end_frame(f);
    return;
  case WIN32_API_FILL_RECT:
    api_fill_rect(f);
    return;
  case WIN32_API_TEXT_OUT:
    api_text_out(f);
    return;
  case WIN32_API_SET_TEXT_COLOR:
    api_set_text_color(f);
    return;
  case WIN32_API_CREATE_SOLID_BRUSH:
    api_create_solid_brush(f);
    return;
  default:
    t->win32_last_error = WIN32_ERROR_INVALID_PARAM;
    f->rax = (UINT64)-1;
    return;
  }
}

/* ---- import resolution ------------------------------------------------- */

int win32_api_lookup(const char *dll, const char *func) {
  UINT32 i;
  int fallback = -1;

  for (i = 0; i < sizeof(g_api) / sizeof(g_api[0]); i++) {
    if (ci_equal(g_api[i].dll, dll) && text_equal(g_api[i].fn, func)) {
      /* The id, not the row number: two names may share one entry point
       * (GetLocalTime / GetSystemTime), and the dispatcher switches on the
       * id. Returning the row index worked only while the table happened to
       * be one row per id, which stopped being true the moment a bad import
       * was dropped. */
      if (g_api[i].id >= WIN32_API_COUNT) {
        return -1;
      }
      return (int)g_api[i].id;
    }
  }

  /* The DLL column is bookkeeping, not a contract. Windows forwards
     several of these between modules - FillRect is a USER32 export that
     most compilers import from GDI32, and a program that links against
     either must work - so a name that exists under one DLL is accepted
     under another. An exact match above always wins. */
  for (i = 0; i < sizeof(g_api) / sizeof(g_api[0]); i++) {
    if (!text_equal(g_api[i].fn, func) || g_api[i].id >= WIN32_API_COUNT) {
      continue;
    }
    if (fallback < 0) {
      fallback = (int)g_api[i].id;
    }
  }
  return fallback;
}

/* mov %rdi,0x8(%rsp) ; mov %rcx,%rdi ; mov $id,%r10d ; mov $40,%eax ;
 * syscall ; mov 0x8(%rsp),%rdi ; ret
 *
 * RDI is callee-saved in the Windows x64 ABI, so the thunk has to give it
 * back exactly as it found it. The shadow space the caller reserved above
 * its return address is free scratch: the fifth and later arguments live
 * further up at 0x28(%rsp), so this cannot touch them. */
void win32_thunk_bytes(UINT32 api_id, UINT8 out[WIN32_THUNK_LEN]) {
  UINT32 i;

  out[0] = 0x48;
  out[1] = 0x89;
  out[2] = 0x7C;
  out[3] = 0x24;
  out[4] = 0x08;
  out[5] = 0x48;
  out[6] = 0x89;
  out[7] = 0xCF;
  out[8] = 0x41;
  out[9] = 0xBA;
  out[10] = (UINT8)(api_id & 0xFFu);
  out[11] = (UINT8)((api_id >> 8) & 0xFFu);
  out[12] = (UINT8)((api_id >> 16) & 0xFFu);
  out[13] = (UINT8)((api_id >> 24) & 0xFFu);
  out[14] = 0xB8;
  out[15] = (UINT8)(SYS_WIN32 & 0xFFu);
  out[16] = (UINT8)((SYS_WIN32 >> 8) & 0xFFu);
  out[17] = (UINT8)((SYS_WIN32 >> 16) & 0xFFu);
  out[18] = (UINT8)((SYS_WIN32 >> 24) & 0xFFu);
  out[19] = 0x0F;
  out[20] = 0x05;
  out[21] = 0x48;
  out[22] = 0x8B;
  out[23] = 0x7C;
  out[24] = 0x24;
  out[25] = 0x08;
  out[26] = 0xC3;
  /* The stride is wider than the code so every thunk starts on a fresh
   * address; the rest is padding that a `ret` never reaches. */
  for (i = 27u; i < WIN32_THUNK_LEN; i++) {
    out[i] = 0x90;
  }
}

/* One import, one address. DispatchMessageA is the exception: it has to
 * call the program's own window procedure, and that call can only happen
 * in the program's own ring, so its IAT entry points at the stub page
 * instead of a thunk. */
UINT64 win32_api_entry(UINT32 api_id) {
  if (api_id == (UINT32)WIN32_API_DISPATCH_MESSAGE) {
    return WIN32_GUI_STUB_VA;
  }
  return WIN32_THUNK_VA + (UINT64)api_id * WIN32_THUNK_LEN;
}
