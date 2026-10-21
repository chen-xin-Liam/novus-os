//! Novus OS - task manager (M8).

#![no_std]
#![no_main]

use novus_apps::{ipc, sinfo, sys, ui, wm};

const CW: i16 = 640;
const CH: i16 = 430;
const MAX_ROWS: usize = 64;
const VISIBLE_ROWS: usize = 11;
const GRAPH_SAMPLES: usize = 60;

#[derive(Clone, Copy)]
struct Row {
    pid: u64,
    ticks: u32,
    state: u8,
    slice: u8,
    name: [u8; 14],
}

impl Row {
    fn name(&self) -> &str {
        let mut n = 0;

        while n < self.name.len() && self.name[n] != 0 {
            n += 1;
        }
        unsafe { core::str::from_utf8_unchecked(&self.name[..n]) }
    }
}

fn state_name(s: u8) -> &'static str {
    match s {
        1 => "就绪",
        2 => "运行",
        3 => "退出",
        4 => "等待",
        5 => "暂停",
        _ => "空闲",
    }
}

fn state_color(s: u8) -> u32 {
    match s {
        2 => ui::GOOD,
        3 => ui::DIM,
        4 => ui::WARN,
        _ => ui::TEXT,
    }
}

fn sort_rows(rows: &mut [Row; MAX_ROWS], n: usize, mode: u8) {
    for end in (1..n).rev() {
        for i in 0..end {
            let swap = match mode {
                1 => rows[i].pid > rows[i + 1].pid,
                2 => rows[i].ticks < rows[i + 1].ticks,
                _ => rows[i].name() > rows[i + 1].name(),
            };
            if swap {
                rows.swap(i, i + 1);
            }
        }
    }
}

fn sample_cpu(rows: &[Row; MAX_ROWS], n: usize, previous: &[Row; MAX_ROWS], elapsed_ticks: u64) -> u32 {
    if elapsed_ticks == 0 {
        return 0;
    }
    let mut active_delta = 0u64;
    for i in 0..n {
        let mut old_ticks = None;
        for old in previous {
            if old.pid == rows[i].pid {
                old_ticks = Some(old.ticks);
                break;
            }
        }
        let Some(old_ticks) = old_ticks else { continue };
        let delta = rows[i].ticks.wrapping_sub(old_ticks) as u64;
        if rows[i].name() != "idle" {
            active_delta += delta;
        }
    }
    ((active_delta.saturating_mul(100) / elapsed_ticks).min(100)) as u32
}

/* CPU usage is derived from per-task tick deltas between snapshots rather
 * than a hardware counter. Matching by PID avoids attributing a recycled
 * scheduler slot to the task that occupied it in the previous sample. */
fn draw_processes(
    w: &wm::Win,
    rows: &[Row; MAX_ROWS],
    n: usize,
    page: usize,
    selected: usize,
    cpu: u32,
    total: u16,
    accent: u32,
    message: &str,
) {
    w.text(12, 89, ui::DIM, "名称");
    w.text(216, 89, ui::DIM, "状态      PID       CPU ticks    时间片");
    let start = page * VISIBLE_ROWS;
    let end = (start + VISIBLE_ROWS).min(n);
    for i in start..end {
        let r = &rows[i];
        let y = 108 + (i - start) as i16 * ui::ROW_H;
        let mut b = [0u8; 12];
        let mut details = ui::Buf::new();
        w.fill(6, y, CW - 12, ui::ROW_H - 1, if i == selected { ui::ROW_SEL } else { ui::ROW });
        w.text(14, y + 5, state_color(r.state), r.name());
        details.s(state_name(r.state)).s("  #").s(ui::fmt_u32(r.pid as u32, &mut b))
            .s("  ").s(ui::fmt_u32(r.ticks, &mut b)).s(" ticks  ").s(ui::fmt_u32(r.slice as u32, &mut b));
        w.text(216, y + 5, state_color(r.state), details.str());
    }
    if n == 0 {
        w.text(14, 116, ui::DIM, "当前没有可显示的任务");
    }
    ui::button(w, 10, 370, 92, 26, "结束任务", accent);
    ui::button(w, 110, 370, 74, 26, "刷新", accent);
    ui::button(w, CW - 166, 370, 66, 26, "上页", accent);
    ui::button(w, CW - 92, 370, 66, 26, "下页", accent);
    w.text(10, 405, ui::DIM, message);
    let mut status = ui::Buf::new();
    status.s("进程: ").num(n as u32).s(" / ").num(total as u32).s("    CPU: ").num(cpu).s("%");
    w.text(390, 405, ui::DIM, status.str());
}

fn draw_performance(w: &wm::Win, history: &[u8; GRAPH_SAMPLES], count: usize, head: usize, cpu: u32, tasks: usize, total: u16, accent: u32) {
    w.text(18, 92, ui::TEXT, "CPU 使用率");
    let x = 130i16;
    let y = 112i16;
    let width = CW - 154;
    let height = 230i16;
    w.fill(x, y, width, height, ui::ROW);
    for i in 0..=4 {
        let gy = y + (height - 1) * i / 4;
        w.shape(wm::OP_HLINE, x, gy, width, 1, ui::LINE);
    }
    let visible = count.min(27);
    for i in 0..visible {
        let sample_offset = if visible <= 1 { 0 } else {
            i * count.min(GRAPH_SAMPLES).saturating_sub(1) / (visible - 1)
        };
        let index = (head + GRAPH_SAMPLES - count.min(GRAPH_SAMPLES) + sample_offset) % GRAPH_SAMPLES;
        let value = history[index] as i16;
        let bar_h = (height - 10) * value / 100;
        let bx = x + 4 + i as i16 * (width - 8) / visible.max(1) as i16;
        w.fill(bx, y + height - 5 - bar_h, 4, bar_h.max(1), accent);
    }
    let mut line = ui::Buf::new();
    line.s("当前 ").num(cpu).s("%     历史采样");
    w.text(18, 365, ui::TEXT, line.str());
    w.text(18, 396, ui::DIM, "CPU 占用按采样周期内非 idle 任务 tick 计算");
    let mut status = ui::Buf::new();
    status.s("进程: ").num(tasks as u32).s(" / ").num(total as u32).s("    CPU: ").num(cpu).s("%");
    w.text(390, 405, ui::DIM, status.str());
}

fn draw(
    w: &wm::Win,
    rows: &[Row; MAX_ROWS],
    n: usize,
    total: u16,
    page: usize,
    selected: usize,
    tab: u8,
    cpu: u32,
    history: &[u8; GRAPH_SAMPLES],
    history_count: usize,
    history_head: usize,
    accent: u32,
    message: &str,
) {
    w.clear();
    ui::shell(w, CW, CH, "任务管理器", accent);
    w.text(10, 38, ui::DIM, "文件    选项    查看");
    w.fill(0, 60, CW, 26, ui::PANEL);
    w.fill(if tab == 0 { 10 } else { 104 }, 84, 82, 2, accent);
    w.text(18, 69, if tab == 0 { ui::TEXT } else { ui::DIM }, "进程");
    w.text(112, 69, if tab == 1 { ui::TEXT } else { ui::DIM }, "性能");
    if tab == 0 {
        draw_processes(w, rows, n, page, selected, cpu, total, accent, message);
    } else {
        draw_performance(w, history, history_count, history_head, cpu, n, total, accent);
    }
}

/* Task records arrive three at a time because each response must fit the
 * bounded IPC payload. The total comes from the service; the returned count
 * controls advancement so this snapshot follows the exact paging contract. */
fn load(port: u64, out: &mut [Row; MAX_ROWS]) -> (usize, u16) {
    let mut n = 0;
    let mut total = 0u16;
    let mut index = 0u16;

    while n < MAX_ROWS {
        let Some(page) = sinfo::tasks(port, index) else {
            break;
        };
        total = page.h.total;
        if page.h.n == 0 {
            break;
        }
        for i in 0..page.h.n as usize {
            if n < MAX_ROWS {
                out[n] = Row {
                    pid: page.rec[i].pid,
                    ticks: page.rec[i].ticks,
                    state: page.rec[i].state,
                    slice: page.rec[i].slice,
                    name: page.rec[i].name,
                };
                n += 1;
            }
        }
        index += page.h.n as u16;
        if index >= page.h.total {
            break;
        }
    }
    (n, total)
}

#[no_mangle]
pub extern "C" fn _start() -> ! {
    sys::write(b"[APP] taskman up\n");
    let Some(wm_port) = ipc::lookup(wm::PORT_NAME) else {
        sys::exit(1);
    };
    let Some(sys_port) = ipc::lookup(sinfo::PORT_NAME) else {
        sys::exit(2);
    };
    let Some(win) = wm::Win::create(wm_port, 320, 190, CW as i32 + 2, CH as i32 + wm::TITLE_H, "任务管理器")
    else {
        sys::exit(3);
    };
    let accent = sinfo::accent(sys_port).unwrap_or(ui::ACCENT);
    let mut rows = [Row {
        pid: 0,
        ticks: 0,
        state: 0,
        slice: 0,
        name: [0; 14],
    }; MAX_ROWS];
    let (mut n, mut total) = load(sys_port, &mut rows);
    let mut previous = rows;
    let mut previous_uptime = sinfo::stats(sys_port).map(|s| s.uptime).unwrap_or(0);
    let mut cpu = 0u32;
    let mut history = [0u8; GRAPH_SAMPLES];
    let mut history_count = 0usize;
    let mut history_head = 0usize;
    let mut selected = 0usize;
    let mut page = 0usize;
    let mut tab = 0u8;
    let mut message = "选择一个进程以结束任务";
    draw(&win, &rows, n, total, page, selected, tab, cpu, &history, history_count, history_head, accent, message);

    let mut idle = 0u32;
    let mut redraw = false;
    loop {
        while let Some(ev) = win.next_event() {
            match ev {
                wm::Ev::Close => {
                    win.destroy();
                    sys::exit(0);
                }
                wm::Ev::Mouse(m) if m.kind == wm::MOUSE_DOWN && m.button == 1 => {
                    redraw = true;
                    if m.y >= 60 && m.y < 86 {
                        tab = if m.x < 100 { 0 } else { 1 };
                    } else if tab == 0 && m.y >= 108 && m.y < 108 + VISIBLE_ROWS as i32 * ui::ROW_H as i32 {
                        let row = page * VISIBLE_ROWS + (m.y as usize - 108) / ui::ROW_H as usize;
                        if row < n {
                            selected = row;
                        }
                    } else if tab == 0 && m.y >= 370 && m.y < 400 && m.x < 104 {
                        if selected < n && rows[selected].pid != sys::getpid() {
                            match sinfo::kill(sys_port, rows[selected].pid) {
                                Some(0) => message = "任务已结束",
                                Some(-2) => message = "系统任务受保护，无法结束",
                                Some(-3) => message = "任务正在退出",
                                _ => message = "结束失败：任务不存在或权限不足",
                            }
                            let (a, b) = load(sys_port, &mut rows);
                            n = a;
                            total = b;
                            if selected >= n && n > 0 { selected = n - 1; }
                            page = selected / VISIBLE_ROWS;
                        } else {
                            message = "不能结束任务管理器自身";
                        }
                    } else if tab == 0 && m.y >= 370 && m.y < 400 && m.x < 194 {
                        let (a, b) = load(sys_port, &mut rows);
                        n = a;
                        total = b;
                    } else if tab == 0 && m.y >= 88 && m.y < 108 {
                        let mode = if m.x < 210 { 0 } else if m.x < 350 { 1 } else { 2 };
                        sort_rows(&mut rows, n, mode);
                    } else if tab == 0 && m.y >= 370 && m.y < 400 && m.x >= CW as i32 - 166 && m.x < CW as i32 - 100 {
                        page = page.saturating_sub(1);
                        selected = page * VISIBLE_ROWS;
                    } else if tab == 0 && m.y >= 370 && m.y < 400 && m.x >= CW as i32 - 100 {
                        if (page + 1) * VISIBLE_ROWS < n {
                            page += 1;
                            selected = page * VISIBLE_ROWS;
                        }
                    }
                }
                _ => {}
            }
        }
        idle += 1;
        if idle >= 40 {
            idle = 0;
            let old = previous;
            let uptime = sinfo::stats(sys_port).map(|s| s.uptime).unwrap_or(previous_uptime);
            let (a, b) = load(sys_port, &mut rows);
            n = a;
            total = b;
            cpu = sample_cpu(&rows, n, &old, uptime.wrapping_sub(previous_uptime));
            previous_uptime = uptime;
            previous = rows;
            history[history_head] = cpu.min(100) as u8;
            history_head = (history_head + 1) % GRAPH_SAMPLES;
            history_count = (history_count + 1).min(GRAPH_SAMPLES);
            if n > 0 && selected >= n { selected = n - 1; }
            page = if n == 0 { 0 } else { (selected / VISIBLE_ROWS).min((n - 1) / VISIBLE_ROWS) };
            redraw = true;
        }
        if redraw {
            draw(&win, &rows, n, total, page, selected, tab, cpu, &history, history_count, history_head, accent, message);
            redraw = false;
        }
        sys::yield_now();
    }
}
