//! Novus OS - network panel (M10).
//!
//! Shows what the kernel actually has: link type (Ethernet / fiber /
//! wireless), the DHCP lease, counters, and two live tests - ICMP echo and
//! a TCP round trip - that run inside the kernel and report through
//! sysinfo.

#![no_std]
#![no_main]

use novus_apps::{ipc, sinfo, sys, ui, wm};

const CW: i16 = 520;
const CH: i16 = 380;
const CARD: u32 = 0x00FAFAFC;

fn ip_str(ip: u32, buf: &mut [u8; 16]) -> &str {
    let mut n = 0;
    let mut part = 0;
    while part < 4 {
        let v = (ip >> (part * 8)) & 0xFF;
        let mut digits = [0u8; 3];
        let mut d = 0;
        let mut x = v;

        if x == 0 {
            digits[0] = b'0';
            d = 1;
        } else {
            while x > 0 {
                digits[d] = b'0' + (x % 10) as u8;
                x /= 10;
                d += 1;
            }
            digits[..d].reverse();
        }
        for i in 0..d {
            buf[n] = digits[i];
            n += 1;
        }
        if part < 3 {
            buf[n] = b'.';
            n += 1;
        }
        part += 1;
    }
    unsafe { core::str::from_utf8_unchecked(&buf[..n]) }
}

fn link_name(link: u8) -> &'static str {
    match link {
        sinfo::LINK_ETHER => "以太网",
        sinfo::LINK_FIBER => "光纤/SFP",
        sinfo::LINK_WIFI => "无线",
        _ => "未知",
    }
}

fn state_name(state: u8) -> &'static str {
    match state {
        1 => "已连接",
        2 => "无驱动",
        _ => "未连接",
    }
}

fn card(w: &mut wm::Win, y: i16, h: i16, title: &str) {
    w.fill(10, y, CW - 20, h, CARD);
    w.frame(10, y, CW - 20, h, ui::line());
    w.text(24, y + 10, ui::text(), title);
}

fn kv(w: &mut wm::Win, y: i16, key: &str, value: &str) {
    w.text(24, y, ui::dim(), key);
    w.text(110, y, ui::text(), value);
}

fn draw(w: &mut wm::Win, n: &sinfo::Net, note: &str) {
    let mut b = [0u8; 16];
    let mut m = [0u8; 16];
    let mut g = [0u8; 16];

    w.clear();
    w.fill(0, 0, CW, CH, ui::bg());
    w.fill(0, 0, CW, 28, ui::card_bg());
    w.shape(wm::OP_HLINE, 0, 28, CW, 1, ui::line());
    w.text(12, 9, ui::text(), "网络");

    card(w, 40, 116, "链路");
    kv(w, 70, "类型", link_name(n.link));
    kv(w, 92, "状态", state_name(n.state));
    kv(w, 114, "速率", if n.speed_mbps >= 1000 { "1 Gbps" } else { "100 Mbps" });
    {
        let mut line = ui::Buf::new();

        line.num(n.ndev as u32);
        kv(w, 136, "设备数", line.str());
    }

    card(w, 164, 110, "地址");
    kv(w, 194, "IP", ip_str(n.ip, &mut b));
    kv(w, 216, "掩码", ip_str(n.mask, &mut m));
    kv(w, 238, "网关", ip_str(n.gw, &mut g));
    kv(w, 260, "DHCP", match n.dhcp {
        2 => "已获取租约",
        1 => "获取中…",
        3 => "超时（静态地址）",
        _ => "未启动",
    });

    card(w, 282, 58, "计数与测试");
    {
        let mut line = ui::Buf::new();

        line.s("收 ").num(n.rx_pkts as u32).s("  发 ").num(n.tx_pkts as u32)
            .s("  错 ").num((n.rx_err + n.tx_err) as u32);
        w.text(24, 306, ui::dim(), line.str());
    }
    {
        let mut line = ui::Buf::new();

        line.s("Ping ");
        match n.ping_state {
            2 => {
                line.s("成功 ").num(n.rtt_ms).s(" ms");
            }
            1 => {
                line.s("进行中…");
            }
            3 => {
                line.s("超时");
            }
            _ => {
                line.s("未测试");
            }
        }
        w.text(24, 322, ui::dim(), line.str());
    }

    /* Buttons. */
    w.fill(24, 348, 110, 26, ui::row_sel());
    w.frame(24, 348, 110, 26, ui::accent());
    w.text(34, 354, ui::text(), "Ping 网关");

    w.fill(146, 348, 130, 26, ui::row_sel());
    w.frame(146, 348, 130, 26, ui::accent());
    w.text(156, 354, ui::text(), "TCP 回显测试");

    w.text(290, 354, ui::dim(), if note.is_empty() { "网关 10.0.2.2" } else { note });
}

/* Network state and test requests go through the named sysinfo IPC service;
 * the app has no direct device handle and only renders the kernel's snapshot.
 * A missing service is a startup error rather than a fabricated live state. */
#[no_mangle]
pub extern "C" fn _start() -> ! {
    sys::write(b"[APP] netman up\n");
    let Some(wm_port) = ipc::lookup(wm::PORT_NAME) else {
        sys::exit(1);
    };
    let Some(sys_port) = ipc::lookup(sinfo::PORT_NAME) else {
        sys::exit(2);
    };
    let Some(mut win) = wm::Win::create(wm_port, 380, 160, CW as i32 + 2,
                                    CH as i32 + wm::TITLE_H, "网络")
    else {
        sys::exit(3);
    };

    ui::init(sys_port);
    let mut note: &str = "";
    let mut n = sinfo::net(sys_port).unwrap_or(sinfo::Net {
        ndev: 0, dhcp: 0, ping_state: 0, tcp_state: 0,
        ip: 0, mask: 0, gw: 0, rtt_ms: 0, tcp_bytes: 0,
        rx_pkts: 0, tx_pkts: 0, rx_err: 0, tx_err: 0,
        link: 0, state: 0, mac: [0; 6], speed_mbps: 0, pad: 0,
    });

    draw(&mut win, &n, note);

    /* Test buttons enqueue work, then later snapshots reveal its completion.
     * Periodic refresh keeps counters and asynchronous results current without
     * blocking the window's event loop on a network operation. */
    let mut idle = 0u32;
    loop {
        while let Some(ev) = win.next_event() {
            match ev {
                wm::Ev::Close => {
                    win.destroy();
                    sys::exit(0);
                }
                wm::Ev::Mouse(m) if m.kind == wm::MOUSE_DOWN && m.button == 1 => {
                    if m.x >= 24 && m.x < 134 && m.y >= 348 && m.y < 374 {
                        let gw = n.gw;
                        let _ = sinfo::ping(sys_port, gw);
                        note = "已发出 ping";
                    } else if m.x >= 146 && m.x < 276 && m.y >= 348 && m.y < 374 {
                        let gw = n.gw;
                        let _ = sinfo::tcp_test(sys_port, gw, 9000);
                        note = "连接 10.0.2.2:9000";
                    }
                    n = sinfo::net(sys_port).unwrap_or(n);
                    draw(&mut win, &n, note);
                }
                _ => {}
            }
        }
        idle += 1;
        if ui::poll(sys_port) {
            draw(&mut win, &n, note);
        }
        if idle >= 40 {
            idle = 0;
            n = sinfo::net(sys_port).unwrap_or(n);
            draw(&mut win, &n, note);
        }
        sys::yield_now();
    }
}
