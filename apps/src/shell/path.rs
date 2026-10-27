//! POSIX path normalisation for the shell.
//!
//! The kernel VFS keeps a single root and has no `.`/`..` directory entries,
//! and it resolves the path it is given relative to the root. So the shell
//! must turn every command argument into a clean absolute path before
//! passing it down, using the current working directory as the base. This
//! module does that lexical folding (it never touches the file system, so it
//! cannot fail on a missing target - that is the caller's job).

pub const PATH_MAX: usize = 256;

/// Normalise `input` against `cwd` and write the result into `out`.
///
/// Returns the number of bytes written, or 0 when the result would not fit.
/// The result always begins with `/`. A leading `~` is treated as the root
/// (the shell's idea of "home" is currently the root; see `drives`/OOBE for
/// where a real home would live). `..` is folded lexically and never escapes
/// the root.
pub fn normalize(cwd: &[u8], input: &[u8], out: &mut [u8]) -> usize {
    if out.is_empty() {
        return 0;
    }

    let mut stack = [0u8; PATH_MAX];
    let mut slen: usize = 0;

    // Pick the base.
    let mut rest: &[u8] = input;
    let mut absolute = false;
    if rest.first() == Some(&b'/') {
        absolute = true;
        rest = &rest[1..];
    } else if rest.first() == Some(&b'~') {
        absolute = true; // home is the root for now
        rest = &rest[1..];
        if rest.first() == Some(&b'/') {
            rest = &rest[1..];
        }
    }

    if absolute {
        slen = 0;
    } else {
        let mut j = 0;
        while j < cwd.len() && slen < PATH_MAX - 1 {
            stack[slen] = cwd[j];
            slen += 1;
            j += 1;
        }
        // Drop a trailing slash so appended components keep a single separator.
        while slen > 1 && stack[slen - 1] == b'/' {
            slen -= 1;
        }
    }

    // Tokenise `rest` on '/' and fold each component.
    let mut i = 0usize;
    while i <= rest.len() {
        let mut j = i;
        while j < rest.len() && rest[j] != b'/' {
            j += 1;
        }
        let comp = &rest[i..j];
        i = j + 1;
        if comp.is_empty() || (comp.len() == 1 && comp[0] == b'.') {
            continue;
        }
        if comp.len() == 2 && comp[0] == b'.' && comp[1] == b'.' {
            // Walk up one component, staying at the root.
            if slen == 0 || (slen == 1 && stack[0] == b'/') {
                slen = if slen == 0 { 0 } else { 1 };
            } else {
                let mut k = slen;
                while k > 1 && stack[k - 1] != b'/' {
                    k -= 1;
                }
                slen = k;
            }
            continue;
        }
        // Append the component, ensuring exactly one leading slash.
        if slen == 0 {
            if slen < PATH_MAX {
                stack[slen] = b'/';
                slen += 1;
            }
        } else if stack[slen - 1] != b'/' {
            if slen < PATH_MAX {
                stack[slen] = b'/';
                slen += 1;
            }
        }
        let mut k = 0;
        while k < comp.len() && slen < PATH_MAX {
            stack[slen] = comp[k];
            slen += 1;
            k += 1;
        }
    }

    if slen == 0 {
        if PATH_MAX >= 1 {
            stack[0] = b'/';
            slen = 1;
        }
    }
    if slen > out.len() {
        return 0;
    }
    out[..slen].copy_from_slice(&stack[..slen]);
    slen
}

/// True when `name` matches the glob `pattern` (`*` matches any run, `?`
/// matches one codepoint). Literal bytes compare byte-wise (no locale
/// rules), but a `?` never lands in the middle of a UTF-8 sequence.
pub fn glob_match(pattern: &[u8], name: &[u8]) -> bool {
    return glob_match_impl(pattern, name);

    /// Next codepoint boundary after `i`.
    fn step(n: &[u8], i: usize) -> usize {
        let mut k = i + 1;
        while k < n.len() && (n[k] & 0xC0) == 0x80 {
            k += 1;
        }
        k
    }

    fn glob_match_impl(p: &[u8], n: &[u8]) -> bool {
        let mut pi = 0;
        let mut ni = 0;
        while pi < p.len() {
            match p[pi] {
                b'*' => {
                    // Collapse runs of '*'.
                    while pi + 1 < p.len() && p[pi + 1] == b'*' {
                        pi += 1;
                    }
                    if pi + 1 == p.len() {
                        return true; // trailing '*' matches the rest
                    }
                    // Try every codepoint alignment for the remainder.
                    let tail = &p[pi + 1..];
                    let mut k = ni;
                    while k <= n.len() {
                        if glob_match_impl(tail, &n[k..]) {
                            return true;
                        }
                        if k == n.len() {
                            break;
                        }
                        k = step(n, k);
                    }
                    return false;
                }
                b'?' => {
                    if ni >= n.len() {
                        return false;
                    }
                    pi += 1;
                    ni = step(n, ni);
                }
                other => {
                    if ni >= n.len() || n[ni] != other {
                        return false;
                    }
                    pi += 1;
                    ni += 1;
                }
            }
        }
        ni == n.len()
    }
}

/// Split `path` into `(dir, name)` at the last `/`. For a path with no
/// separator, `dir` is empty (caller uses the cwd).
pub fn split(path: &[u8]) -> (&[u8], &[u8]) {
    let mut slash = None;
    for (i, &b) in path.iter().enumerate() {
        if b == b'/' {
            slash = Some(i);
        }
    }
    match slash {
        Some(s) => (&path[..s], &path[s + 1..]),
        None => (&[], path),
    }
}
