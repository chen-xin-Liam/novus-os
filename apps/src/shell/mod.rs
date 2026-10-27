//! `nsh` shell engine: a small, allocator-free command interpreter.
//!
//! Everything here runs in a `#![no_std]` binary with no heap, so state is
//! kept in fixed arrays and output goes through a [`Sink`] trait the terminal
//! (or a command-substitution capture buffer) implements. The engine talks to
//! the kernel only through the `fs` and `sinfo` wrappers.
//!
//! Design boundaries that matter:
//!
//! * Paths are always resolved to the single root `/` (see `path`). The shell
//!   owns the cwd; the kernel never sees a relative path.
//! * There is no fork/exec: builtins run inline, external commands are
//!   registered apps launched through `sinfo::spawn_name`, and `|` / `$(...)`
//!   are sequential captures rather than concurrent processes.

#![allow(dead_code)]

pub mod cmds;
pub mod parse;
pub mod path;

use crate::fs;

pub use path::PATH_MAX;

pub const MAX_ARGS: usize = 40;
pub const SCRATCH: usize = 1536;
pub const WORD_MAX: usize = 256;
pub const VAR_MAX: usize = 32;
pub const HIST_MAX: usize = 64;

/// Anything the shell can write to: the terminal window, or a capture buffer
/// used for `$(...)` and pipes.
pub trait Sink {
    fn write(&mut self, data: &[u8]);
    /// `clear` asks the real terminal to wipe its scrollback; a capture buffer
    /// ignores it.
    fn clear_screen(&mut self) {}
}

/// In-memory buffer that implements [`Sink`]; used for command substitution
/// and for the middle of a pipeline.
pub struct Capture {
    pub buf: [u8; 4096],
    pub len: usize,
}

impl Capture {
    pub fn new() -> Capture {
        Capture {
            buf: [0u8; 4096],
            len: 0,
        }
    }
}

impl Sink for Capture {
    fn write(&mut self, data: &[u8]) {
        let n = (self.len + data.len()).min(self.buf.len());
        self.buf[self.len..n].copy_from_slice(&data[..n - self.len]);
        self.len = n;
    }
}

/// One parsed command (a pipeline stage): its argument vector, which points
/// into the scratch buffer passed to [`parse::parse_command`], and any
/// redirections attached to it.
pub struct Cmd<'a> {
    pub argv: [&'a [u8]; MAX_ARGS],
    pub argc: usize,
    pub redir_in: Option<&'a [u8]>,
    pub redir_out: Option<&'a [u8]>,
    pub append: bool,
}

impl<'a> Cmd<'a> {
    pub fn new() -> Cmd<'a> {
        Cmd {
            argv: [&[]; MAX_ARGS],
            argc: 0,
            redir_in: None,
            redir_out: None,
            append: false,
        }
    }
}

#[derive(Clone, Copy)]
struct Var {
    name: [u8; 16],
    val: [u8; 64],
    nlen: usize,
    vlen: usize,
}

#[derive(Clone, Copy)]
struct Alias {
    name: [u8; 16],
    val: [u8; 64],
    nlen: usize,
    vlen: usize,
}

/// The interactive shell's persistent state.
pub struct Shell {
    pub cwd: [u8; PATH_MAX],
    pub cwd_len: usize,
    pub sys_port: u64,
    pub cmd_aliases: bool,
    pub last: i32,
    vars: [Var; VAR_MAX],
    vcount: usize,
    aliases: [Alias; 16],
    acount: usize,
    hist: [[u8; 128]; HIST_MAX],
    hcount: usize,
    sdepth: usize,
}

impl Shell {
    pub fn new(sys_port: u64) -> Shell {
        let mut s = Shell {
            cwd: [0u8; PATH_MAX],
            cwd_len: 1,
            sys_port,
            cmd_aliases: false,
            last: 0,
            vars: [Var {
                name: [0u8; 16],
                val: [0u8; 64],
                nlen: 0,
                vlen: 0,
            }; VAR_MAX],
            vcount: 0,
            aliases: [Alias {
                name: [0u8; 16],
                val: [0u8; 64],
                nlen: 0,
                vlen: 0,
            }; 16],
            acount: 0,
            hist: [[0u8; 128]; HIST_MAX],
            hcount: 0,
            sdepth: 0,
        };
        s.cwd[0] = b'/';
        s.set_var(b"HOME", b"/");
        s.set_var(b"PWD", b"/");
        s.set_var(b"USER", b"user");
        s.set_var(b"SHELL", b"nsh");
        s
    }

    /// Resolve `input` to an absolute path using the cwd, writing into `out`.
    pub fn abs_path(&self, input: &[u8], out: &mut [u8]) -> usize {
        path::normalize(&self.cwd[..self.cwd_len], input, out)
    }

    pub fn set_var(&mut self, name: &[u8], val: &[u8]) {
        // Update an existing variable when present.
        for v in self.vars.iter_mut().take(self.vcount) {
            if v.nlen == name.len() && &v.name[..v.nlen] == name {
                v.vlen = 0;
                for &b in val.iter().take(64) {
                    if v.vlen < 64 {
                        v.val[v.vlen] = b;
                        v.vlen += 1;
                    }
                }
                return;
            }
        }
        if self.vcount < VAR_MAX {
            let i = self.vcount;
            let mut n = 0;
            for &b in name.iter().take(16) {
                if n < 16 {
                    self.vars[i].name[n] = b;
                    n += 1;
                }
            }
            self.vars[i].nlen = n;
            let mut m = 0;
            for &b in val.iter().take(64) {
                if m < 64 {
                    self.vars[i].val[m] = b;
                    m += 1;
                }
            }
            self.vars[i].vlen = m;
            self.vcount += 1;
        }
    }

    pub fn get_var(&self, name: &[u8]) -> Option<&[u8]> {
        for v in self.vars.iter().take(self.vcount) {
            if v.nlen == name.len() && &v.name[..v.nlen] == name {
                return Some(&v.val[..v.vlen]);
            }
        }
        None
    }

    pub fn del_var(&mut self, name: &[u8]) {
        let mut i = 0;
        while i < self.vcount {
            if self.vars[i].nlen == name.len() && &self.vars[i].name[..self.vars[i].nlen] == name {
                self.vars[i] = self.vars[self.vcount - 1].clone();
                self.vcount -= 1;
            } else {
                i += 1;
            }
        }
    }

    pub fn set_alias(&mut self, name: &[u8], val: &[u8]) {
        for a in self.aliases.iter_mut().take(self.acount) {
            if a.nlen == name.len() && &a.name[..a.nlen] == name {
                a.vlen = 0;
                for &b in val.iter().take(64) {
                    if a.vlen < 64 {
                        a.val[a.vlen] = b;
                        a.vlen += 1;
                    }
                }
                return;
            }
        }
        if self.acount < 16 {
            let i = self.acount;
            let mut n = 0;
            for &b in name.iter().take(16) {
                if n < 16 {
                    self.aliases[i].name[n] = b;
                    n += 1;
                }
            }
            self.aliases[i].nlen = n;
            let mut m = 0;
            for &b in val.iter().take(64) {
                if m < 64 {
                    self.aliases[i].val[m] = b;
                    m += 1;
                }
            }
            self.aliases[i].vlen = m;
            self.acount += 1;
        }
    }

    pub fn get_alias(&self, name: &[u8]) -> Option<&[u8]> {
        for a in self.aliases.iter().take(self.acount) {
            if a.nlen == name.len() && &a.name[..a.nlen] == name {
                return Some(&a.val[..a.vlen]);
            }
        }
        None
    }

    /// Push a command line into the history ring.
    pub fn push_history(&mut self, line: &[u8]) {
        if line.is_empty() {
            return;
        }
        let i = if self.hcount < HIST_MAX {
            let i = self.hcount;
            self.hcount += 1;
            i
        } else {
            // Shift the ring down by one (cheap: HIST_MAX is small).
            for k in 1..HIST_MAX {
                self.hist[k - 1] = self.hist[k];
            }
            HIST_MAX - 1
        };
        let mut n = 0;
        for &b in line.iter().take(128) {
            self.hist[i][n] = b;
            n += 1;
        }
    }

    pub fn history(&self, idx: usize) -> Option<&[u8]> {
        if idx < self.hcount {
            // History is stored oldest..newest in the first `hcount` slots;
            // callers pass a 0-based offset from the start.
            Some(&self.hist[idx])
        } else {
            None
        }
    }

    pub fn history_len(&self) -> usize {
        self.hcount
    }

    /// Build the prompt string `user@novus:<cwd>$ ` into `out`.
    pub fn prompt(&self, out: &mut [u8]) -> usize {
        let prefix = b"user@novus:";
        let mut n = 0;
        for &b in prefix.iter().chain(self.cwd[..self.cwd_len].iter()) {
            if n < out.len() {
                out[n] = b;
                n += 1;
            }
        }
        if n + 2 <= out.len() {
            out[n] = b'$';
            out[n + 1] = b' ';
            n += 2;
        }
        n
    }

    /// Run a whole command line (one logical input line). Returns the exit
    /// status of the last command.
    pub fn run_line(&mut self, line: &[u8], out: &mut dyn Sink) -> i32 {
        let mut pos = 0usize;
        let len = line.len();
        let mut skip = false;
        while pos <= len {
            let (sep, next) = scan_sep(line, pos);
            let stmt = trim(&line[pos..sep]);
            if !stmt.is_empty() && !skip {
                self.last = self.run_statement(stmt, out);
            }
            match sep_kind(line, pos) {
                2 => skip = self.last != 0, // &&
                3 => skip = self.last == 0, // ||
                _ => skip = false,
            }
            pos = next;
            if sep == len {
                break;
            }
        }
        self.last
    }

    /// Like [`Shell::run_line`] but for `$(...)`: bumps the substitution depth
    /// so nested scratch buffers do not clobber each other, and aborts deep
    /// nesting rather than risk a stack overflow.
    pub fn run_subst(&mut self, line: &[u8], out: &mut dyn Sink) -> i32 {
        if self.sdepth >= 3 {
            return -1;
        }
        self.sdepth += 1;
        let r = self.run_line(line, out);
        self.sdepth -= 1;
        r
    }

    /// Run one `;`-/`&&`/`||`-separated statement, which may be a pipeline of
    /// `|`-separated commands.
    fn run_statement(&mut self, stmt: &[u8], out: &mut dyn Sink) -> i32 {
        let mut ranges: [(usize, usize); 8] = [(0, 0); 8];
        let n = scan_pipe(stmt, &mut ranges);
        if n == 0 {
            return 0;
        }

        // Stdin for the first stage: from `< file` if present, else empty.
        let mut pipe: [u8; 4096] = [0u8; 4096];
        let mut pipe_len = 0usize;
        let mut first_is_file = false;

        let mut scratch = [0u8; SCRATCH];
        let mut status = 0i32;

        for i in 0..n {
            let seg = &stmt[ranges[i].0..ranges[i].1];
            let cmd = parse::parse_command(self, seg, &mut scratch);
            let is_first = i == 0;
            let is_last = i + 1 == n;

            if is_first && cmd.redir_in.is_some() {
                let mut fb = [0u8; 4096];
                let r = self.read_file(cmd.redir_in.unwrap(), &mut fb);
                pipe_len = r;
                pipe[..r].copy_from_slice(&fb[..r]);
                first_is_file = true;
            }

            let stdin: &[u8] = if is_first {
                if first_is_file {
                    &pipe[..pipe_len]
                } else {
                    &[]
                }
            } else {
                &pipe[..pipe_len]
            };

            if is_last && cmd.redir_out.is_some() {
                let mut cap = Capture::new();
                status = cmds::run(self, &cmd, stdin, &mut cap);
                self.write_file(cmd.redir_out.unwrap(), &cap.buf[..cap.len], cmd.append);
            } else if is_last {
                status = cmds::run(self, &cmd, stdin, out);
            } else {
                let mut cap = Capture::new();
                status = cmds::run(self, &cmd, stdin, &mut cap);
                pipe_len = cap.len.min(pipe.len());
                pipe[..pipe_len].copy_from_slice(&cap.buf[..pipe_len]);
            }
        }
        status
    }

    /// Read a whole file (relative to cwd) into `buf`. Returns bytes read.
    pub fn read_file(&self, path: &[u8], buf: &mut [u8]) -> usize {
        let mut abs = [0u8; PATH_MAX];
        let n = self.abs_path(path, &mut abs);
        if n == 0 {
            return 0;
        }
        let ap = to_str(&abs[..n]);
        let fd = match fs::open(ap, false) {
            Some(f) => f,
            None => return 0,
        };
        let size = match fs::stat(ap) {
            Some(st) => st.size as usize,
            None => return 0,
        };
        let want = size.min(buf.len());
        let got = fs::read(fd, &mut buf[..want]);
        if got < 0 {
            0
        } else {
            got as usize
        }
    }

    /// Write `data` to a file. `append == false` truncates (delete + recreate,
    /// since the VFS has no truncate syscall); `append == true` concatenates
    /// onto the existing contents.
    pub fn write_file(&self, path: &[u8], data: &[u8], append: bool) -> bool {
        let mut abs = [0u8; PATH_MAX];
        let n = self.abs_path(path, &mut abs);
        if n == 0 {
            return false;
        }
        let ap = to_str(&abs[..n]);

        if !append {
            fs::unlink(ap);
            let fd = match fs::open(ap, true) {
                Some(f) => f,
                None => return false,
            };
            let w = fs::write(fd, data);
            w >= 0
        } else {
            // Concatenate: read the old content, then write old+new.
            let mut old = [0u8; 4096];
            let old_n = self.read_file(path, &mut old);
            fs::unlink(ap);
            let fd = match fs::open(ap, true) {
                Some(f) => f,
                None => return false,
            };
            if old_n > 0 && fs::write(fd, &old[..old_n]) < 0 {
                return false;
            }
            let w = fs::write(fd, data);
            w >= 0
        }
    }
}

/// Convert bytes to a `&str`, substituting a placeholder for anything that is
/// not valid UTF-8 (file names are expected to be UTF-8).
pub fn to_str(b: &[u8]) -> &str {
    core::str::from_utf8(b).unwrap_or("<?>")
}

pub fn trim(b: &[u8]) -> &[u8] {
    let mut s = 0;
    let mut e = b.len();
    while s < e && (b[s] == b' ' || b[s] == b'\t' || b[s] == b'\r' || b[s] == b'\n') {
        s += 1;
    }
    while e > s && (b[e - 1] == b' ' || b[e - 1] == b'\t' || b[e - 1] == b'\r' || b[e - 1] == b'\n') {
        e -= 1;
    }
    &b[s..e]
}

/// Find the next statement separator (`;`, `&&`, `||`) at the top level
/// (outside quotes). Returns `(sep_index, next_pos)`; `sep_index == len` means
/// end of line.
fn scan_sep(line: &[u8], mut pos: usize) -> (usize, usize) {
    let len = line.len();
    while pos < len {
        let c = line[pos];
        if c == b'\'' || c == b'"' {
            let q = c;
            pos += 1;
            while pos < len && line[pos] != q {
                pos += 1;
            }
            if pos < len {
                pos += 1;
            }
            continue;
        }
        if c == b';' {
            return (pos, pos + 1);
        }
        if c == b'&' && pos + 1 < len && line[pos + 1] == b'&' {
            return (pos, pos + 2);
        }
        if c == b'|' && pos + 1 < len && line[pos + 1] == b'|' {
            return (pos, pos + 2);
        }
        pos += 1;
    }
    (len, len)
}

/// Classify the separator starting at `pos` (0 none, 1 `;`, 2 `&&`, 3 `||`).
fn sep_kind(line: &[u8], pos: usize) -> u8 {
    if pos >= line.len() {
        return 0;
    }
    match line[pos] {
        b';' => 1,
        b'&' => 2,
        b'|' => 3,
        _ => 0,
    }
}

/// Split a statement into pipeline stages on top-level `|`. Fills `ranges`
/// (up to 8) and returns the count.
fn scan_pipe(stmt: &[u8], ranges: &mut [(usize, usize); 8]) -> usize {
    let len = stmt.len();
    let mut start = 0usize;
    let mut pos = 0usize;
    let mut count = 0usize;
    while pos <= len && count < ranges.len() {
        if pos < len {
            let c = stmt[pos];
            if c == b'\'' || c == b'"' {
                let q = c;
                pos += 1;
                while pos < len && stmt[pos] != q {
                    pos += 1;
                }
                if pos < len {
                    pos += 1;
                }
                continue;
            }
            if c == b'|' {
                let seg = trim(&stmt[start..pos]);
                if !seg.is_empty() {
                    ranges[count] = (start, pos);
                    count += 1;
                }
                pos += 1;
                start = pos;
                continue;
            }
            pos += 1;
        } else {
            let seg = trim(&stmt[start..pos]);
            if !seg.is_empty() {
                ranges[count] = (start, pos);
                count += 1;
            }
            break;
        }
    }
    count
}
