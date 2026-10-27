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

const COLS: usize = 72;
const ROWS: usize = 26;
const LEFT: i16 = 6;
const TOP: i16 = 6;
const CW: i16 = LEFT + (COLS as i16) * 12 + LEFT;
const CH: i16 = (ROWS as i16) * 16 + TOP * 2;

const BG: u32 = 0x0E1117;
const FG: u32 = 0xD4D4D4;
const DIM: u32 = 0x7A8290;

const KEY_ENTER: u32 = 0x109;
const KEY_BACK: u32 = 0x10A;
const KEY_TAB: u32 = 0x108;
const KEY_LEFT: u32 = 0x103;
const KEY_RIGHT: u32 = 0x104;
const KEY_UP: u32 = 0x101;
const KEY_DOWN: u32 = 0x102;
const KEY_HOME: u32 = 0x106;
const KEY_END: u32 = 0x107;
const KEY_DELETE: u32 = 0x105;
const KEY_ESC: u32 = 0x100;
const MOD_CTRL: u8 = 0x02;

const LINE_MAX: usize = 160;
const SCROLL_CAP: usize = 512;
const INPUT_MAX: usize = 128;

fn utf8_len(b: u8) -> usize {
    if b < 0x80 {
        1
    } else if b < 0xE0 {
        2
    } else if b < 0xF0 {
        3
    } else {
        4
    }
}

fn total_chars(text: &[u8]) -> usize {
    let mut n = 0;
    let mut i = 0;
    while i < text.len() {
        i += utf8_len(text[i]);
        n += 1;
    }
    n
}

fn vis_rows(text: &[u8]) -> usize {
    let n = total_chars(text);
    if n == 0 {
        1
    } else {
        (n + COLS - 1) / COLS
    }
}

fn byte_at_char(text: &[u8], ch: usize) -> usize {
    let mut ci = 0;
    let mut i = 0;
    while i < text.len() && ci < ch {
        i += utf8_len(text[i]);
        ci += 1;
    }
    i
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
        } else if self.cur_len < LINE_MAX {
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
    col: usize,
    run: [u8; COLS * 4],
    run_len: usize,
    run_color: u32,
}

impl<'a> Drawer<'a> {
    fn flush(&mut self) {
        if self.run_len > 0 {
            let s = unsafe { core::str::from_utf8_unchecked(&self.run[..self.run_len]) };
            self.win.text_big(self.x, self.y, self.run_color, s);
            self.x += (self.run_len as i16) * 12;
            self.run_len = 0;
        }
    }

    fn newline(&mut self) {
        self.flush();
        self.y += 16;
        self.x = LEFT;
        self.col = 0;
    }

    fn push(&mut self, text: &[u8], color: u32) {
        let mut i = 0;
        while i < text.len() {
            let clen = utf8_len(text[i]);
            if self.col == COLS {
                self.newline();
            }
            if self.run_len > 0 && color != self.run_color {
                self.flush();
            }
            let mut k = 0;
            while k < clen && self.run_len < self.run.len() {
                self.run[self.run_len] = text[i + k];
                self.run_len += 1;
                k += 1;
            }
            self.run_color = color;
            self.col += 1;
            i += clen;
        }
    }
}

fn render_line(win: &mut wm::Win, text: &[u8], color: u32, skip: &mut usize, y: &mut i16) {
    let rows = vis_rows(text);
    for r in 0..rows {
        if *skip > 0 {
            *skip -= 1;
            continue;
        }
        let s = byte_at_char(text, r * COLS);
        let e = byte_at_char(text, (r + 1) * COLS);
        if e > s {
            let slice = unsafe { core::str::from_utf8_unchecked(&text[s..e]) };
            win.text_big(LEFT, *y, color, slice);
        }
        *y += 16;
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
    for &b in &shell.cwd[..shell.cwd_len] {
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
    let prompt_len = {
        let mut p = [0u8; 192];
        build_prompt(shell, &mut p)
    };
    let input_total = prompt_len + ilen;
    let input_rows = if input_total == 0 {
        1
    } else {
        (input_total + COLS - 1) / COLS
    };
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
        col: 0,
        run: [0u8; COLS * 4],
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
                        let mut k = cursor;
                        while k < ilen {
                            input[k - 1] = input[k];
                            k += 1;
                        }
                        ilen -= 1;
                        cursor -= 1;
                    }
                }
                KEY_DELETE => {
                    if cursor < ilen {
                        let mut k = cursor;
                        while k < ilen - 1 {
                            input[k] = input[k + 1];
                            k += 1;
                        }
                        ilen -= 1;
                    }
                }
                KEY_LEFT => {
                    if cursor > 0 {
                        cursor -= 1;
                    }
                }
                KEY_RIGHT => {
                    if cursor < ilen {
                        cursor += 1;
                    }
                }
                KEY_HOME => cursor = 0,
                KEY_END => cursor = ilen,
                KEY_UP => {
                    if shell.history_len() > 0 {
                        if hist_idx > 0 {
                            hist_idx -= 1;
                        }
                        if let Some(h) = shell.history(hist_idx) {
                            let l = h.len().min(INPUT_MAX);
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
                        let l = h.len().min(INPUT_MAX);
                        input[..l].copy_from_slice(&h[..l]);
                        ilen = l;
                        cursor = l;
                    }
                }
                KEY_TAB => {}
                KEY_ESC => {}
                _ => {
                    if (mods & MOD_CTRL) == 0 && key >= 0x20 && key <= 0x7e {
                        if ilen < INPUT_MAX {
                            let mut k = ilen;
                            while k > cursor {
                                input[k] = input[k - 1];
                                k -= 1;
                            }
                            input[cursor] = key as u8;
                            ilen += 1;
                            cursor += 1;
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
