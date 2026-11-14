/**
 * Novus OS - boot menu (M-boot).
 *
 * A small framebuffer menu shown just before the desktop would be painted. It
 * offers two ways into the system:
 *
 *   1. Novus 桌面        (default)  - the full desktop environment
 *   2. 仅命令行 (No Desktop)        - boot straight into the shell (nsh)
 *
 * A five-second countdown selects the default (desktop) when nobody presses a
 * key; Up/Down move the highlight and Enter (or Escape) confirms the current
 * selection. The keyboard arrives over USB HID, which is why the menu is run
 * after net_init() has enumerated the controller.
 *
 * The menu is drawn with the ordinary renderer (nd_fill_rect / nd_text), the
 * same primitives the boot splash and the desktop use, so there is no second
 * drawing path to keep in sync. It runs on the BSP before sched_start(), with
 * interrupts enabled just long enough for the timer (countdown) and the USB
 * keyboard IRQs to be delivered.
 */

#include "kernel.h"
#include "nd.h"
#include "nd_input.h"
#include "sched.h"
#include "msr.h"

#ifndef CONFIG_NOVUS_DISPLAY
/* Without a display the menu cannot be shown; fall straight through to the
 * desktop so the rest of boot is unchanged. */
void boot_menu_select(void) { g_boot_no_desktop = 0; }
void boot_menu_finish(void) {}
#else

int g_boot_no_desktop = 0;

/* The tick is calibrated to 10 ms (sched_init -> lapic_timer_calibrate), so
 * 500 ticks is a ~5 s countdown. Kept in ticks rather than milliseconds so
 * the countdown and the timeout share one clock and stay consistent. */
#define BOOT_MENU_TIMEOUT_TICKS 500u
#define BOOT_MENU_OPTION_COUNT 2u
/* Ignore key events for this many ticks (~200 ms) after the input IRQs are
 * enabled, so a synthetic/initial HID report cannot auto-confirm the menu
 * before the user has actually seen it. The 5 s countdown still runs. */
#define BOOT_MENU_INPUT_GRACE_TICKS 50u

/* Dark gradient backdrop, matching the boot splash palette. */
#define MENU_BG_TOP    0x0A0F17u
#define MENU_BG_BOTTOM 0x131C2Bu
#define MENU_CARD      0x1B2532u
#define MENU_LINE      0x33415Au
#define MENU_TEXT      0xE8EEF8u
#define MENU_DIM       0x9AA6BAu
#define MENU_ACCENT    0x4CC2FFu
#define MENU_SELECT    0x243043u

static const char *g_options[BOOT_MENU_OPTION_COUNT] = {
    "Novus 桌面",
    "仅命令行 (No Desktop)",
};
static const char *g_sub[BOOT_MENU_OPTION_COUNT] = {
    "完整的桌面环境与图形界面",
    "直接进入命令行 shell，不加载桌面",
};

/* Repaint the whole menu. `sel` is the highlighted option and `sec_left` the
 * countdown value still showing. Centred on the primary output. */
static void boot_menu_paint(nd_surface_t *s, UINT32 sel, UINT32 sec_left) {
  INT32 cx = s->w / 2;
  INT32 card_w = (s->w < 480) ? (s->w - 48) : 432;
  INT32 card_h = 232;
  INT32 card_x = cx - card_w / 2;
  INT32 card_y = (s->h - card_h) / 2;
  INT32 i;
  INT32  entry_h = 44;
  INT32  entry_y = card_y + 70;
  INT32  label_y = card_y + 14;
  INT32  foot_y = card_y + card_h - 30;

  if (card_y < 8) {
    card_y = 8;
  }

  /* Backdrop + card. */
  nd_gradient_v(s, nd_rect(0, 0, s->w, s->h), MENU_BG_TOP, MENU_BG_BOTTOM);
  nd_round_rect(s, nd_rect(card_x, card_y, card_w, card_h), 14, MENU_CARD);
  nd_round_frame(s, nd_rect(card_x, card_y, card_w, card_h), 14, 1, MENU_LINE);

  /* Title. */
  {
    const char *t = "Novus OS 启动菜单";
    INT32 tw = (INT32)nd_text_width(t, 2);
    nd_text(s, cx - tw / 2, label_y, t, MENU_TEXT, 2);
  }

  /* Two selectable entries. */
  for (i = 0; i < (INT32)BOOT_MENU_OPTION_COUNT; i++) {
    INT32 y = entry_y + i * entry_h;
    INT32 ex = card_x + 16;
    INT32 ew = card_w - 32;

    if ((UINT32)i == sel) {
      nd_round_rect(s, nd_rect(ex, y, ew, entry_h - 8), 8, MENU_SELECT);
      nd_round_frame(s, nd_rect(ex, y, ew, entry_h - 8), 8, 1, MENU_ACCENT);
    }
    {
      const char *name = g_options[i];
      nd_text(s, ex + 14, y + 7, name,
              ((UINT32)i == sel) ? MENU_ACCENT : MENU_TEXT, 1);
    }
    {
      const char *sub = g_sub[i];
      INT32 sw = (INT32)nd_text_width(sub, 1);
      if (sw > ew - 28) {
        sw = ew - 28;
      }
      nd_text_limit(s, ex + 14, y + 25, sub, (UINT32)sw, MENU_DIM, 1);
    }
  }

  /* Footer: navigation hint + countdown. */
  {
    const char *hint = "↑/↓ 选择    Enter 确认";
    INT32 hw = (INT32)nd_text_width(hint, 1);
    nd_text(s, cx - hw / 2, foot_y, hint, MENU_DIM, 1);
  }
  {
    char    count[3];
    UINT32  cn = 0u;
    char    line[28];
    UINT32  n = 0u;
    const char *pre = "无操作 ";
    const char *mid = " 秒后进入桌面";
    UINT32  m = 0u;

    if (sec_left < 10u) {
      count[cn++] = (char)('0' + (UINT8)sec_left);
    } else {
      count[cn++] = (char)('0' + (UINT8)(sec_left / 10u));
      count[cn++] = (char)('0' + (UINT8)(sec_left % 10u));
    }
    while (pre[n] != '\0') {
      line[n] = pre[n];
      n++;
    }
    for (m = 0u; m < cn; m++) {
      line[n++] = count[m];
    }
    m = 0u;
    while (mid[m] != '\0' && n < sizeof(line) - 1u) {
      line[n++] = mid[m++];
    }
    line[n] = '\0';
    {
      INT32 lw = (INT32)nd_text_width(line, 1);
      nd_text(s, cx - lw / 2, foot_y + 20, line, MENU_DIM, 1);
    }
  }
}

void boot_menu_select(void) {
  /* The menu runs on the BSP, in ring 0, before sched_start() has entered
   * ring 3. The LAPIC timer ISR is already armed (sched_init), and we enable
   * interrupts here so the keyboard IRQs (PS/2 line 1/12, USB HID) can be
   * delivered and decoded by nd_input_poll(). This is safe because
   * sched_tick() gates every subsystem heartbeat (WM / sysinfo / net / update
   * daemon) behind g_sched_started, which is still 0 while the menu is up; a
   * timer tick during the menu only advances the tick counter. A device IRQ
   * merely appends bytes to a ring and trap_dispatch returns the same kernel
   * frame, so no task state is touched and no context switch can happen
   * (no tasks exist yet). */
  nd_surface_t *s = nd_screen();
  UINT32 sel = 0u;
  UINT32 chosen = 0u;
  UINT32 start_tick = (UINT32)sched_ticks();
  UINT32 grace_until = start_tick + BOOT_MENU_INPUT_GRACE_TICKS;
  UINT32 last_poll_tick = start_tick;
  UINT32 last_paint_sec = (UINT32)-1;
  UINT32 last_paint_sel = (UINT32)-1;
  int    confirmed = 0;

  kputs("[BOOT] menu: entered\n");
  if (s == 0) {
    s = splash_surface();
    kputs("[BOOT] menu: nd_screen null, splash used\n");
  }
  g_boot_no_desktop = 0;
  if (s == 0) {
    kputs("[BOOT] menu: no surface, default desktop\n");
    return; /* no drawable output: keep the desktop default */
  }

  nd_input_init();
  kputs("[BOOT] menu: input armed, enabling interrupts\n");
  cpu_sti(); /* safe: see the comment above this function */

  /* Paint the initial frame immediately so the menu is visible at once. */
  boot_menu_paint(s, sel, BOOT_MENU_TIMEOUT_TICKS / 100u);
  last_paint_sec = BOOT_MENU_TIMEOUT_TICKS / 100u;
  last_paint_sel = sel;

  while (!confirmed) {
    UINT32 now = (UINT32)sched_ticks();
    UINT32 elapsed = (now > start_tick) ? (now - start_tick) : 0u;
    UINT32 sec_left;
    nd_event_t ev;

    if (elapsed >= BOOT_MENU_TIMEOUT_TICKS) {
      chosen = 0u; /* timeout: default to the desktop */
      confirmed = 1;
      break;
    }
    /* Round up to whole seconds so the last second still shows "1". */
    sec_left = ((BOOT_MENU_TIMEOUT_TICKS - elapsed) + 99u) / 100u;

    /* Decode the keyboard ring at most once per tick: 100 Hz polling is far
     * more than enough for menu navigation and keeps the IRQ ring quiet. */
    if (now != last_poll_tick) {
      nd_input_poll();
      while (nd_input_pop(&ev)) {
        if (ev.type != ND_EV_KEY) {
          continue;
        }
        if (now < grace_until) {
          continue; /* ignore the initial synthetic HID report */
        }
        if (ev.key == ND_KEY_UP) {
          sel = (sel + BOOT_MENU_OPTION_COUNT - 1u) % BOOT_MENU_OPTION_COUNT;
        } else if (ev.key == ND_KEY_DOWN) {
          sel = (sel + 1u) % BOOT_MENU_OPTION_COUNT;
        } else if (ev.key == ND_KEY_ENTER || ev.key == ND_KEY_ESC) {
          chosen = sel;
          confirmed = 1;
        }
      }
      last_poll_tick = now;
    }

    /* Repaint only when the visible state actually changes. */
    if (sec_left != last_paint_sec || sel != last_paint_sel) {
      boot_menu_paint(s, sel, sec_left);
      last_paint_sec = sec_left;
      last_paint_sel = sel;
    }
  }

  g_boot_no_desktop = (chosen == 1u) ? 1 : 0;
  kputs("[BOOT] menu: selection=");
  kputs_hex(chosen);
  kputs(" no_desktop=");
  kputs_hex((UINT64)g_boot_no_desktop);
  kputs("\n");
}

void boot_menu_finish(void) {
  /* Clear the visible framebuffer (boot splash surface) before ending the
   * splash, so the bare shell starts on a clean, neutral screen. */
  nd_surface_t *s = nd_screen();
  if (s == 0) {
    s = splash_surface();
  }
  if (s != 0) {
    nd_clear(s, MENU_BG_TOP);
  }
  nd_boot_splash_end();
}

#endif /* CONFIG_NOVUS_DISPLAY */
