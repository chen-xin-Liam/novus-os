//! Tokeniser for a single pipeline stage.
//!
//! Turns a byte slice (one `|`-separated segment) into a [`Cmd`] whose
//! argument vector and redirection paths point into the caller-supplied
//! scratch buffer. Handles single/double quotes, backslash escapes, the
//! `$VAR` and `$(...)` expansions, `*`/`?` globbing, and the `<` `>` `>>`
//! redirection markers.

use super::path;
use super::{to_str, Capture, Cmd, MAX_ARGS, Shell, WORD_MAX};
use crate::fs;

fn is_var_char(c: u8) -> bool {
    (c >= b'a' && c <= b'z')
        || (c >= b'A' && c <= b'Z')
        || (c >= b'0' && c <= b'9')
        || c == b'_'
}

fn push_bytes(scratch: &mut [u8], spos: &mut usize, b: &[u8]) -> (usize, usize) {
    let start = *spos;
    let mut k = 0;
    while k < b.len() && *spos < scratch.len() {
        scratch[*spos] = b[k];
        *spos += 1;
        k += 1;
    }
    (start, *spos)
}

fn add_arg_with_glob(
    shell: &mut Shell,
    word: &[u8],
    scratch: &mut [u8],
    spos: &mut usize,
    offs: &mut [(usize, usize); MAX_ARGS],
    argc: &mut usize,
) {
    let has_glob = word.iter().any(|&b| b == b'*' || b == b'?');
    if !has_glob {
        if *argc < MAX_ARGS {
            offs[*argc] = push_bytes(scratch, spos, word);
            *argc += 1;
        }
        return;
    }

    let (dir, pat) = path::split(word);
    let mut base = [0u8; super::path::PATH_MAX];
    let base_len = if dir.is_empty() {
        shell.abs_path(b".", &mut base)
    } else {
        shell.abs_path(dir, &mut base)
    };
    let bstr = to_str(&base[..base_len]);

    let mut matched = 0usize;
    if let Some(fd) = fs::opendir(bstr) {
        while let Some(Some(e)) = fs::readdir(fd) {
            if path::glob_match(pat, e.name().as_bytes()) {
                let mut full = [0u8; super::path::PATH_MAX];
                let mut fl = 0usize;
                if !dir.is_empty() {
                    for &b in dir.iter().take(super::path::PATH_MAX - 1) {
                        full[fl] = b;
                        fl += 1;
                    }
                    if fl < super::path::PATH_MAX - 1 {
                        full[fl] = b'/';
                        fl += 1;
                    }
                }
                for &b in e.name().as_bytes().iter().take(super::path::PATH_MAX - fl - 1) {
                    full[fl] = b;
                    fl += 1;
                }
                if *argc < MAX_ARGS {
                    offs[*argc] = push_bytes(scratch, spos, &full[..fl]);
                    *argc += 1;
                }
                matched += 1;
            }
        }
    }
    if matched > 0 {
        return;
    }
    // No matches: keep the literal word, as POSIX shells do.
    if *argc < MAX_ARGS {
        offs[*argc] = push_bytes(scratch, spos, word);
        *argc += 1;
    }
}

#[allow(clippy::too_many_arguments)]
fn finish_word(
    w: &mut [u8; WORD_MAX],
    wlen: &mut usize,
    redir_mode: &mut u8,
    shell: &mut Shell,
    scratch: &mut [u8],
    spos: &mut usize,
    offs: &mut [(usize, usize); MAX_ARGS],
    argc: &mut usize,
    in_off: &mut Option<(usize, usize)>,
    out_off: &mut Option<(usize, usize)>,
    append_flag: &mut bool,
) {
    if *wlen > 0 {
        if *redir_mode == 0 {
            add_arg_with_glob(shell, &w[..*wlen], scratch, spos, offs, argc);
        } else {
            let o = push_bytes(scratch, spos, &w[..*wlen]);
            match *redir_mode {
                1 => *in_off = Some(o),
                2 | 3 => {
                    *out_off = Some(o);
                    *append_flag = *redir_mode == 3;
                }
                _ => {}
            }
            *redir_mode = 0;
        }
        for b in w.iter_mut() {
            *b = 0;
        }
        *wlen = 0;
    } else {
        *redir_mode = 0;
    }
}

/// Parse one segment (no top-level `|`) into a [`Cmd`].
pub fn parse_command<'a>(shell: &mut Shell, seg: &[u8], scratch: &'a mut [u8]) -> Cmd<'a> {
    let mut cmd = Cmd::new();
    let mut w = [0u8; WORD_MAX];
    let mut wlen = 0usize;
    let mut in_word = false;
    let mut in_single = false;
    let mut in_double = false;
    let mut spos = 0usize;
    let mut redir_mode = 0u8;

    let mut offs: [(usize, usize); MAX_ARGS] = [(0, 0); MAX_ARGS];
    let mut argc = 0usize;
    let mut in_off: Option<(usize, usize)> = None;
    let mut out_off: Option<(usize, usize)> = None;
    let mut append_flag = false;

    let len = seg.len();
    let mut i = 0usize;

    while i < len {
        let c = seg[i];
        if in_single {
            if c == b'\'' {
                in_single = false;
                in_word = true;
            } else if wlen < WORD_MAX {
                w[wlen] = c;
                wlen += 1;
                in_word = true;
            }
            i += 1;
            continue;
        }
        if in_double {
            if c == b'"' {
                in_double = false;
                in_word = true;
            } else if c == b'$' && i + 1 < len && seg[i + 1] == b'(' {
                finish_subst(shell, seg, &mut i, &mut w, &mut wlen);
                in_word = true;
                continue;
            } else if c == b'$' {
                i = expand_var(shell, seg, i, &mut w, &mut wlen);
                in_word = true;
                continue;
            } else if c == b'\\' && i + 1 < len {
                if wlen < WORD_MAX {
                    w[wlen] = seg[i + 1];
                    wlen += 1;
                }
                i += 2;
                in_word = true;
                continue;
            } else if wlen < WORD_MAX {
                w[wlen] = c;
                wlen += 1;
                in_word = true;
            }
            i += 1;
            continue;
        }

        // Normal (unquoted) state.
        if c == b' ' || c == b'\t' || c == b'\r' || c == b'\n' {
            if in_word {
                finish_word(
                    &mut w, &mut wlen, &mut redir_mode, shell, scratch, &mut spos, &mut offs,
                    &mut argc, &mut in_off, &mut out_off, &mut append_flag,
                );
            }
            i += 1;
            continue;
        }
        if c == b'\'' {
            in_single = true;
            in_word = true;
            i += 1;
            continue;
        }
        if c == b'"' {
            in_double = true;
            in_word = true;
            i += 1;
            continue;
        }
        if c == b'$' {
            if i + 1 < len && seg[i + 1] == b'(' {
                finish_subst(shell, seg, &mut i, &mut w, &mut wlen);
                in_word = true;
                continue;
            }
            i = expand_var(shell, seg, i, &mut w, &mut wlen);
            in_word = true;
            continue;
        }
        if c == b'\\' && i + 1 < len {
            if wlen < WORD_MAX {
                w[wlen] = seg[i + 1];
                wlen += 1;
            }
            i += 2;
            in_word = true;
            continue;
        }
        if c == b'<' {
            if in_word {
                finish_word(
                    &mut w, &mut wlen, &mut redir_mode, shell, scratch, &mut spos, &mut offs,
                    &mut argc, &mut in_off, &mut out_off, &mut append_flag,
                );
            }
            redir_mode = 1;
            i += 1;
            continue;
        }
        if c == b'>' {
            if in_word {
                finish_word(
                    &mut w, &mut wlen, &mut redir_mode, shell, scratch, &mut spos, &mut offs,
                    &mut argc, &mut in_off, &mut out_off, &mut append_flag,
                );
            }
            if i + 1 < len && seg[i + 1] == b'>' {
                redir_mode = 3;
                i += 2;
            } else {
                redir_mode = 2;
                i += 1;
            }
            continue;
        }
        if wlen < WORD_MAX {
            w[wlen] = c;
            wlen += 1;
            in_word = true;
        }
        i += 1;
    }
    if in_word {
        finish_word(
            &mut w, &mut wlen, &mut redir_mode, shell, scratch, &mut spos, &mut offs, &mut argc,
            &mut in_off, &mut out_off, &mut append_flag,
        );
    }

    for k in 0..argc {
        cmd.argv[k] = &scratch[offs[k].0..offs[k].1];
    }
    cmd.argc = argc;
    if let Some(o) = in_off {
        cmd.redir_in = Some(&scratch[o.0..o.1]);
    }
    if let Some(o) = out_off {
        cmd.redir_out = Some(&scratch[o.0..o.1]);
        cmd.append = append_flag;
    }
    cmd
}

/// Run `$(...)` starting at a `$` that precedes `(`; advances `i` past the
/// closing `)`. The result is appended to `w`.
fn finish_subst(shell: &mut Shell, seg: &[u8], i: &mut usize, w: &mut [u8; WORD_MAX], wlen: &mut usize) {
    let start = *i;
    let mut depth = 1usize;
    let mut j = start + 2;
    let len = seg.len();
    while j < len && depth > 0 {
        if seg[j] == b'(' {
            depth += 1;
        } else if seg[j] == b')' {
            depth -= 1;
            if depth == 0 {
                break;
            }
        }
        j += 1;
    }
    if depth == 0 {
        let inner = &seg[start + 2..j];
        let mut cap = Capture::new();
        shell.run_subst(inner, &mut cap);
        let mut dlen = cap.len;
        if dlen > 0 && cap.buf[dlen - 1] == b'\n' {
            dlen -= 1;
        }
        let mut k = 0;
        while k < dlen && *wlen < WORD_MAX {
            w[*wlen] = cap.buf[k];
            *wlen += 1;
            k += 1;
        }
        *i = j + 1;
    } else {
        if *wlen < WORD_MAX {
            w[*wlen] = b'$';
            *wlen += 1;
        }
        *i = start + 1;
    }
}

/// Expand `$NAME` starting at a `$`; advances `i` past the name.
fn expand_var(shell: &mut Shell, seg: &[u8], i: usize, w: &mut [u8; WORD_MAX], wlen: &mut usize) -> usize {
    let mut j = i + 1;
    let len = seg.len();
    while j < len && is_var_char(seg[j]) {
        j += 1;
    }
    if j > i + 1 {
        let name = &seg[i + 1..j];
        if let Some(val) = shell.get_var(name) {
            let mut k = 0;
            while k < val.len() && *wlen < WORD_MAX {
                w[*wlen] = val[k];
                *wlen += 1;
                k += 1;
            }
        }
        j
    } else {
        if *wlen < WORD_MAX {
            w[*wlen] = b'$';
            *wlen += 1;
        }
        i + 1
    }
}
