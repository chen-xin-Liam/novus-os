//! Novus OS - file manager (M8).
//!
//! Browses the built-in tree the kernel exposes: /apps (the three system
//! applications, each runnable from here), /dev (the devices the kernel
//! actually drives) and /proc (scheduler and IPC counters). The listing
//! comes from the sysinfo service, three entries per round trip.

#![no_std]
#![no_main]

use novus_apps::{ipc, sinfo, sys, ui, wm};

const CW: i16 = 416;
const CH: i16 = 304;
const MAX_ROWS: usize = 6;

#[derive(Clone, Copy)]
struct Row {
    name: [u8; 24],
    size: u32,
    kind: u8,
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

struct State {
    dir: u8,
    rows: [Row; MAX_ROWS],
    n: usize,
    sel: usize,
    total: u16,
    status: &'static str,
}

impl State {
    fn reload(&mut self, port: u64) {
        self.n = 0;
        self.total = 0;
        let mut index = 0u16;

        while self.n < MAX_ROWS {
            let Some(page) = sinfo::files(port, self.dir, index) else {
                self.status = "无法连接系统服务";
                return;
            };
            self.total = page.h.total;
            if page.h.n == 0 {
                break;
            }
            for i in 0..page.h.n as usize {
                if self.n < MAX_ROWS {
                    self.rows[self.n] = Row {
                        name: page.rec[i].name,
                        size: page.rec[i].size,
                        kind: page.rec[i].kind,
                    };
                    self.n += 1;
                }
            }
            index = (index + page.h.n as u16).min(page.h.total);
            if index >= page.h.total {
                break;
            }
        }
        if self.sel >= self.n {
            self.sel = 0;
        }
    }
}

fn draw(w: &wm::Win, st: &State, accent: u32) {
    w.clear();
    ui::shell(w, CW, CH, "内置目录", accent);

    /* Directory tabs. */
    let names = ["/apps", "/dev", "/proc"];
    for (i, name) in names.iter().enumerate() {
        let x = 110 + i as i16 * 66;
        let on = st.dir == i as u8;

        w.fill(x, 5, 60, 17, if on { ui::ROW_SEL } else { ui::PANEL });
        if on {
            w.fill(x, 21, 60, 2, accent);
        }
        w.text(x + 6, 10, if on { ui::TEXT } else { ui::DIM }, name);
    }

    if st.n == 0 {
        w.text(12, 40, ui::DIM, "（空）");
    }
    for i in 0..st.n {
        let r = &st.rows[i];
        let color = match r.kind {
            sinfo::KIND_APP => ui::ACCENT,
            sinfo::KIND_DEV => ui::GOOD,
            _ => ui::WARN,
        };
        let mut sb = [0u8; 16];
        let value = match r.kind {
            sinfo::KIND_APP => ui::fmt_size(r.size, &mut sb),
            sinfo::KIND_DEV => "设备",
            _ => "虚拟",
        };
        let y = 34 + i as i16 * ui::ROW_H;

        if i == st.sel {
            w.fill(4, y, CW - 8, ui::ROW_H - 2, ui::ROW_SEL);
        } else {
            w.fill(4, y, CW - 8, ui::ROW_H - 2, ui::ROW);
        }
        w.text(12, y + 6, if i == st.sel { ui::TEXT } else { color }, r.name());
        w.text(CW - 12 - ui::width(value), y + 6, ui::DIM, value);
    }

    w.fill(0, CH - 38, CW, 1, ui::LINE);
    let can_run = st.n > 0 && st.rows[st.sel].kind == sinfo::KIND_APP;
    ui::button(w, 10, CH - 30, 66, 22, "运行",
               if can_run { accent } else { ui::LINE });
    w.text(88, CH - 24, ui::DIM, st.status);

    let mut n = [0u8; 12];
    let shown = ui::fmt_u32(st.total as u32, &mut n);
    w.text(CW - 12 - 28, CH - 24, ui::DIM, shown);
    w.text(CW - 12 - ui::width("项"), CH - 24, ui::DIM, "项");
}

#[no_mangle]
pub extern "C" fn _start() -> ! {
    sys::write(b"[APP] fileman up\n");
    let Some(wm_port) = ipc::lookup(wm::PORT_NAME) else {
        sys::exit(1);
    };
    let Some(sys_port) = ipc::lookup(sinfo::PORT_NAME) else {
        sys::exit(2);
    };
    let Some(win) = wm::Win::create(wm_port, 40, 396, CW as i32 + 2, CH as i32 + 25, "文件管理器")
    else {
        sys::exit(3);
    };
    let accent = sinfo::accent(sys_port).unwrap_or(ui::ACCENT);
    let mut st = State {
        dir: sinfo::DIR_APPS,
        rows: [Row {
            name: [0; 24],
            size: 0,
            kind: 0,
        }; MAX_ROWS],
        n: 0,
        sel: 0,
        total: 0,
        status: "选中一行后点运行",
    };
    st.reload(sys_port);
    draw(&win, &st, accent);

    loop {
        while let Some(ev) = win.next_event() {
            match ev {
                wm::Ev::Close => {
                    win.destroy();
                    sys::exit(0);
                }
                wm::Ev::Mouse(m) if m.kind == wm::MOUSE_DOWN && m.button == 1 => {
                    for i in 0..3i16 {
                        if m.x >= (110 + i * 66) as i32
                            && m.x < (170 + i * 66) as i32
                            && m.y >= 5
                            && m.y < 22
                        {
                            st.dir = i as u8;
                            st.sel = 0;
                            st.status = "读取中";
                            st.reload(sys_port);
                            st.status = "选中一行后点运行";
                            draw(&win, &st, accent);
                        }
                    }
                    for i in 0..st.n {
                        let y = 34 + i as i16 * ui::ROW_H;
                        if m.y >= y as i32 && m.y < (y + ui::ROW_H - 2) as i32 && m.x >= 4 {
                            st.sel = i;
                            draw(&win, &st, accent);
                        }
                    }
                    if m.y >= (CH - 30) as i32 && m.x >= 10 && m.x < 76 {
                        if st.n > 0 && st.rows[st.sel].kind == sinfo::KIND_APP {
                            /* The spawn request takes the index in the
                             * kernel's app table, which is the order the
                             * /apps listing is built in. */
                            let which = st.sel as u8;
                            st.status = match sinfo::spawn(sys_port, which) {
                                Some(0) => "已启动",
                                Some(-100) => "已在运行",
                                _ => "无法启动",
                            };
                            draw(&win, &st, accent);
                        }
                    }
                }
                _ => {}
            }
        }
        sys::yield_now();
    }
}
