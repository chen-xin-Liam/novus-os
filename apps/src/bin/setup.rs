//! Novus OS - graphical installer (M10).
//!
//! Six pages over the kernel's install service: welcome, disk, plan,
//! confirm, progress, result. The app owns no disk logic - it asks the
//! kernel for candidates, shows why a disk is greyed out, and drives the
//! copy one bounded step at a time so this window keeps repainting.
//!
//! Destructive actions are gated twice: the kernel only accepts calls
//! from the process apps_launch() started as "setup", and this app
//! requires an explicit tick on the confirmation page.

#![no_std]
#![no_main]

use novus_apps::{install, ipc, sinfo, sys, ui, wm};

const CW: i16 = 900;
const CH: i16 = 600;
const SIDE: i16 = 240;
const CONTENT_X: i16 = 264;
const CONTENT_W: i16 = 612;
const FOOT_Y: i16 = 548;

const MAX_DISKS: usize = 8;

#[derive(Clone, Copy, PartialEq)]
enum Page {
    Welcome,
    Disk,
    Plan,
    Confirm,
    Progress,
    Done,
    Fail,
}

const STEPS: [&str; 6] = ["欢迎", "磁盘", "方案", "确认", "进度", "完成"];

fn step_of(p: Page) -> usize {
    match p {
        Page::Welcome => 0,
        Page::Disk => 1,
        Page::Plan => 2,
        Page::Confirm => 3,
        Page::Progress => 4,
        Page::Done | Page::Fail => 5,
    }
}

struct App {
    win: wm::Win,
    page: Page,
    disks: [install::Disk; MAX_DISKS],
    slot: [u8; MAX_DISKS], /* block-table index of each listed disk */
    n_disks: usize,
    sel: usize,
    confirmed: bool,
    started: bool,
    prog: install::Progress,
    sys_port: u64,
    frame: u32,
}

fn fmt_mib(v: u64, buf: &mut [u8]) -> &str {
    let mib = (v / 2048) as u32;
    let mut tmp = [0u8; 12];

    if mib >= 1024 {
        let gb = mib / 1024;
        let rem = (mib % 1024) / 100;
        let s = ui::fmt_u32(gb, &mut tmp);

        for (i, b) in s.as_bytes().iter().enumerate() {
            buf[i] = *b;
        }
        buf[s.len()] = b'.';
        buf[s.len() + 1] = b'0' + rem as u8;
        buf[s.len() + 2] = b' ';
        buf[s.len() + 3] = b'G';
        buf[s.len() + 4] = b'B';
        return unsafe { core::str::from_utf8_unchecked(&buf[..s.len() + 5]) };
    }
    let s = ui::fmt_u32(mib, &mut tmp);

    for (i, b) in s.as_bytes().iter().enumerate() {
        buf[i] = *b;
    }
    buf[s.len()] = b' ';
    buf[s.len() + 1] = b'M';
    buf[s.len() + 2] = b'B';
    unsafe { core::str::from_utf8_unchecked(&buf[..s.len() + 3]) }
}

fn fmt_mb(kb: u32, buf: &mut [u8]) -> &str {
    let mut tmp = [0u8; 12];
    let s = ui::fmt_u32(kb / 1024, &mut tmp);

    for (i, b) in s.as_bytes().iter().enumerate() {
        buf[i] = *b;
    }
    buf[s.len()] = b' ';
    buf[s.len() + 1] = b'M';
    buf[s.len() + 2] = b'B';
    unsafe { core::str::from_utf8_unchecked(&buf[..s.len() + 3]) }
}

fn scan(app: &mut App) {
    let n = install::count();

    app.n_disks = 0;
    app.sel = 0;
    let mut picked = false;
    let mut i = 0i64;
    while i < n && app.n_disks < MAX_DISKS {
        if let Some(d) = install::disk(i as u8) {
            if d.eligible != 0 && !picked {
                app.sel = app.n_disks;
                picked = true;
            }
            app.disks[app.n_disks] = d;
            app.slot[app.n_disks] = i as u8;
            app.n_disks += 1;
        }
        i += 1;
    }
}

fn sidebar(w: &mut wm::Win, page: Page) {
    w.fill(0, 0, SIDE, CH, ui::card_bg());
    w.fill(SIDE - 1, 0, 1, CH, ui::line());
    w.text_big(24, 22, ui::text(), "Novus OS");
    w.text(24, 50, ui::dim(), "系统安装程序");

    let cur = step_of(page);
    let mut i = 0;
    while i < 6 {
        let y = 96 + i as i16 * 46;
        let done = i < cur;
        let active = i == cur;

        if active {
            w.round_fill(20, y - 6, SIDE - 40, 38, 8, ui::row_sel());
        }
        if done {
            w.round_fill(28, y, 22, 22, 11, ui::accent());
            w.shape(wm::OP_HLINE, 32, y + 8, 4, 2, ui::on_accent());
            w.shape(wm::OP_HLINE, 35, y + 11, 4, 2, ui::on_accent());
            w.shape(wm::OP_HLINE, 38, y + 6, 5, 2, ui::on_accent());
        } else if active {
            w.round_fill(28, y, 22, 22, 11, ui::accent());
            let mut d = [0u8; 2];
            d[0] = b'1' + i as u8;
            w.text(34, y + 3, ui::on_accent(),
                   unsafe { core::str::from_utf8_unchecked(&d[..1]) });
        } else {
            w.round_frame(28, y, 22, 22, 11, ui::line());
            let mut d = [0u8; 2];
            d[0] = b'1' + i as u8;
            w.text(34, y + 3, ui::dim(),
                   unsafe { core::str::from_utf8_unchecked(&d[..1]) });
        }
        w.text(60, y + 3, if active { ui::text() } else { ui::dim() }, STEPS[i]);
        i += 1;
    }

    w.text(24, CH - 42, ui::dim(), "内核安装服务");
    w.text(24, CH - 24, ui::dim(), "写入前会再次确认");
}

fn header(w: &mut wm::Win, title: &str, subtitle: &str) {
    w.text_big(CONTENT_X, 12, ui::text(), title);
    w.text(CONTENT_X, 52, ui::dim(), subtitle);
    w.fill(CONTENT_X, 78, CONTENT_W, 1, ui::line());
    w.round_fill(CONTENT_X, 78, 44, 3, 2, ui::accent());
}

fn footer_hint(w: &mut wm::Win, hint: &str) {
    w.text(CONTENT_X, FOOT_Y - 22, ui::dim(), hint);
}

fn primary(w: &mut wm::Win, x: i16, label: &str, enabled: bool) -> (i16, i16, i16, i16) {
    let color = if enabled { ui::accent() } else { ui::line() };

    ui::button(w, x, FOOT_Y, 176, 36, label, color)
}

fn step_mark(w: &mut wm::Win, x: i16, y: i16, done: bool, active: bool) {
    if done {
        w.round_fill(x, y, 18, 18, 9, ui::accent());
        w.shape(wm::OP_HLINE, x + 3, y + 8, 3, 2, ui::on_accent());
        w.shape(wm::OP_HLINE, x + 5, y + 11, 3, 2, ui::on_accent());
        w.shape(wm::OP_HLINE, x + 8, y + 7, 5, 2, ui::on_accent());
    } else if active {
        w.round_fill(x, y, 18, 18, 9, ui::accent());
        w.round_fill(x + 7, y + 7, 4, 4, 2, ui::on_accent());
    } else {
        w.round_frame(x, y, 18, 18, 9, ui::line());
    }
}

fn draw_welcome(w: &mut wm::Win) {
    header(w, "欢迎使用", "Novus OS 图形安装程序：四步完成系统安装。");
    w.text(CONTENT_X, 108, ui::text(), "安装程序会把一份完整的系统写入所选磁盘。");
    w.text(CONTENT_X, 132, ui::dim(), "过程不联网，也不改动 UEFI 固件设置。");

    ui::card(w, CONTENT_X, 168, CONTENT_W, 164);
    w.text(CONTENT_X + 20, 188, ui::text(), "安装前请确认");
    w.text(CONTENT_X + 20, 216, ui::dim(), "目标磁盘上的所有数据都会被清除");
    w.text(CONTENT_X + 20, 242, ui::dim(), "安装完成后重启，并从新磁盘启动");
    w.text(CONTENT_X + 20, 268, ui::dim(), "本机需要至少 2 GiB 内存");
    ui::divider(w, CONTENT_X + 20, 292, CONTENT_W - 40);
    w.text(CONTENT_X + 20, 304, ui::dim(), "当前是实时系统，安装不会影响正在运行的这一份。");

    /* What the wizard will do, so the first page is not just a warning. */
    ui::card(w, CONTENT_X, 352, CONTENT_W, 150);
    w.text(CONTENT_X + 20, 372, ui::text(), "安装会做什么");
    let steps = [
        ("1", "选择一块整盘作为目标"),
        ("2", "写入引导分区与系统镜像"),
        ("3", "重建完整性树并校验"),
    ];
    let mut i = 0usize;
    while i < 3 {
        let y = 400 + i as i16 * 30;
        w.round_fill(CONTENT_X + 22, y, 18, 18, 9, ui::row_sel());
        w.text(CONTENT_X + 28, y + 2, ui::accent(), steps[i].0);
        w.text(CONTENT_X + 52, y + 2, ui::dim(), steps[i].1);
        i += 1;
    }

    primary(w, CONTENT_X + CONTENT_W - 176, "下一步", true);
    footer_hint(w, "按 Enter 继续");
}

fn draw_disks(w: &mut wm::Win, disks: &[install::Disk; MAX_DISKS], n_disks: usize, sel: usize) {
    header(w, "选择磁盘", "列出所有整块磁盘；灰色项附有不可选原因。");

    if n_disks == 0 {
        ui::empty(w, CONTENT_X, 110, "没有可用的整块磁盘");
        w.text(CONTENT_X, 138, ui::dim(), "安装需要一块未分区的本机磁盘。");
        primary(w, CONTENT_X + CONTENT_W - 176, "刷新", true);
        return;
    }

    let mut i = 0usize;
    while i < n_disks {
        let d = disks[i];
        let y = 96 + i as i16 * 58;
        let selected = i == sel && d.eligible != 0;

        if selected {
            w.round_fill(CONTENT_X, y, CONTENT_W, 50, 8, ui::row_sel());
            w.round_fill(CONTENT_X, y + 8, 3, 34, 1, ui::accent());
        } else {
            w.round_fill(CONTENT_X, y, CONTENT_W, 50, 8, ui::card_bg());
        }
        w.round_frame(CONTENT_X, y, CONTENT_W, 50, 8, ui::line());
        w.round_fill(CONTENT_X + 18, y + 13, 24, 24, 6,
                     if d.eligible != 0 { ui::accent() } else { ui::line() });
        w.text(CONTENT_X + 30, y + 18, ui::on_accent(), "D");

        w.text(CONTENT_X + 56, y + 10, if d.eligible != 0 { ui::text() } else { ui::dim() }, d.label());
        let mut sz = [0u8; 24];
        w.text(CONTENT_X + 56, y + 28, ui::dim(), fmt_mib(d.sectors, &mut sz));

        let reason = if d.eligible != 0 { "可用" } else { install::reason_text(d.reason) };
        let rw = ui::width(reason);
        w.text(CONTENT_X + CONTENT_W - 18 - rw, y + 18,
               if d.eligible != 0 { ui::good() } else { ui::dim() }, reason);
        i += 1;
    }

    let detail_y = 96 + n_disks as i16 * 58 + 12;
    if sel < n_disks && disks[sel].eligible != 0 {
        ui::card(w, CONTENT_X, detail_y, CONTENT_W, 92);
        w.text(CONTENT_X + 20, detail_y + 16, ui::danger(), "将清除整盘数据");
        w.text(CONTENT_X + 20, detail_y + 42, ui::dim(),
               "安装会重建分区表，原磁盘上的系统与文件将无法恢复。");
        w.text(CONTENT_X + 20, detail_y + 64, ui::dim(),
               "确认磁盘名称与容量后再继续。");
    }

    primary(w, CONTENT_X + CONTENT_W - 176, "下一步",
            sel < n_disks && disks[sel].eligible != 0);
    footer_hint(w, "选择一块标记为“可用”的磁盘");
}

fn draw_plan(w: &mut wm::Win, disk: Option<install::Disk>) {
    header(w, "安装方案", "Novus 使用 GPT：一个 EFI 系统分区和一个 NovusFS 系统分区。");

    ui::card(w, CONTENT_X, 96, CONTENT_W, 190);
    w.text(CONTENT_X + 20, 116, ui::text(), "分区布局");
    ui::divider(w, CONTENT_X + 20, 146, CONTENT_W - 40);

    w.round_fill(CONTENT_X + 20, 164, 10, 10, 3, ui::accent());
    w.text(CONTENT_X + 40, 160, ui::text(), "EFI 系统分区");
    w.text(CONTENT_X + 40, 180, ui::dim(), "引导加载器与系统映像模板（FAT）");

    w.round_fill(CONTENT_X + 20, 210, 10, 10, 3, ui::good());
    w.text(CONTENT_X + 40, 206, ui::text(), "NovusFS 系统分区");
    w.text(CONTENT_X + 40, 226, ui::dim(), "带 WAL 与 Merkle 完整性树的系统镜像");

    w.text(CONTENT_X + 20, 254, ui::dim(), "分区大小由镜像自动计算，按 1 MiB 对齐。");

    if let Some(d) = disk {
        let mut sz = [0u8; 24];

        ui::card(w, CONTENT_X, 306, CONTENT_W, 84);
        w.text(CONTENT_X + 20, 326, ui::dim(), "目标磁盘");
        w.text(CONTENT_X + 20, 350, ui::text(), d.label());
        let s = fmt_mib(d.sectors, &mut sz);
        let sw = ui::width(s);
        w.text(CONTENT_X + CONTENT_W - 20 - sw, 350, ui::dim(), s);
    }

    primary(w, CONTENT_X + CONTENT_W - 176, "继续", true);
    footer_hint(w, "下一步需要确认整盘擦除");
}

fn draw_confirm(w: &mut wm::Win, disk: Option<install::Disk>, confirmed: bool) {
    header(w, "确认安装", "这是最后一次可以返回的页面。");

    if let Some(d) = disk {
        let mut sz = [0u8; 24];

        ui::card(w, CONTENT_X, 96, CONTENT_W, 150);
        w.text(CONTENT_X + 20, 116, ui::text(), "即将安装到");
        w.text_big(CONTENT_X + 20, 144, ui::text(), d.label());
        let s = fmt_mib(d.sectors, &mut sz);
        w.text(CONTENT_X + 20, 184, ui::dim(), s);
        w.text(CONTENT_X + 20, 206, ui::dim(), "分区表、引导分区与系统分区都将被重建");
    }

    w.round_fill(CONTENT_X, 266, CONTENT_W, 96, 10, if ui::is_dark() { 0x003A1E1E } else { 0x00FDF0EF });
    w.round_frame(CONTENT_X, 266, CONTENT_W, 96, 10, ui::danger());
    w.text(CONTENT_X + 20, 286, ui::danger(), "整盘数据将被永久清除");
    w.text(CONTENT_X + 20, 312, ui::dim(), "无法撤销，也无法从本安装程序恢复。");
    w.text(CONTENT_X + 20, 334, ui::dim(), "请确保已备份需要的文件。");

    let box_x = CONTENT_X + 20;
    let box_y = 388;
    w.round_fill(box_x, box_y, 22, 22, 5, if confirmed { ui::accent() } else { ui::card_bg() });
    w.round_frame(box_x, box_y, 22, 22, 5, if confirmed { ui::accent() } else { ui::line() });
    if confirmed {
        w.shape(wm::OP_HLINE, box_x + 5, box_y + 11, 13, 1, ui::on_accent());
    }
    w.text(box_x + 34, box_y + 4, ui::text(), "我已知晓：目标磁盘上的全部数据会被清除");

    ui::ghost(w, CONTENT_X, FOOT_Y, 140, 36, "上一步");
    primary(w, CONTENT_X + CONTENT_W - 176, "开始安装", confirmed);
    footer_hint(w, "勾选确认后才能开始");
}

fn phase_name(phase: u8) -> &'static str {
    match phase {
        install::PHASE_TABLE => "写入分区表",
        install::PHASE_ESP => "写入引导分区",
        install::PHASE_SYSTEM => "写入系统分区",
        install::PHASE_VERIFY => "校验完整性树",
        install::PHASE_DONE => "安装完成",
        _ => "准备中",
    }
}

fn draw_progress(w: &mut wm::Win, prog: &install::Progress) {
    header(w, "正在安装", "请勿关闭电源或拔出目标磁盘。");

    let phases = [install::PHASE_TABLE, install::PHASE_ESP, install::PHASE_SYSTEM, install::PHASE_VERIFY];
    let labels = ["分区表", "引导分区", "系统分区", "完整性校验"];
    let cur = prog.phase;
    let mut i = 0usize;
    while i < 4 {
        let y = 100 + i as i16 * 40;
        let done = cur > phases[i] || cur == install::PHASE_DONE;
        let active = cur == phases[i];

        step_mark(w, CONTENT_X, y, done, active);
        w.text(CONTENT_X + 34, y + 2,
               if done || active { ui::text() } else { ui::dim() }, labels[i]);
        if active {
            w.text(CONTENT_X + 160, y + 2, ui::dim(), phase_name(cur));
        }
        i += 1;
    }

    ui::card(w, CONTENT_X, 286, CONTENT_W, 130);
    w.text(CONTENT_X + 20, 306, ui::dim(), "总进度");
    let pw = CONTENT_W - 120;
    ui::progress(w, CONTENT_X + 20, 336, pw, prog.permille);

    let mut done = [0u8; 24];
    let mut total = [0u8; 24];
    let d = fmt_mb(prog.done_kb, &mut done);
    let t = fmt_mb(prog.total_kb, &mut total);
    w.text(CONTENT_X + 20, 366, ui::dim(), "已写入");
    w.text(CONTENT_X + 80, 366, ui::text(), d);
    w.text(CONTENT_X + 190, 366, ui::dim(), "共");
    w.text(CONTENT_X + 220, 366, ui::text(), t);

    w.text(CONTENT_X, 448, ui::dim(),
           if cur <= install::PHASE_TABLE {
               "写入分区表前可以取消"
           } else {
               "分区表已写入，无法取消；完成后可重启"
           });

    if cur <= install::PHASE_TABLE {
        ui::ghost(w, CONTENT_X, FOOT_Y, 140, 36, "取消");
    }
}

fn draw_done(w: &mut wm::Win) {
    header(w, "安装完成", "系统已写入目标磁盘。");

    w.round_fill(CONTENT_X, 108, 64, 64, 32, ui::accent());
    w.shape(wm::OP_HLINE, CONTENT_X + 16, 132, 8, 3, ui::on_accent());
    w.shape(wm::OP_HLINE, CONTENT_X + 22, 140, 8, 3, ui::on_accent());
    w.shape(wm::OP_HLINE, CONTENT_X + 30, 128, 18, 3, ui::on_accent());
    w.text_big(CONTENT_X + 88, 106, ui::text(), "系统已就绪");
    w.text(CONTENT_X + 88, 146, ui::dim(), "重启后从新磁盘启动，进入首次设置。");

    ui::card(w, CONTENT_X, 206, CONTENT_W, 168);
    w.text(CONTENT_X + 20, 226, ui::text(), "完成前请确认");
    w.text(CONTENT_X + 20, 256, ui::dim(), "UEFI 启动顺序中包含新磁盘的 EFI 分区");
    w.text(CONTENT_X + 20, 282, ui::dim(), "安装介质可以拔出，系统不再依赖它");
    w.text(CONTENT_X + 20, 308, ui::dim(), "首次启动会要求创建本地账户");
    ui::divider(w, CONTENT_X + 20, 336, CONTENT_W - 40);
    w.text(CONTENT_X + 20, 348, ui::good(), "完整性树校验通过");

    ui::button(w, CONTENT_X + CONTENT_W - 176, FOOT_Y, 176, 36, "立即重启", ui::accent());
    ui::ghost(w, CONTENT_X, FOOT_Y, 140, 36, "关机");
}

fn draw_fail(w: &mut wm::Win, err: u8) {
    header(w, "安装未完成", "目标磁盘可能已经不再是可以启动的系统盘。");

    w.round_fill(CONTENT_X, 108, 64, 64, 32, ui::danger());
    w.text_big(CONTENT_X + 88, 112, ui::text(), "写入中断");
    w.text(CONTENT_X + 88, 144, ui::dim(), install::error_text(err));

    ui::card(w, CONTENT_X, 206, CONTENT_W, 150);
    w.text(CONTENT_X + 20, 226, ui::text(), "建议");
    w.text(CONTENT_X + 20, 256, ui::dim(), "检查磁盘连接与容量后重试");
    w.text(CONTENT_X + 20, 282, ui::dim(), "若磁盘已写入部分数据，可重新安装覆盖");
    w.text(CONTENT_X + 20, 308, ui::dim(), "安装失败不会影响正在运行的实时系统");

    ui::ghost(w, CONTENT_X, FOOT_Y, 140, 36, "返回");
    ui::button(w, CONTENT_X + CONTENT_W - 176, FOOT_Y, 176, 36, "重试", ui::accent());
}

fn draw(app: &mut App) {
    let page = app.page;

    {
        let w = &mut app.win;

        w.clear();
        w.fill(0, 0, CW, CH, ui::bg());
        sidebar(w, page);
    }

    let sel = app.sel;
    let disk = if sel < app.n_disks { Some(app.disks[sel]) } else { None };

    match page {
        Page::Welcome => draw_welcome(&mut app.win),
        Page::Disk => draw_disks(&mut app.win, &app.disks, app.n_disks, sel),
        Page::Plan => draw_plan(&mut app.win, disk),
        Page::Confirm => draw_confirm(&mut app.win, disk, app.confirmed),
        Page::Progress => draw_progress(&mut app.win, &app.prog),
        Page::Done => draw_done(&mut app.win),
        Page::Fail => draw_fail(&mut app.win, app.prog.err),
    }
    app.win.invalidate();
}

fn primary_hit(x: i16, y: i16) -> bool {
    x >= CONTENT_X + CONTENT_W - 176 && y >= FOOT_Y
}

/// The primary action of the current page. Mouse click and Enter share it,
/// so the keyboard path can never diverge from the button.
fn advance(app: &mut App) {
    match app.page {
        Page::Welcome => {
            app.page = Page::Disk;
            scan(app);
        }
        Page::Disk => {
            if app.n_disks > app.sel && app.disks[app.sel].eligible != 0 {
                app.page = Page::Plan;
            }
        }
        Page::Plan => {
            app.page = Page::Confirm;
            app.confirmed = false;
        }
        Page::Confirm => begin_install(app),
        Page::Done => {
            let _ = sys::reboot();
        }
        Page::Fail => {
            install::reset();
            app.confirmed = false;
            app.started = false;
            app.page = Page::Disk;
            scan(app);
        }
        Page::Progress => {}
    }
}

fn go_back(app: &mut App) {
    match app.page {
        Page::Disk => app.page = Page::Welcome,
        Page::Plan => app.page = Page::Disk,
        Page::Confirm => app.page = Page::Plan,
        Page::Fail => {
            install::reset();
            app.page = Page::Disk;
            scan(app);
        }
        _ => {}
    }
}

fn begin_install(app: &mut App) {
    if app.n_disks == 0 || app.sel >= app.n_disks || app.disks[app.sel].eligible == 0 {
        return;
    }
    if !app.confirmed {
        return;
    }
    if install::begin(app.slot[app.sel]) {
        app.started = true;
        app.page = Page::Progress;
        app.prog = install::Progress::new();
    } else {
        app.page = Page::Fail;
        app.prog.err = 6;
    }
}

#[no_mangle]
pub extern "C" fn _start() -> ! {
    sys::write(b"[APP] setup wizard up\n");
    let Some(wm_port) = ipc::lookup(wm::PORT_NAME) else { sys::exit(1) };
    let Some(win) = wm::Win::create(wm_port, 61, 68, (CW + 2) as i32,
                                    CH as i32 + wm::TITLE_H, "安装 Novus OS")
    else { sys::exit(2) };

    let sys_port = ipc::lookup(sinfo::PORT_NAME).unwrap_or(0);
    if sys_port != 0 {
        ui::init(sys_port);
    }

    let mut app = App {
        win,
        page: Page::Welcome,
        disks: [install::Disk {
            name: [0; 12],
            sectors: 0,
            block_size: 0,
            flags: 0,
            eligible: 0,
            reason: 0,
            pad: 0,
        }; MAX_DISKS],
        slot: [0; MAX_DISKS],
        n_disks: 0,
        sel: 0,
        confirmed: false,
        started: false,
        prog: install::Progress::new(),
        sys_port,
        frame: 0,
    };
    scan(&mut app);
    draw(&mut app);

    loop {
        while let Some(ev) = app.win.next_event() {
            match ev {
                wm::Ev::Mouse(m) if m.kind == wm::MOUSE_DOWN && m.button == 1 => {
                    let x = m.x as i16;
                    let y = m.y as i16;
                    let page = app.page;

                    match page {
                        Page::Welcome | Page::Plan => {
                            if primary_hit(x, y) {
                                advance(&mut app);
                            }
                        }
                        Page::Disk => {
                            if y >= FOOT_Y {
                                if primary_hit(x, y) {
                                    if app.n_disks > 0 {
                                        advance(&mut app);
                                    } else {
                                        scan(&mut app);
                                    }
                                }
                            } else {
                                let mut i = 0usize;
                                while i < app.n_disks {
                                    let ry = 96 + i as i16 * 58;
                                    if x >= CONTENT_X && x < CONTENT_X + CONTENT_W &&
                                       y >= ry && y < ry + 50 &&
                                       app.disks[i].eligible != 0 {
                                        app.sel = i;
                                    }
                                    i += 1;
                                }
                            }
                        }
                        Page::Confirm => {
                            if y >= FOOT_Y {
                                if primary_hit(x, y) {
                                    advance(&mut app);
                                } else if x < CONTENT_X + 160 {
                                    go_back(&mut app);
                                }
                            } else if x >= CONTENT_X + 20 && x < CONTENT_X + 40 &&
                                      y >= 388 && y < 414 {
                                app.confirmed = !app.confirmed;
                            }
                        }
                        Page::Progress => {
                            if app.prog.phase <= install::PHASE_TABLE &&
                               y >= FOOT_Y && x < CONTENT_X + 160 {
                                if install::abort() {
                                    app.started = false;
                                    install::reset();
                                    app.page = Page::Disk;
                                    scan(&mut app);
                                }
                            }
                        }
                        Page::Done => {
                            if y >= FOOT_Y {
                                if primary_hit(x, y) {
                                    let _ = sys::reboot();
                                } else if x < CONTENT_X + 160 {
                                    let _ = sys::power_off();
                                }
                            }
                        }
                        Page::Fail => {
                            if y >= FOOT_Y {
                                if primary_hit(x, y) {
                                    advance(&mut app);
                                } else if x < CONTENT_X + 160 {
                                    app.page = Page::Disk;
                                    scan(&mut app);
                                }
                            }
                        }
                    }
                    draw(&mut app);
                }
                wm::Ev::Key(key) => {
                    match key {
                        0x100 => go_back(&mut app),       /* Esc */
                        0x109 => advance(&mut app),       /* Enter */
                        0x20 => {                          /* Space */
                            if app.page == Page::Confirm {
                                app.confirmed = !app.confirmed;
                            }
                        }
                        0x101 => {                         /* Up */
                            if app.page == Page::Disk {
                                let mut i = app.sel;
                                while i > 0 {
                                    i -= 1;
                                    if app.disks[i].eligible != 0 {
                                        app.sel = i;
                                        break;
                                    }
                                }
                            }
                        }
                        0x102 => {                         /* Down */
                            if app.page == Page::Disk {
                                let mut i = app.sel + 1;
                                while i < app.n_disks {
                                    if app.disks[i].eligible != 0 {
                                        app.sel = i;
                                        break;
                                    }
                                    i += 1;
                                }
                            }
                        }
                        _ => {}
                    }
                    draw(&mut app);
                }
                wm::Ev::Close => {
                    /* The installer is the session's only window: closing it
                     * would leave a live desktop with no way to install. */
                }
                _ => {}
            }
        }

        if app.started {
            let (phase, p) = install::step();

            app.prog = p;
            app.frame = app.frame.wrapping_add(1);
            if phase == install::PHASE_DONE as i64 {
                app.started = false;
                app.page = Page::Done;
                draw(&mut app);
            } else if phase == install::PHASE_FAIL as i64 {
                app.started = false;
                app.page = Page::Fail;
                draw(&mut app);
            } else if app.frame % 6 == 0 {
                draw(&mut app);
            }
        }

        if app.sys_port != 0 && ui::poll(app.sys_port) {
            draw(&mut app);
        }
        sys::yield_now();
    }
}
