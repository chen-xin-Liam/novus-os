//! `nsh` terminal: a small text-grid window driving the shell engine.
//!
//! The window is a dark, monospaced panel - deliberately unlike the Fluent
//! card UI of the rest of the desktop - so it reads as a real terminal. Input
//! is line-buffered with a few editing keys and a history ring; output and the
//! prompt are rendered through the shell's [`Sink`] into a scrollback ring,
//! then painted as wrapped text runs each frame.

#![no_std]
#![no_main]

use novus_apps::{ipc, shell, sinfo, sys, ui, wm};

// A row is 864 px wide (144 ASCII cells) and 16 px tall, which is exactly
// the Unicode cell height: ASCII keeps its 6x9 face, Chinese and other
// 8/16 px glyphs fit without overlap. Wrapping and the caret use painted
// width, not byte or character counts.
const COLS: usize = 144;
const TEXT_PX: i16 = (COLS as i16) * 6;
const ROWS: usize = 26;
const LEFT: i16 = 6;
const TOP: i16 = 6;
const CW: i16 = LEFT + TEXT_PX + LEFT;
const CH: i16 = (ROWS as i16) * 16 + TOP * 2;

const BG: u32 = 0x0E1117;
const FG: u32 = 0xD4D4D4;
const DIM: u32 = 0x7A8290;

const KEY_ENTER: u32 = wm::KEY_ENTER;
const KEY_BACK: u32 = wm::KEY_BACK;
const KEY_TAB: u32 = wm::KEY_TAB;
const KEY_LEFT: u32 = wm::KEY_LEFT;
const KEY_RIGHT: u32 = wm::KEY_RIGHT;
const KEY_UP: u32 = wm::KEY_UP;
const KEY_DOWN: u32 = wm::KEY_DOWN;
const KEY_HOME: u32 = wm::KEY_HOME;
const KEY_END: u32 = wm::KEY_END;
const KEY_DELETE: u32 = wm::KEY_DELETE;
const KEY_ESC: u32 = wm::KEY_ESC;
const MOD_CTRL: u8 = 0x02;

const LINE_MAX: usize = 320;
const SCROLL_CAP: usize = 512;
const INPUT_MAX: usize = 192;

fn utf8_len(b: u8) -> usize {
    if b < 0x80 {
        1
    } else if (b & 0xE0) == 0xC0 {
        2
    } else if (b & 0xF0) == 0xE0 {
        3
    } else if (b & 0xF8) == 0xF0 {
        4
    } else {
        1 /* stray continuation or invalid lead: consume one byte */
    }
}

/// Byte offset just past the codepoint at `i`, never inside a sequence.
fn next_char(text: &[u8], i: usize) -> usize {
    if i >= text.len() {
        return text.len();
    }
    let end = (i + utf8_len(text[i])).min(text.len());
    if core::str::from_utf8(&text[i..end]).is_ok() {
        end
    } else {
        i + 1
    }
}

/// Byte offset of the codepoint that ends just before `i`.
fn prev_char(text: &[u8], i: usize) -> usize {
    if i == 0 {
        return 0;
    }
    let mut p = i - 1;
    while p > 0 && (text[p] & 0xC0) == 0x80 {
        p -= 1;
    }
    if core::str::from_utf8(&text[p..i]).is_ok() {
        p
    } else {
        i - 1
    }
}

fn char_of(text: &[u8], i: usize) -> char {
    let end = next_char(text, i);
    core::str::from_utf8(&text[i..end])
        .ok()
        .and_then(|s| s.chars().next())
        .unwrap_or('\u{FFFD}')
}

/// End of the row that starts at `start`: the last codepoint boundary whose
/// painted width still fits in TEXT_PX.
fn row_end(text: &[u8], start: usize) -> usize {
    let mut i = start;
    let mut w = 0i16;
    while i < text.len() {
        let a = ui::char_adv(char_of(text, i));
        if w + a > TEXT_PX && w > 0 {
            break;
        }
        w += a;
        i = next_char(text, i);
    }
    i
}

fn vis_rows(text: &[u8]) -> usize {
    let mut i = 0;
    let mut rows = 1;
    while i < text.len() {
        let e = row_end(text, i);
        i = if e == i { i + 1 } else { e };
        if i < text.len() {
            rows += 1;
        }
    }
    rows
}

/// Drop trailing bytes that would split a codepoint.
fn truncate_utf8(text: &[u8], max: usize) -> usize {
    let mut n = text.len().min(max);
    while n > 0 && n < text.len() && (text[n] & 0xC0) == 0x80 {
        n -= 1;
    }
    n
}

/// Append a byte to the scrollback, splitting on `'\n'`.
struct Scroll {
    buf: [[u8; LINE_MAX]; SCROLL_CAP],
    len: [usize; SCROLL_CAP],
    head: usize,
    count: usize,
    cur: [u8; LINE_MAX],
    cur_len: usize,
}

impl Scroll {
    fn new() -> Scroll {
        Scroll {
            buf: [[0u8; LINE_MAX]; SCROLL_CAP],
            len: [0usize; SCROLL_CAP],
            head: 0,
            count: 0,
            cur: [0u8; LINE_MAX],
            cur_len: 0,
        }
    }

    fn push_byte(&mut self, b: u8) {
        if b == b'\n' {
            self.commit();
            return;
        }
        /* Wrap before a line fills up, and only at a codepoint start, so a
         * long UTF-8 sequence is never split across two scrollback rows. */
        if self.cur_len + 4 > LINE_MAX && (b & 0xC0) != 0x80 {
            self.commit();
        }
        if self.cur_len < LINE_MAX {
            self.cur[self.cur_len] = b;
            self.cur_len += 1;
        }
    }

    fn commit(&mut self) {
        let idx = self.head;
        self.buf[idx][..self.cur_len].copy_from_slice(&self.cur[..self.cur_len]);
        self.len[idx] = self.cur_len;
        self.head = (idx + 1) % SCROLL_CAP;
        if self.count < SCROLL_CAP {
            self.count += 1;
        }
        self.cur_len = 0;
        for b in self.cur.iter_mut() {
            *b = 0;
        }
    }

    fn flush_pending(&mut self) {
        if self.cur_len > 0 {
            self.commit();
        }
    }

    fn clear(&mut self) {
        self.head = 0;
        self.count = 0;
        self.cur_len = 0;
    }
}

/// The terminal's [`shell::Sink`]: writes into the scrollback ring.
struct TermSink<'a> {
    scroll: &'a mut Scroll,
}

impl<'a> shell::Sink for TermSink<'a> {
    fn write(&mut self, data: &[u8]) {
        for &b in data.iter() {
            self.scroll.push_byte(b);
        }
    }
    fn clear_screen(&mut self) {
        self.scroll.clear();
    }
}

/// Paints text runs across visual rows, tracking colour changes and wrapping.
struct Drawer<'a> {
    win: &'a mut wm::Win,
    y: i16,
    x: i16,
    run_x: i16,
    run: [u8; 320],
    run_len: usize,
    run_color: u32,
}

impl<'a> Drawer<'a> {
    fn flush(&mut self) {
        if self.run_len > 0 {
            let s = core::str::from_utf8(&self.run[..self.run_len]).unwrap_or("");
            self.win.text(self.run_x, self.y, self.run_color, s);
            self.run_len = 0;
        }
    }

    fn newline(&mut self) {
        self.flush();
        self.y += 16;
        self.x = LEFT;
    }

    fn push(&mut self, text: &[u8], color: u32) {
        let mut i = 0;
        while i < text.len() {
            let end = next_char(text, i);
            let a = ui::char_adv(char_of(text, i));

            if self.x + a > LEFT + TEXT_PX {
                self.newline();
            }
            if self.run_len > 0 && color != self.run_color {
                self.flush();
            }
            if self.run_len == 0 {
                self.run_x = self.x;
            }
            let mut k = i;
            while k < end && self.run_len < self.run.len() {
                self.run[self.run_len] = text[k];
                self.run_len += 1;
                k += 1;
            }
            self.run_color = color;
            self.x += a;
            i = end;
        }
    }
}

fn render_line(win: &mut wm::Win, text: &[u8], color: u32, skip: &mut usize, y: &mut i16) {
    let rows = vis_rows(text);
    let mut pos = 0usize;

    for _ in 0..rows {
        let end = if pos < text.len() {
            row_end(text, pos)
        } else {
            pos
        };
        if *skip > 0 {
            *skip -= 1;
        } else {
            if end > pos {
                if let Ok(slice) = core::str::from_utf8(&text[pos..end]) {
                    win.text(LEFT, *y, color, slice);
                }
            }
            *y += 16;
        }
        pos = if end == pos { pos + 1 } else { end };
    }
}

fn build_prompt(shell: &shell::Shell, buf: &mut [u8]) -> usize {
    let mut n = 0;
    for &b in b"user@novus:" {
        if n + 1 < buf.len() {
            buf[n] = b;
            n += 1;
        }
    }
    let cwd = truncate_utf8(&shell.cwd[..shell.cwd_len], buf.len() - n - 1);
    for &b in &shell.cwd[..cwd] {
        if n + 1 < buf.len() {
            buf[n] = b;
            n += 1;
        }
    }
    for &b in b"$ " {
        if n + 1 < buf.len() {
            buf[n] = b;
            n += 1;
        }
    }
    n
}

fn render(win: &mut wm::Win, shell: &shell::Shell, scroll: &Scroll, input: &[u8], ilen: usize, cursor: usize) {
    win.clear();
    win.fill(0, 0, CW, CH, BG);

    let mut y = TOP;
    // The prompt and the edit buffer wrap together, measured in pixels.
    let mut input_rows = {
        let mut line = [0u8; 384];
        let mut ln = build_prompt(shell, &mut line);
        for k in 0..ilen {
            if ln + 1 < line.len() {
                line[ln] = input[k];
                ln += 1;
            }
        }
        vis_rows(&line[..ln])
    };
    if input_rows == 0 {
        input_rows = 1;
    }
    let avail = if ROWS > input_rows { ROWS - input_rows } else { 0 };

    let mut t = 0usize;
    for i in 0..scroll.count {
        let li = (scroll.head + SCROLL_CAP - scroll.count + i) % SCROLL_CAP;
        t += vis_rows(&scroll.buf[li][..scroll.len[li]]);
    }
    let mut skip = if t > avail { t - avail } else { 0 };

    for i in 0..scroll.count {
        let li = (scroll.head + SCROLL_CAP - scroll.count + i) % SCROLL_CAP;
        render_line(win, &scroll.buf[li][..scroll.len[li]], FG, &mut skip, &mut y);
    }

    // Input line, coloured, with the caret drawn inline at the edit position.
    let accent = ui::accent();
    let mut drawer = Drawer {
        win,
        y,
        x: LEFT,
        run_x: LEFT,
        run: [0u8; 320],
        run_len: 0,
        run_color: FG,
    };
    drawer.push(b"user@novus:", accent);
    drawer.push(&shell.cwd[..shell.cwd_len], DIM);
    drawer.push(b"$ ", accent);
    let before = &input[..cursor];
    let after = &input[cursor..ilen];
    drawer.push(before, FG);
    let caret_x = drawer.x;
    let caret_y = drawer.y;
    drawer.push(after, FG);
    drawer.flush();
    drawer.win.fill(caret_x, caret_y, 7, 13, accent);
}

#[no_mangle]
pub extern "C" fn _start() -> ! {
    let wm_port = match ipc::lookup(wm::PORT_NAME) {
        Some(p) => p,
        None => loop {},
    };
    let sys_port = match ipc::lookup(sinfo::PORT_NAME) {
        Some(p) => p,
        None => 0,
    };
    ui::init(sys_port);

    let mut win = match wm::Win::create(wm_port, 200, 160, CW as i32, CH as i32 + wm::TITLE_H, "终端") {
        Some(w) => w,
        None => sys::exit(1),
    };

    let mut shell = shell::Shell::new(sys_port);
    let mut scroll = Scroll::new();
    let welcome = b"Novus OS nsh - type 'help'.\n";
    for &b in welcome.iter() {
        scroll.push_byte(b);
    }

    let mut input = [0u8; INPUT_MAX];
    let mut ilen = 0usize;
    let mut cursor = 0usize;
    let mut hist_idx = shell.history_len();

    loop {
        render(&mut win, &shell, &scroll, &input, ilen, cursor);
        win.invalidate();

        match win.next_event() {
            Some(wm::Ev::Key(key, mods)) => match key {
                KEY_ENTER => {
                    let mut eb = [0u8; 200];
                    let mut en = build_prompt(&shell, &mut eb);
                    for k in 0..ilen {
                        if en < 198 {
                            eb[en] = input[k];
                            en += 1;
                        }
                    }
                    eb[en] = b'\n';
                    en += 1;
                    for k in 0..en {
                        scroll.push_byte(eb[k]);
                    }
                    {
                        let mut sink = TermSink { scroll: &mut scroll };
                        shell.run_line(&input[..ilen], &mut sink);
                        scroll.flush_pending();
                    }
                    if ilen > 0 {
                        shell.push_history(&input[..ilen]);
                    }
                    ilen = 0;
                    cursor = 0;
                    hist_idx = shell.history_len();
                }
                KEY_BACK => {
                    if cursor > 0 {
                        let start = prev_char(&input[..ilen], cursor);
                        let cut = cursor - start;
                        let mut k = start;
                        while k + cut < ilen {
                            input[k] = input[k + cut];
                            k += 1;
                        }
                        ilen -= cut;
                        cursor = start;
                    }
                }
                KEY_DELETE => {
                    if cursor < ilen {
                        let end = next_char(&input[..ilen], cursor);
                        let cut = end - cursor;
                        let mut k = cursor;
                        while k + cut < ilen {
                            input[k] = input[k + cut];
                            k += 1;
                        }
                        ilen -= cut;
                    }
                }
                KEY_LEFT => {
                    cursor = prev_char(&input[..ilen], cursor);
                }
                KEY_RIGHT => {
                    cursor = next_char(&input[..ilen], cursor);
                }
                KEY_HOME => cursor = 0,
                KEY_END => cursor = ilen,
                KEY_UP => {
                    if shell.history_len() > 0 {
                        if hist_idx > 0 {
                            hist_idx -= 1;
                        }
                        if let Some(h) = shell.history(hist_idx) {
                            let l = truncate_utf8(&h, INPUT_MAX);
                            input[..l].copy_from_slice(&h[..l]);
                            ilen = l;
                            cursor = l;
                        }
                    }
                }
                KEY_DOWN => {
                    if hist_idx < shell.history_len() {
                        hist_idx += 1;
                    }
                    if hist_idx >= shell.history_len() {
                        ilen = 0;
                        cursor = 0;
                    } else if let Some(h) = shell.history(hist_idx) {
                        let l = truncate_utf8(&h, INPUT_MAX);
                        input[..l].copy_from_slice(&h[..l]);
                        ilen = l;
                        cursor = l;
                    }
                }
                KEY_TAB => {}
                KEY_ESC => {}
                _ => {
                    if (mods & MOD_CTRL) == 0 && key >= 0x20 && key <= 0x10FFFF {
                        if let Some(ch) = core::char::from_u32(key) {
                            let mut enc = [0u8; 4];
                            let s = ch.encode_utf8(&mut enc);
                            let add = s.len();

                            if ilen + add <= INPUT_MAX {
                                let mut k = ilen;
                                while k > cursor {
                                    input[k + add - 1] = input[k - 1];
                                    k -= 1;
                                }
                                for (j, &b) in s.as_bytes().iter().enumerate() {
                                    input[cursor + j] = b;
                                }
                                ilen += add;
                                cursor += add;
                            }
                        }
                    } else if (mods & MOD_CTRL) != 0 {
                        if key == b'c' as u32 {
                            let mut eb = [0u8; 200];
                            let mut en = build_prompt(&shell, &mut eb);
                            for k in 0..ilen {
                                if en < 196 {
                                    eb[en] = input[k];
                                    en += 1;
                                }
                            }
                            eb[en] = b'^';
                            en += 1;
                            eb[en] = b'C';
                            en += 1;
                            eb[en] = b'\n';
                            en += 1;
                            for k in 0..en {
                                scroll.push_byte(eb[k]);
                            }
                            ilen = 0;
                            cursor = 0;
                            hist_idx = shell.history_len();
                        } else if key == b'l' as u32 {
                            scroll.clear();
                        }
                    }
                }
            },
            Some(wm::Ev::Close) => sys::exit(0),
            Some(wm::Ev::Focus(_)) => {}
            Some(wm::Ev::Mouse(_)) => {}
            Some(wm::Ev::Other(_)) => {}
            None => sys::yield_now(),
        }
    }
}
