//! Drawing helpers shared by the three applications: the palette, integer
//! formatting (no core::fmt - it would drag the formatting machinery into
//! every blob), and the three stock widgets the apps actually use.

use crate::wm;

/* Windows 11, light: the window manager draws the frame in the same
 * palette, so a client's content and its chrome read as one surface. */
pub const BG: u32 = 0x00FFFFFF;
pub const PANEL: u32 = 0x00F3F3F3;
pub const ROW: u32 = 0x00F7F7F9;
pub const ROW_SEL: u32 = 0x00E5F1FB;
pub const LINE: u32 = 0x00E1E5EA;
pub const TEXT: u32 = 0x001B1B1B;
pub const DIM: u32 = 0x005F6368;
pub const ACCENT: u32 = 0x000067C0;
pub const GOOD: u32 = 0x00107C10;
pub const WARN: u32 = 0x00B45309;

pub const ROW_H: i16 = 22;

/// Advance of one character, mirroring nd_char_adv(): ASCII is 6 px at the
/// UI scale, a CJK glyph is a 16 px cell.
pub fn char_adv(c: char) -> i16 {
    if (c as u32) < 0x80 { 6 } else { 16 }
}

/// Painted width of `s`, matching nd_text_width(): the last cell has no
/// inter-character gap.
pub fn width(s: &str) -> i16 {
    let mut w = 0;
    let mut n = 0;

    for c in s.chars() {
        w += char_adv(c);
        n += 1;
    }
    if n == 0 { 0 } else { w - 1 }
}

/// A fixed-width text line. The font is monospaced at 6 px per column, so
/// padding with spaces is what aligns the columns - and one text run per
/// row keeps a full list inside the window's 20-run display list.
pub struct Buf {
    b: [u8; 96],
    n: usize,
}

impl Buf {
    pub fn new() -> Buf {
        Buf {
            b: [0u8; 96],
            n: 0,
        }
    }

    pub fn s(&mut self, s: &str) -> &mut Buf {
        for &c in s.as_bytes() {
            if self.n < 95 {
                self.b[self.n] = c;
                self.n += 1;
            }
        }
        self
    }

    pub fn num(&mut self, v: u32) -> &mut Buf {
        let mut tmp = [0u8; 12];
        let s = fmt_u32(v, &mut tmp);

        for &c in s.as_bytes() {
            if self.n < 95 {
                self.b[self.n] = c;
                self.n += 1;
            }
        }
        self
    }

    /// Pad with spaces up to column `c`.
    pub fn col(&mut self, c: usize) -> &mut Buf {
        while self.n < c && self.n < 95 {
            self.b[self.n] = b' ';
            self.n += 1;
        }
        self
    }

    pub fn str(&self) -> &str {
        unsafe { core::str::from_utf8_unchecked(&self.b[..self.n]) }
    }
}

/// Decimal rendering into a stack buffer. Returns the slice as `&str`.
pub fn fmt_u32(v: u32, buf: &mut [u8]) -> &str {
    let mut digits = [0u8; 10];
    let mut n = 0;
    let mut v = v;

    if v == 0 {
        buf[0] = b'0';
        return unsafe { core::str::from_utf8_unchecked(&buf[..1]) };
    }
    while v > 0 && n < digits.len() {
        digits[n] = b'0' + (v % 10) as u8;
        v /= 10;
        n += 1;
    }
    for i in 0..n {
        buf[i] = digits[n - 1 - i];
    }
    unsafe { core::str::from_utf8_unchecked(&buf[..n]) }
}

/// Same, with a ` KB`/` MB` suffix chosen from the magnitude.
pub fn fmt_size(bytes: u32, buf: &mut [u8]) -> &str {
    let (val, unit): (u32, &[u8]) = if bytes >= 1024 * 1024 {
        (bytes / (1024 * 1024), b" MB")
    } else {
        ((bytes + 1023) / 1024, b" KB")
    };
    let mut digits = [0u8; 10];
    let mut n = 0;
    let mut v = val;

    if v == 0 {
        buf[0] = b'0';
        n = 1;
    } else {
        while v > 0 && n < digits.len() {
            digits[n] = b'0' + (v % 10) as u8;
            v /= 10;
            n += 1;
        }
        for i in 0..n {
            buf[i] = digits[n - 1 - i];
        }
    }
    for (i, b) in unit.iter().enumerate() {
        buf[n + i] = *b;
    }
    unsafe { core::str::from_utf8_unchecked(&buf[..n + unit.len()]) }
}

/// Window chrome: background plus a one-line header.
pub fn shell(w: &wm::Win, cw: i16, ch: i16, title: &str, accent: u32) {
    w.fill(0, 0, cw, ch, BG);
    w.fill(0, 0, cw, 26, PANEL);
    w.shape(wm::OP_HLINE, 0, 26, cw, 1, LINE);
    w.fill(0, 25, 90, 1, accent);
    w.text(10, 8, TEXT, title);
}

/// A list row: optional selection block, label on the left, value on the
/// right. Returns the row rectangle for hit testing: `(x, y, w, h)`.
pub fn row(
    w: &wm::Win,
    y: i16,
    cw: i16,
    label: &str,
    value: &str,
    selected: bool,
    color: u32,
) -> (i16, i16, i16, i16) {
    if selected {
        w.fill(4, y, cw - 8, ROW_H - 2, ROW_SEL);
    } else {
        w.fill(4, y, cw - 8, ROW_H - 2, ROW);
    }
    w.text(12, y + 6, if selected { TEXT } else { color }, label);
    if !value.is_empty() {
        let vw = width(value);
        w.text(cw - 10 - vw, y + 6, DIM, value);
    }
    (4, y, cw - 8, ROW_H - 2)
}

pub fn inside(r: (i16, i16, i16, i16), x: i32, y: i32) -> bool {
    let (rx, ry, rw, rh) = r;

    x >= rx as i32 && x < (rx + rw) as i32 && y >= ry as i32 && y < (ry + rh) as i32
}

/// A button. Returns its rectangle.
pub fn button(w: &wm::Win, x: i16, y: i16, bw: i16, bh: i16, label: &str, accent: u32) -> (i16, i16, i16, i16) {
    w.fill(x, y, bw, bh, ROW_SEL);
    w.frame(x, y, bw, bh, accent);
    w.text(x + 8, y + (bh - 14) / 2, TEXT, label);
    (x, y, bw, bh)
}
