//! Novus OS - graphical installer (M11).
//!
//! Seven pages over the kernel's install service: language, licence, disk,
//! plan, confirm, progress, result. The app owns no disk logic - it asks
//! the kernel for candidates, shows why a disk is greyed out, and drives
//! the copy one bounded step at a time so this window keeps repainting.
//!
//! Language: the buttons call SYSINFO_REQ_LANG, which switches the
//! renderer for every window (including this one) and is persisted by the
//! kernel when the root is writable. Text drawn here only needs the lookup
//! to happen at draw time, which the kernel does.
//!
//! Destructive actions are gated twice: the kernel only accepts calls
//! from the process apps_launch() started as "setup", and this app
//! requires an explicit tick on the confirmation page.

#![no_std]
#![no_main]

use novus_apps::{install, ipc, lang, sinfo, sys, ui, wm};

const CW: i16 = 900;
const CH: i16 = 600;
const SIDE: i16 = 240;
const CONTENT_X: i16 = 264;
const CONTENT_W: i16 = 612;
const FOOT_Y: i16 = 548;

const MAX_DISKS: usize = 8;

#[derive(Clone, Copy, PartialEq)]
enum Page {
    Language,
    License,
    Disk,
    Plan,
    Confirm,
    Progress,
    Done,
    Fail,
}

const STEPS: [&str; 7] = ["语言", "协议", "磁盘", "方案", "确认", "进度", "完成"];

fn step_of(p: Page) -> usize {
    match p {
        Page::Language => 0,
        Page::License => 1,
        Page::Disk => 2,
        Page::Plan => 3,
        Page::Confirm => 4,
        Page::Progress => 5,
        Page::Done | Page::Fail => 6,
    }
}

struct App {
    win: wm::Win,
    page: Page,
    disks: [install::Disk; MAX_DISKS],
    slot: [u8; MAX_DISKS],
    n_disks: usize,
    sel: usize,
    confirmed: bool,
    accepted: bool,
    fs: u8,
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
    w.text(24, 50, ui::dim(), ui::l("系统安装程序", "System installer"));

    let cur = step_of(page);
    let mut i = 0;
    while i < 7 {
        let y = 86 + i as i16 * 40;
        let done = i < cur;
        let active = i == cur;

        if done {
            w.round_fill(28, y, 20, 20, 10, ui::accent());
            w.shape(wm::OP_HLINE, 32, y + 9, 4, 2, ui::on_accent());
            w.shape(wm::OP_HLINE, 35, y + 6, 7, 2, ui::on_accent());
        } else if active {
            w.round_fill(28, y, 20, 20, 10, ui::accent());
            let mut d = [0u8; 2];
            d[0] = b'1' + i as u8;
            w.text(34, y + 3, ui::on_accent(),
                   unsafe { core::str::from_utf8_unchecked(&d[..1]) });
        } else {
            w.round_frame(28, y, 20, 20, 10, ui::line());
            let mut d = [0u8; 2];
            d[0] = b'1' + i as u8;
            w.text(34, y + 3, ui::dim(),
                   unsafe { core::str::from_utf8_unchecked(&d[..1]) });
        }
        w.text(58, y + 3, if active { ui::text() } else { ui::dim() }, STEPS[i]);
        i += 1;
    }

    w.text(24, CH - 42, ui::dim(), ui::l("内核安装服务", "Kernel install service"));
    w.text(24, CH - 24, ui::dim(), ui::l("写入前会再次确认", "You will confirm again before writing"));
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

fn primary_hit(x: i16, y: i16) -> bool {
    x >= CONTENT_X + CONTENT_W - 176 && y >= FOOT_Y
}

fn step_mark(w: &mut wm::Win, x: i16, y: i16, done: bool, active: bool) {
    if done {
        w.round_fill(x, y, 18, 18, 9, ui::accent());
        w.shape(wm::OP_HLINE, x + 4, y + 10, 4, 2, ui::on_accent());
        w.shape(wm::OP_HLINE, x + 7, y + 6, 7, 2, ui::on_accent());
    } else if active {
        w.round_fill(x, y, 18, 18, 9, ui::accent());
        w.round_fill(x + 7, y + 7, 4, 4, 2, ui::on_accent());
    } else {
        w.round_frame(x, y, 18, 18, 9, ui::line());
    }
}

fn draw_language(w: &mut wm::Win) {
    header(w, ui::l("选择安装语言", "Choose the installer language"),
           "Novus OS Setup");

    ui::card(w, CONTENT_X, 112, CONTENT_W, 150);
    w.text(CONTENT_X + 20, 132, ui::dim(),
           ui::l("选择语言后，安装界面会立即切换。",
                 "The installer switches language immediately."));

    let labels = [ui::l("中文", "Chinese"), "English"];
    let mut i = 0usize;
    while i < 2 {
        let x = CONTENT_X + 30 + i as i16 * 250;
        let y = 168;
        let selected = (lang::get() == 1) == (i == 1);

        if selected {
            w.round_fill(x, y, 220, 48, 8, ui::accent());
            w.text_big(x + (220 - ui::width_big(labels[i])) / 2, y + 8,
                       ui::on_accent(), labels[i]);
        } else {
            w.round_fill(x, y, 220, 48, 8, ui::card_bg());
            w.round_frame(x, y, 220, 48, 8, ui::line());
            w.text_big(x + (220 - ui::width_big(labels[i])) / 2, y + 8,
                       ui::text(), labels[i]);
        }
        i += 1;
    }

    w.text(CONTENT_X, 290, ui::dim(),
           ui::l("安装程序会把一份完整的系统写入所选磁盘。",
                 "The installer writes a complete system to the selected disk."));
    w.text(CONTENT_X, 314, ui::dim(),
           ui::l("过程不联网，也不改动 UEFI 固件设置。",
                 "It does not use the network or change UEFI firmware settings."));

    primary(w, CONTENT_X + CONTENT_W - 176, ui::l("下一步", "Next"), true);
    footer_hint(w, ui::l("按 Enter 继续", "Press Enter to continue"));
}

fn draw_license(w: &mut wm::Win, accepted: bool) {
    header(w, ui::l("用户协议", "Licence agreement"),
           ui::l("继续前请阅读以下条款。", "Read the following before continuing."));

    ui::card(w, CONTENT_X, 96, CONTENT_W, 300);
    let lines = [
        "本协议依据 GPLv2 分发 Novus OS，不提供任何担保。",
        "安装会重建分区表，目标磁盘上的全部数据将被永久清除。",
        "请确认已备份需要的文件，并已连接稳定电源。",
        "安装程序会写入引导分区与系统分区，并重建完整性树。",
        "安装完成后需要重启，并从新磁盘启动。",
        "继续即表示你理解并接受以上条款。",
    ];
    let mut i = 0usize;
    while i < lines.len() {
        w.text(CONTENT_X + 20, 120 + i as i16 * 42, ui::dim(), lines[i]);
        i += 1;
    }

    let box_x = CONTENT_X + 20;
    let box_y = 416;
    w.round_fill(box_x, box_y, 22, 22, 5, if accepted { ui::accent() } else { ui::card_bg() });
    w.round_frame(box_x, box_y, 22, 22, 5, if accepted { ui::accent() } else { ui::line() });
    if accepted {
        w.shape(wm::OP_HLINE, box_x + 5, box_y + 11, 13, 1, ui::on_accent());
    }
    w.text(box_x + 34, box_y + 4, ui::text(), "我已阅读并同意");

    ui::ghost(w, CONTENT_X, FOOT_Y, 140, 36, ui::l("返回", "Back"));
    primary(w, CONTENT_X + CONTENT_W - 176,
            ui::l("同意并继续", "Accept and continue"), accepted);
    footer_hint(w, ui::l("勾选后才能继续", "Tick the box to continue"));
}

fn draw_disks(w: &mut wm::Win, disks: &[install::Disk; MAX_DISKS], n_disks: usize, sel: usize) {
    header(w, ui::l("选择磁盘", "Select a disk"),
           ui::l("列出所有整块磁盘；灰色项附有不可选原因。",
                 "Every whole disk is listed; greyed rows say why they cannot be used."));

    if n_disks == 0 {
        ui::empty(w, CONTENT_X, 110, ui::l("没有可用的整块磁盘", "No usable whole disk"));
        w.text(CONTENT_X, 138, ui::dim(),
               ui::l("安装需要一块未分区的本机磁盘。",
                     "Installation needs an unpartitioned local disk."));
        primary(w, CONTENT_X + CONTENT_W - 176, ui::l("刷新", "Refresh"), true);
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
        let removable = d.flags & install::FLAG_REMOVABLE != 0;
        w.round_fill(CONTENT_X + 18, y + 14, 22, 22, 6,
                     if d.eligible != 0 { ui::accent() } else { ui::line() });
        w.text(CONTENT_X + 29, y + 18, ui::on_accent(),
               if removable { "U" } else { "D" });

        w.text(CONTENT_X + 54, y + 10,
               if d.eligible != 0 { ui::text() } else { ui::dim() }, d.label());
        let mut sz = [0u8; 24];
        w.text(CONTENT_X + 54, y + 28, ui::dim(), fmt_mib(d.sectors, &mut sz));
        w.text(CONTENT_X + 190, y + 28, ui::dim(),
               if removable { ui::l("USB 可移除", "USB removable") }
               else { ui::l("内置磁盘", "Internal disk") });

        let reason = if d.eligible != 0 {
            ui::l("可用", "Available")
        } else {
            install::reason_text(d.reason)
        };
        let rw = ui::width(reason);
        w.text(CONTENT_X + CONTENT_W - 18 - rw, y + 18,
               if d.eligible != 0 { ui::good() } else { ui::dim() }, reason);
        i += 1;
    }

    let detail_y = 96 + n_disks as i16 * 58 + 12;
    if sel < n_disks && disks[sel].eligible != 0 {
        ui::card(w, CONTENT_X, detail_y, CONTENT_W, 80);
        w.text(CONTENT_X + 20, detail_y + 14, ui::danger(),
               ui::l("将清除整盘数据", "This erases the whole disk"));
        w.text(CONTENT_X + 20, detail_y + 40, ui::dim(),
               ui::l("安装会重建分区表，原磁盘上的系统与文件将无法恢复。",
                     "The partition table is rebuilt; systems and files on the disk cannot be recovered."));
    }

    primary(w, CONTENT_X + CONTENT_W - 176, ui::l("下一步", "Next"),
            sel < n_disks && disks[sel].eligible != 0);
    footer_hint(w, ui::l("选择一块标记为“可用”的磁盘", "Choose a disk marked Available"));
}

fn draw_plan(w: &mut wm::Win, disk: Option<install::Disk>, fs: u8) {
    header(w, ui::l("安装方案", "Installation plan"),
           ui::l("Novus 使用 GPT：一个 EFI 系统分区和一个系统分区。",
                 "Novus uses GPT: one EFI system partition and one system partition."));

    ui::card(w, CONTENT_X, 96, CONTENT_W, 150);
    w.text(CONTENT_X + 20, 116, ui::text(), ui::l("文件系统", "File system"));

    let options = [
        ("NovusFS", "NovusFS"),
        ("NTFS", "NTFS"),
        ("ext4", "ext4"),
    ];
    let mut i = 0usize;
    while i < 3 {
        let x = CONTENT_X + 20 + i as i16 * 190;
        let y = 144;
        let enabled = i == 0;
        let selected = fs as usize == i;

        if selected && enabled {
            w.round_fill(x, y, 178, 42, 8, ui::accent());
            w.text(x + 14, y + 14, ui::on_accent(), options[i].0);
        } else {
            w.round_fill(x, y, 178, 42, 8, ui::card_bg());
            w.round_frame(x, y, 178, 42, 8, ui::line());
            w.text(x + 14, y + 14, if enabled { ui::text() } else { ui::dim() }, options[i].0);
        }
        if !enabled {
            w.text(x + 14, y + 28, ui::dim(), ui::l("规划中", "Planned"));
        }
        i += 1;
    }

    w.text(CONTENT_X + 20, 202, ui::dim(),
           ui::l("NTFS/ext4 需要后续驱动里程碑；本次安装使用 NovusFS。",
                 "NTFS/ext4 need a later driver milestone; this install uses NovusFS."));

    ui::card(w, CONTENT_X, 262, CONTENT_W, 150);
    w.text(CONTENT_X + 20, 282, ui::text(), ui::l("分区布局", "Partition layout"));
    ui::divider(w, CONTENT_X + 20, 308, CONTENT_W - 40);
    w.round_fill(CONTENT_X + 20, 324, 10, 10, 3, ui::accent());
    w.text(CONTENT_X + 40, 320, ui::text(), ui::l("EFI 系统分区", "EFI system partition"));
    w.text(CONTENT_X + 40, 340, ui::dim(),
           ui::l("引导加载器与系统映像模板（FAT）",
                 "Boot loader and system image template (FAT)"));
    w.round_fill(CONTENT_X + 20, 372, 10, 10, 3, ui::good());
    w.text(CONTENT_X + 40, 368, ui::text(), ui::l("NovusFS 系统分区", "NovusFS system partition"));
    w.text(CONTENT_X + 40, 388, ui::dim(),
           ui::l("带 WAL 与 Merkle 完整性树的系统镜像",
                 "System image with WAL and a Merkle integrity tree"));

    if let Some(d) = disk {
        let mut sz = [0u8; 24];

        w.text(CONTENT_X, 436, ui::dim(), ui::l("目标磁盘", "Target disk"));
        w.text(CONTENT_X + 100, 436, ui::text(), d.label());
        let s = fmt_mib(d.sectors, &mut sz);
        let sw = ui::width(s);
        w.text(CONTENT_X + CONTENT_W - sw, 436, ui::dim(), s);
    }

    primary(w, CONTENT_X + CONTENT_W - 176, ui::l("继续", "Continue"), true);
    footer_hint(w, ui::l("下一步需要确认整盘擦除", "The next page asks you to confirm the erase"));
}

fn draw_confirm(w: &mut wm::Win, disk: Option<install::Disk>, confirmed: bool) {
    header(w, ui::l("确认安装", "Confirm installation"),
           ui::l("这是最后一次可以返回的页面。",
                 "This is the last page you can go back from."));

    if let Some(d) = disk {
        let mut sz = [0u8; 24];

        ui::card(w, CONTENT_X, 96, CONTENT_W, 150);
        w.text(CONTENT_X + 20, 116, ui::text(), ui::l("即将安装到", "Installing to"));
        w.text_big(CONTENT_X + 20, 144, ui::text(), d.label());
        let s = fmt_mib(d.sectors, &mut sz);
        w.text(CONTENT_X + 20, 184, ui::dim(), s);
        w.text(CONTENT_X + 20, 206, ui::dim(),
               ui::l("分区表、引导分区与系统分区都将被重建",
                     "The partition table, boot partition and system partition will be rebuilt"));
    }

    w.round_fill(CONTENT_X, 266, CONTENT_W, 96, 10,
                 if ui::is_dark() { 0x003A1E1E } else { 0x00FDF0EF });
    w.round_frame(CONTENT_X, 266, CONTENT_W, 96, 10, ui::danger());
    w.text(CONTENT_X + 20, 286, ui::danger(),
           ui::l("整盘数据将被永久清除", "All data on the disk will be erased permanently"));
    w.text(CONTENT_X + 20, 312, ui::dim(),
           ui::l("无法撤销，也无法从本安装程序恢复。",
                 "This cannot be undone or recovered from the installer."));
    w.text(CONTENT_X + 20, 334, ui::dim(),
           ui::l("请确保已备份需要的文件。", "Make sure you have backed up anything you need."));

    let box_x = CONTENT_X + 20;
    let box_y = 388;
    w.round_fill(box_x, box_y, 22, 22, 5, if confirmed { ui::accent() } else { ui::card_bg() });
    w.round_frame(box_x, box_y, 22, 22, 5, if confirmed { ui::accent() } else { ui::line() });
    if confirmed {
        w.shape(wm::OP_HLINE, box_x + 5, box_y + 11, 13, 1, ui::on_accent());
    }
    w.text(box_x + 34, box_y + 4, ui::text(), "我已知晓：目标磁盘上的全部数据会被清除");

    ui::ghost(w, CONTENT_X, FOOT_Y, 140, 36, ui::l("上一步", "Back"));
    primary(w, CONTENT_X + CONTENT_W - 176, ui::l("开始安装", "Start installation"), confirmed);
    footer_hint(w, ui::l("勾选确认后才能开始", "Tick the box before starting"));
}

fn phase_name(phase: u8) -> &'static str {
    match phase {
        install::PHASE_TABLE => ui::l("写入分区表", "Writing partition table"),
        install::PHASE_ESP => ui::l("写入引导分区", "Writing boot partition"),
        install::PHASE_SYSTEM => ui::l("写入系统分区", "Writing system partition"),
        install::PHASE_VERIFY => ui::l("校验完整性树", "Verifying integrity tree"),
        install::PHASE_DONE => ui::l("安装完成", "Installation complete"),
        _ => ui::l("准备中", "Preparing"),
    }
}

fn draw_progress(w: &mut wm::Win, prog: &install::Progress) {
    header(w, ui::l("正在安装", "Installing"),
           ui::l("请勿关闭电源或拔出目标磁盘。",
                 "Do not power off or unplug the target disk."));

    let phases = [install::PHASE_TABLE, install::PHASE_ESP, install::PHASE_SYSTEM, install::PHASE_VERIFY];
    let labels = [
        ui::l("分区表", "Partition table"),
        ui::l("引导分区", "Boot partition"),
        ui::l("系统分区", "System partition"),
        ui::l("完整性校验", "Integrity check"),
    ];
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
    w.text(CONTENT_X + 20, 306, ui::dim(), ui::l("总进度", "Overall progress"));
    let pw = CONTENT_W - 120;
    ui::progress(w, CONTENT_X + 20, 336, pw, prog.permille);

    let mut done = [0u8; 24];
    let mut total = [0u8; 24];
    let d = fmt_mb(prog.done_kb, &mut done);
    let t = fmt_mb(prog.total_kb, &mut total);
    w.text(CONTENT_X + 20, 366, ui::dim(), ui::l("已写入", "Written"));
    w.text(CONTENT_X + 80, 366, ui::text(), d);
    w.text(CONTENT_X + 190, 366, ui::dim(), ui::l("共", "of"));
    w.text(CONTENT_X + 220, 366, ui::text(), t);

    w.text(CONTENT_X, 448, ui::dim(),
           if cur <= install::PHASE_TABLE {
               ui::l("写入分区表前可以取消", "You can cancel before the partition table is written")
           } else {
               ui::l("分区表已写入，无法取消；完成后可重启",
                     "The partition table is written; cancelling is no longer possible. Reboot when done.")
           });

    if cur <= install::PHASE_TABLE {
        ui::ghost(w, CONTENT_X, FOOT_Y, 140, 36, ui::l("取消", "Cancel"));
    }
}

fn draw_done(w: &mut wm::Win) {
    header(w, ui::l("安装完成", "Installation complete"),
           ui::l("系统已写入目标磁盘。", "The system is on the target disk."));

    w.round_fill(CONTENT_X, 108, 64, 64, 32, ui::accent());
    w.shape(wm::OP_HLINE, CONTENT_X + 16, 132, 8, 3, ui::on_accent());
    w.shape(wm::OP_HLINE, CONTENT_X + 22, 140, 8, 3, ui::on_accent());
    w.shape(wm::OP_HLINE, CONTENT_X + 30, 128, 18, 3, ui::on_accent());
    w.text_big(CONTENT_X + 88, 106, ui::text(), ui::l("系统已就绪", "System ready"));
    w.text(CONTENT_X + 88, 146, ui::dim(),
           ui::l("重启后从新磁盘启动，进入首次设置。",
                 "Reboot from the new disk to finish first-time setup."));

    ui::card(w, CONTENT_X, 206, CONTENT_W, 168);
    w.text(CONTENT_X + 20, 226, ui::text(), ui::l("完成前请确认", "Before you finish"));
    w.text(CONTENT_X + 20, 256, ui::dim(),
           ui::l("UEFI 启动顺序中包含新磁盘的 EFI 分区",
                 "The UEFI boot order includes the new disk's EFI partition"));
    w.text(CONTENT_X + 20, 282, ui::dim(),
           ui::l("安装介质可以拔出，系统不再依赖它",
                 "You can remove the installation medium; the system no longer needs it"));
    w.text(CONTENT_X + 20, 308, ui::dim(),
           ui::l("首次启动会要求创建本地账户",
                 "The first boot asks you to create a local account"));
    ui::divider(w, CONTENT_X + 20, 336, CONTENT_W - 40);
    w.text(CONTENT_X + 20, 348, ui::good(),
           ui::l("完整性树校验通过", "Integrity tree verified"));

    ui::button(w, CONTENT_X + CONTENT_W - 176, FOOT_Y, 176, 36,
               ui::l("立即重启", "Restart now"), ui::accent());
    ui::ghost(w, CONTENT_X, FOOT_Y, 140, 36, ui::l("关机", "Shut down"));
}

fn draw_fail(w: &mut wm::Win, err: u8) {
    header(w, ui::l("安装未完成", "Installation incomplete"),
           ui::l("目标磁盘可能已经不再是可以启动的系统盘。",
                 "The target disk may no longer be a bootable system disk."));

    w.round_fill(CONTENT_X, 108, 64, 64, 32, ui::danger());
    w.text_big(CONTENT_X + 88, 112, ui::text(), ui::l("写入中断", "Writing stopped"));
    w.text(CONTENT_X + 88, 144, ui::dim(), install::error_text(err));

    ui::card(w, CONTENT_X, 206, CONTENT_W, 150);
    w.text(CONTENT_X + 20, 226, ui::text(), ui::l("建议", "What to try"));
    w.text(CONTENT_X + 20, 256, ui::dim(),
           ui::l("检查磁盘连接与容量后重试",
                 "Check the disk connection and capacity, then retry"));
    w.text(CONTENT_X + 20, 282, ui::dim(),
           ui::l("若磁盘已写入部分数据，可重新安装覆盖",
                 "If part of the disk was written, installing again overwrites it"));
    w.text(CONTENT_X + 20, 308, ui::dim(),
           ui::l("安装失败不会影响正在运行的实时系统",
                 "A failed install does not affect the running live system"));

    ui::ghost(w, CONTENT_X, FOOT_Y, 140, 36, ui::l("返回", "Back"));
    ui::button(w, CONTENT_X + CONTENT_W - 176, FOOT_Y, 176, 36,
               ui::l("重试", "Retry"), ui::accent());
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
        Page::Language => draw_language(&mut app.win),
        Page::License => draw_license(&mut app.win, app.accepted),
        Page::Disk => draw_disks(&mut app.win, &app.disks, app.n_disks, sel),
        Page::Plan => draw_plan(&mut app.win, disk, app.fs),
        Page::Confirm => draw_confirm(&mut app.win, disk, app.confirmed),
        Page::Progress => draw_progress(&mut app.win, &app.prog),
        Page::Done => draw_done(&mut app.win),
        Page::Fail => draw_fail(&mut app.win, app.prog.err),
    }
    app.win.invalidate();
}

fn begin_install(app: &mut App) {
    if app.n_disks == 0 || app.sel >= app.n_disks || app.disks[app.sel].eligible == 0 {
        return;
    }
    if !app.confirmed {
        return;
    }
    install::set_language(lang::get());
    if install::begin(app.slot[app.sel], app.fs) {
        app.started = true;
        app.page = Page::Progress;
        app.prog = install::Progress::new();
    } else {
        app.page = Page::Fail;
        app.prog.err = 6;
    }
}

/// The primary action of the current page. Mouse click and Enter share it,
/// so the keyboard path can never diverge from the button.
fn advance(app: &mut App) {
    match app.page {
        Page::Language => app.page = Page::License,
        Page::License => {
            if app.accepted {
                app.page = Page::Disk;
                scan(app);
            }
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
        Page::License => app.page = Page::Language,
        Page::Disk => app.page = Page::License,
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
        page: Page::Language,
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
        accepted: false,
        fs: 0,
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
                        Page::Language => {
                            let mut i = 0usize;
                            while i < 2 {
                                let bx = CONTENT_X + 30 + i as i16 * 250;
                                if x >= bx && x < bx + 220 && y >= 168 && y < 216 {
                                    let want = i as u32;
                                    if app.sys_port != 0 {
                                        if let Some(t) = sinfo::set_lang(app.sys_port, want) {
                                            lang::set(t.lang);
                                        }
                                    } else {
                                        lang::set(want);
                                    }
                                }
                                i += 1;
                            }
                            if primary_hit(x, y) {
                                advance(&mut app);
                            }
                        }
                        Page::License => {
                            if y >= FOOT_Y {
                                if primary_hit(x, y) {
                                    advance(&mut app);
                                } else if x < CONTENT_X + 160 {
                                    go_back(&mut app);
                                }
                            } else if x >= CONTENT_X + 20 && x < CONTENT_X + 42 &&
                                      y >= 416 && y < 438 {
                                app.accepted = !app.accepted;
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
                        Page::Plan => {
                            let mut i = 0usize;
                            while i < 3 {
                                let bx = CONTENT_X + 20 + i as i16 * 190;
                                if x >= bx && x < bx + 178 && y >= 144 && y < 186 {
                                    if i == 0 {
                                        app.fs = 0;
                                    }
                                    /* NTFS/ext4 are planned, not selectable. */
                                }
                                i += 1;
                            }
                            if primary_hit(x, y) {
                                advance(&mut app);
                            }
                        }
                        Page::Confirm => {
                            if y >= FOOT_Y {
                                if primary_hit(x, y) {
                                    advance(&mut app);
                                } else if x < CONTENT_X + 160 {
                                    go_back(&mut app);
                                }
                            } else if x >= CONTENT_X + 20 && x < CONTENT_X + 42 &&
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
                wm::Ev::Key(key, _mods) => {
                    match key {
                        wm::KEY_ESC => go_back(&mut app),
                        wm::KEY_ENTER => advance(&mut app),
                        0x20 => {
                            if app.page == Page::Confirm {
                                app.confirmed = !app.confirmed;
                            } else if app.page == Page::License {
                                app.accepted = !app.accepted;
                            }
                        }
                        wm::KEY_UP => {
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
                        wm::KEY_DOWN => {
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
