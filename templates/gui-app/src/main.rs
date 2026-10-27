//! Template: the smallest useful Novus OS desktop application.
//!
//! It creates one window, paints a page with the Fluent controls from
//! `ui`, counts button clicks, follows theme/language changes and closes on
//! the title-bar button or Esc. Copy `src/main.rs` into `apps/src/bin/` and
//! register the binary; `templates/README.md` §1.2 lists every edit.

#![no_std]
#![no_main]

use novus_apps::{ipc, sinfo, sys, ui, wm};

/* Client-area size, excluding the title bar the window manager draws. */
const CW: i16 = 380;
const CH: i16 = 220;

/* Hit rectangle of the button. Painting and hit testing share this constant,
 * so a click can never disagree with what the frame shows. */
const BTN: (i16, i16, i16, i16) = (20, 148, 120, 30);

const KEY_ESC: u32 = 0x100;

/// Repaint the whole client area. Called on start, on interactions and when
/// the theme changes - never from inside the event loop for its own sake.
fn draw(w: &mut wm::Win, clicks: u32) {
    ui::page(w, CW, CH, "示例应用", "templates/gui-app");

    ui::card(w, 20, 80, CW - 40, 52);
    ui::section(w, 32, 90, "点击次数");
    let mut num = [0u8; 12];
    w.text_big(32, 106, ui::accent(), ui::fmt_u32(clicks, &mut num));

    ui::button(w, BTN.0, BTN.1, BTN.2, BTN.3, "点我 +1", ui::accent());
    ui::empty(w, 160, 156, "Esc 关闭");
}

/// Entry point. `nuc_spawn_blob()` enters here with no runtime, no allocator
/// and no arguments - everything the program needs comes from syscalls/IPC.
#[no_mangle]
pub extern "C" fn _start() -> ! {
    /* The compositor is an IPC service: no port, no window. */
    let Some(wm_port) = ipc::lookup(wm::PORT_NAME) else {
        sys::exit(1)
    };
    /* sysinfo carries the theme, language and process control. The UI still
     * works without it, so a missing service is not fatal. */
    let sys_port = ipc::lookup(sinfo::PORT_NAME).unwrap_or(0);
    ui::init(sys_port);

    let Some(mut win) = wm::Win::create(
        wm_port,
        320,
        200,
        CW as i32,
        CH as i32 + wm::TITLE_H,
        "示例应用",
    ) else {
        sys::exit(2)
    };

    let mut clicks = 0u32;
    draw(&mut win, clicks);
    win.invalidate();

    loop {
        /* Drain the window's queue before yielding, so a burst of input is
         * handled in one batch. */
        while let Some(ev) = win.next_event() {
            match ev {
                wm::Ev::Close => {
                    win.destroy();
                    sys::exit(0);
                }
                wm::Ev::Key(KEY_ESC, _) => {
                    win.destroy();
                    sys::exit(0);
                }
                wm::Ev::Mouse(m) if m.kind == wm::MOUSE_DOWN && m.button == 1 => {
                    if ui::inside(BTN, m.x, m.y) {
                        clicks += 1;
                        draw(&mut win, clicks);
                        win.invalidate();
                    }
                }
                _ => {}
            }
        }
        /* Theme or language changed: repaint with the new palette. */
        if ui::poll(sys_port) {
            draw(&mut win, clicks);
            win.invalidate();
        }
        /* A task is not preempted while it runs: hand the CPU back. */
        sys::yield_now();
    }
}
