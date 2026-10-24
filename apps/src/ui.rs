//! Novus Fluent component library for the built-in applications.
//!
//! One palette, two resolutions: `theme.rs` has the generated light and
//! dark tables, and the accessors below return the colour of the current
//! one. An application never writes a literal colour, so light/dark is a
//! switch here rather than a second set of drawing code.
//!
//! Cards, pill buttons and selection bars use the WM's v1.2 rounded ops.
//! Everything stays inside the 40-shape / 48-text budget the server
//! enforces; the client side counts and warns instead of silently losing
//! the tail of a frame.

use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};

use crate::lang;
use crate::sinfo;
use crate::theme::{self, Palette};
use crate::wm;

static DARK: AtomicBool = AtomicBool::new(false);
static ACCENT: AtomicU32 = AtomicU32::new(0); /* 0 = palette accent */
static VERSION: AtomicU32 = AtomicU32::new(0);
static POLL: AtomicU32 = AtomicU32::new(0);

pub fn pal() -> &'static Palette {
    if DARK.load(Ordering::Relaxed) {
        &theme::DARK
    } else {
        &theme::LIGHT
    }
}

pub fn is_dark() -> bool {
    DARK.load(Ordering::Relaxed)
}

pub fn bg() -> u32 { pal().surface }
pub fn canvas() -> u32 { pal().canvas_top }
pub fn card_bg() -> u32 { pal().card }
pub fn row_bg() -> u32 { pal().card }
pub fn subtle() -> u32 { pal().subtle }
pub fn row_sel() -> u32 { pal().selection }
pub fn line() -> u32 { pal().stroke }
pub fn text() -> u32 { pal().text }
pub fn dim() -> u32 { pal().dim }
pub fn good() -> u32 { pal().success }
pub fn warn() -> u32 { pal().warn }
pub fn danger() -> u32 { pal().danger }

pub fn accent() -> u32 {
    let a = ACCENT.load(Ordering::Relaxed);

    if a == 0 { pal().accent } else { a }
}

/// Ink that stays readable on `bg`: the theme's on-accent colour for the
/// stock accent, a luminance decision for an override.
pub fn on_color(bg: u32) -> u32 {
    let r = (bg >> 16) & 0xFF;
    let g = (bg >> 8) & 0xFF;
    let b = bg & 0xFF;
    let luma = (r * 299 + g * 587 + b * 114) / 1000;

    if luma >= 150 { 0x00002436 } else { 0x00FFFFFF }
}

pub fn on_accent() -> u32 {
    on_color(accent())
}

fn apply(t: &sinfo::Theme) {
    DARK.store(t.resolved == 1, Ordering::Relaxed);
    ACCENT.store(if t.accent == pal().accent { 0 } else { t.accent }, Ordering::Relaxed);
    VERSION.store(t.version, Ordering::Relaxed);
}

/// One bilingual label: `l("磁盘", "Disk")`.
pub fn l(zh: &'static str, en: &'static str) -> &'static str {
    lang::l(zh, en)
}

/// Read theme and language once, before the first frame.
pub fn init(sys_port: u64) {
    if let Some(t) = sinfo::theme(sys_port) {
        apply(&t);
    }
    lang::init(sys_port);
}

/// Cheap poll (about once a second at 60 yields): true when either the
/// theme or the language changed and the window must be redrawn.
pub fn poll(sys_port: u64) -> bool {
    if POLL.fetch_add(1, Ordering::Relaxed) % 60 != 0 {
        return false;
    }
    let mut changed = false;

    if let Some(t) = sinfo::theme(sys_port) {
        if t.version != VERSION.load(Ordering::Relaxed) {
            apply(&t);
            changed = true;
        }
    }
    if lang::poll(sys_port) {
        changed = true;
    }
    changed
}

/// Settings applies a swatch locally; the reply carries the full state so
/// the panel repaints without a second round trip.
pub fn set_accent_rgb(rgb: u32) {
    ACCENT.store(rgb, Ordering::Relaxed);
}

pub fn set_palette(dark: bool) {
    DARK.store(dark, Ordering::Relaxed);
}

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

/// Advance of one character, mirroring nd_char_adv(): ASCII is 6 px at the
/// UI scale, a CJK glyph is a 16 px cell.
pub fn char_adv(c: char) -> i16 {
    if (c as u32) < 0x80 { 6 } else { 16 }
}

/// Painted width of `s`, matching nd_text_width(): the last cell has no
/// inter-character gap.
pub fn width(s: &str) -> i16 {
    let s = lang::tr(s);
    let mut w = 0;
    let mut n = 0;

    for c in s.chars() {
        w += char_adv(c);
        n += 1;
    }
    if n == 0 { 0 } else { w - 1 }
}

/// Width at text scale 2 (used by `text_big`).
pub fn width_big(s: &str) -> i16 {
    let s = lang::tr(s);
    let mut w = 0;
    let mut n = 0;

    for c in s.chars() {
        let a = char_adv(c);
        w += if (c as u32) < 0x80 { a * 2 } else { a };
        n += 1;
    }
    if n == 0 { 0 } else { w - 1 }
}

/// Longest prefix of `s` that fits in `max_px`, cut on a codepoint edge.
/// English labels are wider than their Chinese source, so callers that
/// have a fixed column should clip rather than overdraw the neighbour.
pub fn fit<'a>(s: &'a str, max_px: i16) -> &'a str {
    let target = lang::tr(s);
    let mut used = 0i16;

    for (i, c) in target.char_indices() {
        let a = char_adv(c);
        if used + a > max_px {
            return &target[..i];
        }
        used += a;
    }
    target
}

/// A fixed-width text line. The font is monospaced at 6 px per column, so
/// padding with spaces is what aligns the columns - and one text run per
/// row keeps a full list inside the window's display list.
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

// ---------------------------------------------------------------------------
// Surfaces
// ---------------------------------------------------------------------------

/// Page frame: page title, one-line subtitle and a short accent underline.
/// The window chrome already carries the app name, so this is a content
/// header, not a second title bar.
pub fn page(w: &mut wm::Win, cw: i16, ch: i16, title: &str, subtitle: &str) {
    w.fill(0, 0, cw, ch, bg());
    w.text_big(20, 14, text(), title);
    w.text(20, 42, dim(), subtitle);
    w.fill(20, 64, cw - 40, 1, line());
    w.round_fill(20, 64, 44, 3, 2, accent());
}

/// Compact header used by the M8 apps (36 px band, title on the left).
/// New pages use `page`; this keeps existing layouts valid while the
/// applications migrate one screen at a time.
pub fn shell(w: &mut wm::Win, cw: i16, ch: i16, title: &str, _accent: u32) {
    w.fill(0, 0, cw, ch, bg());
    w.fill(0, 0, cw, 34, card_bg());
    w.fill(0, 33, cw, 1, line());
    w.round_fill(0, 33, 56, 3, 1, accent());
    w.text(12, 10, text(), title);
}

/// A card: rounded surface with a hairline border.
pub fn card(w: &mut wm::Win, x: i16, y: i16, cw: i16, ch: i16) {
    w.round_fill(x, y, cw, ch, 10, card_bg());
    w.round_frame(x, y, cw, ch, 10, line());
}

/// Section label inside a page (12 px, secondary).
pub fn section(w: &mut wm::Win, x: i16, y: i16, label: &str) {
    w.text(x, y, dim(), label);
}

pub fn divider(w: &mut wm::Win, x: i16, y: i16, cw: i16) {
    w.fill(x, y, cw, 1, line());
}

/// A list row: optional selection block, label on the left, value on the
/// right, and a 3 px accent bar when selected. Returns the hit rectangle.
pub fn row(
    w: &mut wm::Win,
    y: i16,
    cw: i16,
    label: &str,
    value: &str,
    selected: bool,
    color: u32,
) -> (i16, i16, i16, i16) {
    if selected {
        w.round_fill(4, y, cw - 8, 26, 5, row_sel());
        w.round_fill(4, y + 5, 3, 16, 1, accent());
    } else {
        w.round_fill(4, y, cw - 8, 26, 5, row_bg());
    }
    w.text(14, y + 6, if selected { text() } else { color }, label);
    if !value.is_empty() {
        let vw = width(value);
        w.text(cw - 12 - vw, y + 6, dim(), value);
    }
    (4, y, cw - 8, 26)
}

/// Rounded primary button. `color` is the fill; the caption colour is
/// derived so a custom accent stays readable.
pub fn button(w: &mut wm::Win, x: i16, y: i16, bw: i16, bh: i16, label: &str, color: u32) -> (i16, i16, i16, i16) {
    w.round_fill(x, y, bw, bh, 6, color);
    let tw = width(label);
    w.text(x + (bw - tw) / 2, y + (bh - 14) / 2, on_color(color), label);
    (x, y, bw, bh)
}

/// Secondary button: surface + hairline, used for cancel/back.
pub fn ghost(w: &mut wm::Win, x: i16, y: i16, bw: i16, bh: i16, label: &str) -> (i16, i16, i16, i16) {
    w.round_fill(x, y, bw, bh, 6, card_bg());
    w.round_frame(x, y, bw, bh, 6, line());
    let tw = width(label);
    w.text(x + (bw - tw) / 2, y + (bh - 14) / 2, text(), label);
    (x, y, bw, bh)
}

/// Sidebar navigation item: selected gets a soft fill and an accent bar.
pub fn nav(w: &mut wm::Win, x: i16, y: i16, nw: i16, label: &str, selected: bool) -> (i16, i16, i16, i16) {
    if selected {
        w.round_fill(x, y, nw, 32, 6, row_sel());
        w.round_fill(x, y + 7, 3, 18, 1, accent());
        w.text(x + 14, y + 9, text(), label);
    } else {
        w.text(x + 14, y + 9, dim(), label);
    }
    (x, y, nw, 32)
}

/// Progress bar with a percentage caption.
pub fn progress(w: &mut wm::Win, x: i16, y: i16, pw: i16, permille: u16) {
    let fill = (pw as i32 * permille as i32 / 1000) as i16;

    w.round_fill(x, y, pw, 8, 4, line());
    if fill > 2 {
        w.round_fill(x, y, fill, 8, 4, accent());
    }
    let mut buf = [0u8; 8];
    let pct = (permille as u32 + 5) / 10;
    let s = fmt_u32(pct, &mut buf);

    w.text(x + pw + 8, y + 1, dim(), s);
}

/// Empty-state hint: a quiet line with a small accent dot.
pub fn empty(w: &mut wm::Win, x: i16, y: i16, label: &str) {
    w.round_fill(x, y + 4, 8, 8, 4, line());
    w.text(x + 16, y, dim(), label);
}

pub fn inside(r: (i16, i16, i16, i16), x: i32, y: i32) -> bool {
    let (rx, ry, rw, rh) = r;

    x >= rx as i32 && x < (rx + rw) as i32 && y >= ry as i32 && y < (ry + rh) as i32
}

/// Height of a list row, so callers do not repeat the number.
pub const ROW_H: i16 = 22;
