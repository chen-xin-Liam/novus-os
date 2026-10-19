//! Novus OS - task manager (M8).
//!
//! Shows what the scheduler actually holds: one row per task slot with
//! pid, name, state and the ticks it has been charged. Refreshes twice a
//! second; the bar at the bottom is the share of the task table in use.

#![no_std]
#![no_main]

use novus_apps::{ipc, sinfo, sys, ui, wm};

const CW: i16 = 426;
const CH: i16 = 286;
const MAX_ROWS: usize = 8;

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

fn draw(w: &wm::Win, rows: &[Row; MAX_ROWS], n: usize, total: u16, accent: u32) {
    w.clear();
    ui::shell(w, CW, CH, "调度器", accent);
    w.text(372, 10, ui::DIM, "时间片");

    if n == 0 {
        w.text(12, 40, ui::DIM, "（无任务）");
    }
    for i in 0..n {
        let r = &rows[i];
        let y = 32 + i as i16 * ui::ROW_H;
        let selected = r.state == 2;
        let mut b = [0u8; 12];

        w.fill(4, y, CW - 8, ui::ROW_H - 2, if selected { ui::ROW_SEL } else { ui::ROW });
        w.text(12, y + 6, state_color(r.state), r.name());
        w.text(150, y + 6, ui::DIM, state_name(r.state));
        let p = ui::fmt_u32(r.pid as u32, &mut b);
        w.text(210, y + 6, ui::DIM, p);
        let t = ui::fmt_u32(r.ticks, &mut b);
        w.text(300, y + 6, ui::DIM, t);
        let slice = ui::fmt_u32(r.slice as u32, &mut b);
        w.text(388, y + 6, if r.slice > 4 { ui::GOOD } else { ui::DIM }, slice);
    }

    /* Task-table occupancy. */
    let filled = if total == 0 { 0 } else { (n as i16 * 100) / total as i16 };
    w.fill(10, CH - 44, CW - 20, 10, ui::ROW);
    w.fill(10, CH - 44, (CW - 20) * filled / 100, 10, accent);
    let mut line = ui::Buf::new();

    line.num(n as u32).s(" / ").num(total as u32).s(" 个任务槽在用");
    w.text(10, CH - 28, ui::DIM, line.str());
    ui::button(w, CW - 84, CH - 30, 74, 22, "刷新", accent);
}

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
    let Some(win) = wm::Win::create(wm_port, 470, 402, CW as i32 + 2, CH as i32 + wm::TITLE_H, "任务管理器")
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
    draw(&win, &rows, n, total, accent);

    let mut idle = 0u32;
    loop {
        while let Some(ev) = win.next_event() {
            match ev {
                wm::Ev::Close => {
                    win.destroy();
                    sys::exit(0);
                }
                wm::Ev::Mouse(m) if m.kind == wm::MOUSE_DOWN && m.button == 1 => {
                    if m.y >= (CH - 30) as i32 && m.x >= (CW - 84) as i32 {
                        let (a, b) = load(sys_port, &mut rows);
                        n = a;
                        total = b;
                        draw(&win, &rows, n, total, accent);
                    }
                }
                _ => {}
            }
        }
        idle += 1;
        if idle >= 40 {
            idle = 0;
            let (a, b) = load(sys_port, &mut rows);
            n = a;
            total = b;
            draw(&win, &rows, n, total, accent);
        }
        sys::yield_now();
    }
}
