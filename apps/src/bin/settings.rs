//! Novus OS - settings panel (M8).
//!
//! The desktop accent is a real kernel-side theme value: the WM and the
//! shell both draw from it, so picking a colour here repaints the whole
//! desktop (title bars, focus rings, progress bar). The panel also shows
//! live numbers from the sysinfo service, which makes it the quickest way
//! to see that IPC and the scheduler are alive.

#![no_std]
#![no_main]

use novus_apps::{ipc, sinfo, sys, ui, wm};

const CW: i16 = 356;
const CH: i16 = 250;

const PRESETS: [u32; 4] = [0x004C8DFF, 0x0059C98A, 0x00E0A34A, 0x00C06CE0];
const NAMES: [&str; 4] = ["蓝", "绿", "琥珀", "紫"];

fn draw(w: &wm::Win, accent: u32, st: &sinfo::Stats) {
    w.clear();
    ui::shell(w, CW, CH, "桌面与系统", accent);

    w.text(10, 38, ui::DIM, "桌面强调色");
    for (i, c) in PRESETS.iter().enumerate() {
        let x = 10 + i as i16 * 40;

        w.fill(x, 54, 32, 26, *c);
        if *c == accent {
            w.frame(x - 2, 52, 36, 30, ui::TEXT);
        }
    }
    /* Which preset is active, by name. */
    let mut idx = 0;
    for (i, c) in PRESETS.iter().enumerate() {
        if *c == accent {
            idx = i;
        }
    }
    w.text(180, 60, ui::TEXT, NAMES[idx]);

    w.shape(wm::OP_HLINE, 0, 88, CW, 1, ui::LINE);
    let mut b = [0u8; 16];

    w.text(10, 98, ui::DIM, "运行时间");
    let s = ui::fmt_u32((st.uptime / 100) as u32, &mut b);
    w.text(120, 98, ui::TEXT, s);
    let s = s; /* width() below measures the same string */
    w.text(120 + ui::width(s) + 4, 98, ui::DIM, "秒");

    w.text(10, 116, ui::DIM, "空闲内存");
    let s = ui::fmt_size((st.free_kb * 1024) as u32, &mut b);
    w.text(120, 116, ui::TEXT, s);

    w.text(10, 134, ui::DIM, "任务数");
    let s = ui::fmt_u32(st.tasks, &mut b);
    w.text(120, 134, ui::TEXT, s);

    w.text(10, 152, ui::DIM, "窗口数");
    let s = ui::fmt_u32(st.windows, &mut b);
    w.text(120, 152, ui::TEXT, s);

    w.text(10, 170, ui::DIM, "IPC 收发");
    let mut line = ui::Buf::new();

    line.num(st.ipc_sent).s(" / ").num(st.ipc_drop);
    w.text(150, 170, ui::TEXT, line.str());

    ui::button(w, CW - 84, CH - 30, 74, 22, "刷新", accent);
}

#[no_mangle]
pub extern "C" fn _start() -> ! {
    sys::write(b"[APP] settings up\n");
    let Some(wm_port) = ipc::lookup(wm::PORT_NAME) else {
        sys::exit(1);
    };
    let Some(sys_port) = ipc::lookup(sinfo::PORT_NAME) else {
        sys::exit(2);
    };
    let Some(win) = wm::Win::create(wm_port, 902, 438, CW as i32 + 2, CH as i32 + 25, "设置")
    else {
        sys::exit(3);
    };
    let mut accent = sinfo::accent(sys_port).unwrap_or(ui::ACCENT);
    let mut st = sinfo::stats(sys_port).unwrap_or(sinfo::Stats {
        uptime: 0,
        free_kb: 0,
        tasks: 0,
        windows: 0,
        ipc_sent: 0,
        ipc_drop: 0,
        accent,
    });
    draw(&win, accent, &st);

    let mut idle = 0u32;
    loop {
        while let Some(ev) = win.next_event() {
            match ev {
                wm::Ev::Close => {
                    win.destroy();
                    sys::exit(0);
                }
                wm::Ev::Mouse(m) if m.kind == wm::MOUSE_DOWN && m.button == 1 => {
                    for (i, c) in PRESETS.iter().enumerate() {
                        let x = 10 + i as i16 * 40;

                        if m.x >= (x - 2) as i32
                            && m.x < (x + 34) as i32
                            && m.y >= 52
                            && m.y < 82
                        {
                            if sinfo::set_accent(sys_port, *c) {
                                accent = *c;
                            }
                        }
                    }
                    if m.y >= (CH - 30) as i32 && m.x >= (CW - 84) as i32 {
                        st = sinfo::stats(sys_port).unwrap_or(st);
                    }
                    draw(&win, accent, &st);
                }
                _ => {}
            }
        }
        idle += 1;
        if idle >= 60 {
            idle = 0;
            st = sinfo::stats(sys_port).unwrap_or(st);
            draw(&win, accent, &st);
        }
        sys::yield_now();
    }
}
