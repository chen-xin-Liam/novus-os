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

#define SYS_YIELD 1u
#define SYS_WRITE 2u
#define SYS_EXIT  60u

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

static void syscall_dispatch(trap_frame_t *f) {
  switch ((UINT32)f->rax) {
  case SYS_YIELD:
    f->rax = 0;
    sched_yield();
    break;
  case SYS_WRITE:
    sys_write(f);
    break;
  case SYS_EXIT:
    sched_exit_current((int)(UINT32)f->rdi);
    f->rax = 0;
    break;
  default:
    kputs("[TRAP] unknown syscall ");
    kputs_hex(f->rax);
    kputs("\n");
    f->rax = (UINT64)-1;
    break;
  }
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
