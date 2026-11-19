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
static int stack_arg(trap_frame_t *f, task_t *t, UINT32 n, UINT64 *out) {
  UINT64 va;

  if (t == NULL || n < 4u) {
    return -1;
  }
  va = f->rsp + 0x28u + (UINT64)(n - 4u) * 8u;
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
      stack_arg(f, t, 4u, &out_va) != 0 || out_va == 0u) {
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
      stack_arg(f, t, 4u, &data_va) != 0 ||
      stack_arg(f, t, 5u, &size_va) != 0) {
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
      stack_arg(f, t, 4u, &data_va) != 0 ||
      stack_arg(f, t, 5u, &data_len) != 0) {
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
  default:
    t->win32_last_error = WIN32_ERROR_INVALID_PARAM;
    f->rax = (UINT64)-1;
    return;
  }
}

/* ---- import resolution ------------------------------------------------- */

int win32_api_lookup(const char *dll, const char *func) {
  UINT32 i;

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
  return -1;
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
