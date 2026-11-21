use std::collections::HashSet;
use std::fs;

pub fn list_pids() -> HashSet<u32> {
    let mut out = HashSet::new();
    let Ok(entries) = fs::read_dir("/proc") else {
        return out;
    };
    for entry in entries.flatten() {
        if let Some(name) = entry.file_name().to_str() {
            if let Ok(pid) = name.parse::<u32>() {
                out.insert(pid);
            }
        }
    }
    out
}

pub fn read_comm(pid: u32) -> Vec<u8> {
    let path = format!("/proc/{pid}/comm");
    fs::read(path).unwrap_or_default()
}

pub fn cap_effective() -> Option<u64> {
    let status = fs::read_to_string("/proc/self/status").ok()?;
    for line in status.lines() {
        if let Some(rest) = line.strip_prefix("CapEff:") {
            return u64::from_str_radix(rest.trim(), 16).ok();
        }
    }
    None
}
