/**
 * Novus OS - kernel stop screen.
 *
 * This module is deliberately self-contained. It owns one static snapshot,
 * draws straight into the display surface and presents one full frame. It
 * never allocates, never takes an IPC path and never waits on a scheduler
 * primitive, so it is safe to call from a fault handler with interrupts
 * already disabled.
 */

#include "bsod.h"
#include "kernel.h"
#include "sched.h"
#include "klog.h"
#include "irq.h"
#include "nd.h"
#include "cpu.h"
#include <stddef.h>

#define BSOD_LOG_CAP        6144u
#define BSOD_LOG_LINES      24u
#define BSOD_SEQ_TIMEOUT    300u  /* ticks (~3 s at the 10 ms timer tick) */
#define BSOD_SEQ_TABS       5u
#define BSOD_SEQ_SHIFTS     2u

#define BSOD_C_BG_TOP   ND_RGB(0, 0, 170)
#define BSOD_C_BG_BOT   ND_RGB(0, 0, 112)
#define BSOD_C_BAR      ND_RGB(0, 0, 80)
#define BSOD_C_TEXT     ND_RGB(255, 255, 255)
#define BSOD_C_DIM      ND_RGB(192, 192, 220)
#define BSOD_C_LINE     ND_RGB(96, 96, 190)
#define BSOD_C_ACCENT   ND_RGB(79, 195, 247)
#define BSOD_C_WARN     ND_RGB(255, 107, 107)

typedef struct {
  UINT32 code;
  UINT32 vector;
  UINT64 errcode;
  UINT64 cr2;
  UINT64 rip, cs, rflags, rsp, ss;
  UINT64 rax, rbx, rcx, rdx, rsi, rdi, rbp;
  UINT64 r8, r9, r10, r11, r12, r13, r14, r15;
  UINT64 pid;
  char task[16];
  char short_name[48];
  char desc[2][96];
  char log[BSOD_LOG_CAP];
} bsod_snapshot_t;

static bsod_snapshot_t g_snap;
static volatile int g_active;
static volatile UINT32 g_ticks;
static volatile UINT8 g_mods;
static volatile UINT8 g_seq_tabs;
static volatile UINT8 g_seq_shifts;
static volatile UINT32 g_seq_start;

/* ------------------------------------------------------------------ */
/* Small formatters: no libc printf in the freestanding kernel.         */
/* ------------------------------------------------------------------ */

static UINT32 bsod_len(const char *s) {
  UINT32 n = 0u;
  while (s[n] != '\0') n++;
  return n;
}

static void bsod_copy(char *dst, const char *src) {
  UINT32 i = 0u;
  while (src[i] != '\0') {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

static void bsod_hex64(char *out, UINT64 v) {
  static const char hx[] = "0123456789ABCDEF";
  UINT32 i;

  out[0] = '0';
  out[1] = 'x';
  for (i = 0u; i < 16u; i++) {
    out[2u + i] = hx[(UINT32)((v >> (60u - 4u * i)) & 0xFu)];
  }
  out[18] = '\0';
}

static void bsod_hex32(char *out, UINT32 v) {
  static const char hx[] = "0123456789ABCDEF";
  UINT32 i;

  out[0] = '0';
  out[1] = 'x';
  for (i = 0u; i < 8u; i++) {
    out[2u + i] = hx[(UINT32)((v >> (28u - 4u * i)) & 0xFu)];
  }
  out[10] = '\0';
}

static void bsod_dec64(char *out, UINT64 v) {
  char tmp[21];
  UINT32 n = 0u;
  UINT32 i;

  if (v == 0u) {
    out[0] = '0';
    out[1] = '\0';
    return;
  }
  while (v != 0u && n < sizeof(tmp)) {
    tmp[n++] = (char)('0' + (UINT32)(v % 10u));
    v /= 10u;
  }
  for (i = 0u; i < n; i++) {
    out[i] = tmp[n - 1u - i];
  }
  out[n] = '\0';
}

static void bsod_label_hex(char *out, const char *label, UINT64 v) {
  bsod_copy(out, label);
  bsod_copy(out + bsod_len(out), " = ");
  bsod_hex64(out + bsod_len(out), v);
}

/* ------------------------------------------------------------------ */
/* Stop-code mapping. Keep DEBUG.md in sync with these six entries.    */
/* ------------------------------------------------------------------ */

static UINT32 bsod_code_for_vector(UINT32 vector) {
  switch (vector) {
  case 8u:  /* #DF  */
  case 12u: /* #SS  */
  case 17u: /* #AC  */
  case 19u: /* #XM  */
    return BSOD_BC_IRQL;
  case 14u: /* #PF  */
    return BSOD_BC_PAGE_FAULT;
  case 18u: /* #MC  */
    return BSOD_BC_WHEA;
  default:
    return BSOD_BC_KMODE_EXCEPTION;
  }
}

static void bsod_set_names(bsod_snapshot_t *s) {
  switch (s->code) {
  case BSOD_BC_PAGE_FAULT:
    bsod_copy(s->short_name, "PAGE_FAULT_IN_NONPAGED_AREA");
    bsod_copy(s->desc[0], "A thread referenced memory the kernel cannot map.");
    bsod_copy(s->desc[1], "CR2 is the faulting address; RIP is the instruction.");
    break;
  case BSOD_BC_IRQL:
    bsod_copy(s->short_name, "IRQL_NOT_LESS_OR_EQUAL");
    bsod_copy(s->desc[0], "A fault or invalid access occurred at elevated IRQL.");
    bsod_copy(s->desc[1], "Check the vector, error code and the caller in RIP.");
    break;
  case BSOD_BC_WHEA:
    bsod_copy(s->short_name, "WHEA_UNCORRECTABLE_ERROR");
    bsod_copy(s->desc[0], "The CPU reported a machine-check exception.");
    bsod_copy(s->desc[1], "Hardware, microcode or a corrupt page table may be at fault.");
    break;
  case BSOD_BC_BAD_POOL:
    bsod_copy(s->short_name, "BAD_POOL_CALLER");
    bsod_copy(s->desc[0], "A kernel component panicked or exhausted its memory pool.");
    bsod_copy(s->desc[1], "The serial log above the screen has the original message.");
    break;
  case BSOD_BC_MANUAL:
    bsod_copy(s->short_name, "MANUAL_STOP");
    bsod_copy(s->desc[0], "The stop screen was triggered from the keyboard.");
    bsod_copy(s->desc[1], "Press Ctrl+Alt+R to restart the machine.");
    break;
  default:
    bsod_copy(s->short_name, "KMODE_EXCEPTION_NOT_HANDLED");
    bsod_copy(s->desc[0], "An unhandled exception was raised in kernel mode.");
    bsod_copy(s->desc[1], "Use the vector, RIP and log tail below to locate the fault.");
    break;
  }
}

/* ------------------------------------------------------------------ */
/* Snapshot and drawing                                                */
/* ------------------------------------------------------------------ */

static void bsod_init_snapshot(UINT32 code, bsod_snapshot_t *s) {
  task_t *t;
  UINT32 i;

  memset(s, 0, sizeof(*s));
  s->code = code;
  bsod_set_names(s);

  t = sched_current();
  if (t != 0) {
    s->pid = t->pid;
    for (i = 0u; i + 1u < sizeof(s->task) && t->name[i] != '\0'; i++) {
      s->task[i] = t->name[i];
    }
  }
  (void)klog_snapshot(s->log, (UINT32)sizeof(s->log));
}

/* Split the log tail in place into at most BSOD_LOG_LINES pointers. */
static UINT32 bsod_split_log(bsod_snapshot_t *s, const char **lines) {
  UINT32 n = bsod_len(s->log);
  UINT32 start = 0u;
  UINT32 count = 0u;
  UINT32 i;

  for (i = 0u; i < n; i++) {
    if (s->log[i] == '\n') {
      s->log[i] = '\0';
      if (count < BSOD_LOG_LINES) {
        lines[count++] = &s->log[start];
      } else {
        UINT32 k;
        for (k = 0u; k + 1u < BSOD_LOG_LINES; k++) {
          lines[k] = lines[k + 1u];
        }
        lines[BSOD_LOG_LINES - 1u] = &s->log[start];
      }
      start = i + 1u;
    }
  }
  if (start < n && s->log[start] != '\0') {
    if (count < BSOD_LOG_LINES) {
      lines[count++] = &s->log[start];
    } else {
      UINT32 k;
      for (k = 0u; k + 1u < BSOD_LOG_LINES; k++) {
        lines[k] = lines[k + 1u];
      }
      lines[BSOD_LOG_LINES - 1u] = &s->log[start];
    }
  }
  return count;
}

static void bsod_draw(nd_surface_t *s, bsod_snapshot_t *snap) {
  static const char *const reg_names[15] = {
      "RAX", "RBX", "RCX", "RDX", "RSI", "RDI", "RBP", "R8",
      "R9",  "R10", "R11", "R12", "R13", "R14", "R15"};
  const UINT64 regs[15] = {
      snap->rax, snap->rbx, snap->rcx, snap->rdx, snap->rsi,
      snap->rdi, snap->rbp, snap->r8,  snap->r9,  snap->r10,
      snap->r11, snap->r12, snap->r13, snap->r14, snap->r15};
  const char *lines[BSOD_LOG_LINES];
  char buf[64];
  INT32 W = s->w;
  INT32 H = s->h;
  INT32 y;
  INT32 info_y;
  INT32 footer_y;
  UINT32 count;
  UINT32 i;
  UINT32 shown;

  nd_gradient_v(s, nd_rect(0, 0, W, H), BSOD_C_BG_TOP, BSOD_C_BG_BOT);

  /* Header band: sad face, product name and a compact stop-code chip. */
  nd_fill_rect(s, nd_rect(0, 0, W, 64), BSOD_C_BAR);
  nd_frame(s, nd_rect(16, 14, 52, 36), 2, BSOD_C_TEXT);
  nd_text(s, 26, 18, ":(", BSOD_C_TEXT, 2u);
  nd_text(s, 84, 14, "Novus OS", BSOD_C_TEXT, 2u);
  nd_text(s, 84, 40, "STOP / KERNEL FAULT", BSOD_C_WARN, 1u);
  bsod_hex32(buf, snap->code);
  if (W > 420) {
    nd_text(s, W - (INT32)nd_text_width(buf, 2u) - 20, 22, buf,
            BSOD_C_TEXT, 2u);
  }
  nd_hline(s, 0, W - 1, 64, BSOD_C_LINE);

  y = 84;
  nd_text(s, 24, y,
          "A problem has been detected and Novus OS has been stopped.",
          BSOD_C_TEXT, 1u);
  y += 23;
  bsod_hex32(buf, snap->code);
  nd_text(s, 24, y, buf, BSOD_C_TEXT, 2u);
  if (W > 520) {
    nd_text(s, 190, y + 4, snap->short_name, BSOD_C_TEXT, 2u);
  } else {
    nd_text(s, 24, y + 22, snap->short_name, BSOD_C_TEXT, 1u);
    y += 20;
  }
  y += 36;
  nd_text(s, 24, y, snap->desc[0], BSOD_C_DIM, 1u);
  y += 18;
  nd_text(s, 24, y, snap->desc[1], BSOD_C_DIM, 1u);
  y += 27;

  if (y + 4 >= H - 60) {
    footer_y = H - 40;
    nd_fill_rect(s, nd_rect(0, footer_y, W, 40), BSOD_C_BAR);
    nd_hline(s, 0, W - 1, footer_y, BSOD_C_LINE);
    nd_text(s, 24, footer_y + 12,
            "Press Ctrl+Alt+R to restart / 按 Ctrl+Alt+R 重启",
            BSOD_C_DIM, 1u);
    return;
  }
  nd_hline(s, 24, W - 24, y, BSOD_C_LINE);
  y += 14;
  info_y = y;

  /* Left column: fault context and task identity. */
  bsod_label_hex(buf, "VECTOR", snap->vector);
  nd_text(s, 24, y, buf, BSOD_C_TEXT, 1u);
  y += 18;
  bsod_label_hex(buf, "ERROR_CODE", snap->errcode);
  nd_text(s, 24, y, buf, BSOD_C_TEXT, 1u);
  y += 18;
  bsod_label_hex(buf, "CR2", snap->cr2);
  nd_text(s, 24, y, buf, BSOD_C_TEXT, 1u);
  y += 18;
  bsod_label_hex(buf, "RIP", snap->rip);
  nd_text(s, 24, y, buf, BSOD_C_ACCENT, 1u);
  y += 18;
  bsod_label_hex(buf, "CS", snap->cs);
  nd_text(s, 24, y, buf, BSOD_C_TEXT, 1u);
  y += 18;
  bsod_label_hex(buf, "RSP", snap->rsp);
  nd_text(s, 24, y, buf, BSOD_C_TEXT, 1u);
  y += 18;
  bsod_label_hex(buf, "RFLAGS", snap->rflags);
  nd_text(s, 24, y, buf, BSOD_C_TEXT, 1u);
  y += 18;
  bsod_copy(buf, "PID = ");
  bsod_dec64(buf + bsod_len(buf), snap->pid);
  nd_text(s, 24, y, buf, BSOD_C_TEXT, 1u);
  y += 18;
  bsod_copy(buf, "TASK = ");
  bsod_copy(buf + bsod_len(buf), snap->task[0] != '\0' ? snap->task : "kernel");
  nd_text(s, 24, y, buf, BSOD_C_TEXT, 1u);

  /* Right column: general-purpose register dump in two sub-columns. */
  if (W > 700) {
    INT32 col2 = W / 2 + 12;

    for (i = 0u; i < 15u; i++) {
      bsod_label_hex(buf, reg_names[i], regs[i]);
      nd_text(s, col2 + (i >= 8u ? 190 : 0),
              info_y + (INT32)(i % 8u) * 18, buf, BSOD_C_TEXT, 1u);
    }
  }
  y = info_y + 9 * 18 + 10;

  /* Log panel: newest lines, clipped per line and capped by screen height. */
  if (y + 40 < H - 60) {
    nd_hline(s, 24, W - 24, y, BSOD_C_LINE);
    y += 12;
    nd_text(s, 24, y, "KERNEL LOG (tail)", BSOD_C_ACCENT, 1u);
    y += 20;
    count = bsod_split_log(snap, lines);
    footer_y = H - 52;
    shown = 0u;
    while (shown < count && y + 14 < footer_y) {
      const char *ln = lines[count - 1u - shown];

      nd_text_limit(s, 24, y, ln, (UINT32)(W - 48), BSOD_C_DIM, 1u);
      y += 16;
      shown++;
    }
  }

  footer_y = H - 40;
  nd_fill_rect(s, nd_rect(0, footer_y, W, 40), BSOD_C_BAR);
  nd_hline(s, 0, W - 1, footer_y, BSOD_C_LINE);
  nd_text(s, 24, footer_y + 12,
          "Press Ctrl+Alt+R to restart / 按 Ctrl+Alt+R 重启",
          BSOD_C_DIM, 1u);
}

/* ------------------------------------------------------------------ */
/* Reset and key handling                                              */
/* ------------------------------------------------------------------ */

/* In the stop screen interrupts stay off, so the regular reboot path's
 * disk standby would be unsafe. Use the legacy reset registers directly. */
static void bsod_hard_reset(void) {
  io_out8(0xCF9u, 0x06u);
  io_wait();
  io_out8(0x64u, 0xFEu);
  for (;;) {
    __asm__ volatile("pause");
  }
}

static void bsod_request_reset(void) {
  /* The hotkey fires from the timer interrupt (IF=0). The regular reboot
   * path starts with disk standby, which is unsafe there, so use the same
   * direct reset from both normal and stop-screen mode. */
  bsod_hard_reset();
}

static void bsod_seq_reset(void) {
  g_seq_tabs = 0u;
  g_seq_shifts = 0u;
  g_seq_start = g_ticks;
}

static void bsod_seq_key(UINT8 sc, UINT8 mods) {
  (void)mods;

  if (g_seq_tabs != 0u &&
      (g_ticks - g_seq_start) > BSOD_SEQ_TIMEOUT) {
    bsod_seq_reset();
  }

  if (sc == BSOD_SCAN_TAB) {
    if (g_seq_tabs < BSOD_SEQ_TABS) {
      if (g_seq_tabs == 0u) {
        g_seq_start = g_ticks;
      }
      g_seq_tabs++;
    } else {
      g_seq_tabs = 1u;
      g_seq_start = g_ticks;
    }
    g_seq_shifts = 0u;
    return;
  }
  if (sc == BSOD_SCAN_SHIFT_L || sc == BSOD_SCAN_SHIFT_R) {
    if (g_seq_tabs == BSOD_SEQ_TABS) {
      g_seq_shifts++;
      if (g_seq_shifts >= BSOD_SEQ_SHIFTS) {
        bsod_seq_reset();
        bsod_manual(BSOD_BC_MANUAL);
      }
    } else {
      bsod_seq_reset();
    }
    return;
  }
  bsod_seq_reset();
}

static void bsod_key_common(UINT8 sc, int down, UINT8 mods) {
  g_mods = mods;
  if (down == 0) {
    return;
  }
  if (sc == BSOD_SCAN_R &&
      (mods & (BSOD_MOD_CTRL | BSOD_MOD_ALT)) ==
          (BSOD_MOD_CTRL | BSOD_MOD_ALT)) {
    bsod_request_reset();
    return;
  }
  bsod_seq_key(sc, mods);
}

void bsod_key_hook(UINT8 sc, int down) {
  UINT8 mods = (UINT8)g_mods;

  if (sc == BSOD_SCAN_SHIFT_L || sc == BSOD_SCAN_SHIFT_R) {
    mods = down ? (UINT8)(mods | BSOD_MOD_SHIFT)
                : (UINT8)(mods & (UINT8)~BSOD_MOD_SHIFT);
  } else if (sc == BSOD_SCAN_CTRL) {
    mods = down ? (UINT8)(mods | BSOD_MOD_CTRL)
                : (UINT8)(mods & (UINT8)~BSOD_MOD_CTRL);
  } else if (sc == BSOD_SCAN_ALT) {
    mods = down ? (UINT8)(mods | BSOD_MOD_ALT)
                : (UINT8)(mods & (UINT8)~BSOD_MOD_ALT);
  }
  bsod_key_common(sc, down, mods);
}

void bsod_key_hook_mods(UINT8 sc, int down, UINT8 mods) {
  bsod_key_common(sc, down, mods);
}

void bsod_hotkey_tick(void) {
  g_ticks++;
}

int bsod_active(void) {
  return g_active;
}

/* Poll 8042 directly. Interrupts are already off and stay off, so no other
 * driver can consume the reset keystrokes before this loop sees them. */
static void bsod_wait_reboot(void) __attribute__((noreturn));

static void bsod_wait_reboot(void) {
  for (;;) {
    UINT8 st = io_in8(0x64u);

    if ((st & 1u) != 0u) {
      UINT8 b;

      /* Bit 5 marks a mouse byte: IRQs are off, so stale AUX data would
       * otherwise be decoded as keyboard scancodes. */
      if ((st & 0x20u) != 0u) {
        (void)io_in8(0x60u);
        continue;
      }
      b = io_in8(0x60u);
      if (b == 0xE0u || b == 0xE1u) {
        continue;
      }
      if ((b & 0x80u) != 0u) {
        bsod_key_hook((UINT8)(b & 0x7Fu), 0);
      } else {
        bsod_key_hook(b, 1);
      }
    }
    __asm__ volatile("pause");
  }
}

/* ------------------------------------------------------------------ */
/* Entry points                                                        */
/* ------------------------------------------------------------------ */

static void bsod_present(bsod_snapshot_t *s) __attribute__((noreturn));

static void bsod_present(bsod_snapshot_t *s) {
  nd_surface_t *surf;
  UINT64 flags;

  g_active = 1;
  flags = irq_save();
  (void)flags; /* deliberately never restored: the stop screen is terminal */

  kputs("[BSOD] *** STOP: ");
  kputs_hex((UINT64)s->code);
  kputs(" ");
  kputs(s->short_name);
  kputs(" ***\n");

  surf = nd_canvas();
  if (surf != 0 && surf->px != 0 && surf->w > 320 && surf->h > 240) {
    nd_cursor_hide();
    bsod_draw(surf, s);
    nd_present(nd_rect(0, 0, surf->w, surf->h));
  } else {
    kputs("[BSOD] framebuffer unavailable; serial log only\n");
  }
  bsod_wait_reboot();
}

void bsod_fault(const trap_frame_t *f) {
  bsod_snapshot_t *s = &g_snap;
  UINT64 cr2;

  if (g_active != 0) {
    bsod_wait_reboot();
  }
  bsod_init_snapshot(bsod_code_for_vector((UINT32)f->vector), s);
  s->vector = (UINT32)f->vector;
  s->errcode = f->errcode;
  s->rip = f->rip;
  s->cs = f->cs;
  s->rflags = f->rflags;
  s->rsp = f->rsp;
  s->ss = f->ss;
  s->rax = f->rax;
  s->rbx = f->rbx;
  s->rcx = f->rcx;
  s->rdx = f->rdx;
  s->rsi = f->rsi;
  s->rdi = f->rdi;
  s->rbp = f->rbp;
  s->r8 = f->r8;
  s->r9 = f->r9;
  s->r10 = f->r10;
  s->r11 = f->r11;
  s->r12 = f->r12;
  s->r13 = f->r13;
  s->r14 = f->r14;
  s->r15 = f->r15;
  __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
  s->cr2 = cr2;
  bsod_present(s);
}

void bsod_manual(UINT32 code) {
  bsod_snapshot_t *s = &g_snap;

  if (g_active != 0) {
    bsod_wait_reboot();
  }
  bsod_init_snapshot(code, s);
  bsod_present(s);
}
