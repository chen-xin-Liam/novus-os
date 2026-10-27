//! Built-in commands for `nsh`.
//!
//! Every builtin matches the signature `fn(&mut Shell, &Cmd, &[u8], &mut dyn
//! Sink) -> i32`, so they can read their arguments out of the parsed [`Cmd`],
//! pull redirected stdin from the byte slice, and write to the active [`Sink`]
//! (terminal or a capture buffer). External commands are resolved through
//! `sinfo::spawn_name` by [`run`]; there is no exec, so a builtin that needs
//! another program simply runs it as a nested pipeline stage.

use super::{to_str, Cmd, MAX_ARGS, PATH_MAX, Shell, Sink};
use crate::fs;
use crate::sinfo;

/* ---- output helpers ---------------------------------------------------- */

fn puts(out: &mut dyn Sink, s: &str) {
    out.write(s.as_bytes());
}

fn putb(out: &mut dyn Sink, b: &[u8]) {
    out.write(b);
}

fn putc(out: &mut dyn Sink, c: u8) {
    out.write(&[c]);
}

fn put_dec(out: &mut dyn Sink, mut v: u64) {
    if v == 0 {
        putc(out, b'0');
        return;
    }
    let mut tmp = [0u8; 20];
    let mut n = 0;
    while v > 0 && n < 20 {
        tmp[n] = b'0' + (v % 10) as u8;
        v /= 10;
        n += 1;
    }
    while n > 0 {
        n -= 1;
        putc(out, tmp[n]);
    }
}

fn put_size(out: &mut dyn Sink, bytes: u64, human: bool) {
    if human {
        if bytes >= 1024 * 1024 {
            put_dec(out, bytes / (1024 * 1024));
            putc(out, b'M');
        } else if bytes >= 1024 {
            put_dec(out, bytes / 1024);
            putc(out, b'K');
        } else {
            put_dec(out, bytes);
        }
    } else {
        put_dec(out, bytes);
    }
}

fn starts_with(hay: &[u8], prefix: &[u8]) -> bool {
    hay.len() >= prefix.len() && &hay[..prefix.len()] == prefix
}

fn ends_with(hay: &[u8], suffix: &[u8]) -> bool {
    hay.len() >= suffix.len() && &hay[hay.len() - suffix.len()..] == suffix
}

/// Join `parent` and `name` into `out` (inserting `/`), returning the length.
fn join_path(parent: &[u8], name: &[u8], out: &mut [u8]) -> usize {
    let mut n = 0;
    for &b in parent.iter() {
        if n < out.len() {
            out[n] = b;
            n += 1;
        }
    }
    if n > 0 && n < out.len() && out[n - 1] != b'/' {
        out[n] = b'/';
        n += 1;
    }
    for &b in name.iter() {
        if n < out.len() {
            out[n] = b;
            n += 1;
        }
    }
    n
}

const KNOWN_APPS: &[&str] = &["fileman", "taskman", "settings", "netman", "session", "setup", "nsh"];

type Bfn = fn(&mut Shell, &Cmd, &[u8], &mut dyn Sink) -> i32;

fn is_builtin(name: &[u8]) -> bool {
    BUILTINS.iter().any(|(n, _)| n.as_bytes() == name)
}

fn lookup_builtin(name: &[u8]) -> Option<Bfn> {
    for (n, f) in BUILTINS.iter() {
        if n.as_bytes() == name {
            return Some(*f);
        }
    }
    None
}

/* ---- builtins ---------------------------------------------------------- */

fn cmd_ls(shell: &mut Shell, cmd: &Cmd, _stdin: &[u8], out: &mut dyn Sink) -> i32 {
    let mut long = false;
    let mut all = false;
    let mut human = false;
    let mut paths: [&[u8]; MAX_ARGS] = [&[]; MAX_ARGS];
    let mut pc = 0;
    for k in 1..cmd.argc {
        let a = cmd.argv[k];
        if !a.is_empty() && a[0] == b'-' {
            for &f in &a[1..] {
                match f {
                    b'l' => long = true,
                    b'a' => all = true,
                    b'h' => human = true,
                    _ => {}
                }
            }
        } else if pc < MAX_ARGS {
            paths[pc] = a;
            pc += 1;
        }
    }
    if pc == 0 {
        paths[0] = b".";
        pc = 1;
    }
    let mut status = 0;
    for pi in 0..pc {
        let mut abs = [0u8; PATH_MAX];
        let n = shell.abs_path(paths[pi], &mut abs);
        if n == 0 {
            puts(out, "ls: ");
            putb(out, paths[pi]);
            puts(out, ": path too long\n");
            status = 1;
            continue;
        }
        let ap = to_str(&abs[..n]);
        match fs::stat(ap) {
            None => {
                puts(out, "ls: ");
                putb(out, paths[pi]);
                puts(out, ": no such file or directory\n");
                status = 1;
            }
            Some(st) => {
                if st.mode == fs::MODE_DIR {
                    if pc > 1 {
                        putb(out, paths[pi]);
                        puts(out, ":\n");
                    }
                    if let Some(fd) = fs::opendir(ap) {
                        let mut entries: [( [u8; 57], u16, usize ); 64] = [([0u8; 57], 0, 0); 64];
                        let mut ec = 0;
                        while let Some(Some(e)) = fs::readdir(fd) {
                            let nm = e.name();
                            if !all && nm.as_bytes().first() == Some(&b'.') {
                                continue;
                            }
                            if ec < entries.len() {
                                let mut buf = [0u8; 57];
                                let l = super::fit_bytes(nm.as_bytes(), 56);
                                buf[..l].copy_from_slice(&nm.as_bytes()[..l]);
                                entries[ec] = (buf, e.mode, l);
                                ec += 1;
                            }
                        }
                        for ei in 0..ec {
                            let (nm, mode, l) = entries[ei];
                            if long {
                                putc(out, if mode == fs::MODE_DIR { b'd' } else { b'-' });
                                putc(out, b' ');
                                let mut full = [0u8; PATH_MAX];
                                let fl = join_path(&abs[..n], &nm[..l], &mut full);
                                let sz = fs::stat(to_str(&full[..fl]))
                                    .map(|s| s.size as u64)
                                    .unwrap_or(0);
                                put_size(out, sz, human);
                                putc(out, b' ');
                                putb(out, &nm[..l]);
                                putc(out, b'\n');
                            } else {
                                putb(out, &nm[..l]);
                                putc(out, b' ');
                            }
                        }
                        if !long {
                            putc(out, b'\n');
                        }
                    }
                } else {
                    if long {
                        putc(out, b'-');
                        putc(out, b' ');
                        put_size(out, st.size, human);
                        putc(out, b' ');
                        putb(out, paths[pi]);
                        putc(out, b'\n');
                    } else {
                        putb(out, paths[pi]);
                        putc(out, b'\n');
                    }
                }
            }
        }
    }
    status
}

fn cmd_cd(shell: &mut Shell, cmd: &Cmd, _stdin: &[u8], out: &mut dyn Sink) -> i32 {
    let target: &[u8] = if cmd.argc > 1 {
        cmd.argv[1]
    } else {
        shell.get_var(b"HOME").unwrap_or(b"/")
    };
    let mut abs = [0u8; PATH_MAX];
    let n = shell.abs_path(target, &mut abs);
    if n == 0 {
        puts(out, "cd: path too long\n");
        return 1;
    }
    let ap = to_str(&abs[..n]);
    match fs::stat(ap) {
        Some(st) if st.mode == fs::MODE_DIR => {
            shell.cwd[..n].copy_from_slice(&abs[..n]);
            shell.cwd_len = n;
            shell.set_var(b"PWD", &abs[..n]);
            0
        }
        Some(_) => {
            puts(out, "cd: not a directory: ");
            putb(out, target);
            putc(out, b'\n');
            1
        }
        None => {
            puts(out, "cd: no such file or directory: ");
            putb(out, target);
            putc(out, b'\n');
            1
        }
    }
}

fn cmd_pwd(_shell: &mut Shell, _cmd: &Cmd, _stdin: &[u8], out: &mut dyn Sink) -> i32 {
    // The cwd lives in the shell; pwd reads it through the accessor.
    puts(out, to_str(&_shell.cwd[.._shell.cwd_len]));
    putc(out, b'\n');
    0
}

fn cmd_cat(shell: &mut Shell, cmd: &Cmd, stdin: &[u8], out: &mut dyn Sink) -> i32 {
    if cmd.argc <= 1 {
        out.write(stdin);
        return 0;
    }
    let mut status = 0;
    for k in 1..cmd.argc {
        let a = cmd.argv[k];
        let mut buf = [0u8; 4096];
        let r = shell.read_file(a, &mut buf);
        if r == 0 {
            puts(out, "cat: ");
            putb(out, a);
            puts(out, ": no such file\n");
            status = 1;
        } else {
            out.write(&buf[..r]);
        }
    }
    status
}

fn cmd_echo(_shell: &mut Shell, cmd: &Cmd, _stdin: &[u8], out: &mut dyn Sink) -> i32 {
    let mut nflag = false;
    let mut first = true;
    for k in 1..cmd.argc {
        let a = cmd.argv[k];
        if !nflag && a == b"-n" {
            nflag = true;
            continue;
        }
        if !first {
            putc(out, b' ');
        }
        putb(out, a);
        first = false;
    }
    if !nflag {
        putc(out, b'\n');
    }
    0
}

fn mkdir_one(_shell: &Shell, ap: &str) -> bool {
    fs::mkdir(ap)
}

fn cmd_mkdir(shell: &mut Shell, cmd: &Cmd, _stdin: &[u8], out: &mut dyn Sink) -> i32 {
    let mut parents = false;
    let mut paths: [&[u8]; MAX_ARGS] = [&[]; MAX_ARGS];
    let mut pc = 0;
    for k in 1..cmd.argc {
        let a = cmd.argv[k];
        if a == b"-p" {
            parents = true;
        } else if pc < MAX_ARGS {
            paths[pc] = a;
            pc += 1;
        }
    }
    let mut status = 0;
    for pi in 0..pc {
        let mut abs = [0u8; PATH_MAX];
        let n = shell.abs_path(paths[pi], &mut abs);
        if n == 0 {
            puts(out, "mkdir: path too long\n");
            status = 1;
            continue;
        }
        let ap = to_str(&abs[..n]);
        if parents {
            // Walk components, creating each that is missing.
            let mut acc = [0u8; PATH_MAX];
            let mut acc_len = 0usize;
            if abs[0] == b'/' {
                acc[0] = b'/';
                acc_len = 1;
            }
            let mut i = if abs[0] == b'/' { 1 } else { 0 };
            while i <= n {
                let mut j = i;
                while j < n && abs[j] != b'/' {
                    j += 1;
                }
                if j > i {
                    if acc_len > 0 && acc_len < PATH_MAX && acc[acc_len - 1] != b'/' {
                        acc[acc_len] = b'/';
                        acc_len += 1;
                    }
                    let mut k = i;
                    while k < j && acc_len < PATH_MAX {
                        acc[acc_len] = abs[k];
                        acc_len += 1;
                        k += 1;
                    }
                    let cur = to_str(&acc[..acc_len]);
                    if fs::stat(cur).is_none() {
                        if !mkdir_one(shell, cur) {
                            puts(out, "mkdir: cannot create directory: ");
                            putb(out, paths[pi]);
                            putc(out, b'\n');
                            status = 1;
                            break;
                        }
                    }
                }
                i = j + 1;
            }
        } else {
            if !fs::stat(ap).is_none() {
                puts(out, "mkdir: cannot create directory: ");
                putb(out, paths[pi]);
                puts(out, ": file exists\n");
                status = 1;
            } else if !mkdir_one(shell, ap) {
                puts(out, "mkdir: cannot create directory: ");
                putb(out, paths[pi]);
                putc(out, b'\n');
                status = 1;
            }
        }
    }
    status
}

fn remove_recursive(shell: &Shell, ap: &str, path: &[u8], out: &mut dyn Sink, status: &mut i32) {
    match fs::stat(ap) {
        None => {
            puts(out, "rm: cannot remove: ");
            putb(out, path);
            puts(out, ": no such file or directory\n");
            *status = 1;
        }
        Some(st) if st.mode == fs::MODE_DIR => {
            if let Some(fd) = fs::opendir(ap) {
                while let Some(Some(e)) = fs::readdir(fd) {
                    let nm = e.name();
                    let mut child = [0u8; PATH_MAX];
                    let cl = join_path(path, nm.as_bytes(), &mut child);
                    let mut cab = [0u8; PATH_MAX];
                    let cn = shell.abs_path(&child[..cl], &mut cab);
                    let cap = to_str(&cab[..cn]);
                    remove_recursive(shell, cap, &child[..cl], out, status);
                }
            }
            if !fs::unlink(ap) && *status == 0 {
                puts(out, "rm: cannot remove directory: ");
                putb(out, path);
                putc(out, b'\n');
                *status = 1;
            }
        }
        Some(_) => {
            if !fs::unlink(ap) {
                puts(out, "rm: cannot remove: ");
                putb(out, path);
                putc(out, b'\n');
                *status = 1;
            }
        }
    }
}

fn cmd_rm(shell: &mut Shell, cmd: &Cmd, _stdin: &[u8], out: &mut dyn Sink) -> i32 {
    let mut recursive = false;
    let mut force = false;
    let mut paths: [&[u8]; MAX_ARGS] = [&[]; MAX_ARGS];
    let mut pc = 0;
    for k in 1..cmd.argc {
        let a = cmd.argv[k];
        if !a.is_empty() && a[0] == b'-' {
            for &f in &a[1..] {
                match f {
                    b'r' => recursive = true,
                    b'f' => force = true,
                    _ => {}
                }
            }
        } else if pc < MAX_ARGS {
            paths[pc] = a;
            pc += 1;
        }
    }
    let mut status = 0;
    for pi in 0..pc {
        let mut abs = [0u8; PATH_MAX];
        let n = shell.abs_path(paths[pi], &mut abs);
        if n == 0 {
            if !force {
                puts(out, "rm: path too long\n");
                status = 1;
            }
            continue;
        }
        let ap = to_str(&abs[..n]);
        match fs::stat(ap) {
            None => {
                if !force {
                    puts(out, "rm: cannot remove: ");
                    putb(out, paths[pi]);
                    puts(out, ": no such file or directory\n");
                    status = 1;
                }
            }
            Some(st) if st.mode == fs::MODE_DIR && !recursive => {
                if !force {
                    puts(out, "rm: cannot remove '");
                    putb(out, paths[pi]);
                    puts(out, "': is a directory (use -r)\n");
                    status = 1;
                }
            }
            _ => {
                if recursive {
                    remove_recursive(shell, ap, paths[pi], out, &mut status);
                } else if !fs::unlink(ap) {
                    if !force {
                        puts(out, "rm: cannot remove: ");
                        putb(out, paths[pi]);
                        putc(out, b'\n');
                        status = 1;
                    }
                }
            }
        }
    }
    status
}

fn cmd_rmdir(shell: &mut Shell, cmd: &Cmd, _stdin: &[u8], out: &mut dyn Sink) -> i32 {
    let mut status = 0;
    for k in 1..cmd.argc {
        let a = cmd.argv[k];
        let mut abs = [0u8; PATH_MAX];
        let n = shell.abs_path(a, &mut abs);
        if n == 0 {
            puts(out, "rmdir: path too long\n");
            status = 1;
            continue;
        }
        let ap = to_str(&abs[..n]);
        match fs::stat(ap) {
            None => {
                puts(out, "rmdir: failed to remove '");
                putb(out, a);
                puts(out, "': no such file or directory\n");
                status = 1;
            }
            Some(st) if st.mode != fs::MODE_DIR => {
                puts(out, "rmdir: failed to remove '");
                putb(out, a);
                puts(out, "': not a directory\n");
                status = 1;
            }
            _ => {
                if !fs::unlink(ap) {
                    puts(out, "rmdir: failed to remove '");
                    putb(out, a);
                    puts(out, "': directory not empty\n");
                    status = 1;
                }
            }
        }
    }
    status
}

fn copy_file(shell: &Shell, src: &str, dst: &str, out: &mut dyn Sink, status: &mut i32) {
    let fd = match fs::open(src, false) {
        Some(f) => f,
        None => {
            puts(out, "cp: cannot open for read\n");
            *status = 1;
            return;
        }
    };
    let size = match fs::stat(src) {
        Some(s) => s.size as usize,
        None => {
            *status = 1;
            return;
        }
    };
    let mut buf = [0u8; 4096];
    let want = size.min(buf.len());
    let got = fs::read(fd, &mut buf[..want]);
    if got < 0 {
        *status = 1;
        return;
    }
    shell.write_file(dst.as_bytes(), &buf[..got as usize], false);
}

fn copy_recursive(shell: &Shell, src_ap: &str, src_path: &[u8], dst_ap: &str, dst_path: &[u8], out: &mut dyn Sink, status: &mut i32) {
    match fs::stat(src_ap) {
        None => {
            puts(out, "cp: cannot stat source\n");
            *status = 1;
        }
        Some(st) if st.mode == fs::MODE_DIR => {
            if !fs::stat(dst_ap).is_none() {
                // ok, already exists
            } else if !fs::mkdir(dst_ap) {
                puts(out, "cp: cannot create directory: ");
                putb(out, dst_path);
                putc(out, b'\n');
                *status = 1;
                return;
            }
            if let Some(fd) = fs::opendir(src_ap) {
                while let Some(Some(e)) = fs::readdir(fd) {
                    let nm = e.name();
                    let mut csrc = [0u8; PATH_MAX];
                    let cl = join_path(src_path, nm.as_bytes(), &mut csrc);
                    let mut csrc_abs = [0u8; PATH_MAX];
                    let cn = shell.abs_path(&csrc[..cl], &mut csrc_abs);
                    let cap = to_str(&csrc_abs[..cn]);
                    let mut cdst = [0u8; PATH_MAX];
                    let dl = join_path(dst_path, nm.as_bytes(), &mut cdst);
                    let mut cdst_abs = [0u8; PATH_MAX];
                    let dn = shell.abs_path(&cdst[..dl], &mut cdst_abs);
                    let dap = to_str(&cdst_abs[..dn]);
                    copy_recursive(shell, cap, &csrc[..cl], dap, &cdst[..dl], out, status);
                }
            }
        }
        Some(_) => copy_file(shell, src_ap, dst_ap, out, status),
    }
}

fn cmd_cp(shell: &mut Shell, cmd: &Cmd, _stdin: &[u8], out: &mut dyn Sink) -> i32 {
    let mut recursive = false;
    let mut args: [&[u8]; MAX_ARGS] = [&[]; MAX_ARGS];
    let mut ac = 0;
    for k in 1..cmd.argc {
        let a = cmd.argv[k];
        if a == b"-r" || a == b"-R" || a == b"--recursive" {
            recursive = true;
        } else if ac < MAX_ARGS {
            args[ac] = a;
            ac += 1;
        }
    }
    if ac < 2 {
        puts(out, "cp: missing file operand\n");
        return 1;
    }
    let dest = args[ac - 1];
    let mut status = 0;
    for k in 0..ac - 1 {
        let mut sabs = [0u8; PATH_MAX];
        let sn = shell.abs_path(args[k], &mut sabs);
        let sap = to_str(&sabs[..sn]);
        let mut dabs = [0u8; PATH_MAX];
        let dn = shell.abs_path(dest, &mut dabs);
        let mut cdst = [0u8; PATH_MAX];
        let mut cdst_abs = [0u8; PATH_MAX];
        let dap_str;
        let dpath;
        // If the destination is an existing directory, copy into it.
        if fs::stat(to_str(&dabs[..dn])).map(|s| s.mode == fs::MODE_DIR).unwrap_or(false) {
            let nm = match fs::stat(sap) {
                Some(_) => {
                    // derive name from source
                    let mut i = args[k].len();
                    while i > 0 && args[k][i - 1] != b'/' {
                        i -= 1;
                    }
                    &args[k][i..]
                }
                None => &[][..],
            };
            let cdn = join_path(dest, nm, &mut cdst);
            let cn = shell.abs_path(&cdst[..cdn], &mut cdst_abs);
            dap_str = to_str(&cdst_abs[..cn]);
            dpath = &cdst[..cdn];
        } else {
            dap_str = to_str(&dabs[..dn]);
            dpath = dest;
        }
        if recursive {
            copy_recursive(shell, sap, args[k], dap_str, dpath, out, &mut status);
        } else {
            copy_file(shell, sap, dap_str, out, &mut status);
        }
    }
    status
}

fn cmd_mv(shell: &mut Shell, cmd: &Cmd, _stdin: &[u8], out: &mut dyn Sink) -> i32 {
    if cmd.argc < 3 {
        puts(out, "mv: missing destination\n");
        return 1;
    }
    let dest = cmd.argv[cmd.argc - 1];
    let mut dabs = [0u8; PATH_MAX];
    let dn = shell.abs_path(dest, &mut dabs);
    let dap = to_str(&dabs[..dn]);
    let dest_is_dir = fs::stat(dap).map(|s| s.mode == fs::MODE_DIR).unwrap_or(false);
    let mut status = 0;
    for k in 1..cmd.argc - 1 {
        let src = cmd.argv[k];
        let mut sabs = [0u8; PATH_MAX];
        let sn = shell.abs_path(src, &mut sabs);
        let sap = to_str(&sabs[..sn]);
        // Resolve the target path (into an existing directory, or the literal
        // destination) into a stack buffer so we need no allocation.
        let mut tbuf = [0u8; PATH_MAX];
        let tn = if dest_is_dir {
            let mut i = src.len();
            while i > 0 && src[i - 1] != b'/' {
                i -= 1;
            }
            join_path(dest, &src[i..], &mut tbuf)
        } else {
            shell.abs_path(dest, &mut tbuf)
        };
        let target = to_str(&tbuf[..tn]);
        if !fs::rename(sap, target) {
            puts(out, "mv: cannot move '");
            putb(out, src);
            puts(out, "'\n");
            status = 1;
        }
    }
    status
}

fn cmd_stat(shell: &mut Shell, cmd: &Cmd, _stdin: &[u8], out: &mut dyn Sink) -> i32 {
    if cmd.argc < 2 {
        puts(out, "stat: missing operand\n");
        return 1;
    }
    let mut status = 0;
    for k in 1..cmd.argc {
        let a = cmd.argv[k];
        let mut abs = [0u8; PATH_MAX];
        let n = shell.abs_path(a, &mut abs);
        let ap = to_str(&abs[..n]);
        match fs::stat(ap) {
            Some(st) => {
                puts(out, "  File: ");
                putb(out, a);
                putc(out, b'\n');
                puts(out, "  Size: ");
                put_dec(out, st.size as u64);
                putc(out, b'\n');
                puts(out, "  Type: ");
                puts(out, if st.mode == fs::MODE_DIR { "directory" } else { "regular file" });
                putc(out, b'\n');
            }
            None => {
                puts(out, "stat: cannot stat '");
                putb(out, a);
                puts(out, "'\n");
                status = 1;
            }
        }
    }
    status
}

fn cmd_env(shell: &mut Shell, _cmd: &Cmd, _stdin: &[u8], out: &mut dyn Sink) -> i32 {
    for v in shell.vars.iter() {
        putb(out, &v.name[..v.nlen]);
        putc(out, b'=');
        putb(out, &v.val[..v.vlen]);
        putc(out, b'\n');
    }
    0
}

fn cmd_export(shell: &mut Shell, cmd: &Cmd, _stdin: &[u8], out: &mut dyn Sink) -> i32 {
    if cmd.argc <= 1 {
        return cmd_env(shell, cmd, _stdin, out);
    }
    for k in 1..cmd.argc {
        let a = cmd.argv[k];
        let mut eq = None;
        for (i, &b) in a.iter().enumerate() {
            if b == b'=' {
                eq = Some(i);
                break;
            }
        }
        match eq {
            Some(p) => {
                shell.set_var(&a[..p], &a[p + 1..]);
            }
            None => {
                // Export an existing variable (no value change). Copy the value
                // out first so the immutable borrow ends before the mutable one.
                if let Some(val) = shell.get_var(a) {
                    let mut v = [0u8; 64];
                    let mut vl = 0;
                    for &b in val {
                        if vl < 64 {
                            v[vl] = b;
                            vl += 1;
                        }
                    }
                    shell.set_var(a, &v[..vl]);
                }
            }
        }
    }
    0
}

fn cmd_unset(shell: &mut Shell, cmd: &Cmd, _stdin: &[u8], out: &mut dyn Sink) -> i32 {
    for k in 1..cmd.argc {
        shell.del_var(cmd.argv[k]);
    }
    let _ = out;
    0
}

fn cmd_clear(_shell: &mut Shell, _cmd: &Cmd, _stdin: &[u8], out: &mut dyn Sink) -> i32 {
    out.clear_screen();
    0
}

fn cmd_help(_shell: &mut Shell, _cmd: &Cmd, _stdin: &[u8], out: &mut dyn Sink) -> i32 {
    puts(out, "nsh built-ins:\n");
    for (n, _) in BUILTINS.iter() {
        putb(out, n.as_bytes());
        putc(out, b' ');
    }
    putc(out, b'\n');
    puts(
        out,
        "Pipes (|), redirection (> >> <), command substitution $(...),\nand $VAR expansion are supported. Run `help <name>` for no detail yet.\n",
    );
    0
}

fn cmd_history(shell: &mut Shell, _cmd: &Cmd, _stdin: &[u8], out: &mut dyn Sink) -> i32 {
    for i in 0..shell.history_len() {
        if let Some(line) = shell.history(i) {
            put_dec(out, (i + 1) as u64);
            putc(out, b' ');
            putb(out, line);
            putc(out, b'\n');
        }
    }
    0
}

fn cmd_true(_shell: &mut Shell, _cmd: &Cmd, _stdin: &[u8], _out: &mut dyn Sink) -> i32 {
    0
}

fn cmd_false(_shell: &mut Shell, _cmd: &Cmd, _stdin: &[u8], _out: &mut dyn Sink) -> i32 {
    1
}

fn cmd_whoami(_shell: &mut Shell, _cmd: &Cmd, _stdin: &[u8], out: &mut dyn Sink) -> i32 {
    puts(out, "user\n");
    0
}

fn cmd_which(_shell: &mut Shell, cmd: &Cmd, _stdin: &[u8], out: &mut dyn Sink) -> i32 {
    for k in 1..cmd.argc {
        let name = cmd.argv[k];
        if is_builtin(name) {
            putb(out, name);
            puts(out, " is a shell builtin\n");
        } else if KNOWN_APPS.iter().any(|a| a.as_bytes() == name) {
            putb(out, name);
            puts(out, " is /bin/");
            putb(out, name);
            putc(out, b'\n');
        } else {
            putb(out, name);
            puts(out, ": not found\n");
        }
    }
    0
}

fn cmd_touch(shell: &mut Shell, cmd: &Cmd, _stdin: &[u8], out: &mut dyn Sink) -> i32 {
    let mut status = 0;
    for k in 1..cmd.argc {
        let a = cmd.argv[k];
        let mut abs = [0u8; PATH_MAX];
        let n = shell.abs_path(a, &mut abs);
        let ap = to_str(&abs[..n]);
        if fs::open(ap, true).is_none() {
            puts(out, "touch: cannot touch '");
            putb(out, a);
            putc(out, b'\n');
            status = 1;
        }
    }
    status
}

fn count_bytes(b: &[u8]) -> (u64, u64, u64) {
    let mut lines = 0u64;
    let mut words = 0u64;
    let mut bytes = 0u64;
    let mut in_word = false;
    for &c in b.iter() {
        bytes += 1;
        if c == b'\n' {
            lines += 1;
        }
        if c == b' ' || c == b'\t' || c == b'\n' || c == b'\r' {
            in_word = false;
        } else if !in_word {
            words += 1;
            in_word = true;
        }
    }
    (lines, words, bytes)
}

fn cmd_wc(shell: &mut Shell, cmd: &Cmd, stdin: &[u8], out: &mut dyn Sink) -> i32 {
    let mut tl = 0u64;
    let mut tw = 0u64;
    let mut tb = 0u64;
    let mut files = 0;
    if cmd.argc <= 1 {
        let (l, w, b) = count_bytes(stdin);
        tl += l;
        tw += w;
        tb += b;
        put_dec(out, l);
        putc(out, b' ');
        put_dec(out, w);
        putc(out, b' ');
        put_dec(out, b);
        putc(out, b'\n');
    } else {
        for k in 1..cmd.argc {
            let a = cmd.argv[k];
            let mut buf = [0u8; 4096];
            let r = shell.read_file(a, &mut buf);
            let (l, w, b) = count_bytes(&buf[..r]);
            tl += l;
            tw += w;
            tb += b;
            files += 1;
            put_dec(out, l);
            putc(out, b' ');
            put_dec(out, w);
            putc(out, b' ');
            put_dec(out, b);
            putc(out, b' ');
            putb(out, a);
            putc(out, b'\n');
        }
    }
    if files > 1 {
        put_dec(out, tl);
        putc(out, b' ');
        put_dec(out, tw);
        putc(out, b' ');
        put_dec(out, tb);
        puts(out, " total\n");
    }
    0
}

fn print_head_tail(out: &mut dyn Sink, data: &[u8], lines: usize, head: bool) {
    // Collect line boundaries.
    let mut bounds: [usize; 256] = [0; 256];
    let mut bc = 0;
    for i in 0..data.len() {
        if data[i] == b'\n' {
            if bc < bounds.len() {
                bounds[bc] = i + 1;
                bc += 1;
            }
        }
    }
    if bc == 0 && !data.is_empty() {
        bounds[0] = data.len();
        bc = 1;
    }
    let take = lines.min(bc);
    if head {
        let mut pos = 0;
        for i in 0..take {
            let end = bounds[i];
            out.write(&data[pos..end]);
            pos = end;
        }
    } else {
        let start_idx = if bc > lines { bc - lines } else { 0 };
        let mut pos = if start_idx > 0 { bounds[start_idx - 1] } else { 0 };
        for i in start_idx..bc {
            let end = bounds[i];
            out.write(&data[pos..end]);
            pos = end;
        }
    }
}

fn parse_n_arg(cmd: &Cmd, default: usize) -> usize {
    for k in 1..cmd.argc {
        let a = cmd.argv[k];
        if a.len() > 2 && &a[..2] == b"-n" {
            let mut v: usize = 0;
            for &c in &a[2..] {
                if c.is_ascii_digit() {
                    v = v * 10 + (c - b'0') as usize;
                }
            }
            return if v == 0 { default } else { v };
        }
    }
    default
}

fn cmd_head(shell: &mut Shell, cmd: &Cmd, stdin: &[u8], out: &mut dyn Sink) -> i32 {
    let n = parse_n_arg(cmd, 10);
    if cmd.argc <= 1 {
        print_head_tail(out, stdin, n, true);
        return 0;
    }
    for k in 1..cmd.argc {
        let a = cmd.argv[k];
        if a.len() > 2 && &a[..2] == b"-n" {
            continue;
        }
        let mut buf = [0u8; 4096];
        let r = shell.read_file(a, &mut buf);
        print_head_tail(out, &buf[..r], n, true);
    }
    0
}

fn cmd_tail(shell: &mut Shell, cmd: &Cmd, stdin: &[u8], out: &mut dyn Sink) -> i32 {
    let n = parse_n_arg(cmd, 10);
    if cmd.argc <= 1 {
        print_head_tail(out, stdin, n, false);
        return 0;
    }
    for k in 1..cmd.argc {
        let a = cmd.argv[k];
        if a.len() > 2 && &a[..2] == b"-n" {
            continue;
        }
        let mut buf = [0u8; 4096];
        let r = shell.read_file(a, &mut buf);
        print_head_tail(out, &buf[..r], n, false);
    }
    0
}

fn tree_walk(shell: &Shell, ap: &str, path: &[u8], depth: usize, out: &mut dyn Sink, status: &mut i32) {
    // Indentation.
    for _ in 0..depth {
        puts(out, "  ");
    }
    putb(out, if depth == 0 { path } else {
        let mut i = path.len();
        while i > 0 && path[i - 1] != b'/' {
            i -= 1;
        }
        &path[i..]
    });
    putc(out, b'\n');
    match fs::stat(ap) {
        Some(st) if st.mode == fs::MODE_DIR => {
            if let Some(fd) = fs::opendir(ap) {
                while let Some(Some(e)) = fs::readdir(fd) {
                    let nm = e.name();
                    let mut child = [0u8; PATH_MAX];
                    let cl = join_path(path, nm.as_bytes(), &mut child);
                    let mut cab = [0u8; PATH_MAX];
                    let cn = shell.abs_path(&child[..cl], &mut cab);
                    let cap = to_str(&cab[..cn]);
                    tree_walk(shell, cap, &child[..cl], depth + 1, out, status);
                }
            }
        }
        _ => {}
    }
}

fn cmd_tree(shell: &mut Shell, cmd: &Cmd, _stdin: &[u8], out: &mut dyn Sink) -> i32 {
    let target = if cmd.argc > 1 { cmd.argv[1] } else { b"." };
    let mut abs = [0u8; PATH_MAX];
    let n = shell.abs_path(target, &mut abs);
    let ap = to_str(&abs[..n]);
    let mut status = 0;
    tree_walk(shell, ap, target, 0, out, &mut status);
    status
}

fn cmd_alias(shell: &mut Shell, cmd: &Cmd, _stdin: &[u8], out: &mut dyn Sink) -> i32 {
    if cmd.argc <= 1 {
        for a in shell.aliases.iter() {
            if a.nlen == 0 {
                continue;
            }
            putb(out, &a.name[..a.nlen]);
            puts(out, "='");
            putb(out, &a.val[..a.vlen]);
            puts(out, "'\n");
        }
        return 0;
    }
    for k in 1..cmd.argc {
        let a = cmd.argv[k];
        let mut eq = None;
        for (i, &b) in a.iter().enumerate() {
            if b == b'=' {
                eq = Some(i);
                break;
            }
        }
        match eq {
            Some(p) if p > 0 => shell.set_alias(&a[..p], &a[p + 1..]),
            Some(_) => {}
            None => match shell.get_alias(a) {
                Some(val) => {
                    putb(out, a);
                    puts(out, "='");
                    putb(out, val);
                    puts(out, "'\n");
                }
                None => {
                    puts(out, "alias: ");
                    putb(out, a);
                    puts(out, ": not found\n");
                }
            },
        }
    }
    0
}

/// Run a Nuomi (糯米) script. The interpreter lives in [`crate::nuomi`]; this
/// builtin only exists so the language is reachable as `nuomi script.nuomi`.
fn cmd_nuomi(shell: &mut Shell, cmd: &Cmd, _stdin: &[u8], out: &mut dyn Sink) -> i32 {
    crate::nuomi::command(shell, cmd, out)
}

fn cmd_drives(_shell: &mut Shell, _cmd: &Cmd, _stdin: &[u8], out: &mut dyn Sink) -> i32 {
    puts(out, "Mounts (single root, see design notes):\n");
    puts(out, "  /   NovusFS root\n");
    0
}

/* ---- dispatch ---------------------------------------------------------- */

/// Resolve and run a command. Returns the exit status (127 = not found).
pub fn run(shell: &mut Shell, cmd: &Cmd, stdin: &[u8], out: &mut dyn Sink) -> i32 {
    if cmd.argc == 0 {
        return 0;
    }
    let name = cmd.argv[0];

    // User-defined aliases (single expansion, no recursion guard needed).
    if let Some(alias_val) = shell.get_alias(name) {
        let alen = alias_val.len().min(63);
        let mut owned = [0u8; 64];
        owned[..alen].copy_from_slice(&alias_val[..alen]);
        return shell.run_line(&owned[..alen], out);
    }

    if let Some(f) = lookup_builtin(name) {
        return f(shell, cmd, stdin, out);
    }

    // Optional cmd.exe compatibility aliases (default off).
    if shell.cmd_aliases {
        let real: &str = match to_str(name) {
            "dir" => "ls",
            "copy" => "cp",
            "del" => "rm",
            "move" => "mv",
            "type" => "cat",
            "cls" => "clear",
            _ => "",
        };
        if !real.is_empty() {
            if let Some(f) = lookup_builtin(real.as_bytes()) {
                return f(shell, cmd, stdin, out);
            }
        }
    }

    // External: a registered built-in application launched by name.
    if shell.sys_port != 0 {
        if let Some(rc) = sinfo::spawn_name(shell.sys_port, to_str(name)) {
            if rc >= 0 {
                return 0;
            }
        }
    }
    puts(out, to_str(name));
    puts(out, ": command not found\n");
    127
}

const BUILTINS: &[(&str, Bfn)] = &[
    ("ls", cmd_ls),
    ("cd", cmd_cd),
    ("pwd", cmd_pwd),
    ("cat", cmd_cat),
    ("echo", cmd_echo),
    ("mkdir", cmd_mkdir),
    ("rmdir", cmd_rmdir),
    ("rm", cmd_rm),
    ("cp", cmd_cp),
    ("mv", cmd_mv),
    ("stat", cmd_stat),
    ("env", cmd_env),
    ("export", cmd_export),
    ("unset", cmd_unset),
    ("clear", cmd_clear),
    ("help", cmd_help),
    ("history", cmd_history),
    ("true", cmd_true),
    ("false", cmd_false),
    ("whoami", cmd_whoami),
    ("which", cmd_which),
    ("touch", cmd_touch),
    ("wc", cmd_wc),
    ("head", cmd_head),
    ("tail", cmd_tail),
    ("tree", cmd_tree),
    ("alias", cmd_alias),
    ("drives", cmd_drives),
    ("nuomi", cmd_nuomi),
];
