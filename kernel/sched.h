/**
 * Novus OS - tasks and the scheduler (M4).
 *
 * One task per NUC process. Each has its own page table (CR3), its own
 * ring-0 stack, and a saved trap frame. Switching tasks is not a separate
 * assembly routine: trap_dispatch() returns the frame to resume, and the
 * trap stub iretq's into it. That is why there is a task_t::rsp and no
 * task_t::context - the full register state lives in the frame on the
 * task's kernel stack.
 *
 * Every task runs in ring 3. A kernel thread would need a second kind of
 * frame (ring0->ring0 iret pops three words, not five); making the idle
 * task a user program that loops on the yield syscall keeps one frame
 * shape for all of them and one return path.
 *
 * Preemption: the LAPIC timer fires at IDT_TIMER_VECTOR, sched_tick()
 * counts down the slice, and once it expires the timer's own return path
 * switches to the next task. An interrupt arriving while the kernel is
 * already running is counted but never switches - interrupt gates clear
 * IF on entry, so the kernel never runs with a half-saved context.
 */

#ifndef NOVUS_SCHED_H
#define NOVUS_SCHED_H

#include "uefi.h"
#include "idt.h"
#include "vm.h"

#define SCHED_MAX_TASKS   10
#define SCHED_KSTACK_PAGES 16u /* 64 KiB ring-0 stack per task */
#define SCHED_SLICE_TICKS 4u   /* timer ticks per time slice */
#define SCHED_FOREGROUND_NONE (~0ULL)
#define TASK_FD_MAX 16u

#define TASK_FREE    0
#define TASK_READY   1
#define TASK_RUNNING 2
#define TASK_EXITED  3
#define TASK_BLOCKED 4 /* parked on an IPC port (M6) */
#define TASK_SUSPENDED 5 /* stopped by the task manager (M10) */

/* Which user ABI the task was started under. NUC images get the full
 * syscall table; Win32 images are held to SYS_WIN32 and SYS_EXIT by the
 * dispatcher, which is the prototype's stand-in for a separate CAP group. */
#define TASK_ABI_NUC   0
#define TASK_ABI_WIN32 1

typedef struct task {
  int      id;
  UINT64   pid;   /* equals id, but 64-bit like ipc_hdr_t::sender */
  int      state;
  char     name[16];
  UINT64   kstack_top; /* top of the ring-0 stack: TSS.RSP0 and the
                        * syscall entry stack */
  UINT64   kstack_phys; /* base of that stack's contiguous run, so the
                         * pages can be returned when the task is reaped */
  UINT64   rsp;        /* saved trap frame inside that stack */
  UINT64   cr3;
  vm_space_t space;    /* owned by the task; a copy of the loader's space,
                        * freed only after the task has been switched away */
  UINT32   ticks;
  UINT32   slice;
  UINT8    foreground;
  UINT32   started; /* tick when the task was created */
  int      exit_code;
  UINT64   wait_port;     /* IPC port a blocked task is parked on */
  UINT64   wait_deadline; /* tick at which the park expires, 0 = forever */
  int      wait_err;      /* error to hand back when the task resumes */
  UINT8    abi;           /* TASK_ABI_*: what the task is allowed to call */
  UINT32   win32_last_error; /* Win32 GetLastError state */
  UINT64   win32_image_base; /* PE image base, for GetModuleHandleA */
  struct {
    UINT32 ino;
    UINT32 dir_index;
    UINT64 offset;
    UINT8  used;
    UINT8  is_dir;
    UINT8  pad[6];
  } fd[TASK_FD_MAX];
  struct task *next;
} task_t;

/* GDT, TSS, IDT, the timer and the idle task. Interrupts stay off. */
void sched_init(UINT64 mmap, UINT64 size, UINT64 stride);

/* Build a ready task for a ring-3 entry point. The space is copied into
 * the task, which then owns it: on success the caller must not free it.
 * Returns the task, or NULL. The task's CR3 is installed on the first
 * switch to it. */
task_t *sched_create_user(const char *name, const vm_space_t *space,
                          UINT64 entry, UINT64 user_rsp);

/* Same, but records the user ABI the task runs under (TASK_ABI_*). The
 * Win32 loader uses this so the syscall dispatcher can hold the process to
 * its own call set; sched_create_user() is the NUC wrapper. */
task_t *sched_create_user_ex(const char *name, const vm_space_t *space,
                             UINT64 entry, UINT64 user_rsp, UINT8 abi);

/* Load the first task and drop into ring 3. Never returns. */
void sched_start(void) __attribute__((noreturn));

/* Called from trap_dispatch() with the frame the trap arrived on; returns
 * the frame to resume (the same one, or another task's). */
UINT64 sched_switch_frame(UINT64 current_frame);

/* Timer tick: charge the slice, ask for a switch when it runs out. */
void sched_tick(void);

/* Kernel WM only, with interrupts disabled; NONE selects the kernel shell. */
int sched_set_foreground(UINT64 pid);
int sched_selftest(void);

/* Mark the running task as finished. The switch happens on the way out of
 * the trap. */
void sched_exit_current(int code);

/* End a task by pid, from outside (the settings panel's "end task").
 * 0 = terminated, -1 = no such task, -2 = refused (idle/spin),
 * -3 = it was already exiting. Reaped by the next scheduler sweep. */
int sched_kill(UINT64 pid);

/* Give up the rest of the slice (SYS_YIELD). */
void sched_yield(void);

/* Park the running task on `port` until a message arrives (or the timeout
 * expires). The switch happens on the way out of the trap, so the caller
 * must also rewind the frame it wants re-executed. */
void sched_park(UINT64 port, UINT32 timeout_ticks);

/* Make every task parked on `port` runnable again. */
void sched_wake_port(UINT64 port);

/* Timer tick count, used for park deadlines. */
UINT64 sched_ticks(void);

task_t *sched_current(void);

/* Task by pid, or NULL. */
task_t *sched_find_pid(UINT64 pid);

/* Task by name among live tasks, or NULL. */
task_t *sched_find_name(const char *name);

/* Snapshot of one task slot, for the M8 task manager. Returns 0 on
 * success, -1 for a free or out-of-range slot. */
/* Rich task record for the task manager (M10): 32 bytes so three of them
 * plus the list header still fit in one 128-byte IPC payload. */
typedef struct {
  UINT64 pid;
  UINT32 cpu_ticks;
  UINT32 mem_pages;
  UINT32 started;
  UINT8  state;
  UINT8  prio;
  char   name[10];
} sched_info2_t;

typedef struct {
  UINT64 pid;
  UINT32 ticks;
  INT32  exit_code;
  UINT8  state;
  UINT8  slice;
  char   name[16];
  UINT8  pad[2];
} sched_info_t;

UINT32 sched_task_count(void);
int    sched_task_info(UINT32 idx, sched_info_t *out);
int    sched_task_info2(UINT32 idx, sched_info2_t *out);
UINT64 sched_total_ticks(void);

/* Suspend/resume for the task manager. 0 = done, -1 = no such task,
 * -2 = refused (idle/spin/current). */
int    sched_suspend(UINT64 pid);
int    sched_resume(UINT64 pid);

/* Count of tasks that have exited, for the boot log. */
UINT32 sched_exit_count(void);

#endif /* NOVUS_SCHED_H */
