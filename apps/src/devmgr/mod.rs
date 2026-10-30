//! Device Manager - a Windows-style console for the machine's hardware.
//!
//! One main window (menu bar, toolbar, collapsible device tree, status bar)
//! and a modal property window opened per device with five tabs. Data comes
//! from the sysinfo device-list / device-field requests, cached here in fixed
//! arrays because the app has no allocator.

use crate::sinfo::DevRec;
use crate::ui;
use crate::wm;
use crate::{sys, sinfo};

use crate::devmgr::props::Props;
use crate::devmgr::tree::{Row, MAX_NODES, NO_ROW, ROOT_PARENT};

pub mod fields;
pub mod tree;
pub mod props;

pub const CW: i32 = 820;
pub const CH: i32 = 540;
pub const MAIN_X: i32 = 80;
pub const MAIN_Y: i32 = 60;

pub const PROP_CW: i32 = 520;
pub const PROP_CH: i32 = 560;
pub const PROP_X: i32 = 250;
pub const PROP_Y: i32 = 40;

const MENU_ITEMS: [&str; 4] = ["文件(F)", "操作(A)", "查看(V)", "帮助(H)"];

pub struct App {
    sys_port: u64,
    main: wm::Win,
    prop_win: Option<wm::Win>,
    nodes: [DevRec; MAX_NODES],
    n: usize,
    expanded: u64,
    rows: [Row; MAX_NODES],
    row_count: usize,
    selected: usize,
    scroll: usize,
    hover: usize,
    props: Props,
    menu: i8,
    click_row: usize,
    click_tick: u32,
    tick: u32,
    redraw: bool,
}

impl App {
    pub fn new(wm_port: u64, sys_port: u64) -> Option<App> {
        let main = wm::Win::create(wm_port, MAIN_X, MAIN_Y, CW + 2, CH + wm::TITLE_H,
                                   ui::l("设备管理器", "Device Manager"))?;
        ui::init(sys_port);
        let mut app = App {
            sys_port,
            main,
            prop_win: None,
            nodes: unsafe { core::mem::zeroed() },
            n: 0,
            expanded: 0,
            rows: unsafe { core::mem::zeroed() },
            row_count: 0,
            selected: 0,
            scroll: 0,
            hover: NO_ROW,
            props: Props::new(),
            menu: -1,
            click_row: NO_ROW,
            click_tick: 0,
            tick: 0,
            redraw: true,
        };
        app.refresh();
        Some(app)
    }

    fn refresh(&mut self) {
        self.close_props();
        self.load_devices();
        // default expansion: root + every category
        self.expanded = 1u64 << 0;
        for i in 0..self.n {
            if self.nodes[i].kind == tree::KIND_CATEGORY {
                self.expanded |= 1u64 << i;
            }
        }
        self.rebuild();
    }

    fn load_devices(&mut self) {
        let mut index = 0u16;
        self.n = 0;
        loop {
            let r = match sinfo::devices(self.sys_port, index) {
                Some(r) => r,
                None => break,
            };
            let total = r.h.total as usize;
            for i in 0..(r.h.n as usize) {
                if self.n < MAX_NODES {
                    self.nodes[self.n] = r.rec[i];
                    self.n += 1;
                }
            }
            index = index.saturating_add((r.h.n as u16).max(1));
            if (index as usize) >= total || r.h.n == 0 || self.n >= MAX_NODES {
                break;
            }
        }
    }

    fn rebuild(&mut self) {
        self.row_count = tree::flatten(&self.nodes, self.n, self.expanded, ROOT_PARENT, &mut self.rows);
        if self.selected >= self.row_count && self.row_count > 0 {
            self.selected = self.row_count - 1;
        }
    }

    fn ensure_visible(&mut self) {
        let visible = ((CH - 60 - 24) / (tree::ROW_H as i32)) as usize;
        if self.selected < self.scroll {
            self.scroll = self.selected;
        } else if self.selected >= self.scroll + visible {
            self.scroll = self.selected + 1 - visible;
        }
    }

    fn open_props(&mut self, dev: usize) {
        if dev >= self.n || self.nodes[dev].parent == ROOT_PARENT {
            return; // no properties for the root computer node
        }
        if self.prop_win.is_none() {
            let win = match wm::Win::create_ex(
                self.main.port,
                PROP_X,
                PROP_Y,
                PROP_CW + 2,
                PROP_CH + wm::TITLE_H,
                ui::l("设备属性", "Device Properties"),
                wm::CREATE_MODAL,
            ) {
                Some(w) => w,
                None => return,
            };
            self.prop_win = Some(win);
        }
        self.props.show(dev);
        self.redraw = true;
    }

    fn close_props(&mut self) {
        if let Some(w) = self.prop_win.take() {
            w.destroy();
        }
        self.props.open = false;
        self.redraw = true;
    }

    /* ---- main window drawing ------------------------------------------- */

    fn draw_main(&mut self) {
        let w = &mut self.main;
        let cw = CW as i16;
        let ch = CH as i16;
        w.clear();
        w.fill(0, 0, cw, ch, ui::bg());

        // menu bar
        w.fill(0, 0, cw, 22, ui::card_bg());
        w.fill(0, 21, cw, 1, ui::line());
        let mut mx = 8i16;
        for (i, item) in MENU_ITEMS.iter().enumerate() {
            let label = ui::l(item, item);
            if self.menu == i as i8 {
                w.round_fill(mx - 4, 2, ui::width(label) + 12, 18, 3, ui::row_sel());
            }
            w.text(mx, 5, if self.menu == i as i8 { ui::text() } else { ui::text() }, label);
            mx += ui::width(label) + 18;
        }
        if self.menu >= 0 {
            let m = self.menu as usize;
            let items = Self::menu_items(m);
            let mx = Self::menu_x(m);
            let h = (items.len() as i16) * 26 + 8;
            w.round_fill(mx, 22, 168, h, 6, ui::card_bg());
            w.round_frame(mx, 22, 168, h, 6, ui::line());
            for (i, it) in items.iter().enumerate() {
                let y = 30 + (i as i16) * 26;
                w.text(mx + 12, y, ui::text(), ui::l(it, it));
            }
        }

        // toolbar
        w.fill(0, 22, cw, 36, ui::subtle());
        w.fill(0, 57, cw, 1, ui::line());
        let tools = [
            ui::l("刷新", "Refresh"),
            ui::l("展开", "Expand"),
            ui::l("折叠", "Collapse"),
            ui::l("属性", "Properties"),
        ];
        let mut tx = 12i16;
        for t in tools.iter() {
            let bw = ui::width(t) + 24;
            w.round_fill(tx, 28, bw, 24, 5, ui::card_bg());
            w.round_frame(tx, 28, bw, 24, 5, ui::line());
            w.text(tx + 12, 34, ui::text(), t);
            tx += bw + 8;
        }

        // tree area
        let tree_y = 60i16;
        let tree_h = ch - 24 - tree_y;
        tree::draw(w, 0, tree_y, cw, tree_h, &self.nodes, self.n, &self.rows,
                   self.row_count, self.expanded, self.scroll, self.selected, self.hover);

        // status bar
        let sy = ch - 24;
        w.fill(0, sy, cw, 24, ui::card_bg());
        w.fill(0, sy, cw, 1, ui::line());
        w.text(12, sy + 6, ui::dim(),
               ui::l("选择设备后按 Enter 查看属性", "Select a device and press Enter for properties"));
        let mut buf = [0u8; 16];
        let s = ui::fmt_u32(self.n as u32, &mut buf);
        let label = ui::l("设备: ", "Devices: ");
        w.text(cw - 12 - (ui::width(label) + ui::width(s)), sy + 6, ui::dim(), label);
        w.text(cw - 12 - ui::width(s), sy + 6, ui::dim(), s);

        w.invalidate();
    }

    fn menu_x(idx: usize) -> i16 {
        let mut mx = 8i16;
        for i in 0..idx {
            let label = ui::l(MENU_ITEMS[i], MENU_ITEMS[i]);
            mx += ui::width(label) + 18;
        }
        mx - 4
    }

    fn menu_items(idx: usize) -> &'static [&'static str] {
        match idx {
            0 => &["退出"],
            1 => &["属性", "刷新"],
            2 => &["展开全部", "折叠全部"],
            3 => &["关于设备管理器"],
            _ => &[],
        }
    }

    /* ---- main window input --------------------------------------------- */

    fn main_mouse(&mut self, x: i32, y: i32, down: bool) {
        if !down {
            return;
        }
        let _cw = CW as i16;
        // menu dropdown is on top of everything
        if self.menu >= 0 {
            let items = Self::menu_items(self.menu as usize);
            let mx = Self::menu_x(self.menu as usize);
            let h = (items.len() as i16) * 26 + 8;
            if x >= mx as i32 && x < (mx + 168) as i32 && y >= 22 && y < (22 + h) as i32 {
                let i = ((y - 22 - 8) / 26) as usize;
                if i < items.len() {
                    self.menu_action(self.menu as usize, i);
                }
            }
            self.menu = -1;
            self.redraw = true;
            return;
        }
        if y < 22 {
            // menu bar: open the clicked menu
            let mut mx = 8i16;
            for (i, item) in MENU_ITEMS.iter().enumerate() {
                let label = ui::l(item, item);
                let next = mx + ui::width(label) + 18;
                if x >= mx as i32 && x < next as i32 {
                    self.menu = i as i8;
                    self.redraw = true;
                    return;
                }
                mx = next;
            }
            return;
        }
        if y >= 28 && y < 52 {
            // toolbar
            let tools = [
                ui::l("刷新", "Refresh"),
                ui::l("展开", "Expand"),
                ui::l("折叠", "Collapse"),
                ui::l("属性", "Properties"),
            ];
            let mut tx = 12i16;
            for (i, t) in tools.iter().enumerate() {
                let bw = ui::width(t) + 24;
                if x >= tx as i32 && x < (tx + bw) as i32 {
                    self.tool_action(i);
                    return;
                }
                tx += bw + 8;
            }
            return;
        }
        let tree_y = 60i16;
        let tree_h = CH as i16 - 24 - tree_y;
        if y >= tree_y as i32 && self.row_count > 0 {
            let (row, on_arrow) = tree::hit(0, tree_y, tree_h, self.row_count, self.scroll, x, y);
            if row != NO_ROW {
                self.selected = row;
                if on_arrow && self.rows[row].has_child {
                    self.expanded ^= 1u64 << self.rows[row].idx;
                    self.rebuild();
                } else if self.rows[row].idx != 0 && self.nodes[self.rows[row].idx].kind != tree::KIND_CATEGORY {
                    // double-click -> open properties
                    if self.click_row == row && self.tick - self.click_tick < 40 {
                        self.open_props(self.rows[row].idx);
                        self.click_row = NO_ROW;
                    } else {
                        self.click_row = row;
                        self.click_tick = self.tick;
                    }
                }
                self.ensure_visible();
                self.redraw = true;
            }
        }
    }

    fn menu_action(&mut self, menu: usize, item: usize) {
        match (menu, item) {
            (0, 0) => {
                self.main.destroy();
                sys::exit(0);
            }
            (1, 0) => {
                if self.row_count > 0 && self.selected < self.row_count {
                    self.open_props(self.rows[self.selected].idx);
                }
            }
            (1, 1) => self.refresh(),
            (2, 0) => {
                self.expanded = 1u64 << 0;
                for i in 0..self.n {
                    if self.nodes[i].kind == tree::KIND_CATEGORY {
                        self.expanded |= 1u64 << i;
                    }
                }
                self.rebuild();
            }
            (2, 1) => {
                self.expanded = 1u64 << 0; // keep only the root expanded
                self.rebuild();
            }
            (3, 0) => {
                self.props.show(0); // show root "about" device quietly? keep no-op
                self.props.open = false;
            }
            _ => {}
        }
        self.redraw = true;
    }

    fn tool_action(&mut self, i: usize) {
        match i {
            0 => self.refresh(),
            1 => {
                self.expanded = 1u64 << 0;
                for k in 0..self.n {
                    if self.nodes[k].kind == tree::KIND_CATEGORY {
                        self.expanded |= 1u64 << k;
                    }
                }
                self.rebuild();
            }
            2 => {
                self.expanded = 1u64 << 0;
                self.rebuild();
            }
            3 => {
                if self.row_count > 0 && self.selected < self.row_count {
                    self.open_props(self.rows[self.selected].idx);
                }
            }
            _ => {}
        }
        self.redraw = true;
    }

    fn main_key(&mut self, key: u32) {
        let visible = ((CH - 60 - 24) / (tree::ROW_H as i32)) as usize;
        match key {
            wm::KEY_DOWN => {
                if self.selected + 1 < self.row_count {
                    self.selected += 1;
                    self.ensure_visible();
                }
            }
            wm::KEY_UP => {
                if self.selected > 0 {
                    self.selected -= 1;
                    self.ensure_visible();
                }
            }
            wm::KEY_ENTER => {
                if self.row_count > 0 && self.selected < self.row_count {
                    self.open_props(self.rows[self.selected].idx);
                }
            }
            wm::KEY_ESC => {
                if self.menu >= 0 {
                    self.menu = -1;
                }
            }
            _ => return,
        }
        let _ = visible;
        self.redraw = true;
    }

    /* ---- property window drawing/input ---------------------------------- */

    fn draw_props(&mut self) {
        if let Some(win) = self.prop_win.as_mut() {
            if self.props.dev < self.n {
                self.props.draw(win, self.sys_port, &self.nodes[self.props.dev],
                                PROP_CW as i16, PROP_CH as i16);
                win.invalidate();
            }
        }
    }

    fn prop_mouse(&mut self, x: i32, y: i32, down: bool) {
        if !down {
            return;
        }
        if self.props.dev < self.n {
            let close = self.props.click(&self.nodes[self.props.dev], PROP_CW as i16, PROP_CH as i16, x, y);
            if close {
                self.close_props();
                return;
            }
        }
        self.redraw = true;
    }

    /* ---- event loop ---------------------------------------------------- */

    pub fn run(&mut self) {
        loop {
            while let Some((wid, ev)) = self.main.next_event_any() {
                if wid == self.main.id {
                    match ev {
                        wm::Ev::Close => {
                            self.main.destroy();
                            sys::exit(0);
                        }
                        wm::Ev::Mouse(m) => {
                            if m.kind == wm::MOUSE_DOWN && m.button == 1 {
                                self.main_mouse(m.x as i32, m.y as i32, true);
                            } else if m.kind == 1 {
                                // hover tracking for the tree highlight
                                let tree_y = 60i16;
                                let tree_h = CH as i16 - 24 - tree_y;
                                let (row, _) = tree::hit(0, tree_y, tree_h, self.row_count,
                                                        self.scroll, m.x as i32, m.y as i32);
                                if row != self.hover {
                                    self.hover = row;
                                    self.redraw = true;
                                }
                            }
                        }
                        wm::Ev::Key(k, _mods) => self.main_key(k),
                        _ => {}
                    }
                } else if let Some(win) = self.prop_win.as_ref() {
                    if wid == win.id {
                        match ev {
                            wm::Ev::Close => self.close_props(),
                            wm::Ev::Mouse(m) => {
                                let down = m.kind == wm::MOUSE_DOWN && m.button == 1;
                                self.prop_mouse(m.x as i32, m.y as i32, down);
                            }
                            _ => {}
                        }
                    }
                }
            }
            if ui::poll(self.sys_port) {
                self.redraw = true;
            }
            if self.redraw {
                self.draw_main();
                if self.props.open {
                    self.draw_props();
                }
                self.redraw = false;
            }
            sys::yield_now();
            self.tick = self.tick.wrapping_add(1);
        }
    }
}
