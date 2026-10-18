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

/* Syscall numbers (README part 8: one table, one ABI). */
#define SYS_YIELD       1u
#define SYS_WRITE       2u
#define SYS_GETPID      3u
#define SYS_IPC_SEND    4u
#define SYS_IPC_RECV    5u
#define SYS_IPC_LOOKUP  6u
#define SYS_IPC_STATS   7u
#define SYS_EXIT        60u

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

  if (cr3 == 0 || (len != 0 && buf == 0)) {
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
    lapic_write(lapic_base(), LAPIC_EOI, 0);
    sched_tick();
  } else {
    trap_exception(f);
  }
  return sched_switch_frame((UINT64)(UINTN)f);
}
