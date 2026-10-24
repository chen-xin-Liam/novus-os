/**
 * Novus OS - trap dispatch: exceptions, the timer, and syscalls (M4).
 *
 * One entry point for all three, because trap.S builds one frame for all
 * three. The return value is the frame to resume - normally the one the
 * trap came in on, but after a schedule it is another task's, which is
 * how the trap stub ends up iretq-ing into a different process.
 *
 * A fault raised in ring 3 kills that task and nothing else (README part
 * 9: a buggy program shows an error and the system keeps running). A
 * fault in ring 0 is a kernel bug; it is reported with enough state to
 * find it and the machine is halted rather than left corrupt.
 */

#include "kernel.h"
#include "idt.h"
#include "sched.h"
#include "lapic.h"
#include "vm.h"
#include "mm.h"
#include "msr.h"
#include "ipc.h"
#include "irq.h"
#include "../fs/novusfs/novusfs.h"
#include "../fs/vfs.h"
#include "power.h"
#include "session.h"
#include "install.h"

/* Syscall numbers (README part 8: one table, one ABI). */
#define SYS_YIELD       1u
#define SYS_WRITE       2u
#define SYS_GETPID      3u
#define SYS_IPC_SEND    4u
#define SYS_IPC_RECV    5u
#define SYS_IPC_LOOKUP  6u
#define SYS_IPC_STATS   7u
#define SYS_OPEN        8u
#define SYS_READ        9u
#define SYS_FWRITE      10u
#define SYS_CLOSE       11u
#define SYS_STAT        12u
#define SYS_OPENDIR     13u
#define SYS_READDIR     14u
#define SYS_MKDIR       15u
#define SYS_POWEROFF    16u
#define SYS_REBOOT      17u
#define SYS_SESSION     18u
#define SYS_SETUP       19u
#define SYS_EXIT        60u

#define FS_PATH_MAX 256u
#define FS_IO_MAX 4096u
#define FS_FD_BASE 3u

typedef struct {
  UINT64 size;
  UINT16 mode;
  UINT8  pad[6];
} user_stat_t;

typedef struct {
  UINT32 ino;
  UINT16 mode;
  UINT16 name_len;
  char   name[NOVUSFS_NAME_MAX + 1u];
} user_dirent_t;

_Static_assert(sizeof(user_stat_t) == 16u, "user stat ABI drift");
_Static_assert(sizeof(user_dirent_t) == 68u, "user dirent ABI drift");

static UINTN bounded_strlen(const char *s, UINTN cap) {
  UINTN n = 0;
  while (n < cap && s[n] != '\0') n++;
  return n;
}

/* x86_64 `syscall` is two bytes (0F 05). A blocking syscall parks its
 * task and rewinds RIP by this much so the call re-executes when a
 * message wakes it - no continuation state, just a replay. */
#define SYSCALL_INSN_LEN 2u

#define LAPIC_EOI 0xB0u

static const char *trap_name(UINT64 v) {
  static const char *names[32] = {
      "#DE divide error",     "#DB debug",          "NMI",
      "#BP breakpoint",       "#OF overflow",       "#BR bound range",
      "#UD invalid opcode",   "#NM device n/a",     "#DF double fault",
      "coprocessor",          "#TS invalid TSS",    "#NP segment",
      "#SS stack",            "#GP general protection", "#PF page fault",
      "reserved",             "#MF x87",            "#AC alignment",
      "#MC machine check",    "#XM SIMD",           "#VE virtualization",
      "#CP control protection", "#23",              "#24",
      "#25",                  "#26",                "#27",
      "#28",                  "#HV",                "#VC",
      "#SX",                  "#31"};

  return (v < 32u) ? names[v] : "unknown";
}

/* Read one byte of user memory through the task's own tables. The kernel
 * runs on the kernel CR3 while a syscall is handled, so it cannot simply
 * dereference the pointer - and it must not: a user pointer can be
 * unmapped or point at kernel memory. */
static int user_read_byte(UINT64 cr3, UINT64 va, UINT8 *out) {
  vm_space_t sp;
  UINT64     phys;

  /* Borrowed view of the task's space: nothing here allocates or frees, so
   * the accounting fields stay empty. */
  memset(&sp, 0, sizeof(sp));
  sp.pml4_phys = cr3 & ~0xFFFULL;
  if (va > 0x00007FFFFFFFFFFFULL || !vm_user_access(&sp, va, 0)) {
    return -1;
  }
  phys = vm_translate(&sp, va);
  if (phys == 0) {
    return -1;
  }
  *out = *(const UINT8 *)phys_to_virt(phys);
  return 0;
}

static void sys_write(trap_frame_t *f) {
  UINT64 cr3 = (sched_current() != 0) ? sched_current()->cr3 : 0;
  UINT64 len = f->rdx;
  UINT64 i;
  char   buf[129];

  if (len > 128u) {
    len = 128u;
  }
  if (cr3 == 0) {
    f->rax = (UINT64)-1;
    return;
  }
  for (i = 0; i < len; i++) {
    UINT8 b;

    if (user_read_byte(cr3, f->rsi + i, &b) != 0) {
      buf[i] = '\0';
      kputs("[USER] ");
      kputs(buf);
      kputs("\n[TRAP] sys_write: user buffer ended early\n");
      f->rax = (UINT64)-1;
      return;
    }
    buf[i] = (char)b;
  }
  buf[len] = '\0';
  kputs("[USER] ");
  kputs(buf);
  kputs("\n");
  f->rax = len;
}

/* Copy `len` bytes between user memory and the kernel buffer, following
 * the task's page table a page at a time. Returns 0, or -1 as soon as a
 * page is not mapped - a bad pointer must fail the syscall, not fault the
 * kernel. */
static int user_copy(UINT64 cr3, UINT64 va, void *buf, UINT64 len, int to_user) {
  UINT8 *p = (UINT8 *)buf;
  UINT64 done = 0;

  if (cr3 == 0 || (len != 0 && buf == 0) ||
      (len != 0 && (va > 0x00007FFFFFFFFFFFULL ||
                    len - 1u > 0x00007FFFFFFFFFFFULL - va))) {
    return -1;
  }
  while (done < len) {
    UINT64 page = (va + done) & ~0xFFFULL;
    UINT64 off = (va + done) & 0xFFFULL;
    UINT64 chunk = 4096u - off;
    vm_space_t sp;
    UINT64 phys;
    UINT8 *k;

    if (chunk > len - done) {
      chunk = len - done;
    }
    memset(&sp, 0, sizeof(sp));
    sp.pml4_phys = cr3 & ~0xFFFULL;
    if (!vm_user_access(&sp, va + done, to_user)) {
      return -1;
    }
    phys = vm_translate(&sp, page);
    if (phys == 0) {
      return -1;
    }
    k = (UINT8 *)phys_to_virt(phys + off);
    if (to_user) {
      memcpy(k, p + done, chunk);
    } else {
      memcpy(p + done, k, chunk);
    }
    done += chunk;
  }
  return 0;
}

static int user_path(task_t *t, UINT64 va, char path[FS_PATH_MAX]) {
  UINTN i;
  memset(path, 0, FS_PATH_MAX);
  for (i = 0; i < FS_PATH_MAX - 1u; i++) {
    if (user_copy(t->cr3, va + i, &path[i], 1u, 0) != 0) {
      return -1;
    }
    if (path[i] == '\0') {
      return i == 0 ? -1 : 0;
    }
  }
  return -1;
}

static int fs_fd_index(task_t *t, UINT64 fd) {
  UINT64 index;
  /* Filesystem handles are task-local slots, not global kernel objects:
   * subtracting the public base selects only the current task's table.
   * Reusing another task's numeric descriptor therefore cannot grant access. */
  if (fd < FS_FD_BASE) return -1;
  index = fd - FS_FD_BASE;
  if (index >= TASK_FD_MAX || !t->fd[index].used) return -1;
  return (int)index;
}

static void sys_fs_open(trap_frame_t *f) {
  task_t *t = sched_current();
  vfs_fs_t *fs = vfs_root();
  char path[FS_PATH_MAX];
  UINT32 ino;
  UINT32 parent;
  char *leaf;
  UINT32 i;
  if (t == NULL || !session_logged_in() || fs == NULL || user_path(t, f->rdi, path) != 0) { f->rax = (UINT64)-1; return; }
  ino = vfs_lookup_path(fs, path);
  if (ino == 0 && (f->rsi & 1u) != 0u && vfs_parent_path(fs, path, &parent, &leaf) == 0) {
    ino = fs->creat(fs, parent, leaf);
  }
  if (ino == 0) { f->rax = (UINT64)-1; return; }
  for (i = 0; i < TASK_FD_MAX; i++) {
    if (!t->fd[i].used) {
      t->fd[i].ino = ino;
      t->fd[i].offset = 0;
      t->fd[i].dir_index = 0;
      t->fd[i].is_dir = 0;
      t->fd[i].used = 1;
      f->rax = FS_FD_BASE + i;
      return;
    }
  }
  f->rax = (UINT64)-1;
}

static void sys_fs_read(trap_frame_t *f) {
  task_t *t = sched_current();
  vfs_fs_t *fs = vfs_root();
  UINT8 buf[FS_IO_MAX];
  INT64 n;
  int index;
  UINT64 len = f->rdx > FS_IO_MAX ? FS_IO_MAX : f->rdx;
  if (t == NULL || !session_logged_in() || fs == NULL || (index = fs_fd_index(t, f->rdi)) < 0 ||
      t->fd[index].is_dir) { f->rax = (UINT64)-1; return; }
  n = fs->read(fs, t->fd[index].ino, t->fd[index].offset, buf, len);
  if (n < 0 || user_copy(t->cr3, f->rsi, buf, (UINT64)n, 1) != 0) { f->rax = (UINT64)-1; return; }
  t->fd[index].offset += (UINT64)n;
  f->rax = (UINT64)n;
}

static void sys_fs_write(trap_frame_t *f) {
  task_t *t = sched_current();
  vfs_fs_t *fs = vfs_root();
  UINT8 buf[FS_IO_MAX];
  int index;
  UINT64 len = f->rdx > FS_IO_MAX ? FS_IO_MAX : f->rdx;
  INT64 n;
  if (t == NULL || !session_logged_in() || fs == NULL || (index = fs_fd_index(t, f->rdi)) < 0 ||
      t->fd[index].is_dir || user_copy(t->cr3, f->rsi, buf, len, 0) != 0) { f->rax = (UINT64)-1; return; }
  n = fs->write(fs, t->fd[index].ino, t->fd[index].offset, buf, len);
  if (n < 0) { f->rax = (UINT64)-1; return; }
  t->fd[index].offset += (UINT64)n;
  f->rax = (UINT64)n;
}

static void sys_fs_close(trap_frame_t *f) {
  task_t *t = sched_current();
  int index = (t == NULL || !session_logged_in()) ? -1 : fs_fd_index(t, f->rdi);
  if (index < 0) { f->rax = (UINT64)-1; return; }
  memset(&t->fd[index], 0, sizeof(t->fd[index]));
  f->rax = 0;
}

static void sys_fs_stat(trap_frame_t *f) {
  task_t *t = sched_current();
  vfs_fs_t *fs = vfs_root();
  char path[FS_PATH_MAX];
  user_stat_t out;
  UINT32 ino;
  UINT64 size;
  UINT16 mode;
  if (t == NULL || !session_logged_in() || fs == NULL || user_path(t, f->rdi, path) != 0 ||
      (ino = vfs_lookup_path(fs, path)) == 0 ||
      fs->stat(fs, ino, &size, &mode) != 0) { f->rax = (UINT64)-1; return; }
  memset(&out, 0, sizeof(out));
  out.size = size;
  out.mode = mode;
  f->rax = user_copy(t->cr3, f->rsi, &out, sizeof(out), 1) == 0 ? 0 : (UINT64)-1;
}

static void sys_fs_opendir(trap_frame_t *f) {
  task_t *t = sched_current();
  vfs_fs_t *fs = vfs_root();
  char path[FS_PATH_MAX];
  UINT32 ino;
  UINT64 size;
  UINT16 mode;
  UINT32 i;
  if (t == NULL || !session_logged_in() || fs == NULL || user_path(t, f->rdi, path) != 0 ||
      (ino = vfs_lookup_path(fs, path)) == 0 ||
      fs->stat(fs, ino, &size, &mode) != 0 || mode != VFS_MODE_DIR) { f->rax = (UINT64)-1; return; }
  for (i = 0; i < TASK_FD_MAX; i++) {
    if (!t->fd[i].used) {
      t->fd[i].ino = ino;
      t->fd[i].offset = 0;
      t->fd[i].dir_index = 0;
      t->fd[i].is_dir = 1;
      t->fd[i].used = 1;
      f->rax = FS_FD_BASE + i;
      return;
    }
  }
  f->rax = (UINT64)-1;
}

typedef struct { UINT32 skip; UINT32 seen; user_dirent_t *entry; vfs_fs_t *fs; } readdir_ctx_t;
static void fs_readdir_pick(void *arg, UINT32 ino, const char *name) {
  readdir_ctx_t *ctx = (readdir_ctx_t *)arg;
  if (ctx->seen++ != ctx->skip) return;
  ctx->entry->ino = ino;
  ctx->entry->name_len = (UINT16)bounded_strlen(name, NOVUSFS_NAME_MAX);
  memcpy(ctx->entry->name, name, ctx->entry->name_len + 1u);
  (void)ctx->fs->stat(ctx->fs, ino, NULL, &ctx->entry->mode);
}

static void sys_fs_readdir(trap_frame_t *f) {
  task_t *t = sched_current();
  vfs_fs_t *fs = vfs_root();
  user_dirent_t out;
  readdir_ctx_t ctx;
  int index;
  if (t == NULL || !session_logged_in() || fs == NULL || (index = fs_fd_index(t, f->rdi)) < 0 || !t->fd[index].is_dir) { f->rax = (UINT64)-1; return; }
  memset(&out, 0, sizeof(out));
  ctx.skip = t->fd[index].dir_index;
  ctx.seen = 0;
  ctx.entry = &out;
  ctx.fs = fs;
  if (fs->readdir(fs, t->fd[index].ino, fs_readdir_pick, &ctx) != 0) { f->rax = (UINT64)-1; return; }
  if (ctx.seen <= ctx.skip) { f->rax = 0; return; }
  if (user_copy(t->cr3, f->rsi, &out, sizeof(out), 1) != 0) { f->rax = (UINT64)-1; return; }
  t->fd[index].dir_index++;
  f->rax = 1;
}

static void sys_fs_mkdir(trap_frame_t *f) {
  task_t *t = sched_current();
  vfs_fs_t *fs = vfs_root();
  char path[FS_PATH_MAX];
  UINT32 parent;
  UINT32 existing;
  char *leaf;
  if (t == NULL || !session_logged_in() || fs == NULL || user_path(t, f->rdi, path) != 0 ||
      vfs_parent_path(fs, path, &parent, &leaf) != 0) { f->rax = (UINT64)-1; return; }
  existing = fs->lookup(fs, parent, leaf);
  if (existing != 0) { f->rax = (UINT64)-1; return; }
  f->rax = fs->mkdir(fs, parent, leaf) == 0 ? (UINT64)-1 : 0;
}

/* Installer control. The pid check is the capability: only the process
 * apps_launch() started as "setup" can ask for a disk to be written, and
 * no other app can inherit it because the value is set at spawn time. */
static void sys_setup(trap_frame_t *f) {
  task_t *t = sched_current();
  UINT64  op = f->rdi;
  UINT64  arg = f->rsi;
  UINT64  ptr = f->rdx;

  if (t == NULL || !install_owner(t->pid)) {
    f->rax = (UINT64)-1;
    return;
  }
  switch (op) {
  case INSTALL_OP_COUNT:
    f->rax = (UINT64)install_count();
    return;

  case INSTALL_OP_GET: {
    install_disk_t d;

    if (install_disk((UINT8)arg, &d) != 0 ||
        user_copy(t->cr3, ptr, &d, sizeof(d), 1) != 0) {
      f->rax = (UINT64)-1;
      return;
    }
    f->rax = 0;
    return;
  }

  case INSTALL_OP_BEGIN:
    f->rax = (UINT64)(INT64)install_begin((UINT8)arg, (UINT8)ptr);
    return;

  case INSTALL_OP_LANG:
    install_set_language((UINT8)arg);
    f->rax = 0;
    return;

  case INSTALL_OP_STEP: {
    install_progress_t p;
    int phase = install_step(&p);

    if (ptr != 0 && user_copy(t->cr3, ptr, &p, sizeof(p), 1) != 0) {
      f->rax = (UINT64)-1;
      return;
    }
    f->rax = (UINT64)(INT64)phase;
    return;
  }

  case INSTALL_OP_ABORT:
    f->rax = (UINT64)(INT64)install_abort();
    return;

  case INSTALL_OP_STAT: {
    install_progress_t p;

    if (install_progress(&p) != 0 ||
        user_copy(t->cr3, ptr, &p, sizeof(p), 1) != 0) {
      f->rax = (UINT64)-1;
      return;
    }
    f->rax = 0;
    return;
  }

  case INSTALL_OP_RESET:
    f->rax = (UINT64)(INT64)install_reset();
    return;

  default:
    f->rax = (UINT64)-1;
    return;
  }
}

static void sys_power_off(trap_frame_t *f) {
  task_t *t = sched_current();
  if (t == NULL || !session_power_allowed(t->pid)) { f->rax = (UINT64)-1; return; }
  kputs("[POWER] user requested shutdown\n");
  power_off();
  f->rax = (UINT64)-1;
}

static void sys_power_reboot(trap_frame_t *f) {
  task_t *t = sched_current();
  if (t == NULL || !session_power_allowed(t->pid)) { f->rax = (UINT64)-1; return; }
  kputs("[POWER] user requested reboot\n");
  power_reboot();
  f->rax = (UINT64)-1;
}

static void sys_session(trap_frame_t *f) {
  task_t *t = sched_current();
  session_req_t req;
  INT32 rc;
  if (t == NULL || user_copy(t->cr3, f->rdi, &req, sizeof(req), 0) != 0) {
    f->rax = (UINT64)-1;
    return;
  }
  rc = session_handle(&req, t->pid);
  memset(&req, 0, sizeof(req));
  f->rax = (UINT64)(INT64)rc;
}

static void sys_getpid(trap_frame_t *f) {
  task_t *t = sched_current();

  f->rax = (t != 0) ? t->pid : 0u;
}

/* ipc_send(port, type | flags << 16, payload_va, len) */
static void sys_ipc_send(trap_frame_t *f) {
  task_t *t = sched_current();
  UINT8   payload[IPC_PAYLOAD_MAX];
  UINT32  len = (UINT32)f->r10;
  UINT64  src = f->rdx;

  if (t == 0) {
    f->rax = (UINT64)(INT64)IPC_EBADF;
    return;
  }
  if (len > IPC_PAYLOAD_MAX) {
    f->rax = (UINT64)(INT64)IPC_EMSGSIZE;
    return;
  }
  memset(payload, 0, sizeof(payload));
  if (user_copy(t->cr3, src, payload, len, 0) != 0) {
    f->rax = (UINT64)(INT64)IPC_EBADF;
    return;
  }
  f->rax = (UINT64)(INT64)ipc_send(f->rdi, t->pid, (UINT16)(f->rsi & 0xFFFFu),
                                   (UINT16)(f->rsi >> 16), payload, len);
}

/* ipc_recv(port, buf_va, cap, flags). port 0 means the caller's inbox.
 * A blocking call with an empty queue parks the task and rewinds RIP, so
 * this same instruction runs again once a message lands. */
static void sys_ipc_recv(trap_frame_t *f) {
  task_t    *t = sched_current();
  ipc_msg_t  msg;
  UINT64     port = f->rdi;
  UINT32     cap = (UINT32)f->rdx;
  UINT32     flags = (UINT32)f->r10;
  int        rc;

  if (t == 0) {
    f->rax = (UINT64)(INT64)IPC_EBADF;
    return;
  }
  /* A previous park that timed out reports its error here, once. */
  if (t->wait_err != 0) {
    f->rax = (UINT64)(INT64)t->wait_err;
    t->wait_err = 0;
    return;
  }
  if (port == 0) {
    port = t->pid;
  }
  if (cap < IPC_HDR_SIZE) {
    f->rax = (UINT64)(INT64)IPC_EMSGSIZE;
    return;
  }
  rc = ipc_recv(port, &msg, (flags & IPC_F_BLOCK) != 0u);
  if (rc == IPC_ERETRY) {
    /* Rewind to the `syscall` instruction and leave RAX alone: it still
     * holds the syscall number, which is exactly what the replay needs.
     * The other argument registers were never touched either. */
    f->rip -= SYSCALL_INSN_LEN;
    return;
  }
  if (rc < 0) {
    f->rax = (UINT64)(INT64)rc;
    return;
  }
  {
    UINT32 total = (UINT32)IPC_HDR_SIZE + (UINT32)rc;
    UINT8  frame[IPC_HDR_SIZE + IPC_PAYLOAD_MAX];

    if (total > sizeof(frame)) {
      total = (UINT32)sizeof(frame);
    }
    memcpy(frame, &msg.hdr, IPC_HDR_SIZE);
    memcpy(frame + IPC_HDR_SIZE, msg.payload, (UINT64)rc);
    if (total > cap) {
      total = cap; /* the caller gets a truncated payload, not a fault */
    }
    if (user_copy(t->cr3, f->rsi, frame, total, 1) != 0) {
      f->rax = (UINT64)(INT64)IPC_EBADF;
      return;
    }
  }
  f->rax = (UINT64)rc;
}

/* ipc_lookup(name_va) -> port id, or IPC_ENOENT. */
static void sys_ipc_lookup(trap_frame_t *f) {
  task_t *t = sched_current();
  char    name[IPC_NAME_MAX];
  UINT64  id = 0;
  int     i;

  if (t == 0) {
    f->rax = (UINT64)(INT64)IPC_EBADF;
    return;
  }
  memset(name, 0, sizeof(name));
  if (user_copy(t->cr3, f->rdi, name, sizeof(name) - 1u, 0) != 0) {
    f->rax = (UINT64)(INT64)IPC_EBADF;
    return;
  }
  for (i = 0; i < (int)sizeof(name); i++) {
    if (name[i] == '\0') {
      break;
    }
  }
  if (i == (int)sizeof(name)) {
    name[sizeof(name) - 1u] = '\0';
  }
  if (ipc_port_lookup(name, &id) != IPC_OK) {
    f->rax = (UINT64)(INT64)IPC_ENOENT;
    return;
  }
  f->rax = id;
}

/* ipc_stats(buf_va, cap) -> bytes written. */
static void sys_ipc_stats(trap_frame_t *f) {
  task_t            *t = sched_current();
  const ipc_stats_t *s = ipc_stats();
  UINT32             cap = (UINT32)f->rdx;
  UINT32             n = (UINT32)sizeof(*s);

  if (t == 0) {
    f->rax = (UINT64)(INT64)IPC_EBADF;
    return;
  }
  if (cap < n) {
    n = cap;
  }
  if (user_copy(t->cr3, f->rsi, (void *)s, n, 1) != 0) {
    f->rax = (UINT64)(INT64)IPC_EBADF;
    return;
  }
  f->rax = n;
}

static void sys_yield_call(trap_frame_t *f) {
  f->rax = 0;
  sched_yield();
}

static void sys_exit_call(trap_frame_t *f) {
  /* Exit is routed through the scheduler so task-owned state is retired in
   * one place, including the inbox and named IPC ports; the syscall does not
   * return to user code after the scheduler switches away. */
  sched_exit_current((int)(UINT32)f->rdi);
  f->rax = 0;
}

typedef void (*sys_fn_t)(trap_frame_t *);

typedef struct {
  UINT32      num;
  const char *name; /* what the log calls it */
  sys_fn_t    fn;
} sys_ent_t;

/* README part 8: one numbered table, one ABI. A new call is a new row;
 * nothing else in the kernel needs to know its number. */
static const sys_ent_t g_syscalls[] = {
    {SYS_YIELD, "yield", sys_yield_call},
    {SYS_WRITE, "write", sys_write},
    {SYS_GETPID, "getpid", sys_getpid},
    {SYS_IPC_SEND, "ipc_send", sys_ipc_send},
    {SYS_IPC_RECV, "ipc_recv", sys_ipc_recv},
    {SYS_IPC_LOOKUP, "ipc_lookup", sys_ipc_lookup},
    {SYS_IPC_STATS, "ipc_stats", sys_ipc_stats},
    {SYS_OPEN, "open", sys_fs_open},
    {SYS_READ, "read", sys_fs_read},
    {SYS_FWRITE, "fs_write", sys_fs_write},
    {SYS_CLOSE, "close", sys_fs_close},
    {SYS_STAT, "stat", sys_fs_stat},
    {SYS_OPENDIR, "opendir", sys_fs_opendir},
    {SYS_READDIR, "readdir", sys_fs_readdir},
    {SYS_MKDIR, "mkdir", sys_fs_mkdir},
    {SYS_POWEROFF, "power_off", sys_power_off},
    {SYS_REBOOT, "power_reboot", sys_power_reboot},
    {SYS_SESSION, "session", sys_session},
    {SYS_SETUP, "setup", sys_setup},
    {SYS_EXIT, "exit", sys_exit_call},
};

/* Unknown numbers are reported once each and refused; a wrong ABI must be
 * visible in the log, not silently ignored. */
static UINT32 g_unknown_calls;

static void syscall_dispatch(trap_frame_t *f) {
  UINT32 i;

  for (i = 0; i < sizeof(g_syscalls) / sizeof(g_syscalls[0]); i++) {
    if (g_syscalls[i].num == (UINT32)f->rax) {
      g_syscalls[i].fn(f);
      return;
    }
  }
  if (g_unknown_calls < 8u) {
    g_unknown_calls++;
    kputs("[TRAP] unknown syscall ");
    kputs_hex(f->rax);
    kputs("\n");
  }
  f->rax = (UINT64)-1;
}

static void trap_exception(trap_frame_t *f) {
  kputs("[TRAP] ");
  kputs(trap_name(f->vector));
  kputs(" err=");
  kputs_hex(f->errcode);
  kputs(" rip=");
  kputs_hex(f->rip);
  kputs(" cs=");
  kputs_hex(f->cs);
  kputs(" rsp=");
  kputs_hex(f->rsp);
  kputs("\n");

  if ((f->cs & 3u) == 3u) {
    kputs("[TRAP] fault in user code: task terminated, kernel continues\n");
    sched_exit_current(-1);
    return;
  }
  kputs("[TRAP] kernel fault: halting\n");
  for (;;) {
    cpu_halt();
  }
}

UINT64 trap_dispatch(trap_frame_t *f) {
  if (f->vector == IDT_SYSCALL_MARK) {
    syscall_dispatch(f);
  } else if (f->vector == IDT_TIMER_VECTOR) {
    /* Retire the LAPIC timer interrupt before running tick work; the
     * timer branch then shares the ordinary frame-return scheduler path.
     * The saved frame remains the continuation selected for interrupt return. */
    lapic_write(lapic_base(), LAPIC_EOI, 0);
    sched_tick();
  } else if (f->vector >= IRQ_FIRST_VECTOR && f->vector <= IRQ_SPURIOUS_VECTOR) {
    /* Device IRQ dispatch performs its own EOI and preserves this frame.
     * A scheduling decision is therefore left to a later timer or syscall
     * path rather than switching directly from this branch. */
    irq_dispatch((UINT32)f->vector);
    return (UINT64)(UINTN)f;
  } else {
    trap_exception(f);
  }
  return sched_switch_frame((UINT64)(UINTN)f);
}
