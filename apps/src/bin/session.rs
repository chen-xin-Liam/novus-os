//! Novus OS login and session manager.

#![no_std]
#![no_main]

use novus_apps::{ipc, sinfo, sys, ui, wm};

const CW: i16 = 420;
const CH: i16 = 252;
const KEY_BACK: u32 = wm::KEY_BACK;
const KEY_ENTER: u32 = wm::KEY_ENTER;

fn as_str(bytes: &[u8], len: usize) -> &str {
    core::str::from_utf8(&bytes[..len]).unwrap_or("")
}

fn draw(w: &mut wm::Win, username: &[u8; 32], user_len: usize,
        password_len: usize, focus: u8, setup: bool, logged: bool, note: &str,
        menu: &ui::Menu) {
    w.clear();
    ui::shell(w, CW, CH, "本地账户", ui::accent());
    w.text(18, 38, ui::dim(), if setup { "首次启动：创建本地账户" } else { "Novus OS 登录" });
    w.text(22, 72, ui::text(), "用户名");
    w.fill(112, 64, 278, 28, ui::bg());
    w.frame(112, 64, 278, 28, if focus == 0 { ui::accent() } else { ui::line() });
    w.text(120, 72, ui::text(), as_str(username, user_len));
    w.text(22, 112, ui::text(), "密码");
    w.fill(112, 104, 278, 28, ui::bg());
    w.frame(112, 104, 278, 28, if focus == 1 { ui::accent() } else { ui::line() });
    let mut masked = [b' '; 64];
    for ch in masked.iter_mut().take(password_len) { *ch = b'*'; }
    w.text(120, 112, ui::text(), core::str::from_utf8(&masked[..password_len]).unwrap_or(""));
    if !logged {
        ui::button(w, 112, 150, 112, 28, if setup { "创建账户" } else { "登录" }, ui::accent());
    } else {
        w.text(114, 158, ui::good(), "会话已登录");
        ui::button(w, 240, 150, 76, 28, "注销", ui::accent());
    }
    ui::button(w, 112, 192, 96, 26, "关机", ui::warn());
    ui::button(w, 218, 192, 96, 26, "重启", ui::accent());
    w.text(18, 232, ui::dim(), note);
    menu.draw(w);
    w.invalidate();
}

fn add_char(buf: &mut [u8], len: &mut usize, key: u32, cap: usize) {
    if key < 0x20 || key > 0x10FFFF {
        return;
    }
    let Some(ch) = core::char::from_u32(key) else { return };
    let mut enc = [0u8; 4];
    let s = ch.encode_utf8(&mut enc);

    if *len + s.len() <= cap {
        buf[*len..*len + s.len()].copy_from_slice(s.as_bytes());
        *len += s.len();
    }
}

fn erase(buf: &mut [u8], len: &mut usize) {
    if *len > 0 {
        *len -= 1;
        while *len > 0 && (buf[*len] & 0xC0) == 0x80 {
            *len -= 1;
        }
        buf[*len] = 0;
    }
}

/* The session operation travels as a fixed-size syscall request, while the
 * username/password buffers stay owned by this process. The syscall copies
 * the request before returning, so these stack buffers need no shared memory. */
/* B-42: one place that performs the session call and maps its return code to
 * the line under the buttons. It used to be copy-pasted into the context menu
 * and into the button handler, which is how Enter ended up with no submit
 * path at all: with the pointer dead (the reason this window exists as the
 * session bootstrap service) a keyboard-only user could never log in, because
 * Enter only toggled focus between the two fields. */
fn attempt(op: u8, username: &[u8; 32], user_len: usize, password: &[u8; 64],
           pass_len: usize) -> (i64, &'static str) {
    let rc = sys::session(op, username, user_len, password, pass_len);
    let note = match rc {
        0 => {
            if op == 1 { "账户已创建并登录" } else { "登录成功" }
        }
        -2 => "账户已存在或尚未创建",
        -3 => "尝试次数过多，请稍后再试",
        -4 => "需要可写持久 NovusFS 磁盘才能创建账户",
        _ => "登录失败：检查用户名和密码",
    };
    (rc, note)
}

fn session_menu(menu: &mut ui::Menu, x: i32, y: i32, logged: bool) {
    if logged {
        menu.set(&[
            ui::MenuItem::new("用户名", 1, 0),
            ui::MenuItem::new("密码", 2, 0),
            ui::MenuItem::new("注销", 3, ui::MENU_DANGER),
            ui::MenuItem::new("", 0, ui::MENU_SEPARATOR),
            ui::MenuItem::new("关机", 4, ui::MENU_DANGER),
            ui::MenuItem::new("重启", 5, 0),
        ]);
    } else {
        menu.set(&[
            ui::MenuItem::new("用户名", 1, 0),
            ui::MenuItem::new("密码", 2, 0),
            ui::MenuItem::new("", 0, ui::MENU_SEPARATOR),
            ui::MenuItem::new("关机", 4, ui::MENU_DANGER),
            ui::MenuItem::new("重启", 5, 0),
        ]);
    }
    menu.open(x, y, CW, CH);
}

#[no_mangle]
pub extern "C" fn _start() -> ! {
    sys::write(b"[APP] session manager up\n");
    let Some(wm_port) = ipc::lookup(wm::PORT_NAME) else { sys::exit(1) };
    let Some(mut win) = wm::Win::create(wm_port, 302, 210, CW as i32 + 2,
                                    CH as i32 + wm::FRAME_H, "用户会话") else { sys::exit(2) };
    let mut username = [0u8; 32];
    let mut password = [0u8; 64];
    let mut user_len = 0usize;
    let mut pass_len = 0usize;
    let mut focus = 0u8;
    let mut setup = sys::session(0, &username, 0, &password, 0) == 0;
    let mut logged = false;
    let sys_port = ipc::lookup(sinfo::PORT_NAME).unwrap_or(0);
    if sys_port != 0 {
        ui::init(sys_port);
    }
    let mut note = if setup { "创建首个账户，密码至少 4 字节" } else { "输入用户名和密码" };
    let mut menu = ui::Menu::new();
    draw(&mut win, &username, user_len, pass_len, focus, setup, logged, note, &menu);
    /* The login window deliberately remains the session bootstrap service:
     * dismissing its close button cannot strand the desktop without a way to
     * authenticate, log out, or request a power transition. */
    loop {
        while let Some(ev) = win.next_event() {
            match ev {
                wm::Ev::Close => {
                    note = "登录窗口保持运行；请登录、注销或关机";
                    draw(&mut win, &username, user_len, pass_len, focus, setup, logged, note, &menu);
                }
                wm::Ev::Mouse(m)
                    if menu.is_open() && m.kind == wm::MOUSE_MOVE =>
                {
                    menu.motion(m.x, m.y);
                    draw(&mut win, &username, user_len, pass_len, focus, setup, logged, note, &menu);
                }
                wm::Ev::Mouse(m)
                    if menu.is_open()
                        && m.kind == wm::MOUSE_DOWN
                        && m.button == wm::BUTTON_RIGHT =>
                {
                    session_menu(&mut menu, m.x, m.y, logged);
                    draw(&mut win, &username, user_len, pass_len, focus, setup, logged, note, &menu);
                }
                wm::Ev::Mouse(m)
                    if menu.is_open()
                        && m.kind == wm::MOUSE_DOWN
                        && m.button == wm::BUTTON_LEFT =>
                {
                    if let Some(action) = menu.click(m.x, m.y) {
                        match action {
                            1 => focus = 0,
                            2 => focus = 1,
                            3 => {
                                if logged {
                                    let _ = sys::session(3, &username, user_len, &password, pass_len);
                                    logged = false;
                                    pass_len = 0;
                                    password = [0; 64];
                                    note = "已注销";
                                } else {
                                    let op = if setup { 1 } else { 2 };
                                    let (rc, msg) = attempt(op, &username, user_len, &password, pass_len);
                                    if rc == 0 {
                                        logged = true;
                                        setup = false;
                                    }
                                    note = msg;
                                    pass_len = 0;
                                    password = [0; 64];
                                }
                            }
                            4 => { let _ = sys::power_off(); }
                            5 => { let _ = sys::reboot(); }
                            _ => {}
                        }
                    } else {
                        menu.close();
                    }
                    draw(&mut win, &username, user_len, pass_len, focus, setup, logged, note, &menu);
                }
                wm::Ev::Mouse(m)
                    if m.kind == wm::MOUSE_DOWN && m.button == wm::BUTTON_RIGHT =>
                {
                    session_menu(&mut menu, m.x, m.y, logged);
                    draw(&mut win, &username, user_len, pass_len, focus, setup, logged, note, &menu);
                }
                wm::Ev::Key(k, _) if k == wm::KEY_ESC && menu.is_open() => {
                    menu.close();
                    draw(&mut win, &username, user_len, pass_len, focus, setup, logged, note, &menu);
                }
                wm::Ev::Mouse(m)
                    if !menu.is_open()
                        && m.kind == wm::MOUSE_DOWN
                        && m.button == wm::BUTTON_LEFT =>
                {
                    if m.y >= 64 && m.y < 94 { focus = 0; }
                    else if m.y >= 104 && m.y < 134 { focus = 1; }
                    else if m.y >= 150 && m.y < 180 {
                        if logged && m.x >= 240 && m.x < 316 {
                            let _ = sys::session(3, &username, user_len, &password, pass_len);
                            logged = false;
                            pass_len = 0;
                            password = [0; 64];
                            note = "已注销";
                        } else if !logged && m.x >= 112 && m.x < 224 {
                            let op = if setup { 1 } else { 2 };
                            let (rc, msg) = attempt(op, &username, user_len, &password, pass_len);
                            if rc == 0 {
                                logged = true;
                                setup = false;
                            }
                            note = msg;
                            pass_len = 0;
                            password = [0; 64];
                        }
                    } else if m.y >= 192 && m.y < 222 {
                        if m.x >= 112 && m.x < 208 { let _ = sys::power_off(); }
                        else if m.x >= 218 && m.x < 314 { let _ = sys::reboot(); }
                    }
                    draw(&mut win, &username, user_len, pass_len, focus, setup, logged, note, &menu);
                }
                wm::Ev::Key(key, _mods) => {
                    /* B-42: Enter is the submit key, not just a focus switch.
                     * In the password field it behaves exactly like pressing
                     * 登录; in the username field it still moves to the
                     * password. Tab keeps cycling. */
                    if key == KEY_ENTER && focus == 1 && !logged {
                        let op = if setup { 1 } else { 2 };
                        let (rc, msg) = attempt(op, &username, user_len, &password, pass_len);
                        if rc == 0 {
                            logged = true;
                            setup = false;
                        }
                        note = msg;
                        pass_len = 0;
                        password = [0; 64];
                    } else if key == KEY_ENTER || key == wm::KEY_TAB {
                        focus = 1 - focus;
                    } else if key == KEY_BACK {
                        if focus == 0 { erase(&mut username, &mut user_len); }
                        else { erase(&mut password, &mut pass_len); }
                    } else if focus == 0 {
                        add_char(&mut username, &mut user_len, key, 31);
                    } else {
                        add_char(&mut password, &mut pass_len, key, 63);
                    }
                    draw(&mut win, &username, user_len, pass_len, focus, setup, logged, note, &menu);
                }
                _ => {}
            }
        }
        if sys_port != 0 && ui::poll(sys_port) {
            draw(&mut win, &username, user_len, pass_len, focus, setup, logged, note, &menu);
        }
        /* B-39: block for a tick between event polls instead of spinning. */
        sys::sleep_ticks(1);
    }
}
