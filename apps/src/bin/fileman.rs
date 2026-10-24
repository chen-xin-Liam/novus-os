//! Novus OS - file manager (M8).
//!
//! Windows Explorer's layout: a toolbar with history buttons and an address
//! bar, a navigation pane on the left, a details list with name/type/size
//! columns, and a status bar. The built-in tree (/apps, /dev, /proc, /exe)
//! comes from the sysinfo service, three entries per round trip; 磁盘
//! browses NovusFS through fs.rs.

#![no_std]
#![no_main]

use novus_apps::{fs, ipc, sinfo, sys, ui, wm};

const CW: i16 = 560;
const CH: i16 = 322;
const ROW_CAP: usize = 24; /* entries kept per view */
const MAX_ROWS: usize = 9; /* rows that fit on screen */
const ROW_H: i16 = 26;
const ROW_BODY: i16 = 24;
const TOOL_H: i16 = 40;
const COL_H: i16 = 22;
const NAV_W: i16 = 132;
const STATUS_H: i16 = 26;
const PATH_CAP: usize = 128;
const LIST_X: i16 = NAV_W + 8;
const LIST_Y: i16 = TOOL_H + COL_H;
const LIST_W: i16 = CW - LIST_X - 8;
const NAV_Y: i16 = 74;
const NAV_ITEM_H: i16 = 26;
const NAV_ITEM_PITCH: i16 = 30;
const NAME_X: i16 = LIST_X + 30;
const TYPE_X: i16 = LIST_X + 244;
const SIZE_R: i16 = CW - 14;
const BTN_W: i16 = 44;
const BTN_H: i16 = 24;
const ADDR_X: i16 = 204;
const ADDR_W: i16 = 274;
const OPEN_X: i16 = 486;
const OPEN_W: i16 = 66;
const HIST: usize = 8;

const KEY_UP: u32 = 0x101;
const KEY_DOWN: u32 = 0x102;
const KEY_ENTER: u32 = 0x109;
const KEY_BACK: u32 = 0x10A;

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

#[derive(Clone, Copy)]
struct View {
    disk: bool,
    dir: u8,
    path: [u8; PATH_CAP],
    path_len: usize,
}

impl View {
    fn disk_path(path: [u8; PATH_CAP], path_len: usize) -> View {
        View { disk: true, dir: 0, path, path_len }
    }

    fn virt(dir: u8) -> View {
        View { disk: false, dir, path: [0u8; PATH_CAP], path_len: 0 }
    }
}

struct State {
    disk: bool,
    dir: u8,
    path: [u8; PATH_CAP],
    path_len: usize,
    rows: [Row; ROW_CAP],
    n: usize,
    total: u16,
    sel: usize,
    offset: usize,
    status: &'static str,
    hover_btn: i8,
    hover_nav: i8,
    hover_row: i8,
    views: [View; HIST],
    pos: usize,
    count: usize,
}

impl State {
    fn path(&self) -> &str {
        unsafe { core::str::from_utf8_unchecked(&self.path[..self.path_len]) }
    }

    /// Navigation-pane index for the current place (4 = 磁盘).
    fn nav(&self) -> usize {
        if self.disk { 4 } else { self.dir as usize }
    }

    /* The sysinfo tree is paged in fixed-size IPC replies; the index advances
     * by the returned count so the final short page is not skipped or read
     * again as if it were a full page. */
    fn reload(&mut self, port: u64) -> bool {
        self.n = 0;
        self.total = 0;
        let mut index = 0u16;

        while self.n < ROW_CAP {
            let Some(page) = sinfo::files(port, self.dir, index) else {
                return false;
            };
            self.total = page.h.total;
            if page.h.n == 0 {
                break;
            }
            for i in 0..page.h.n as usize {
                if self.n < ROW_CAP {
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
        self.clamp_selection();
        true
    }

    /* Directory iteration is stateful in the task-local descriptor. Close
     * it after the bounded listing even when a read reports an error, so a
     * refresh does not consume another slot in the process descriptor table. */
    fn reload_disk(&mut self) -> bool {
        self.n = 0;
        self.total = 0;
        let Some(fd) = fs::opendir(self.path()) else {
            return false;
        };
        loop {
            let entry = match fs::readdir(fd) {
                Some(Some(entry)) => entry,
                Some(None) => break,
                None => {
                    self.status = "读取目录失败";
                    break;
                }
            };
            let name = entry.name();
            if name == "." || name == ".." {
                continue;
            }
            self.total = self.total.saturating_add(1);
            if self.n < ROW_CAP {
                let mut row = Row {
                    name: [0; 57],
                    size: 0,
                    kind: if entry.mode == fs::MODE_DIR { 2 } else { 1 },
                };
                let len = name.len().min(row.name.len() - 1);

                row.name[..len].copy_from_slice(&name.as_bytes()[..len]);
                if row.kind != 2 {
                    row.size = self.stat_child(name);
                }
                self.rows[self.n] = row;
                self.n += 1;
            }
        }
        fs::close(fd);
        self.clamp_selection();
        true
    }

    fn clamp_selection(&mut self) {
        if self.sel >= self.n {
            self.sel = 0;
        }
        if self.offset >= self.n {
            self.offset = 0;
        }
    }

    /// Size of `name` inside the current directory, 0 when it cannot be read.
    fn stat_child(&self, name: &str) -> u32 {
        let mut p = [0u8; 192];
        let mut len = self.path_len;

        p[..len].copy_from_slice(&self.path[..len]);
        if len > 1 {
            p[len] = b'/';
            len += 1;
        }
        let n = name.len().min(p.len() - 1 - len);

        p[len..len + n].copy_from_slice(&name.as_bytes()[..n]);
        let Some(full) = core::str::from_utf8(&p[..len + n]).ok() else {
            return 0;
        };
        match fs::stat(full) {
            Some(st) => st.size.min(u32::MAX as u64) as u32,
            None => 0,
        }
    }

    /// Show `v`, replacing the current location but leaving the history alone.
    fn apply(&mut self, v: View, port: u64) {
        self.disk = v.disk;
        self.dir = v.dir;
        if v.disk {
            self.path = v.path;
            self.path_len = v.path_len;
        } else {
            self.path = [0u8; PATH_CAP];
            self.path[0] = b'/';
            self.path_len = 1;
        }
        self.sel = 0;
        self.offset = 0;
        if self.disk {
            if self.reload_disk() {
                self.status = "单击选择，再次单击或按 Enter 打开";
            } else {
                self.status = "未检测到可用的文件系统";
            }
        } else {
            if self.reload(port) {
                self.status = "选中程序后按 Enter 运行";
            } else {
                self.status = "无法连接系统服务";
            }
        }
    }

    /// Navigate somewhere new: drop the forward entries, push, show it.
    fn go(&mut self, v: View, port: u64) {
        if self.count == HIST {
            let mut i = 1;

            while i < HIST {
                self.views[i - 1] = self.views[i];
                i += 1;
            }
            self.count -= 1;
            if self.pos > 0 {
                self.pos -= 1;
            }
        }
        self.views[self.count] = v;
        self.count += 1;
        self.pos = self.count - 1;
        self.apply(v, port);
    }

    fn back(&mut self, port: u64) {
        if self.pos == 0 {
            return;
        }
        self.pos -= 1;
        let v = self.views[self.pos];

        self.apply(v, port);
    }

    fn forward(&mut self, port: u64) {
        if self.pos + 1 >= self.count {
            return;
        }
        self.pos += 1;
        let v = self.views[self.pos];

        self.apply(v, port);
    }

    fn up(&mut self, port: u64) {
        if !self.disk || self.path_len <= 1 {
            self.status = "已在顶层";
            return;
        }
        let mut len = self.path_len;

        while len > 1 && self.path[len - 1] != b'/' {
            len -= 1;
        }
        if len > 1 {
            len -= 1;
        }
        let mut path = [0u8; PATH_CAP];

        path[..len].copy_from_slice(&self.path[..len]);
        self.go(View::disk_path(path, len), port);
    }

    fn refresh(&mut self, port: u64) {
        if self.disk {
            if !self.reload_disk() {
                self.status = "未检测到可用的文件系统";
            }
        } else if !self.reload(port) {
            self.status = "无法连接系统服务";
        }
    }

    fn nav_go(&mut self, i: usize, port: u64) {
        if i == 4 {
            if self.disk {
                self.refresh(port);
                return;
            }
            let mut path = [0u8; PATH_CAP];

            path[0] = b'/';
            self.go(View::disk_path(path, 1), port);
        } else {
            if !self.disk && self.dir == i as u8 {
                self.refresh(port);
                return;
            }
            self.go(View::virt(i as u8), port);
        }
    }

    fn open_selected(&mut self, port: u64) {
        if self.n == 0 {
            return;
        }
        let row = self.rows[self.sel];

        if self.disk {
            if row.kind != fs::MODE_DIR as u8 {
                self.status = "选中项是文件";
                return;
            }
            let name = row.name();
            let mut path = [0u8; PATH_CAP];
            let mut len = self.path_len;

            if len > 1 {
                if len + 1 >= PATH_CAP {
                    self.status = "路径过长";
                    return;
                }
                path[len] = b'/';
                len += 1;
            }
            if len + name.len() >= PATH_CAP {
                self.status = "路径过长";
                return;
            }
            path[len..len + name.len()].copy_from_slice(name.as_bytes());
            len += name.len();
            self.go(View::disk_path(path, len), port);
        } else {
            match row.kind {
                sinfo::KIND_APP | sinfo::KIND_EXE => {
                    self.status = match sinfo::spawn_name(port, row.name()) {
                        Some(0) => "已启动",
                        Some(-100) => "已在运行",
                        _ => "无法启动",
                    };
                }
                sinfo::KIND_DEV => self.status = "设备由内核驱动，不能单独运行",
                _ => self.status = "系统信息项，不能运行",
            }
        }
    }
}

/* ---- geometry --------------------------------------------------------- */

fn btn_rect(i: u8) -> (i16, i16, i16, i16) {
    (8 + i as i16 * (BTN_W + 4), 8, BTN_W, BTN_H)
}

fn nav_rect(i: usize) -> (i16, i16, i16, i16) {
    (8, NAV_Y + i as i16 * NAV_ITEM_PITCH, NAV_W - 16, NAV_ITEM_H)
}

fn inside(r: (i16, i16, i16, i16), x: i32, y: i32) -> bool {
    x >= r.0 as i32
        && x < (r.0 + r.2) as i32
        && y >= r.1 as i32
        && y < (r.1 + r.3) as i32
}

fn nav_label(i: usize) -> &'static str {
    match i {
        0 => ui::l("应用", "Apps"),
        1 => ui::l("设备", "Devices"),
        2 => ui::l("进程", "Processes"),
        3 => ui::l("程序", "Programs"),
        _ => ui::l("磁盘", "Disk"),
    }
}

fn nav_color(i: usize) -> u32 {
    match i {
        0 | 3 => ui::accent(),
        1 => ui::good(),
        2 => ui::warn(),
        _ => ui::dim(),
    }
}

fn kind_label(st: &State, r: &Row) -> &'static str {
    if st.disk {
        if r.kind == fs::MODE_DIR as u8 {
            ui::l("文件夹", "Folder")
        } else {
            ui::l("文件", "File")
        }
    } else {
        match r.kind {
            sinfo::KIND_APP => ui::l("应用", "App"),
            sinfo::KIND_DEV => ui::l("设备", "Device"),
            sinfo::KIND_EXE => ui::l("程序", "Program"),
            _ => ui::l("虚拟", "Virtual"),
        }
    }
}

fn icon_color(st: &State, r: &Row) -> u32 {
    if st.disk {
        if r.kind == fs::MODE_DIR as u8 { ui::warn() } else { ui::dim() }
    } else {
        match r.kind {
            sinfo::KIND_APP | sinfo::KIND_EXE => ui::accent(),
            sinfo::KIND_DEV => ui::good(),
            _ => ui::dim(),
        }
    }
}

fn size_label<'a>(st: &State, r: &Row, buf: &'a mut [u8; 16]) -> &'a str {
    if st.disk {
        if r.kind == fs::MODE_DIR as u8 {
            "-"
        } else {
            ui::fmt_size(r.size, buf)
        }
    } else {
        match r.kind {
            sinfo::KIND_APP | sinfo::KIND_EXE => ui::fmt_size(r.size, buf),
            _ => "-",
        }
    }
}

fn visible_rows(st: &State) -> usize {
    if st.n > MAX_ROWS { MAX_ROWS } else { st.n }
}

#[derive(Clone, Copy, PartialEq)]
enum Hit {
    None,
    Btn(u8),
    Open,
    Nav(usize),
    Row(usize),
}

fn hit_test(st: &State, x: i32, y: i32) -> Hit {
    for i in 0..4u8 {
        if inside(btn_rect(i), x, y) {
            return Hit::Btn(i);
        }
    }
    if inside((OPEN_X, 8, OPEN_W, BTN_H), x, y) {
        return Hit::Open;
    }
    for i in 0..5usize {
        if inside(nav_rect(i), x, y) {
            return Hit::Nav(i);
        }
    }
    for i in 0..visible_rows(st) {
        let y0 = LIST_Y + i as i16 * ROW_H;

        if x >= LIST_X as i32
            && x < (LIST_X + LIST_W) as i32
            && y >= y0 as i32
            && y < (y0 + ROW_BODY) as i32
        {
            return Hit::Row(st.offset + i);
        }
    }
    Hit::None
}

/* ---- painting --------------------------------------------------------- */

fn draw(w: &mut wm::Win, st: &mut State, accent: u32) {
    w.clear();
    w.fill(0, 0, CW, CH, ui::bg());

    /* Toolbar. */
    w.fill(0, 0, CW, TOOL_H, ui::card_bg());
    w.fill(0, TOOL_H - 1, CW, 1, ui::line());

    let labels = [
        ui::l("返回", "Back"),
        ui::l("前进", "Forward"),
        ui::l("上级", "Up"),
        ui::l("刷新", "Refresh"),
    ];
    let enabled = [st.pos > 0, st.pos + 1 < st.count, st.disk && st.path_len > 1, true];

    for i in 0..4u8 {
        let (bx, by, bw, bh) = btn_rect(i);
        let on = enabled[i as usize];

        if on && st.hover_btn == i as i8 {
            w.round_fill(bx, by, bw, bh, 6, ui::subtle());
        }
        let tw = ui::width(labels[i as usize]);

        w.text(bx + (bw - tw) / 2, by + 5,
               if on { ui::text() } else { ui::line() },
               labels[i as usize]);
    }

    /* Address bar: a breadcrumb for the built-in tree, the path on disk. */
    w.round_fill(ADDR_X, 8, ADDR_W, BTN_H, 6, ui::bg());
    w.round_frame(ADDR_X, 8, ADDR_W, BTN_H, 6, ui::line());
    {
        let mut ab = ui::Buf::new();

        if st.disk {
            ab.s(st.path());
        } else {
            ab.s(ui::l("此电脑", "This PC")).s("  >  ").s(nav_label(st.dir as usize));
        }
        w.text(ADDR_X + 8, 13, ui::text(), ui::fit(ab.str(), ADDR_W - 16));
    }

    /* Open / run. */
    let can_run = st.n > 0
        && !st.disk
        && (st.rows[st.sel].kind == sinfo::KIND_APP
            || st.rows[st.sel].kind == sinfo::KIND_EXE);
    let verb = if can_run { ui::l("运行", "Run") } else { ui::l("打开", "Open") };
    let fill = if st.n > 0 { accent } else { ui::line() };

    w.round_fill(OPEN_X, 8, OPEN_W, BTN_H, 6, fill);
    {
        let tw = ui::width(verb);

        w.text(OPEN_X + (OPEN_W - tw) / 2, 13, ui::on_color(fill), verb);
    }

    /* Navigation pane. */
    w.fill(NAV_W, TOOL_H, 1, CH - TOOL_H - STATUS_H, ui::line());
    w.text(12, 52, ui::dim(), ui::l("此电脑", "This PC"));
    let cur = st.nav();

    for i in 0..5usize {
        let (nx, ny, nw, nh) = nav_rect(i);
        let sel = cur == i;

        if sel {
            w.round_fill(nx, ny, nw, nh, 5, ui::row_sel());
            w.round_fill(nx, ny + 5, 3, nh - 10, 1, accent);
        } else if st.hover_nav == i as i8 {
            w.round_fill(nx, ny, nw, nh, 5, ui::subtle());
        }
        w.round_fill(nx + 14, ny + 9, 8, 8, 2, nav_color(i));
        w.text(nx + 28, ny + 6,
               if sel { ui::text() } else { ui::dim() },
               nav_label(i));
    }

    /* Column header. */
    w.text(NAME_X, 44, ui::dim(), ui::l("名称", "Name"));
    w.text(TYPE_X, 44, ui::dim(), ui::l("类型", "Type"));
    {
        let sl = ui::l("大小", "Size");

        w.text(SIZE_R - ui::width(sl), 44, ui::dim(), sl);
    }
    w.fill(LIST_X, LIST_Y - 1, LIST_W, 1, ui::line());

    /* Rows. Keep the selection on screen before painting, so the hit test
     * and the picture agree on which entries are visible. */
    let mut shown = visible_rows(st);

    if st.sel < st.offset {
        st.offset = st.sel;
    }
    if st.sel >= st.offset + shown {
        st.offset = st.sel + 1 - shown;
    }
    if st.offset + shown > st.n {
        st.offset = st.n.saturating_sub(shown);
    }
    shown = visible_rows(st);

    if st.n == 0 {
        let hint = if st.disk {
            ui::l("未检测到磁盘", "No drives found")
        } else {
            ui::l("（空）", "(empty)")
        };

        w.text(LIST_X + 8, LIST_Y + 10, ui::dim(), hint);
    }
    for i in 0..shown {
        let idx = st.offset + i;
        let r = st.rows[idx];
        let y = LIST_Y + i as i16 * ROW_H;
        let sel = idx == st.sel;

        if sel {
            w.round_fill(LIST_X, y, LIST_W, ROW_BODY, 5, ui::row_sel());
            w.round_fill(LIST_X, y + 4, 3, ROW_BODY - 8, 1, accent);
        } else if st.hover_row == idx as i8 {
            w.round_fill(LIST_X, y, LIST_W, ROW_BODY, 5, ui::subtle());
        }
        w.round_fill(LIST_X + 6, y + 5, 14, 14, 3, icon_color(st, &r));
        w.text(NAME_X, y + 5, ui::text(),
               ui::fit(r.name(), TYPE_X - NAME_X - 10));
        w.text(TYPE_X, y + 5, ui::dim(), kind_label(st, &r));
        {
            let mut sb = [0u8; 16];
            let sz = size_label(st, &r, &mut sb);

            w.text(SIZE_R - ui::width(sz), y + 5, ui::dim(), sz);
        }
    }

    /* Status bar. */
    w.fill(0, CH - STATUS_H, CW, 1, ui::line());
    w.text(12, CH - STATUS_H + 7, ui::dim(), st.status);
    {
        let mut cb = ui::Buf::new();

        cb.num(st.total as u32).s(" ").s(ui::l("项", "items"));
        w.text(CW - 12 - ui::width(cb.str()), CH - STATUS_H + 7, ui::dim(),
               cb.str());
    }

    w.invalidate();
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
    let Some(mut win) = wm::Win::create(wm_port, 40, 396, CW as i32 + 2,
                                        CH as i32 + wm::TITLE_H, "文件管理器")
    else {
        sys::exit(3);
    };
    ui::init(sys_port);
    let mut accent = ui::accent();

    let mut path = [0u8; PATH_CAP];

    path[0] = b'/';
    let start = View::virt(sinfo::DIR_APPS);
    let mut st = State {
        disk: start.disk,
        dir: start.dir,
        path,
        path_len: 1,
        rows: [Row { name: [0; 57], size: 0, kind: 0 }; ROW_CAP],
        n: 0,
        total: 0,
        sel: 0,
        offset: 0,
        status: "选中程序后按 Enter 运行",
        hover_btn: -1,
        hover_nav: -1,
        hover_row: -1,
        views: [start; HIST],
        pos: 0,
        count: 1,
    };

    if !st.reload(sys_port) {
        st.status = "无法连接系统服务";
    }
    draw(&mut win, &mut st, accent);

    loop {
        while let Some(ev) = win.next_event() {
            match ev {
                wm::Ev::Close => {
                    win.destroy();
                    sys::exit(0);
                }
                wm::Ev::Mouse(m) if m.kind == 1 => {
                    let (hb, hn, hr) = match hit_test(&st, m.x, m.y) {
                        Hit::Btn(i) => (i as i8, -1, -1),
                        Hit::Open => (4, -1, -1),
                        Hit::Nav(i) => (-1, i as i8, -1),
                        Hit::Row(i) => (-1, -1, i as i8),
                        Hit::None => (-1, -1, -1),
                    };

                    if hb != st.hover_btn || hn != st.hover_nav || hr != st.hover_row {
                        st.hover_btn = hb;
                        st.hover_nav = hn;
                        st.hover_row = hr;
                        draw(&mut win, &mut st, accent);
                    }
                }
                wm::Ev::Mouse(m) if m.kind == wm::MOUSE_DOWN && m.button == 1 => {
                    let mut redraw = true;

                    match hit_test(&st, m.x, m.y) {
                        Hit::Btn(i) => match i {
                            0 => st.back(sys_port),
                            1 => st.forward(sys_port),
                            2 => st.up(sys_port),
                            _ => st.refresh(sys_port),
                        },
                        Hit::Open => st.open_selected(sys_port),
                        Hit::Nav(i) => st.nav_go(i, sys_port),
                        Hit::Row(i) => {
                            if i == st.sel {
                                st.open_selected(sys_port);
                            } else {
                                st.sel = i;
                            }
                        }
                        Hit::None => redraw = false,
                    }
                    if redraw {
                        draw(&mut win, &mut st, accent);
                    }
                }
                wm::Ev::Key(k) => {
                    let mut redraw = true;

                    match k {
                        KEY_UP => {
                            if st.sel > 0 {
                                st.sel -= 1;
                            }
                        }
                        KEY_DOWN => {
                            if st.sel + 1 < st.n {
                                st.sel += 1;
                            }
                        }
                        KEY_ENTER => st.open_selected(sys_port),
                        KEY_BACK => st.up(sys_port),
                        _ => redraw = false,
                    }
                    if redraw {
                        draw(&mut win, &mut st, accent);
                    }
                }
                _ => {}
            }
        }
        if ui::poll(sys_port) {
            accent = ui::accent();
            draw(&mut win, &mut st, accent);
        }
        sys::yield_now();
    }
}
