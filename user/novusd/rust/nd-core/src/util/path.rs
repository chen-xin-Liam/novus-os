/// Validate a path for the single-root, no-`..` VFS model shared with Novus OS.
pub fn validate(path: &[u8]) -> bool {
    if path.is_empty() || path.len() > 255 {
        return false;
    }
    if path[0] != b'/' {
        return false;
    }
    let mut i = 1usize;
    while i < path.len() {
        if path[i] == b'.' && i + 1 < path.len() && path[i + 1] == b'.' {
            return false;
        }
        if path[i] == 0 {
            return false;
        }
        i += 1;
    }
    true
}

/// Join a directory and a directory-entry name into a caller-provided buffer.
pub fn join(dir: &[u8], name: &[u8], out: &mut [u8]) -> Option<usize> {
    if !validate(dir) || name.is_empty() || name.len() > 63 {
        return None;
    }
    let need = if dir == b"/" {
        1 + name.len()
    } else {
        dir.len() + 1 + name.len()
    };
    if need > out.len() || need > 255 {
        return None;
    }
    let mut n = 0usize;
    if dir == b"/" {
        out[n] = b'/';
        n += 1;
    } else {
        out[..dir.len()].copy_from_slice(dir);
        n = dir.len();
        out[n] = b'/';
        n += 1;
    }
    out[n..n + name.len()].copy_from_slice(name);
    Some(need)
}

pub fn file_name(path: &[u8]) -> &[u8] {
    match path.iter().rposition(|b| *b == b'/') {
        Some(i) => &path[i + 1..],
        None => path,
    }
}
