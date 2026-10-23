//! Novus OS - file manager (M8).
//!
//! Browses the built-in tree the kernel exposes: /apps (the three system
//! applications, each runnable from here), /dev (the devices the kernel
//! actually drives) and /proc (scheduler and IPC counters). The listing
//! comes from the sysinfo service, three entries per round trip.

#![no_std]
#![no_main]

use novus_apps::{fs, ipc, sinfo, sys, ui, wm};

const CW: i16 = 416;
const CH: i16 = 304;
const MAX_ROWS: usize = 6;
const PATH_CAP: usize = 128;

#[derive(Clone, Copy)]
struct Row {
    name: [u8; 57],
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
    disk: bool,
    path: [u8; PATH_CAP],
    path_len: usize,
    rows: [Row; MAX_ROWS],
    n: usize,
    sel: usize,
    total: u16,
    status: &'static str,
}

impl State {
    fn path(&self) -> &str {
        unsafe { core::str::from_utf8_unchecked(&self.path[..self.path_len]) }
    }

    /* The sysinfo tree is paged in fixed-size IPC replies; the index advances
     * by the returned count so the final short page is not skipped or read
     * again as if it were a full page. */
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
                    let mut row = Row {
                        name: [0; 57],
                        size: page.rec[i].size,
                        kind: page.rec[i].kind,
                    };
                    row.name[..24].copy_from_slice(&page.rec[i].name);
                    self.rows[self.n] = row;
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

    /* Directory iteration is stateful in the task-local descriptor. Close
     * it after the bounded listing even when a read reports an error, so a
     * refresh does not consume another slot in the process descriptor table. */
    fn reload_disk(&mut self) {
        self.n = 0;
        self.total = 0;
        let Some(fd) = fs::opendir(self.path()) else {
            self.status = "无法打开目录";
            return;
        };
        while self.n < MAX_ROWS {
            let Some(entry) = fs::readdir(fd) else {
                self.status = "读取目录失败";
                break;
            };
            let Some(entry) = entry else { break };
            let name = entry.name();
            if name == "." || name == ".." { continue; }
            let row = &mut self.rows[self.n];
            row.name = [0; 57];
            let len = name.len().min(row.name.len() - 1);
            row.name[..len].copy_from_slice(&name.as_bytes()[..len]);
            row.kind = if entry.mode == fs::MODE_DIR { 2 } else { 1 };
            row.size = 0;
            self.n += 1;
            self.total += 1;
        }
        fs::close(fd);
        if self.sel >= self.n { self.sel = 0; }
        self.status = "选择目录后点打开，或运行文件测试";
    }

    fn enter_selected(&mut self) -> bool {
        if !self.disk || self.n == 0 || self.rows[self.sel].kind != fs::MODE_DIR as u8 { return false; }
        let name = self.rows[self.sel].name();
        let separator = if self.path_len == 1 { 0 } else { 1 };
        if self.path_len + separator + name.len() >= PATH_CAP { return false; }
        if separator != 0 {
            self.path[self.path_len] = b'/';
            self.path_len += 1;
        }
        self.path[self.path_len..self.path_len + name.len()].copy_from_slice(name.as_bytes());
        self.path_len += name.len();
        self.path[self.path_len] = 0;
        self.sel = 0;
        true
    }

    fn go_parent(&mut self) {
        if self.path_len <= 1 { return; }
        while self.path_len > 1 && self.path[self.path_len - 1] != b'/' { self.path_len -= 1; }
        if self.path_len > 1 { self.path_len -= 1; }
        self.path[self.path_len] = 0;
        self.sel = 0;
    }
}

fn draw(w: &mut wm::Win, st: &State, accent: u32) {
    w.clear();
    ui::shell(w, CW, CH, "内置目录", accent);

    /* Directory tabs. */
    let names = ["/apps", "/dev", "/proc", "磁盘"];
    for (i, name) in names.iter().enumerate() {
        let x = 110 + i as i16 * 66;
        let on = if i == 3 { st.disk } else { !st.disk && st.dir == i as u8 };

        w.fill(x, 5, 60, 17, if on { ui::row_sel() } else { ui::card_bg() });
        if on {
            w.fill(x, 21, 60, 2, accent);
        }
        w.text(x + 6, 10, if on { ui::text() } else { ui::dim() }, name);
    }

    if st.disk {
        w.text(12, 31, ui::dim(), st.path());
    }
    if st.n == 0 {
        w.text(12, if st.disk { 62 } else { 40 }, ui::dim(), "（空）");
    }
    for i in 0..st.n {
        let r = &st.rows[i];
        let color = if st.disk && r.kind == fs::MODE_DIR as u8 {
            ui::accent()
        } else {
            match r.kind {
                sinfo::KIND_APP => ui::accent(),
                sinfo::KIND_DEV => ui::good(),
                _ => ui::warn(),
            }
        };
        let mut sb = [0u8; 16];
        let value = if st.disk {
            if r.kind == fs::MODE_DIR as u8 { "目录" } else { "文件" }
        } else {
            match r.kind {
                sinfo::KIND_APP => ui::fmt_size(r.size, &mut sb),
                sinfo::KIND_DEV => "设备",
                _ => "虚拟",
            }
        };
        let y = if st.disk { 58 + i as i16 * ui::ROW_H } else { 34 + i as i16 * ui::ROW_H };

        if i == st.sel {
            w.fill(4, y, CW - 8, ui::ROW_H - 2, ui::row_sel());
        } else {
            w.fill(4, y, CW - 8, ui::ROW_H - 2, ui::row_bg());
        }
        w.text(12, y + 6, if i == st.sel { ui::text() } else { color }, r.name());
        w.text(CW - 12 - ui::width(value), y + 6, ui::dim(), value);
    }

    w.fill(0, CH - 38, CW, 1, ui::line());
    if st.disk {
        w.text(12, CH - 44, ui::dim(), st.status);
        ui::button(w, 10, CH - 30, 62, 22, "打开", accent);
        ui::button(w, 78, CH - 30, 62, 22, "上级", accent);
        ui::button(w, 146, CH - 30, 96, 22, "建测试目录", accent);
        ui::button(w, 248, CH - 30, 152, 22, "写入往返测试", accent);
    } else {
        let can_run = st.n > 0 && st.rows[st.sel].kind == sinfo::KIND_APP;
        ui::button(w, 10, CH - 30, 66, 22, "运行",
                   if can_run { accent } else { ui::line() });
        w.text(88, CH - 24, ui::dim(), st.status);

        let mut n = [0u8; 12];
        let shown = ui::fmt_u32(st.total as u32, &mut n);
        w.text(CW - 12 - 28, CH - 24, ui::dim(), shown);
        w.text(CW - 12 - ui::width("项"), CH - 24, ui::dim(), "项");
    }
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
    let Some(mut win) = wm::Win::create(wm_port, 40, 396, CW as i32 + 2, CH as i32 + wm::TITLE_H, "文件管理器")
    else {
        sys::exit(3);
    };
    ui::init(sys_port);
    let mut accent = ui::accent();
    let mut st = State {
        dir: sinfo::DIR_APPS,
        disk: false,
        path: {
            let mut path = [0u8; PATH_CAP];
            path[0] = b'/';
            path
        },
        path_len: 1,
        rows: [Row {
            name: [0; 57],
            size: 0,
            kind: 0,
        }; MAX_ROWS],
        n: 0,
        sel: 0,
        total: 0,
        status: "选中一行后点运行",
    };
    st.reload(sys_port);
    draw(&mut win, &st, accent);

    loop {
        while let Some(ev) = win.next_event() {
            match ev {
                wm::Ev::Close => {
                    win.destroy();
                    sys::exit(0);
                }
                wm::Ev::Mouse(m) if m.kind == wm::MOUSE_DOWN && m.button == 1 => {
                    for i in 0..4i16 {
                        if m.x >= (110 + i * 66) as i32
                            && m.x < (170 + i * 66) as i32
                            && m.y >= 5
                            && m.y < 22
                        {
                            st.sel = 0;
                            st.disk = i == 3;
                            if st.disk {
                                st.path = [0; PATH_CAP];
                                st.path[0] = b'/';
                                st.path_len = 1;
                                st.reload_disk();
                                draw(&mut win, &st, accent);
                                continue;
                            }
                            st.dir = i as u8;
                            st.sel = 0;
                            st.status = "读取中";
                            st.reload(sys_port);
                            st.status = "选中一行后点运行";
                            draw(&mut win, &st, accent);
                        }
                    }
                    if st.disk && m.y >= (CH - 30) as i32 {
                        if m.x >= 10 && m.x < 72 {
                            if st.enter_selected() { st.reload_disk(); }
                        } else if m.x >= 78 && m.x < 140 {
                            st.go_parent();
                            st.reload_disk();
                        } else if m.x >= 146 && m.x < 242 {
                            let result = if fs::mkdir("/taskman-test") {
                                "测试目录已创建"
                            } else if fs::opendir("/taskman-test").map(fs::close).unwrap_or(false) {
                                "测试目录已存在"
                            } else {
                                "创建目录失败"
                            };
                            st.reload_disk();
                            st.status = result;
                        } else if m.x >= 248 && m.x < 400 {
                            let payload = b"NovusFS user API round-trip";
                            let ok = if let Some(fd) = fs::open("/taskman-test/roundtrip.txt", true) {
                                let wrote = fs::write(fd, payload);
                                fs::close(fd);
                                if wrote == payload.len() as i64 {
                                    if let Some(fd) = fs::open("/taskman-test/roundtrip.txt", false) {
                                        let mut readback = [0u8; 64];
                                        let n = fs::read(fd, &mut readback);
                                        fs::close(fd);
                                        n == payload.len() as i64 && &readback[..payload.len()] == payload
                                    } else { false }
                                } else { false }
                            } else { false };
                            st.reload_disk();
                            st.status = if ok { "open/write/read 往返通过" } else { "文件读写失败" };
                        }
                        draw(&mut win, &st, accent);
                        continue;
                    }
                    for i in 0..st.n {
                        let y = if st.disk { 58 + i as i16 * ui::ROW_H } else { 34 + i as i16 * ui::ROW_H };
                        if m.y >= y as i32 && m.y < (y + ui::ROW_H - 2) as i32 && m.x >= 4 {
                            st.sel = i;
                            draw(&mut win, &st, accent);
                        }
                    }
                    if !st.disk && m.y >= (CH - 30) as i32 && m.x >= 10 && m.x < 76 {
                        if st.n > 0 && st.rows[st.sel].kind == sinfo::KIND_APP {
                            /* By name: the table gained the installer, so
                             * a row index is no longer a stable id. */
                            let name = st.rows[st.sel].name();
                            st.status = match sinfo::spawn_name(sys_port, name) {
                                Some(0) => "已启动",
                                Some(-100) => "已在运行",
                                _ => "无法启动",
                            };
                            draw(&mut win, &st, accent);
                        }
                    }
                }
                _ => {}
            }
        }
        if ui::poll(sys_port) {
            accent = ui::accent();
            draw(&mut win, &st, accent);
        }
        sys::yield_now();
    }
}
