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

/* ---- helpers ----------------------------------------------------------- */

/* Contiguous physical pages, addressed through the physmap: a kernel stack
 * has to be contiguous in virtual memory and the VMM hands out single
 * pages, so this asks the PMM until it gets a run. First-fit makes that
 * the normal case; the loop is for when it is not. */
static void *alloc_pages_contig(UINT32 count) {
  UINT32 attempt;

  for (attempt = 0; attempt < 64u; attempt++) {
    UINT64 first = (UINT64)(UINTN)pmm_alloc_page();
    UINT32 i;
    int    ok = 1;

    if (first == 0) {
      return NULL;
    }
    for (i = 1; i < count; i++) {
      UINT64 next = (UINT64)(UINTN)pmm_alloc_page();

      if (next != first + (UINT64)i * 4096u) {
        if (next != 0) {
          pmm_free_page((void *)(UINTN)next);
        }
        ok = 0;
        break;
      }
    }
    if (ok) {
      UINT8 *p = (UINT8 *)phys_to_virt(first);
      memset(p, 0, (UINT64)count * 4096u);
      return p;
    }
    /* Release the partial run and try again. */
    {
      UINT32 j;
      for (j = 0; j < i; j++) {
        pmm_free_page((void *)(UINTN)(first + (UINT64)j * 4096u));
      }
    }
  }
  return NULL;
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
  UINT32 i;

  if (t == NULL || t == g_current) {
    return;
  }
  freed = vm_space_free(&t->space);
  if (t->kstack_phys != 0) {
    for (i = 0; i < SCHED_KSTACK_PAGES; i++) {
      pmm_free_page((void *)(UINTN)(t->kstack_phys + (UINT64)i * 4096u));
      freed++;
    }
  }
  kputs("[REAP] ");
  kputs(t->name);
  kputs(": ");
  kputs_hex((UINT64)freed);
  kputs(" pages back, free=");
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
  task_t *t = (g_current != NULL) ? g_current->next : g_first;
  task_t *start = t;

  if (t == NULL) {
    return NULL;
  }
  do {
    if (t->state == TASK_READY) {
      return t;
    }
    t = (t->next != NULL) ? t->next : &g_tasks[0];
  } while (t != start);
  return NULL;
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
    return NULL;
  }
  kstack = (UINT8 *)alloc_pages_contig(SCHED_KSTACK_PAGES);
  if (kstack == NULL) {
    kputs("[SCHED] ERROR: no contiguous pages for a kernel stack\n");
    return NULL;
  }

  memset(t, 0, sizeof(*t));
  t->id = (int)g_next_id++;
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
    }
    if (!g_need_resched && g_current->state != TASK_EXITED) {
      return current_frame;
    }
  }
  g_need_resched = 0;

  next = task_pick_next();
  if (next == NULL) {
    if (g_current != NULL && g_current->state == TASK_EXITED) {
      /* Every task is gone. There is no frame left that could make
       * progress, so stop rather than iretq into a dead context. */
      kputs("[SCHED] no runnable task, halting\n");
      for (;;) {
        cpu_halt();
      }
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

void sched_tick(void) {
  g_ticks++;
  if (g_current == NULL) {
    return;
  }
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
}

void sched_exit_current(int code) {
  if (g_current == NULL) {
    return;
  }
  g_current->state = TASK_EXITED;
  g_current->exit_code = code;
  g_exit_count++;
  g_need_resched = 1;

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
