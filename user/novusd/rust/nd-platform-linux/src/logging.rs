use std::io::Write;

pub fn write(level: u8, msg: &[u8]) {
    let prefix: &[u8] = match level {
        0 => b"[DEBUG] ",
        1 => b"[INFO ] ",
        2 => b"[WARN ] ",
        3 => b"[ERROR] ",
        _ => b"[ALERT] ",
    };
    let stdout = std::io::stdout();
    let mut out = stdout.lock();
    let _ = out.write_all(prefix);
    let _ = out.write_all(msg);
    let _ = out.write_all(b"\n");
    let _ = out.flush();
}
