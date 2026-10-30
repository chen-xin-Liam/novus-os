//! The modal property window: five tabs (General / Driver / Details / Events
//! / Resources) drawn with the WM's rounded primitives, plus the hit-testing
//! that makes the tabs, the details dropdown, the buttons and the checkbox
//! interactive.
//!
//! Every tab pages its fields through the sysinfo device-field request, cached
//! in this struct so a repaint never re-fetches. The cache is at most
//! `CACHE_N` fields - enough for any tab the kernel builds.

use crate::sinfo::DevField;
use crate::ui;
use crate::wm;
use crate::devmgr::fields::*;

pub const CACHE_N: usize = 24;

pub struct Props {
    pub open: bool,
    pub dev: usize,
    pub tab: u8,
    pub field_sel: usize,
    pub dd_open: bool,
    cache_dev: usize,
    cache_tab: u8,
    cache: [DevField; CACHE_N],
    cache_n: usize,
}

impl Props {
    pub fn new() -> Props {
        Props {
            open: false,
            dev: 0,
            tab: TAB_GENERAL,
            field_sel: 0,
            dd_open: false,
            cache_dev: 0xFFFF,
            cache_tab: 0xFF,
            cache: unsafe { core::mem::zeroed() },
            cache_n: 0,
        }
    }

    /// Open for `dev`, resetting the tab to General.
    pub fn show(&mut self, dev: usize) {
        self.open = true;
        self.dev = dev;
        self.tab = TAB_GENERAL;
        self.field_sel = 0;
        self.dd_open = false;
        self.cache_dev = 0xFFFF;
        self.cache_tab = 0xFF;
        self.cache_n = 0;
    }

    /// Fetch the fields for `(dev, tab)` into the cache, once per tab switch.
    fn load(&mut self, port: u64, dev: u16, tab: u8) {
        if self.cache_dev == dev as usize && self.cache_tab == tab && self.cache_n > 0 {
            return;
        }
        let mut got = 0usize;
        let mut index = 0u8;

        loop {
            let r = match crate::sinfo::dev_fields(port, dev, tab, index) {
                Some(r) => r,
                None => break,
            };
            let total = r.h.total as usize;
            for i in 0..(r.h.n as usize) {
                if got < CACHE_N {
                    self.cache[got] = r.rec[i];
                    got += 1;
                }
            }
            index = index.saturating_add(r.h.n.max(1));
            if (index as usize) >= total || r.h.n == 0 || got >= CACHE_N {
                break;
            }
        }
        self.cache_dev = dev as usize;
        self.cache_tab = tab;
        self.cache_n = got;
        if self.field_sel >= got {
            self.field_sel = 0;
        }
    }

    /// First cached field with `key`, or the empty string.
    fn field(&self, key: u8) -> &str {
        for i in 0..self.cache_n {
            if self.cache[i].key == key {
                return self.cache[i].value();
            }
        }
        ""
    }

    /// Count of distinct property keys in the (Details) cache, in order.
    fn field_count(&self) -> usize {
        self.cache_n
    }

    /// The key at position `i` in the cache (for the Details dropdown).
    fn field_key(&self, i: usize) -> u8 {
        if i < self.cache_n {
            self.cache[i].key
        } else {
            0
        }
    }

    /* ---- layout helpers ------------------------------------------------- */

    const TAB_H: i16 = 44;

    fn tab_rect(cw: i16, i: usize) -> (i16, i16, i16, i16) {
        let w = cw / 5;
        (i as i16 * w, 6, w, 34)
    }

    fn footer(cw: i16, ch: i16) -> (i16, i16, i16, i16) {
        (cw - 176, ch - 40, 80, 28)
    }

    fn footer_cancel(cw: i16, ch: i16) -> (i16, i16, i16, i16) {
        (cw - 88, ch - 40, 80, 28)
    }

    fn dd_rect(cw: i16) -> (i16, i16, i16, i16) {
        (24, 84, cw - 48, 28)
    }

    fn inside(r: (i16, i16, i16, i16), px: i32, py: i32) -> bool {
        px >= r.0 as i32 && px < (r.0 + r.2) as i32 && py >= r.1 as i32 && py < (r.1 + r.3) as i32
    }

    /* ---- painting ------------------------------------------------------- */

    pub fn draw(&mut self, w: &mut wm::Win, port: u64, node: &crate::sinfo::DevRec, cw: i16, ch: i16) {
        self.load(port, self.dev as u16, self.tab);
        w.fill(0, 0, cw, ch, ui::bg());
        w.fill(0, 0, cw, 1, ui::accent());

        // tab strip
        w.fill(0, 0, cw, Self::TAB_H, ui::bg());
        w.fill(0, Self::TAB_H - 1, cw, 1, ui::line());
        for i in 0..5usize {
            let r = Self::tab_rect(cw, i);
            let (zh, en) = TAB_NAMES[i];
            let label = ui::l(zh, en);
            if self.tab == i as u8 {
                w.text(r.0 + (r.2 - ui::width(label)) / 2, r.1 + 9, ui::text(), label);
                w.round_fill(r.0, r.1 + r.3 - 2, r.2, 3, 1, ui::accent());
            } else {
                w.text(r.0 + (r.2 - ui::width(label)) / 2, r.1 + 9, ui::dim(), label);
            }
        }

        let top = Self::TAB_H + 8;
        match self.tab {
            TAB_GENERAL => self.draw_general(w, node, cw, ch, top),
            TAB_DRIVER => self.draw_driver(w, cw, ch, top),
            TAB_DETAILS => self.draw_details(w, cw, ch, top),
            TAB_EVENTS => self.draw_events(w, cw, ch, top),
            TAB_RESOURCES => self.draw_resources(w, cw, ch, top),
            _ => {}
        }

        // footer
        let (okx, oky, okw, okh) = Self::footer(cw, ch);
        let (ccx, ccy, ccw, cch) = Self::footer_cancel(cw, ch);
        w.round_fill(okx, oky, okw, okh, 6, ui::accent());
        w.text(okx + (okw - ui::width(ui::l("确定", "OK"))) / 2, oky + (okh - 14) / 2,
               ui::on_accent(), ui::l("确定", "OK"));
        w.round_fill(ccx, ccy, ccw, cch, 6, ui::card_bg());
        w.round_frame(ccx, ccy, ccw, cch, 6, ui::line());
        w.text(ccx + (ccw - ui::width(ui::l("取消", "Cancel"))) / 2, ccy + (cch - 14) / 2,
               ui::text(), ui::l("取消", "Cancel"));
    }

    fn draw_general(&mut self, w: &mut wm::Win, node: &crate::sinfo::DevRec, cw: i16, _ch: i16, top: i16) {
        let (zh, _en) = class_label(node.class_id);
        // icon + name
        w.round_fill(20, top + 4, 30, 30, 6, cat_color(node.class_id));
        let name = node.name();
        w.text(60, top + 6, ui::text(), ui::fit(name, cw - 80));
        w.text(60, top + 26, ui::dim(), ui::l(zh, zh));

        let devtype = ui::l(class_label(node.class_id).0, class_label(node.class_id).1);
        let manuf = self.field(F_MANUF);
        let loc = self.field(F_LOCATION);
        self.kv(w, 24, top + 56, ui::l("设备类型", "Device type"), devtype);
        self.kv(w, 24, top + 84, ui::l("制造商", "Manufacturer"), ui::fit(manuf, cw - 170));
        self.kv(w, 24, top + 112, ui::l("位置", "Location"), ui::fit(loc, cw - 170));

        // device status box
        let bx = 20i16;
        let by = top + 150;
        let bw = cw - 40;
        let bh = 190i16;
        w.round_fill(bx, by, bw, bh, 6, ui::card_bg());
        w.round_frame(bx, by, bw, bh, 6, ui::line());
        w.text(bx + 10, by + 10, ui::dim(), ui::l("设备状态", "Device status"));
        let (zhs, ens) = status_text(node.status, node.flags);
        let msg = ui::l(zhs, ens);
        let color = if node.status == ST_WARN { ui::warn() } else { ui::good() };
        draw_wrapped(w, bx + 10, by + 36, bw - 30, 22, color, msg);
        // static scrollbar on the right edge
        w.round_fill(bx + bw - 8, by + 8, 4, bh - 16, 2, ui::line());
        w.round_fill(bx + bw - 9, by + 10, 6, 40, 3, ui::dim());
    }

    fn draw_driver(&mut self, w: &mut wm::Win, cw: i16, _ch: i16, top: i16) {
        self.kv(w, 24, top + 6, ui::l("驱动程序提供商", "Driver provider"), ui::fit(self.field(F_PROVIDER), cw - 170));
        self.kv(w, 24, top + 34, ui::l("驱动程序日期", "Driver date"), self.field(F_DRVDATE));
        self.kv(w, 24, top + 62, ui::l("驱动程序版本", "Driver version"), self.field(F_DRVVER));
        self.kv(w, 24, top + 90, ui::l("数字签名", "Digital signer"), ui::fit(self.field(F_SIGNER), cw - 170));

        // five action buttons, the first one in accent (the others disabled)
        let bx = 24i16;
        let bw = cw - 270;
        let bh = 30i16;
        let labels = [
            ui::l("驱动程序详细信息(I)", "Driver details (I)"),
            ui::l("更新驱动程序(P)", "Update driver (P)"),
            ui::l("回退驱动程序(R)", "Roll back driver (R)"),
            ui::l("禁用设备(D)", "Disable device (D)"),
            ui::l("卸载设备(U)", "Uninstall device (U)"),
        ];
        let notes = [
            ui::l("显示此设备上安装的驱动程序文件。", "Show the driver files installed on this device."),
            ui::l("搜索并安装此设备的新驱动程序。", "Search for and install a newer driver for this device."),
            ui::l("将驱动程序还原到以前安装的版本。", "Restore the driver to a previously installed version."),
            ui::l("停止此设备，使其不再工作，直到重新启用。", "Stop this device from working until you re-enable it."),
            ui::l("从系统中删除此设备的驱动程序。", "Remove this device's driver from the system."),
        ];
        for i in 0..5usize {
            let by = top + 130 + (i as i16) * 36;
            if i == 0 {
                w.round_fill(bx, by, bw, bh, 6, ui::accent());
                w.text(bx + 10, by + 8, ui::on_accent(), ui::fit(labels[i], bw - 20));
            } else {
                w.round_fill(bx, by, bw, bh, 6, ui::subtle());
                w.text(bx + 10, by + 8, ui::dim(), ui::fit(labels[i], bw - 20));
            }
            let ny = by + 8;
            draw_wrapped(w, bx + bw + 16, ny, cw - (bx + bw + 16) - 20, 18, ui::dim(), notes[i]);
        }
    }

    fn draw_details(&mut self, w: &mut wm::Win, cw: i16, _ch: i16, top: i16) {
        w.text(24, top + 2, ui::dim(), ui::l("属性(P)", "Property (P)"));
        let r = Self::dd_rect(cw);
        w.round_fill(r.0, r.1, r.2, r.3, 5, ui::card_bg());
        w.round_frame(r.0, r.1, r.2, r.3, 5, if self.dd_open { ui::accent() } else { ui::line() });
        let key = self.field_key(self.field_sel);
        let (zh, en) = field_label(key);
        w.text(r.0 + 10, r.1 + 7, ui::text(), ui::l(zh, en));
        w.text(r.0 + r.2 - 18, r.1 + 7, ui::dim(), if self.dd_open { "v" } else { ">" });

        if self.dd_open {
            let n = self.field_count();
            let list_h = (n as i16) * 22 + 6;
            let ly = r.1 + r.3;
            w.round_fill(r.0, ly, r.2, list_h, 5, ui::card_bg());
            w.round_frame(r.0, ly, r.2, list_h, 5, ui::line());
            for i in 0..n {
                let ky = ly + 4 + (i as i16) * 22;
                if i == self.field_sel {
                    w.round_fill(r.0 + 2, ky, r.2 - 4, 20, 3, ui::row_sel());
                }
                let (lz, le) = field_label(self.field_key(i));
                w.text(r.0 + 10, ky + 4, ui::text(), ui::l(lz, le));
            }
        }

        // value box
        let bx = 24i16;
        let by = r.1 + r.3 + (if self.dd_open { (self.field_count() as i16) * 22 + 10 } else { 16 });
        let bw = cw - 48;
        let bh = 220i16;
        w.round_fill(bx, by, bw, bh, 6, ui::card_bg());
        w.round_frame(bx, by, bw, bh, 6, ui::line());
        let val = self.field(key);
        draw_wrapped(w, bx + 10, by + 12, bw - 24, 20, ui::text(), val);
        w.round_fill(bx + bw - 8, by + 8, 4, bh - 16, 2, ui::line());
        w.round_fill(bx + bw - 9, by + 10, 6, 40, 3, ui::dim());
    }

    fn draw_events(&mut self, w: &mut wm::Win, cw: i16, _ch: i16, top: i16) {
        w.fill(24, top + 2, cw - 48, 20, ui::subtle());
        w.text(30, top + 6, ui::dim(), ui::l("时间戳", "Time"));
        w.text(220, top + 6, ui::dim(), ui::l("描述", "Description"));
        let mut rows = 0usize;
        let sel = 0usize;
        for i in 0..self.cache_n {
            if self.cache[i].key != F_EVTIME {
                continue;
            }
            let ry = top + 26 + (rows as i16) * 22;
            if rows == sel {
                w.round_fill(24, ry, cw - 48, 20, 3, ui::row_sel());
            }
            w.text(30, ry + 4, ui::text(), self.cache[i].value());
            rows += 1;
        }
        // info box for the selected event
        let mut ev = 0usize;
        for i in 0..self.cache_n {
            if self.cache[i].key == F_EVDESC && ev == sel {
                let bx = 24i16;
                let by = top + 26 + (rows as i16) * 22 + 16;
                w.round_fill(bx, by, cw - 48, 120, 6, ui::card_bg());
                w.round_frame(bx, by, cw - 48, 120, 6, ui::line());
                w.text(bx + 10, by + 10, ui::dim(), ui::l("信息(I)", "Information (I)"));
                draw_wrapped(w, bx + 10, by + 34, cw - 80, 20, ui::text(), self.cache[i].value());
                break;
            }
            if self.cache[i].key == F_EVDESC {
                ev += 1;
            }
        }
        let (vx, vy, vw, vh) = (24i16, top + 26 + (rows as i16) * 22 + 150, cw - 240, 28);
        w.round_fill(vx, vy, vw, vh, 6, ui::subtle());
        w.text(vx + 10, vy + 7, ui::dim(), ui::fit(ui::l("查看所有事件(V)...", "View all events (V)..."), vw - 20));
    }

    fn draw_resources(&mut self, w: &mut wm::Win, cw: i16, _ch: i16, top: i16) {
        w.text(24, top + 2, ui::dim(), ui::l("资源设置(R)", "Resource settings (R)"));
        w.fill(24, top + 22, cw - 48, 20, ui::subtle());
        w.text(30, top + 26, ui::dim(), ui::l("资源类型", "Resource type"));
        w.text(300, top + 26, ui::dim(), ui::l("设置", "Setting"));
        let mut rows = 0usize;
        for i in 0..self.cache_n {
            let key = self.cache[i].key;
            if key != F_MEMRANGE && key != F_IORANGE && key != F_IRQ {
                continue;
            }
            let ry = top + 46 + (rows as i16) * 22;
            let (tz, te) = field_label(key);
            w.text(30, ry + 4, ui::text(), ui::l(tz, te));
            w.text(300, ry + 4, ui::dim(), ui::fit(self.cache[i].value(), cw - 320));
            rows += 1;
        }
        // use automatic settings checkbox
        let cbx = 24i16;
        let cby = top + 46 + (rows as i16) * 22 + 14;
        w.round_frame(cbx, cby, 16, 16, 3, ui::line());
        w.text(cbx, cby - 1, ui::accent(), "x");
        w.text(cbx + 22, cby + 1, ui::text(), ui::l("使用自动设置(U)", "Use automatic settings (U)"));
        w.text(24, cby + 30, ui::dim(), ui::l("设置基于(B)", "Setting based on (B)"));
        // conflict list
        let bx = 24i16;
        let by = cby + 60;
        w.round_fill(bx, by, cw - 48, 60, 6, ui::card_bg());
        w.round_frame(bx, by, cw - 48, 60, 6, ui::line());
        w.text(bx + 10, by + 8, ui::dim(), ui::l("冲突设备列表", "Conflicting device list"));
        w.text(bx + 10, by + 32, ui::text(), ui::l("没有冲突。", "No conflicts."));
    }

    fn kv(&self, w: &mut wm::Win, x: i16, y: i16, label: &str, value: &str) {
        w.text(x, y, ui::dim(), label);
        w.text(x + 120, y, ui::text(), value);
    }

    /* ---- interaction ---------------------------------------------------- */

    /// Handle a mouse press; returns true when the window should close.
    pub fn click(&mut self, _node: &crate::sinfo::DevRec, cw: i16, ch: i16, px: i32, py: i32) -> bool {
        if Self::inside(Self::footer(cw, ch), px, py) {
            return true;
        }
        if Self::inside(Self::footer_cancel(cw, ch), px, py) {
            return true;
        }
        // tab strip
        if py >= 6 && py < Self::TAB_H as i32 && px >= 0 && px < cw as i32 {
            let w = cw / 5;
            let t = (px / w as i32) as usize;
            if t < 5 {
                if self.tab != t as u8 {
                    self.tab = t as u8;
                    self.field_sel = 0;
                    self.dd_open = false;
                    self.cache_dev = 0xFFFF;
                    self.cache_tab = 0xFF;
                    self.cache_n = 0;
                }
            }
            return false;
        }
        if self.tab == TAB_DETAILS {
            let r = Self::dd_rect(cw);
            if Self::inside(r, px, py) {
                self.dd_open = !self.dd_open;
                return false;
            }
            if self.dd_open {
                let ly = r.1 + r.3;
                let n = self.field_count() as i32;
                let i = (py - ly as i32) / 22;
                if i >= 0 && i < n {
                    self.field_sel = i as usize;
                    self.dd_open = false;
                    return false;
                }
            }
        }
        false
    }
}
