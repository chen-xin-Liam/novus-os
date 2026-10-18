/**
 * Novus OS - round-robin scheduler over ring-3 tasks (M4).
 *
 * The switch is the trap return path (see sched.h). What lives here is the
 * bookkeeping around it: the task table, the kernel stacks, the frames
 * that start a task, and the timer that ends a slice.
 */

#include "kernel.h"
#include "sched.h"
#include "gdt.h"
#include "idt.h"
#include "lapic.h"
#include "msr.h"
#include "mm.h"
#include "pmm.h"
#include "vm.h"
#include "ipc.h"
#include "nd.h"
#include "nwm.h"
#include "sysinfo.h"
#include "nuc.h"

/* Written by trap.S: the stack the syscall gate switches to, and the
 * gate itself. */
extern UINT64 sys_kernel_rsp;
extern void syscall_entry(void);

/* Enter a frame and iretq into it (trap.S). Never returns to the caller. */
extern void sched_enter(UINT64 frame) __attribute__((noreturn));

#define LAPIC_TIMER_DIV   0x3E0u
#define LAPIC_LVT_TIMER   0x320u
#define LAPIC_TIMER_INIT  0x380u
#define LAPIC_EOI         0xB0u

#define LAPIC_TIMER_COUNT 200000u /* ~10 ms on QEMU's bus clock / 16 */

static task_t g_tasks[SCHED_MAX_TASKS];
static task_t *g_current;
static task_t *g_first; /* head of the round-robin ring, set before the
                         * first switch so several tasks can be created
                         * while g_current is still NULL */
static int     g_need_resched;
static UINT32  g_next_id;
static UINT32  g_exit_count;
static UINT64  g_ticks;
static UINT32  g_logged_switches;

/* A task that exited on the last switch. Its pages cannot be returned at
 * the point of the switch - the CPU is still running on its ring-0 stack
 * and CR3 still named it a few instructions earlier - so it waits here for
 * the next trap, which by then runs on another task's stack. */
static task_t *g_reap;

/* Kernel idle context (M8). When no task is READY the kernel must not
 * stop: the timer is what drives the desktop, and the taskbar has to stay
 * able to start a process again. This little stack and the loop below are
 * all that runs in that state - ring 0, no address space of its own. */
static UINT8 g_kidle_stack[8192] __attribute__((aligned(16)));
static int   g_kidle_reported;

/* Rotating start index for the round-robin scan. Kept separate from the
 * task ring so a link mistake can never hide a runnable task. */
static UINT32 g_pick_hint;

/* ---- helpers ----------------------------------------------------------- */

/* Kernel stacks live in a static pool, one slot per task slot.
 *
 * Asking the PMM for 16 contiguous pages at run time works exactly once:
 * the bitmap's first-fit hands out the same run every attempt, so as soon
 * as a spawn/exit cycle leaves a hole at the bottom of the heap the search
 * can never succeed again - start-up was fine, the first relaunch was not.
 * A fixed pool has no such failure mode and costs 64 KiB of BSS per slot. */
static UINT8 g_kstacks[SCHED_MAX_TASKS][SCHED_KSTACK_PAGES * 4096u]
    __attribute__((aligned(16)));

static void *kstack_for(task_t *t) {
  UINT32 slot = (UINT32)(t - &g_tasks[0]);

  if (slot >= SCHED_MAX_TASKS) {
    return NULL;
  }
  return g_kstacks[slot];
}

static void task_build_frame(task_t *t, UINT64 entry, UINT64 user_rsp) {
  trap_frame_t *f =
      (trap_frame_t *)(UINTN)(t->kstack_top - sizeof(trap_frame_t));

  memset(f, 0, sizeof(*f));
  f->rip = entry;
  f->cs = GDT_SEL_UCODE;
  f->rflags = 0x202u; /* IF, so the task can be preempted */
  f->rsp = user_rsp;
  f->ss = GDT_SEL_UDATA;
  t->rsp = (UINT64)(UINTN)f;
}

/* Take a dead task out of the round-robin ring. A FREE slot must not be
 * reachable through `next`, or reusing it would splice it into the ring
 * twice and make task_pick_next() walk in circles. */
static void task_unlink(task_t *t) {
  task_t *p;
  UINT32  guard = 0;

  if (g_first == NULL) {
    t->next = NULL;
    return;
  }
  if (g_first == t && t->next == t) {
    g_first = NULL;
    t->next = NULL;
    return;
  }
  p = g_first;
  while (p->next != t && p->next != NULL && ++guard < SCHED_MAX_TASKS) {
    p = p->next;
  }
  if (p->next == t) {
    p->next = t->next;
  }
  if (g_first == t) {
    g_first = t->next;
  }
  t->next = NULL;
}

/* Return everything an exited task owned: its user pages and page tables
 * through vm_space_free, and its ring-0 stack. Only safe from another
 * task's stack and with another CR3 installed, which is what the caller
 * guarantees. */
static void task_reap(task_t *t) {
  UINT32 freed;
  UINT64 pml4;

  /* EXITED only. A slot can be reused between the moment a task is queued
   * for reaping and the reap itself, and freeing the *new* owner's space
   * from a stale pointer would take a running app down with it. */
  if (t == NULL || t == g_current || t->state != TASK_EXITED) {
    return;
  }
  /* vm_space_free() clears the space, so remember what it was for the log
   * - it is the one number that says "this task's tables are gone". */
  pml4 = t->space.pml4_phys;
  freed = vm_space_free(&t->space);
  /* The kernel stack belongs to the static pool, not to the PMM: nothing
   * to release, and the next task in this slot reuses it. */
  kputs("[REAP] ");
  kputs(t->name);
  kputs(": ");
  kputs_hex((UINT64)freed);
  kputs(" pages back, pml4=");
  kputs_hex(pml4);
  kputs(", free=");
  kputs_hex(pmm_free_count());
  kputs("\n");

  task_unlink(t);
  t->cr3 = 0;
  t->kstack_top = 0;
  t->kstack_phys = 0;
  t->rsp = 0;
  t->exit_code = 0;
  /* Last: the slot is only reusable once nothing above can still need it. */
  t->state = TASK_FREE;
}

static task_t *task_find_free(void) {
  UINT32 i;

  for (i = 0; i < SCHED_MAX_TASKS; i++) {
    if (g_tasks[i].state == TASK_FREE) {
      return &g_tasks[i];
    }
  }
  return NULL;
}

static task_t *task_pick_next(void) {
  UINT32 i;

  for (i = 0; i < SCHED_MAX_TASKS; i++) {
    UINT32  idx = (g_pick_hint + i) % SCHED_MAX_TASKS;
    task_t *t = &g_tasks[idx];

    if (t->state == TASK_READY) {
      g_pick_hint = (idx + 1u) % SCHED_MAX_TASKS;
      return t;
    }
  }
  return NULL;
}

static int task_name_alive(const char *name) {
  UINT32 i;
  UINT32 k;

  for (i = 0; i < SCHED_MAX_TASKS; i++) {
    const task_t *t = &g_tasks[i];

    if (t->state == TASK_FREE || t->state == TASK_EXITED) {
      continue;
    }
    for (k = 0; k < sizeof(t->name) && t->name[k] == name[k]; k++) {
      if (name[k] == '\0') {
        return 1;
      }
    }
  }
  return 0;
}

/* One line per idle task left in the table, so "the machine looks frozen"
 * comes with the reason attached. */
static void sched_dump_tasks(void) {
  UINT32 i;

  for (i = 0; i < SCHED_MAX_TASKS; i++) {
    const task_t *t = &g_tasks[i];

    if (t->state == TASK_FREE) {
      continue;
    }
    kputs("[SCHED]   task ");
    kputs(t->name);
    kputs(" pid=");
    kputs_hex(t->pid);
    kputs(" state=");
    kputs_hex(t->state);
    kputs("\n");
  }
}

static void sched_kernel_idle(void) {
  for (;;) {
    __asm__ volatile("sti; hlt");
  }
}

/* A ring-0 frame that iretq's into sched_kernel_idle(). Built fresh each
 * time: the loop never returns, so its stack is only ever used by the
 * interrupt that will bring a task back. */
static UINT64 sched_kidle_frame(void) {
  UINT64        top = (UINT64)(UINTN)g_kidle_stack + sizeof(g_kidle_stack);
  trap_frame_t *f = (trap_frame_t *)(UINTN)(top - sizeof(trap_frame_t));

  memset(f, 0, sizeof(*f));
  f->rip = (UINT64)(UINTN)sched_kernel_idle;
  f->cs = GDT_SEL_KCODE;
  f->rflags = 0x202u; /* IF: the timer has to keep firing */
  f->rsp = (UINT64)(UINTN)g_kidle_stack + 16u;
  f->ss = GDT_SEL_KDATA;
  tss_set_rsp0(top);
  sys_kernel_rsp = top;
  return (UINT64)(UINTN)f;
}

/* Enter the kernel idle context: drop the dead task, give the kernel its
 * own page tables back, and return the frame to iretq into. */
static UINT64 sched_enter_kidle(void) {
  if (!g_kidle_reported) {
    g_kidle_reported = 1;
    kputs("[SCHED] no runnable task: kernel idle, desktop keeps running\n");
    sched_dump_tasks();
  }
  if (g_current != NULL && g_current->state == TASK_EXITED) {
    g_reap = g_current;
  }
  g_current = NULL;
  vm_switch_cr3(mm_kernel_cr3());
  return sched_kidle_frame();
}

/* ---- public ------------------------------------------------------------ */

void sched_init(void) {
  UINT64 base;

  memset(g_tasks, 0, sizeof(g_tasks));
  g_current = NULL;
  g_first = NULL;
  g_need_resched = 0;
  g_next_id = 0;
  g_exit_count = 0;
  g_ticks = 0;
  g_reap = NULL;

  gdt_init();
  idt_init();

  /* syscall was #UD until here: EFER.SCE is what turns it on, LSTAR is
   * where it lands, STAR supplies the ring-0 selectors, FMASK clears IF
   * on the way in (the entry runs before the stack is trusted). */
  /* NX: CPUID.80000001H:EDX[20]. QEMU and every x86_64 CPU made since
   * 2004 set it; without it the NX bit in the page tables is ignored and
   * user data would be executable again. */
  {
    UINT32 eax = 0x80000001u, ebx, ecx, edx;

    __asm__ volatile("cpuid"
                     : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    if ((edx & (1u << 20)) != 0u) {
      msr_write(MSR_EFER, msr_read(MSR_EFER) | (1ULL << 11));
      kputs("[SCHED] NX enabled: data pages are not executable\n");
    }
    (void)ebx;
    (void)ecx;
  }
  msr_write(MSR_EFER, msr_read(MSR_EFER) | EFER_SCE);
  msr_write(MSR_STAR,
            ((UINT64)GDT_SEL_KCODE << 32) | ((UINT64)GDT_SEL_UCODE << 48));
  msr_write(MSR_LSTAR, (UINT64)(UINTN)syscall_entry);
  msr_write(MSR_FMASK, 0x200u | 0x400u); /* IF and DF */
  kputs("[SCHED] syscall gate at ");
  kputs_hex((UINT64)(UINTN)syscall_entry);
  kputs("\n");

  base = lapic_base();
  if (base == 0) {
    kputs("[SCHED] WARN no LAPIC: no timer, no preemption\n");
    return;
  }
  lapic_write(base, LAPIC_TIMER_DIV, 0x3u); /* divide by 16 */
  lapic_write(base, LAPIC_LVT_TIMER,
              IDT_TIMER_VECTOR | (1u << 17)); /* periodic */
  lapic_write(base, LAPIC_TIMER_INIT, LAPIC_TIMER_COUNT);
  kputs("[SCHED] timer armed: vector 32, initial count ");
  kputs_hex(LAPIC_TIMER_COUNT);
  kputs("\n");
}

task_t *sched_create_user(const char *name, const vm_space_t *space,
                          UINT64 entry, UINT64 user_rsp) {
  task_t *t = task_find_free();
  UINT8  *kstack;
  UINT32  i;

  if (t == NULL) {
    kputs("[SCHED] no free task slot for ");
    kputs(name);
    kputs("\n");
    sched_dump_tasks();
    return NULL;
  }
  kstack = (UINT8 *)kstack_for(t);
  if (kstack == NULL) {
    kputs("[SCHED] ERROR: no kernel stack slot\n");
    return NULL;
  }

  memset(t, 0, sizeof(*t));
  t->id = (int)g_next_id++;
  t->pid = (UINT64)t->id;
  t->state = TASK_READY;
  t->cr3 = space->pml4_phys;
  t->space = *space;
  t->kstack_top = (UINT64)(UINTN)(kstack + SCHED_KSTACK_PAGES * 4096u);
  t->kstack_phys = virt_to_phys(kstack);
  t->slice = SCHED_SLICE_TICKS;
  t->exit_code = -1;
  for (i = 0; i < sizeof(t->name) - 1u && name[i] != '\0'; i++) {
    t->name[i] = name[i];
  }
  /* Every task gets an inbox port named by its pid (M6): that is the
   * address a server replies to, and the reason hdr.sender is enough. */
  (void)ipc_task_attach(t->pid);
  task_build_frame(t, entry, user_rsp);

  /* Ring the task into the round-robin list behind the current head. */
  if (g_first == NULL) {
    g_first = t;
    t->next = t;
  } else {
    t->next = g_first->next;
    g_first->next = t;
  }
  return t;
}

UINT64 sched_switch_frame(UINT64 current_frame) {
  task_t *next;

  /* Reap the task that exited on the previous switch. This runs on the
   * stack of whatever task is running now, with its CR3 installed, so the
   * dead task's stack and tables are genuinely idle. */
  if (g_reap != NULL) {
    task_t *dead = g_reap;

    g_reap = NULL;
    task_reap(dead);
  }

  if (g_current != NULL) {
    if (g_current->state == TASK_RUNNING) {
      /* Save the frame and put the task back in the ring: without the
       * state change it would be RUNNING forever and never picked again,
       * which is how a scheduled-out idle task disappears. */
      g_current->rsp = current_frame;
      g_current->state = TASK_READY;
    } else if (g_current->state == TASK_BLOCKED) {
      /* A parked task still needs its frame saved - that frame holds the
       * rewound RIP a blocked syscall left behind, and it is the only way
       * back into the task when a message wakes it. */
      g_current->rsp = current_frame;
    }
    if (!g_need_resched && g_current->state != TASK_EXITED) {
      return current_frame;
    }
  }
  g_need_resched = 0;

  next = task_pick_next();
  if (next == NULL) {
    if (g_current != NULL && g_current->state == TASK_EXITED) {
      /* The last runnable task just exited. Halting here would stop the
       * timer and with it the desktop; park in the kernel idle context
       * instead, where the tick keeps compositing and can start a new
       * process from the taskbar. */
      return sched_enter_kidle();
    }
    return current_frame; /* nothing else runnable: keep this frame */
  }

  /* Hand the outgoing task to the next trap only after the switch: until
   * then its ring-0 stack is the one this function is running on. */
  if (g_current != NULL && g_current->state == TASK_EXITED) {
    g_reap = g_current;
  }

  g_current = next;
  next->state = TASK_RUNNING;
  next->slice = SCHED_SLICE_TICKS;
  g_kidle_reported = 0; /* the next idle entry is a new event */

  /* A dozen lines is enough to see whether the timer is switching tasks
   * or the tasks are only switching themselves. */
  if (g_logged_switches < 12u) {
    g_logged_switches++;
    kputs("[SCHED] switch -> ");
    kputs(next->name);
    kputs("\n");
  }

  tss_set_rsp0(next->kstack_top);
  sys_kernel_rsp = next->kstack_top;
  vm_switch_cr3(next->cr3);
  return next->rsp;
}

/* A parked task whose deadline passed runs again with an error, so a
 * blocking receive can never wedge a process forever. */
static void sched_expire_waiters(void) {
  UINT32 i;

  for (i = 0; i < SCHED_MAX_TASKS; i++) {
    task_t *t = &g_tasks[i];

    if (t->state == TASK_BLOCKED && t->wait_deadline != 0 &&
        g_ticks >= t->wait_deadline) {
      t->wait_err = IPC_ETIMEDOUT;
      t->wait_deadline = 0;
      t->wait_port = 0;
      t->state = TASK_READY;
      g_need_resched = 1;
    }
  }
}

void sched_tick(void) {
  g_ticks++;
  sched_expire_waiters();

  /* Reap tasks that were killed while they were not the running one:
   * their pages are still mapped and their slot is still taken. Doing it
   * here is safe - this runs on the current task's stack, with its CR3. */
  if ((g_ticks % 40u) == 0u) {
    UINT32 i;

    for (i = 0; i < SCHED_MAX_TASKS; i++) {
      if (g_tasks[i].state == TASK_EXITED && &g_tasks[i] != g_current &&
          &g_tasks[i] != g_reap) {
        task_reap(&g_tasks[i]);
      }
    }
  }

  if (g_current != NULL) {
    g_current->ticks++;
    if (g_current->slice > 0u) {
      g_current->slice--;
    }
    if (g_current->slice == 0u) {
      g_need_resched = 1;
    }
    if ((g_ticks % 40u) == 0u) {
      kputs("[SCHED] ticks=");
      kputs_hex(g_ticks);
      kputs(" current=");
      kputs(g_current->name);
      kputs("\n");
    }
  } else {
    /* Kernel idle: ask for a switch every tick so the first process that
     * becomes runnable (a taskbar click forks one) is entered promptly. */
    g_need_resched = 1;
    if ((g_ticks % 40u) == 0u) {
      kputs("[SCHED] ticks=");
      kputs_hex(g_ticks);
      kputs(" current=(kernel idle)\n");
    }
  }

  /* M8: if the fallback task ever dies, bring it back. It is the safety
   * net behind the safety net - the kernel idle context covers the case
   * where this fails as well. */
  if ((g_ticks % 200u) == 0u && !task_name_alive("idle")) {
    kputs("[SCHED] idle task missing, recreating\n");
    (void)nuc_create_idle();
  }

#if CONFIG_NOVUS_WM
  /* M6: Novus-WM drives the whole desktop from the tick - input routing,
   * the IPC server queue and the compositor. M5's nd_demo_tick() is what
   * runs instead when the window manager is configured out. */
  nwm_tick();
  /* M8: the sysinfo service shares the tick with the WM. */
  sysinfo_tick();
#elif CONFIG_NOVUS_DISPLAY
  /* M5: NovusDisplay shares the tick. Input is polled (no new IRQ lines)
   * and at most one damage rectangle is repainted per event, so the cost
   * stays bounded and the scheduler's own work is unaffected. */
  nd_demo_tick();
#endif
}

void sched_exit_current(int code) {
  if (g_current == NULL) {
    return;
  }
  g_current->state = TASK_EXITED;
  g_current->exit_code = code;
  g_exit_count++;
  g_need_resched = 1;
  /* Its inbox and any named port it owned go with it; a window whose
   * owner is gone is closed by the WM on the next tick. */
  ipc_owner_exit(g_current->pid);

  kputs("[SCHED] task ");
  kputs(g_current->name);
  kputs(" exited with code ");
  kputs_hex((UINT64)(UINT32)code);
  kputs("\n");
}

void sched_yield(void) {
  if (g_current != NULL) {
    g_current->slice = 0;
  }
  g_need_resched = 1;
}

void sched_park(UINT64 port, UINT32 timeout_ticks) {
  if (g_current == NULL) {
    return;
  }
  g_current->wait_port = port;
  g_current->wait_deadline = (timeout_ticks != 0u) ? g_ticks + timeout_ticks : 0;
  g_current->wait_err = 0;
  g_current->state = TASK_BLOCKED;
  g_need_resched = 1;
}

void sched_wake_port(UINT64 port) {
  UINT32 i;

  for (i = 0; i < SCHED_MAX_TASKS; i++) {
    task_t *t = &g_tasks[i];

    if (t->state == TASK_BLOCKED && t->wait_port == port) {
      t->wait_port = 0;
      t->wait_deadline = 0;
      t->state = TASK_READY;
      g_need_resched = 1;
    }
  }
}

UINT64 sched_ticks(void) { return g_ticks; }

/* Task by name, for the M8 launcher: refusing a second copy of an
 * application is a name lookup, and names are what the file manager and
 * the task manager show. Exited tasks are skipped. */
/* The two tasks the scheduler owns: killing them would take the desktop
 * down with the process, so the service refuses instead. */
static int task_is_vital(const task_t *t) {
  const char *a = t->name;
  const char *b = "idle";

  while (*a != '\0' && *a == *b) {
    a++;
    b++;
  }
  if (*a == '\0' && *b == '\0') {
    return 1;
  }
  a = t->name;
  b = "spin";
  while (*a != '\0' && *a == *b) {
    a++;
    b++;
  }
  return *a == '\0' && *b == '\0';
}

/* End a task from outside (M9 settings panel). 0 = gone, -1 = no such
 * task, -2 = refused, -3 = it was already on its way out.
 *
 * The task is not reaped here: that has to happen from another task's
 * stack with another CR3, which is what the sweep in sched_tick() is
 * for. Marking it EXITED is enough to make every other path ignore it. */
int sched_kill(UINT64 pid) {
  task_t *t = NULL;
  UINT32  i;

  for (i = 0; i < SCHED_MAX_TASKS; i++) {
    if (g_tasks[i].state != TASK_FREE && g_tasks[i].pid == pid) {
      t = &g_tasks[i];
      break;
    }
  }

  if (t == NULL) {
    return -1;
  }
  if (task_is_vital(t)) {
    return -2;
  }
  if (t->state == TASK_EXITED) {
    return -3;
  }
  t->state = TASK_EXITED;
  t->exit_code = -1;
  g_exit_count++;
  g_need_resched = 1;
  /* Same cleanup as a self-exit: its inbox and named ports go, its
   * waiters are woken so they fail fast instead of timing out, and the
   * WM closes its windows on the next tick (owner gone). */
  ipc_owner_exit(pid);
  sched_wake_port(pid);
  /* Reap now when it is not the running task: this runs in the timer tick
   * with the current task's CR3, which is exactly what task_reap() needs.
   * Leaving it to the sweep would keep the slot busy for up to 40 ticks and
   * a "start it again" click right after the kill would fail with "no free
   * task slot". */
  if (t != g_current) {
    task_reap(t);
  }

  kputs("[SCHED] task ");
  kputs(t->name);
  kputs(" killed (pid ");
  kputs_hex(pid);
  kputs(")\n");
  return 0;
}

task_t *sched_find_name(const char *name) {
  UINT32 i;

  for (i = 0; i < SCHED_MAX_TASKS; i++) {
    const char *a = g_tasks[i].name;
    const char *b = name;

    if (g_tasks[i].state == TASK_FREE || g_tasks[i].state == TASK_EXITED) {
      continue;
    }
    while (*a != '\0' && *a == *b) {
      a++;
      b++;
    }
    if (*a == '\0' && *b == '\0') {
      return &g_tasks[i];
    }
  }
  return NULL;
}

task_t *sched_find_pid(UINT64 pid) {
  UINT32 i;

  for (i = 0; i < SCHED_MAX_TASKS; i++) {
    /* EXITED counts as gone: the WM closes its windows one tick earlier
     * and nothing can address it any more. sched_kill() does its own raw
     * lookup so it can still answer "already exiting". */
    if (g_tasks[i].state != TASK_FREE && g_tasks[i].state != TASK_EXITED &&
        g_tasks[i].pid == pid) {
      return &g_tasks[i];
    }
  }
  return NULL;
}

task_t *sched_current(void) { return g_current; }

UINT32 sched_exit_count(void) { return g_exit_count; }

void sched_start(void) {
  task_t *first = task_pick_next();

  if (first == NULL) {
    kputs("[SCHED] no task to run\n");
    for (;;) {
      cpu_halt();
    }
  }
  g_current = first;
  first->state = TASK_RUNNING;
  tss_set_rsp0(first->kstack_top);
  sys_kernel_rsp = first->kstack_top;
  vm_switch_cr3(first->cr3);

  kputs("[SCHED] entering ring 3: ");
  kputs(first->name);
  kputs("\n");
  sched_enter(first->rsp);
}

/* ---- introspection (M8 task manager) ---------------------------------- */

UINT32 sched_task_count(void) { return SCHED_MAX_TASKS; }

int sched_task_info(UINT32 idx, sched_info_t *out) {
  task_t *t;

  if (out == 0 || idx >= SCHED_MAX_TASKS) {
    return -1;
  }
  t = &g_tasks[idx];
  if (t->state == TASK_FREE) {
    return -1;
  }
  out->pid = t->pid;
  out->state = (UINT8)t->state;
  out->exit_code = t->exit_code;
  out->ticks = t->ticks;
  out->slice = t->slice;
  {
    UINT32 i;

    for (i = 0; i + 1u < sizeof(out->name) && t->name[i] != '\0'; i++) {
      out->name[i] = t->name[i];
    }
    out->name[i] = '\0';
  }
  return 0;
}
