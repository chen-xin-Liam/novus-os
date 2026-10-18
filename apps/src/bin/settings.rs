//! Novus OS - settings (M9).
//!
//! A Windows 11 style settings window: a narrow navigation rail on the left
//! (系统 / 个性化 / 应用 / 关于) and card content on the right. Everything
//! here is real: the accent and the wallpaper are kernel theme values (the
//! WM and the shell repaint from them), the running-application list comes
//! from the scheduler through sysinfo, and "结束任务" calls a real
//! scheduler kill.
//!
//! Nothing is persisted - the filesystem is read-only - so a reboot brings
//! back the defaults. The About page says so rather than pretending.

#![no_std]
#![no_main]

use novus_apps::{ipc, sinfo, sys, ui, wm};

const CW: i16 = 720;
const CH: i16 = 470;

const NAV_W: i16 = 168;
const CONTENT_X: i16 = 188;
const CONTENT_W: i16 = 512;

const CARD: u32 = 0x00FAFAFC;
const CARD_TITLE: u32 = 0x00242428;
const DANGER: u32 = 0x00C42B1C;

/* Eight accents: the four from M8 plus four more so the grid is 2x4 like
 * the Windows colour picker. */
const PRESETS: [u32; 8] = [
    0x000067C0, 0x00107C10, 0x00B45309, 0x007C3AED,
    0x00C42B1C, 0x000E7490, 0x00BE185D, 0x004B5563,
];
const PRESET_NAMES: [&str; 8] = ["蓝", "绿", "琥珀", "紫", "红", "青", "玫瑰", "石墨"];

/* Wallpaper presets, index i must match nwm.c g_papers[]. */
const PAPERS: [(u32, u32); 3] = [
    (0x00EEF2F8, 0x00E3EAF3),
    (0x00F2F2F2, 0x00E2E2E2),
    (0x00EAF4F0, 0x00DCEDE6),
];
const PAPER_NAMES: [&str; 3] = ["云母蓝", "晨雾灰", "青瓷"];

const APPS: [&str; 3] = ["fileman", "taskman", "settings"];
const APP_NAMES: [&str; 3] = ["文件管理器", "任务管理器", "设置"];

const MAX_ROWS: usize = 8;

#[derive(Clone, Copy, PartialEq)]
enum Page {
    System,
    Look,
    Apps,
    About,
}

const PAGES: [Page; 4] = [Page::System, Page::Look, Page::Apps, Page::About];
const PAGE_NAMES: [&str; 4] = ["系统", "个性化", "应用", "关于"];
const PAGE_SUB: [&str; 4] = [
    "设备信息与实时资源",
    "颜色与应用外观",
    "内置应用的运行状态",
    "版本与实现说明",
];

struct State {
    page: Page,
    accent: u32,
    wallpaper: u32,
    st: sinfo::Stats,
    tasks: [sinfo::TaskRec; MAX_ROWS],
    n_tasks: usize,
    hover: (i16, i16),
    confirm_pid: u64,
    confirm_left: u32,
    note: &'static str,
}

fn state_name(s: u8) -> &'static str {
    match s {
        1 => "就绪",
        2 => "运行",
        3 => "退出",
        4 => "等待",
        _ => "空闲",
    }
}


/* ---- drawing primitives ------------------------------------------------ */

fn card(w: &wm::Win, x: i16, y: i16, cw: i16, ch: i16, title: &str) {
    w.fill(x, y, cw, ch, CARD);
    w.frame(x, y, cw, ch, ui::LINE);
    w.text(x + 14, y + 12, CARD_TITLE, title);
}

/// One "label ... value" line inside a card.
fn kv(w: &wm::Win, x: i16, y: i16, key: &str, value: &str) {
    w.text(x, y, ui::DIM, key);
    w.text(x + 96, y, ui::TEXT, value);
}

/// A 1 px outlined bar with a filled fraction, used for memory.
fn meter(w: &wm::Win, x: i16, y: i16, mw: i16, pct: i16, color: u32) {
    w.fill(x, y, mw, 10, ui::LINE);
    w.fill(x, y, mw * pct / 100, 10, color);
}

fn button(w: &wm::Win, x: i16, y: i16, bw: i16, bh: i16, label: &str, accent: u32,
          dim: bool, danger: bool) {
    let fg = if dim { ui::DIM } else { ui::TEXT };
    let edge = if danger { DANGER } else if dim { ui::LINE } else { accent };

    w.fill(x, y, bw, bh, if dim { ui::PANEL } else { ui::ROW_SEL });
    w.frame(x, y, bw, bh, edge);
    w.text(x + 8, y + (bh - 14) / 2, fg, label);
}

fn nav(w: &wm::Win, s: &State) {
    w.fill(0, 0, NAV_W, CH, ui::PANEL);
    w.shape(wm::OP_VLINE, NAV_W, 0, CH, 1, ui::LINE);
    w.text_big(20, 18, ui::TEXT, "设置");

    for (i, p) in PAGES.iter().enumerate() {
        let y = 62 + i as i16 * 42;
        let sel = *p == s.page;
        let hot = s.hover.0 < NAV_W && s.hover.1 >= y && s.hover.1 < y + 40;

        if sel {
            w.fill(8, y, NAV_W - 16, 40, ui::ROW_SEL);
            w.fill(8, y + 9, 3, 22, s.accent);
        } else if hot {
            w.fill(8, y, NAV_W - 16, 40, 0x00EFEFF3);
        }
        w.fill(28, y + 16, 8, 8, if sel { s.accent } else { ui::DIM });
        w.text(46, y + 12, if sel { ui::TEXT } else { ui::DIM }, PAGE_NAMES[i]);
    }
}

fn page_system(w: &wm::Win, s: &State) {
    card(w, CONTENT_X, 80, CONTENT_W, 150, "设备");
    kv(w, CONTENT_X + 14, 116, "名称", "Novus 虚拟机");
    kv(w, CONTENT_X + 14, 142, "架构", "x86_64 · 64 位");
    kv(w, CONTENT_X + 14, 168, "显示", "1280 × 800 · 32 位色");
    kv(w, CONTENT_X + 14, 194, "输入", "PS/2 键盘与鼠标");

    card(w, CONTENT_X, 244, CONTENT_W, 176, "资源");

    let mut b = [0u8; 16];
    let secs = (s.st.uptime / 100) as u32;
    let mut line = ui::Buf::new();

    line.num(secs / 60).s(" 分 ").num(secs % 60).s(" 秒");
    kv(w, CONTENT_X + 14, 280, "运行时间", line.str());

    /* Memory: used = total - free, as a bar plus exact numbers. */
    let total_mb = (s.st.total_kb / 1024) as u32;
    let free_mb = (s.st.free_kb / 1024) as u32;
    let used_mb = total_mb.saturating_sub(free_mb);
    let pct = if total_mb == 0 { 0 } else { (used_mb * 100 / total_mb) as i16 };
    w.text(CONTENT_X + 14, 306, ui::DIM, "内存");
    meter(w, CONTENT_X + 110, 306, 240, pct, s.accent);
    let mut mem = ui::Buf::new();

    mem.num(used_mb).s(" / ").num(total_mb).s(" MB");
    w.text(CONTENT_X + 360, 306, ui::TEXT, mem.str());

    let t = ui::fmt_u32(s.st.tasks, &mut b);
    kv(w, CONTENT_X + 14, 330, "任务数", t);
    let t = ui::fmt_u32(s.st.windows, &mut b);
    kv(w, CONTENT_X + 14, 352, "窗口数", t);

    let mut ipc_line = ui::Buf::new();
    ipc_line.num(s.st.ipc_sent).s(" 发出 / ").num(s.st.ipc_drop).s(" 丢弃");
    kv(w, CONTENT_X + 14, 374, "IPC", ipc_line.str());

    kv(w, CONTENT_X + 14, 398, "强调色", PRESET_NAMES[preset_index(s.accent)]);
}

fn preset_index(accent: u32) -> usize {
    let mut i = 0;

    while i < PRESETS.len() {
        if PRESETS[i] == accent {
            return i;
        }
        i += 1;
    }
    0
}

fn page_look(w: &wm::Win, s: &State) {
    card(w, CONTENT_X, 80, CONTENT_W, 120, "颜色");
    for (i, c) in PRESETS.iter().enumerate() {
        let x = CONTENT_X + 14 + (i % 4) as i16 * 62;
        let y = 112 + (i / 4) as i16 * 40;

        w.fill(x, y, 44, 28, *c);
        if *c == s.accent {
            w.frame(x - 2, y - 2, 48, 32, ui::TEXT);
        }
    }
    let idx = preset_index(s.accent);
    w.text(CONTENT_X + 14 + 4 * 62 + 6, 118, ui::TEXT, PRESET_NAMES[idx]);

    card(w, CONTENT_X, 216, CONTENT_W, 132, "桌面背景");
    for (i, p) in PAPERS.iter().enumerate() {
        let x = CONTENT_X + 14 + i as i16 * 164;
        let y = 248;

        w.fill(x, y, 150, 44, p.0);
        w.fill(x, y + 22, 150, 22, p.1);
        w.frame(x, y, 150, 44, if i as u32 == s.wallpaper { s.accent } else { ui::LINE });
        w.text(x + 6, y + 50, ui::DIM, PAPER_NAMES[i]);
    }

    /* Live preview of the current accent on a miniature window. */
    let (top, bottom) = PAPERS[(s.wallpaper as usize).min(PAPERS.len() - 1)];
    let x = CONTENT_X + 14;
    let y = 360;

    w.fill(x, y, 220, 76, top);
    w.fill(x, y + 38, 220, 38, bottom);
    w.fill(x + 24, y + 14, 172, 52, ui::BG);
    w.fill(x + 24, y + 14, 172, 16, ui::PANEL);
    w.fill(x + 24, y + 14, 60, 2, s.accent);
    w.text(x + 32, y + 17, ui::TEXT, "预览");
    w.fill(x + 32, y + 38, 90, 4, ui::LINE);
    w.fill(x + 32, y + 48, 120, 4, ui::LINE);
    w.fill(x + 160, y + 40, 26, 14, s.accent);
}

/// The running task for built-in application `i`, if it is up. The list is
/// ordered by application, not by scheduler slot: a relaunched app must not
/// jump to the top just because it got a lower slot.
fn running_for<'a>(s: &'a State, i: usize) -> Option<&'a sinfo::TaskRec> {
    s.tasks
        .iter()
        .take(s.n_tasks)
        .find(|t| t.name() == APPS[i] && t.state != 3)
}

fn page_apps(w: &wm::Win, s: &State) {
    card(w, CONTENT_X, 80, CONTENT_W, 190, "正在运行");

    let mut shown = 0usize;
    for i in 0..APPS.len() {
        let Some(t) = running_for(s, i) else {
            continue;
        };
        let y = 112 + shown as i16 * 36;
        let confirming = s.confirm_pid != 0 && s.confirm_pid == t.pid;
        let mut b = [0u8; 12];
        let pid = ui::fmt_u32(t.pid as u32, &mut b);

        w.text(CONTENT_X + 14, y + 6, ui::TEXT, APP_NAMES[i]);
        w.text(CONTENT_X + 140, y + 6, ui::DIM, pid);
        w.text(CONTENT_X + 220, y + 6, ui::DIM, state_name(t.state));
        button(w, CONTENT_X + CONTENT_W - 84, y, 70, 24,
               if confirming { "确认结束" } else { "结束" }, s.accent, false,
               confirming);
        shown += 1;
        if shown == 3 {
            break;
        }
    }
    if shown == 0 {
        w.text(CONTENT_X + 14, 116, ui::DIM, "没有正在运行的内置应用");
    }

    card(w, CONTENT_X, 286, CONTENT_W, 96, "可启动");
    for i in 0..APPS.len() {
        let running = s
            .tasks
            .iter()
            .take(s.n_tasks)
            .any(|t| t.name() == APPS[i] && t.state != 3);
        let x = CONTENT_X + 14 + i as i16 * 160;

        button(w, x, 318, 140, 28, APP_NAMES[i], s.accent, running, false);
    }
}

fn page_about(w: &wm::Win, s: &State) {
    card(w, CONTENT_X, 80, CONTENT_W, 170, "版本");
    kv(w, CONTENT_X + 14, 116, "版本", "Novus OS 0.9（M9）");
    kv(w, CONTENT_X + 14, 142, "内核", "C · UEFI · x86_64");
    kv(w, CONTENT_X + 14, 168, "用户态", "Rust（no_std，扁平镜像）");
    kv(w, CONTENT_X + 14, 194, "桌面", "Novus-WM · 双缓冲合成");
    kv(w, CONTENT_X + 14, 220, "持久化", "不落盘，重启恢复默认");

    card(w, CONTENT_X, 266, CONTENT_W, 150, "里程碑");
    w.text(CONTENT_X + 14, 302, ui::TEXT, "M1–M3  引导 / 内核 / 内存与中断");
    w.text(CONTENT_X + 14, 326, ui::TEXT, "M4–M5  进程与地址空间 / 2D 渲染与控件");
    w.text(CONTENT_X + 14, 350, ui::TEXT, "M6      IPC 与窗口管理器");
    w.text(CONTENT_X + 14, 374, ui::TEXT, "M8–M9   Rust 应用 / 中文与 Win11 桌面");

    let mut b = [0u8; 12];
    let t = ui::fmt_u32(s.st.tasks, &mut b);
    let mut line = ui::Buf::new();

    line.s("当前 ").s(t).s(" 个任务 · 强调色 ").s(PRESET_NAMES[preset_index(s.accent)]);
    w.text(CONTENT_X + 14, 400, ui::DIM, line.str());
}

fn draw(w: &wm::Win, s: &State) {
    w.clear();
    w.fill(0, 0, CW, CH, ui::BG);
    nav(w, s);

    w.text_big(CONTENT_X, 24, ui::TEXT, PAGE_NAMES[match s.page {
        Page::System => 0,
        Page::Look => 1,
        Page::Apps => 2,
        Page::About => 3,
    }]);
    w.text(CONTENT_X, 48, ui::DIM, PAGE_SUB[match s.page {
        Page::System => 0,
        Page::Look => 1,
        Page::Apps => 2,
        Page::About => 3,
    }]);

    match s.page {
        Page::System => page_system(w, s),
        Page::Look => page_look(w, s),
        Page::Apps => page_apps(w, s),
        Page::About => page_about(w, s),
    }

    /* Footer: what just happened, plus the version. */
    w.shape(wm::OP_HLINE, 0, CH - 34, CW, 1, ui::LINE);
    if s.note.is_empty() {
        w.text(12, CH - 24, ui::DIM, "Novus OS 设置");
    } else {
        w.text(12, CH - 24, ui::TEXT, s.note);
    }
    let v = "0.9";
    w.text(CW - 14 - ui::width(v), CH - 24, ui::DIM, v);
    /* One paint for the whole frame: every item is on the list by now. */
    w.invalidate();
}

/* ---- data ------------------------------------------------------------- */

fn load_stats(port: u64, st: &mut sinfo::Stats) {
    if let Some(s) = sinfo::stats(port) {
        *st = s;
    }
}

fn load_tasks(port: u64, out: &mut [sinfo::TaskRec; MAX_ROWS]) -> usize {
    let mut n = 0usize;
    let mut index = 0u16;

    while n < MAX_ROWS {
        let Some(page) = sinfo::tasks(port, index) else {
            break;
        };
        if page.h.n == 0 {
            break;
        }
        for i in 0..page.h.n as usize {
            if n < MAX_ROWS {
                out[n] = page.rec[i];
                n += 1;
            }
        }
        index += page.h.n as u16;
        if index >= page.h.total {
            break;
        }
    }
    n
}

/* ---- entry ------------------------------------------------------------ */

#[no_mangle]
pub extern "C" fn _start() -> ! {
    sys::write(b"[APP] settings up\n");
    let Some(wm_port) = ipc::lookup(wm::PORT_NAME) else {
        sys::exit(1);
    };
    let Some(sys_port) = ipc::lookup(sinfo::PORT_NAME) else {
        sys::exit(2);
    };
    let Some(win) = wm::Win::create(wm_port, 300, 190, CW as i32 + 2,
                                    CH as i32 + wm::TITLE_H, "设置")
    else {
        sys::exit(3);
    };

    let mut s = State {
        page: Page::System,
        accent: ui::ACCENT,
        wallpaper: 0,
        st: sinfo::Stats {
            uptime: 0,
            free_kb: 0,
            total_kb: 0,
            tasks: 0,
            windows: 0,
            ipc_sent: 0,
            ipc_drop: 0,
            accent: ui::ACCENT,
            wallpaper: 0,
        },
        tasks: [sinfo::TaskRec {
            pid: 0,
            ticks: 0,
            exit_code: 0,
            state: 0,
            slice: 0,
            name: [0; 14],
        }; MAX_ROWS],
        n_tasks: 0,
        hover: (-1, -1),
        confirm_pid: 0,
        confirm_left: 0,
        note: "",
    };

    if let Some(t) = sinfo::theme(sys_port) {
        s.accent = t.accent;
        s.wallpaper = t.wallpaper;
    }
    load_stats(sys_port, &mut s.st);
    s.n_tasks = load_tasks(sys_port, &mut s.tasks);
    draw(&win, &s);

    let mut idle = 0u32;
    loop {
        while let Some(ev) = win.next_event() {
            match ev {
                wm::Ev::Close => {
                    win.destroy();
                    sys::exit(0);
                }
                wm::Ev::Mouse(m) if m.kind == wm::MOUSE_DOWN && m.button == 1 => {
                    handle_click(&win, &mut s, m.x, m.y, sys_port);
                    draw(&win, &s);
                }
                wm::Ev::Mouse(m) => {
                    /* Hover only needs a repaint when the nav rail changes
                     * highlight; everything else is static between clicks. */
                    let old = s.hover;
                    s.hover = (m.x as i16, m.y as i16);
                    let nav_area = |p: (i16, i16)| p.0 < NAV_W && p.1 >= 62 && p.1 < 62 + 42 * 4;
                    if nav_area(old) || nav_area(s.hover) {
                        draw(&win, &s);
                    }
                }
                _ => {}
            }
        }

        idle += 1;
        if s.confirm_left > 0 {
            s.confirm_left -= 1;
            if s.confirm_left == 0 && s.confirm_pid != 0 {
                s.confirm_pid = 0;
                draw(&win, &s);
            }
        }
        if idle >= 40 {
            idle = 0;
            if s.page == Page::System || s.page == Page::Apps {
                load_stats(sys_port, &mut s.st);
                s.n_tasks = load_tasks(sys_port, &mut s.tasks);
                draw(&win, &s);
            }
        }
        sys::yield_now();
    }
}

fn handle_click(w: &wm::Win, s: &mut State, x: i32, y: i32, sys_port: u64) {
    /* Navigation rail. */
    if x < NAV_W as i32 {
        for (i, p) in PAGES.iter().enumerate() {
            let top = 62 + i as i16 * 42;

            if y >= top as i32 && y < (top + 40) as i32 {
                s.page = *p;
                s.note = "";
                return;
            }
        }
        return;
    }

    match s.page {
        Page::Look => {
            /* Accent swatches. */
            for (i, c) in PRESETS.iter().enumerate() {
                let px = CONTENT_X + 14 + (i % 4) as i16 * 62;
                let py = 112 + (i / 4) as i16 * 40;

                if x >= (px - 2) as i32 && x < (px + 46) as i32
                    && y >= (py - 2) as i32 && y < (py + 30) as i32
                {
                    if let Some(t) = sinfo::set_theme(sys_port, sinfo::THEME_ACCENT, *c) {
                        s.accent = t.accent;
                        s.note = "已应用强调色";
                    }
                    return;
                }
            }
            /* Wallpaper previews. */
            for i in 0..PAPERS.len() {
                let px = CONTENT_X + 14 + i as i16 * 164;
                let py = 248;

                if x >= px as i32 && x < (px + 150) as i32
                    && y >= py as i32 && y < (py + 44) as i32
                {
                    if let Some(t) = sinfo::set_theme(sys_port, sinfo::THEME_WALLPAPER, i as u32) {
                        s.wallpaper = t.wallpaper;
                        s.note = "已更换桌面背景";
                    }
                    return;
                }
            }
        }
        Page::Apps => {
            /* "结束" buttons on the running list. */
            let mut shown = 0usize;
            for i in 0..APPS.len() {
                let Some(t) = running_for(s, i) else {
                    continue;
                };
                let by = 112 + shown as i16 * 36;
                let bx = CONTENT_X + CONTENT_W - 84;

                shown += 1;
                if x >= bx as i32 && x < (bx + 70) as i32
                    && y >= by as i32 && y < (by + 24) as i32
                {
                    if s.confirm_pid == t.pid {
                        /* Second click inside the window: really end it. */
                        let pid = t.pid;
                        let me = sys::getpid();

                        s.confirm_pid = 0;
                        if pid == me {
                            /* Ending ourselves: asking the kernel first
                             * would destroy our inbox and leave this call
                             * waiting for a reply that can never arrive.
                             * Close the window and exit instead - the
                             * scheduler does the rest. */
                            w.destroy();
                            sys::exit(0);
                        }
                        let rc = sinfo::kill(sys_port, pid);

                        s.note = match rc {
                            Some(0) => "已结束所选应用",
                            Some(-2) => "该系统任务不可结束",
                            _ => "无法结束该应用",
                        };
                        s.n_tasks = load_tasks(sys_port, &mut s.tasks);
                    } else {
                        s.confirm_pid = t.pid;
                        s.confirm_left = 75; /* ~3 s at the idle cadence */
                        s.note = "再次点击确认结束";
                    }
                    return;
                }
                if shown == 3 {
                    break;
                }
            }
            /* "启动" buttons. */
            for i in 0..APPS.len() {
                let bx = CONTENT_X + 14 + i as i16 * 160;
                let running = running_for(s, i).is_some();

                if x >= bx as i32 && x < (bx + 140) as i32
                    && y >= 318 && y < 346 && !running
                {
                    s.note = match sinfo::spawn(sys_port, i as u8) {
                        Some(0) => "已启动应用",
                        Some(-100) => "该应用已在运行",
                        _ => "无法启动该应用",
                    };
                    return;
                }
            }
        }
        Page::System => {
            if x >= (CONTENT_X + 300) as i32 && y >= 300 && y < 320 {
                s.note = "内存数据来自内核分页器";
            }
        }
        Page::About => {}
    }
}
