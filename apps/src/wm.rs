//! Novus-WM client (wm/novus-wm/nwm_proto.h), version 1.1.
//!
//! A window's content is a small display list the WM replays: shapes
//! (fills, frames, lines) and text runs. `clear()` starts a frame,
//! `shape()`/`text()` append to it; that is one message per item, which at
//! these sizes is cheaper than a shared-memory protocol and keeps every
//! client sandboxed - it can only ever draw inside its own window.

use crate::ipc;

pub const PORT_NAME: &[u8] = b"nwm\0";

pub const REQ_CREATE: u16 = 0x0101;
pub const REQ_DESTROY: u16 = 0x0102;
pub const REQ_MOVE: u16 = 0x0103;
pub const REQ_RAISE: u16 = 0x0105;
pub const REQ_FOCUS: u16 = 0x0109;
pub const REQ_TITLE: u16 = 0x010B;
pub const REQ_EFFECT: u16 = 0x010C;
pub const REQ_CLEAR: u16 = 0x010D;
pub const REQ_SHAPE: u16 = 0x010E;
pub const REQ_TEXT: u16 = 0x010F;
pub const REQ_INVALIDATE: u16 = 0x0110;

pub const RSP_OK: u16 = 0x0001;
pub const RSP_ERR: u16 = 0x0002;

pub const EV_MOUSE: u16 = 0x0202;
pub const EV_KEY: u16 = 0x0203;
pub const EV_FOCUS: u16 = 0x0204;
pub const EV_CLOSE: u16 = 0x0206;

pub const OP_FILL: u8 = 1;
pub const OP_FRAME: u8 = 2;
pub const OP_HLINE: u8 = 3;
pub const OP_VLINE: u8 = 4;
pub const OP_CIRCLE: u8 = 5;
/* v1.2: `pad` carries the corner radius. */
pub const OP_ROUND_FILL: u8 = 6;
pub const OP_ROUND_FRAME: u8 = 7;

/* The WM's per-window display list. A client that sends more is cut off
 * silently by the server, which looks like a rendering bug; counting here
 * turns it into a log line naming the budget. */
pub const MAX_SHAPES: u16 = 40;
pub const MAX_TEXTS: u16 = 48;

/// Height of the title bar the WM draws above the client area. A window's
/// `h` passed to create() must be client height plus this, or the bottom
/// rows of the content are cut off.
pub const TITLE_H: i32 = 32;

pub const TITLE_MAX: usize = 24;
pub const TEXT_MAX: usize = 96;

#[repr(C)]
struct Create {
    win: u32,
    x: i32,
    y: i32,
    w: i32,
    h: i32,
    flags: u16,
    pad: u16,
    title: [u8; TITLE_MAX],
}

#[repr(C)]
struct Move {
    win: u32,
    x: i32,
    y: i32,
}

#[repr(C)]
struct Title {
    win: u32,
    title: [u8; TITLE_MAX],
}

#[repr(C)]
struct Shape {
    win: u32,
    op: u8,
    pad: u8,
    x: i16,
    y: i16,
    w: i16,
    h: i16,
    color: u32,
}

#[repr(C)]
struct Text {
    win: u32,
    x: i16,
    y: i16,
    scale: u8,
    len: u8,
    color: u32,
    text: [u8; TEXT_MAX],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct MouseEvent {
    pub win: u32,
    pub x: i32,
    pub y: i32,
    pub kind: u8,
    pub button: u8,
    pub mods: u8,
    pub pad: u8,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct KeyEvent {
    win: u32,
    key: u32,
    mods: u8,
    pad: [u8; 3],
}

pub const MOUSE_DOWN: u8 = 2;
pub const MOUSE_UP: u8 = 3;

// Key values that are not text. Text keys carry their Unicode codepoint, so
// these live above U+10FFFF and can never collide with a character.
pub const KEY_BASE: u32 = 0x11_0000;
pub const KEY_ESC: u32 = KEY_BASE;
pub const KEY_UP: u32 = KEY_BASE + 1;
pub const KEY_DOWN: u32 = KEY_BASE + 2;
pub const KEY_LEFT: u32 = KEY_BASE + 3;
pub const KEY_RIGHT: u32 = KEY_BASE + 4;
pub const KEY_DELETE: u32 = KEY_BASE + 5;
pub const KEY_HOME: u32 = KEY_BASE + 6;
pub const KEY_END: u32 = KEY_BASE + 7;
pub const KEY_TAB: u32 = KEY_BASE + 8;
pub const KEY_ENTER: u32 = KEY_BASE + 9;
pub const KEY_BACK: u32 = KEY_BASE + 10;

pub enum Ev {
    Mouse(MouseEvent),
    Key(u32, u8),
    Focus(u32),
    Close,
    Other(u16),
}

pub struct Win {
    pub id: u32,
    pub port: u64,
    shapes: u16,
    texts: u16,
}

/// Send, retrying while the WM's queue is full. A repaint is a burst of
/// messages; without the retry a burst longer than the queue would leave
/// the frame half drawn.
fn send(port: u64, ty: u16, flags: u16, payload: &[u8]) {
    loop {
        let rc = ipc::send(port, ty, flags, payload);

        if rc != ipc::EBUSY {
            return;
        }
        crate::sys::yield_now();
    }
}

/// Longest prefix of `s` that fits in `max` bytes without splitting a
/// codepoint. Every buffer the protocol fills goes through this, so a
/// Chinese title or text run can never end in U+FFFD.
fn fit_bytes(s: &str, max: usize) -> &[u8] {
    let bytes = s.as_bytes();
    let mut n = bytes.len().min(max);

    while n > 0 && n < bytes.len() && (bytes[n] & 0xC0) == 0x80 {
        n -= 1;
    }
    &bytes[..n]
}

fn copy_title(title: &str) -> [u8; TITLE_MAX] {
    let mut out = [0u8; TITLE_MAX];
    let src = fit_bytes(title, TITLE_MAX - 1);

    out[..src.len()].copy_from_slice(src);
    out
}

impl Win {
    /// Create a window and wait for the id. CREATE always gets a reply
    /// (the id is the result), so a blocking receive is correct here.
    pub fn create(port: u64, x: i32, y: i32, w: i32, h: i32, title: &str) -> Option<Win> {
        let c = Create {
            win: 0,
            x,
            y,
            w,
            h,
            flags: 0,
            pad: 0,
            title: copy_title(title),
        };

        if ipc::send(port, REQ_CREATE, ipc::F_SYNC, ipc::bytes_of(&c)) != 0 {
            return None;
        }
        loop {
            let m = ipc::recv(0, true)?;

            if m.ty == RSP_OK && m.len >= 8 {
                let p = m.payload();
                let status = i32::from_le_bytes(p[4..8].try_into().unwrap());

                if status != 0 {
                    return None;
                }
                return Some(Win {
                    id: u32::from_le_bytes(p[0..4].try_into().unwrap()),
                    port,
                    shapes: 0,
                    texts: 0,
                });
            }
            if m.ty == RSP_ERR {
                return None;
            }
        }
    }

    pub fn clear(&mut self) {
        let w = self.id.to_le_bytes();

        self.shapes = 0;
        self.texts = 0;
        send(self.port, REQ_CLEAR, 0, &w);
    }

    pub fn shape(&mut self, op: u8, x: i16, y: i16, w: i16, h: i16, color: u32) {
        self.shape_round(op, x, y, w, h, 0, color);
    }

    pub fn shape_round(&mut self, op: u8, x: i16, y: i16, w: i16, h: i16,
                       radius: u8, color: u32) {
        let s = Shape {
            win: self.id,
            op,
            pad: radius,
            x,
            y,
            w,
            h,
            color,
        };

        self.shapes = self.shapes.saturating_add(1);
        if self.shapes == MAX_SHAPES + 1 {
            crate::sys::write(b"[APP] display list over shape budget\n");
        }
        send(self.port, REQ_SHAPE, 0, ipc::bytes_of(&s));
    }

    pub fn round_fill(&mut self, x: i16, y: i16, w: i16, h: i16, radius: u8, color: u32) {
        self.shape_round(OP_ROUND_FILL, x, y, w, h, radius, color);
    }

    pub fn round_frame(&mut self, x: i16, y: i16, w: i16, h: i16, radius: u8, color: u32) {
        self.shape_round(OP_ROUND_FRAME, x, y, w, h, radius, color);
    }

    pub fn fill(&mut self, x: i16, y: i16, w: i16, h: i16, color: u32) {
        self.shape(OP_FILL, x, y, w, h, color);
    }

    pub fn frame(&mut self, x: i16, y: i16, w: i16, h: i16, color: u32) {
        self.shape(OP_FRAME, x, y, w, h, color);
    }

    pub fn text(&mut self, x: i16, y: i16, color: u32, s: &str) {
        self.text_at(x, y, 1, color, s);
    }

    pub fn text_big(&mut self, x: i16, y: i16, color: u32, s: &str) {
        self.text_at(x, y, 2, color, s);
    }

    fn text_at(&mut self, x: i16, y: i16, scale: u8, color: u32, s: &str) {
        let mut t = Text {
            win: self.id,
            x,
            y,
            scale,
            len: 0,
            color,
            text: [0u8; TEXT_MAX],
        };
        let src = fit_bytes(s, TEXT_MAX - 1);
        let n = src.len();

        t.text[..n].copy_from_slice(src);
        t.len = n as u8;
        self.texts = self.texts.saturating_add(1);
        if self.texts == MAX_TEXTS + 1 {
            crate::sys::write(b"[APP] display list over text budget\n");
        }
        send(self.port, REQ_TEXT, 0, ipc::bytes_of(&t));
    }

    pub fn move_to(&self, x: i32, y: i32) {
        let m = Move {
            win: self.id,
            x,
            y,
        };

        send(self.port, REQ_MOVE, 0, ipc::bytes_of(&m));
    }

    pub fn set_title(&self, title: &str) {
        let t = Title {
            win: self.id,
            title: copy_title(title),
        };

        send(self.port, REQ_TITLE, 0, ipc::bytes_of(&t));
    }

    /// Repaint the whole client area from the display list. Call it once
    /// at the end of a frame: the WM then composites every item together
    /// instead of repainting one rectangle per message.
    pub fn invalidate(&self) {
        let w = self.id.to_le_bytes();

        send(self.port, REQ_INVALIDATE, 0, &w);
    }

    pub fn raise(&self) {
        let w = self.id.to_le_bytes();

        send(self.port, REQ_RAISE, 0, &w);
    }

    pub fn destroy(&self) {
        let w = self.id.to_le_bytes();

        send(self.port, REQ_DESTROY, 0, &w);
    }

    /// Next event for this window, or None when the queue is empty.
    pub fn next_event(&self) -> Option<Ev> {
        loop {
            let m = ipc::recv(0, false)?;

            match m.ty {
                EV_MOUSE => {
                    if let Some(e) = m.read::<MouseEvent>() {
                        if e.win == self.id {
                            return Some(Ev::Mouse(e));
                        }
                    }
                }
                EV_CLOSE => {
                    let w = ipc::read::<u32>(m.payload());
                    if w == Some(self.id) {
                        return Some(Ev::Close);
                    }
                }
                EV_KEY => {
                    if let Some(e) = m.read::<KeyEvent>() {
                        if e.win == self.id {
                            return Some(Ev::Key(e.key, e.mods));
                        }
                    }
                }
                EV_FOCUS => return Some(Ev::Focus(self.id)),
                other => return Some(Ev::Other(other)),
            }
        }
    }
}
