//! Labels, category colours and value formatting for the device manager.
//!
//! The numbers here mirror the devtab.h ABI: the class ids, the status byte
//! and the DEV_F_* property keys. Keeping them in one place means the drawing
//! code never hard-codes a magic number, and a single table drives both the
//! tree and the property sheet.

use crate::ui;

/* ---- category ids (DEV_CAT_* in kernel/devtab.h) ----------------------- */
pub const CAT_ROOT: u8 = 0;
pub const CAT_PROC: u8 = 1;
pub const CAT_FW: u8 = 2;
pub const CAT_DISP: u8 = 3;
pub const CAT_MON: u8 = 4;
pub const CAT_DISK: u8 = 5;
pub const CAT_STOR: u8 = 6;
pub const CAT_NET: u8 = 7;
pub const CAT_USB: u8 = 8;
pub const CAT_HID: u8 = 9;
pub const CAT_IN: u8 = 10;
pub const CAT_SYS: u8 = 11;
pub const CAT_UNK: u8 = 12;

/* ---- status / flags --------------------------------------------------- */
pub const ST_OK: u8 = 0;
pub const ST_WARN: u8 = 1;

pub const F_REMOVABLE: u8 = 1;
pub const F_PRIMARY: u8 = 2;
pub const F_NODRV: u8 = 4;
pub const F_DISABLED: u8 = 8;

/* ---- property tabs (DEV_TAB_* in kernel/devtab.h) ---------------------- */
pub const TAB_GENERAL: u8 = 0;
pub const TAB_DRIVER: u8 = 1;
pub const TAB_DETAILS: u8 = 2;
pub const TAB_EVENTS: u8 = 3;
pub const TAB_RESOURCES: u8 = 4;

pub const TAB_NAMES: [(&str, &str); 5] = [
    ("常规", "General"),
    ("驱动程序", "Driver"),
    ("详细信息", "Details"),
    ("事件", "Events"),
    ("资源", "Resources"),
];

/* ---- field keys (DEV_F_* in kernel/devtab.h) --------------------------- */
pub const F_MANUF: u8 = 1;
pub const F_LOCATION: u8 = 2;
pub const F_PROVIDER: u8 = 3;
pub const F_DRVDATE: u8 = 4;
pub const F_DRVVER: u8 = 5;
pub const F_SIGNER: u8 = 6;
pub const F_DESC: u8 = 10;
pub const F_INSTPATH: u8 = 11;
pub const F_HWIDS: u8 = 12;
pub const F_COMPATID: u8 = 13;
pub const F_STATE: u8 = 14;
pub const F_PROBLEM: u8 = 15;
pub const F_CONFIG: u8 = 16;
pub const F_CLASS: u8 = 17;
pub const F_CLASSGUID: u8 = 18;
pub const F_INF: u8 = 19;
pub const F_SERVICE: u8 = 20;
pub const F_MATCHID: u8 = 21;
pub const F_RANK: u8 = 22;
pub const F_CAPACITY: u8 = 23;
pub const F_MAC: u8 = 24;
pub const F_LINK: u8 = 25;
pub const F_CORES: u8 = 26;
pub const F_CPUID: u8 = 27;
pub const F_RAM: u8 = 28;
pub const F_FWVER: u8 = 29;
pub const F_EVTIME: u8 = 40;
pub const F_EVDESC: u8 = 41;
pub const F_MEMRANGE: u8 = 50;
pub const F_IORANGE: u8 = 51;
pub const F_IRQ: u8 = 52;
pub const F_CONFLICT: u8 = 53;

/// Bilingual category name. The categories are exactly the Windows device
/// manager groups, so users see the names they already know.
pub fn class_label(cat: u8) -> (&'static str, &'static str) {
    match cat {
        CAT_PROC => ("处理器", "Processors"),
        CAT_FW => ("固件", "Firmware"),
        CAT_DISP => ("显示适配器", "Display adapters"),
        CAT_MON => ("监视器", "Monitors"),
        CAT_DISK => ("磁盘驱动器", "Disk drives"),
        CAT_STOR => ("存储控制器", "Storage controllers"),
        CAT_NET => ("网络适配器", "Network adapters"),
        CAT_USB => ("通用串行总线控制器", "Universal Serial Bus controllers"),
        CAT_HID => ("人体学输入设备", "Human Interface Devices"),
        CAT_IN => ("键盘和鼠标", "Keyboards and mice"),
        CAT_SYS => ("系统设备", "System devices"),
        CAT_UNK => ("其他设备", "Other devices"),
        CAT_ROOT => ("电脑", "Computer"),
        _ => ("设备", "Devices"),
    }
}

/// A colour swatch per category: display blue, network teal, USB purple, and
/// so on. These are chosen to read on both the light and dark themes.
pub fn cat_color(cat: u8) -> u32 {
    match cat {
        CAT_PROC => 0x0067C0,
        CAT_FW => 0x0078D4,
        CAT_DISP => 0x0078D4,
        CAT_MON => 0x8A8A8A,
        CAT_DISK => 0x5B5FC7,
        CAT_STOR => 0x8764B8,
        CAT_NET => 0x0F7B6C,
        CAT_USB => 0x8764B8,
        CAT_HID => 0x107C10,
        CAT_IN => 0x8A8A8A,
        CAT_SYS => 0x8A8A8A,
        CAT_UNK => 0xC19C00,
        _ => 0x0067C0,
    }
}

/// Bilingual label for a property key. The details tab lists these verbatim,
/// so order and wording mirror what Windows shows.
pub fn field_label(key: u8) -> (&'static str, &'static str) {
    match key {
        F_MANUF => ("制造商", "Manufacturer"),
        F_LOCATION => ("位置", "Location"),
        F_PROVIDER => ("驱动程序提供商", "Driver provider"),
        F_DRVDATE => ("驱动程序日期", "Driver date"),
        F_DRVVER => ("驱动程序版本", "Driver version"),
        F_SIGNER => ("数字签名", "Digital signer"),
        F_CAPACITY => ("容量", "Capacity"),
        F_MAC => ("MAC 地址", "MAC address"),
        F_LINK => ("连接", "Connection"),
        F_DESC => ("设备描述", "Device description"),
        F_INSTPATH => ("设备实例路径", "Device instance path"),
        F_HWIDS => ("硬件 Id", "Hardware Ids"),
        F_COMPATID => ("兼容 Id", "Compatible Ids"),
        F_STATE => ("状态", "State"),
        F_PROBLEM => ("问题代码", "Problem code"),
        F_CONFIG => ("配置标志", "Config flags"),
        F_CLASS => ("类", "Class"),
        F_CLASSGUID => ("类 Guid", "Class Guid"),
        F_INF => ("INF 名称", "Inf name"),
        F_SERVICE => ("服务", "Service"),
        F_MATCHID => ("匹配设备 Id", "Matching device Id"),
        F_RANK => ("驱动程序等级", "Driver rank"),
        F_CORES => ("内核数量", "Number of cores"),
        F_CPUID => ("CPU 厂商", "CPU vendor"),
        F_RAM => ("安装的物理内存", "Installed RAM"),
        F_FWVER => ("固件版本", "Firmware version"),
        F_EVTIME => ("时间戳", "Time"),
        F_EVDESC => ("描述", "Description"),
        F_MEMRANGE => ("内存范围", "Memory range"),
        F_IORANGE => ("I/O 范围", "I/O range"),
        F_IRQ => ("中断请求", "IRQ"),
        F_CONFLICT => ("冲突设备", "Conflicting device"),
        _ => ("属性", "Property"),
    }
}

/// The "device status" message on the General tab - exactly the wording
/// Windows shows, because a device manager's whole job is to report this.
pub fn status_text(status: u8, flags: u8) -> (&'static str, &'static str) {
    if status == ST_WARN {
        if flags & F_NODRV != 0 {
            (
                "该设备的驱动程序未被安装。\n(代码 28)",
                "The drivers for this device are not installed.\n(Code 28)",
            )
        } else if flags & F_DISABLED != 0 {
            (
                "由于该设备有问题，Windows 已将其停止。\n(代码 43)",
                "Windows has stopped this device because it has a problem.\n(Code 43)",
            )
        } else {
            (
                "该设备无法启动。\n(代码 10)",
                "This device cannot start.\n(Code 10)",
            )
        }
    } else {
        (
            "该设备运转正常。",
            "This device is working properly.",
        )
    }
}

/// Format a 32-bit value as an uppercase hex string of `digits` columns,
/// matching the resource page's fixed-width ranges.
pub fn hex(v: u64, digits: u32, buf: &mut [u8]) -> &str {
    let mut n = 0usize;

    for i in 0..digits as usize {
        if n >= buf.len() {
            break;
        }
        let shift = (digits - 1 - i as u32) * 4;
        let nib = if shift < 64 {
            ((v >> shift) & 0xF) as u8
        } else {
            0
        };
        buf[n] = if nib < 10 {
            b'0' + nib
        } else {
            b'A' + (nib - 10)
        };
        n += 1;
    }
    core::str::from_utf8(&buf[..n]).unwrap_or("")
}

/// Word-wrap `s` into lines no wider than `max_px`, returning up to `cap`
/// line slices. CJK characters are not split mid-glyph; the renderer's cell
/// width (ui::char_adv) decides where a line breaks.
pub fn wrap<'a>(s: &'a str, max_px: i16, out: &mut [&'a str]) -> usize {
    if out.is_empty() {
        return 0;
    }
    // Byte-offset walk that never splits a UTF-8 codepoint.
    let bytes = s.as_bytes();
    let mut start = 0usize;
    let mut i = 0usize;
    let mut line = 0usize;
    let mut used = 0i16;

    while i < bytes.len() {
        let mut j = i + 1;
        while j < bytes.len() && (bytes[j] & 0xC0) == 0x80 {
            j += 1;
        }
        let ch = core::str::from_utf8(&bytes[i..j]).unwrap_or(" ");
        let adv = ui::width(ch);

        if used + adv > max_px && i > start {
            out[line] = &s[start..i];
            line += 1;
            if line >= out.len() {
                return line;
            }
            start = i;
            used = adv;
        } else {
            used += adv;
        }
        i = j;
    }
    if start < s.len() {
        out[line] = &s[start..];
        line += 1;
    }
    line
}

/// Convenience: wrap and paint, line by line, starting at `y` with leading
/// `lh`. Each line is one text run, so a five-line status box costs five
/// text items - well inside the per-window budget.
pub fn draw_wrapped(
    w: &mut crate::wm::Win,
    x: i16,
    y: i16,
    max_px: i16,
    lh: i16,
    color: u32,
    s: &str,
) {
    let mut lines: [&str; 12] = [""; 12];
    let n = wrap(s, max_px, &mut lines);

    for i in 0..n {
        w.text(x, y + (i as i16) * lh, color, lines[i]);
    }
}
