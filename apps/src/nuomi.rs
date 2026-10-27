//! Nuomi (糯米) interpreter for `nsh`.
//!
//! This is the on-device port of `user/Nuomi_pl`: the same Lisp-flavoured
//! language, the same special forms and the same primitive names, rewritten
//! for the constraints of a built-in application (no `std`, no heap, no
//! libm). The original C implementation is the specification; where this
//! port deviates, the difference is a documented limitation below.
//!
//! Storage is one bump arena handed in by the caller and carved into
//! variable-sized objects (strings, cons cells, vectors, environments,
//! closures). There is no collector: a script gets what the arena holds and
//! an "out of memory" error when it runs dry, so loops that build a list per
//! iteration should keep the lists short.
//!
//! Deliberate limits relative to `user/Nuomi_pl`:
//!   * no JSON / HTTP / process / window primitives (different platform
//!     surface, not needed to run a script from the terminal);
//!   * `input`/`key` cannot block on the terminal, so `key` yields `nil` and
//!     `input` is absent - a script in nsh is not interactive;
//!   * `sqrt`/`pow` are computed here (core has no libm); `pow` only takes
//!     an integer exponent and returns `nan` otherwise;
//!   * recursion depth is bounded by [`MAX_DEPTH`] (about 500 nested calls)
//!     instead of a 256 MiB host thread, because the interpreter shares the
//!     shell's stack; a script has 6 KiB of source, 256 KiB of objects,
//!     24 call arguments and 64-element list/vector literals.
//!
//! The shell entry point is [`command`]; everything below it is private.

#![allow(dead_code)]

use crate::fs;
use crate::shell::{Cmd, Shell, Sink, PATH_MAX};
use crate::sys;

/// Bytes available for values. Lives on `cmd_nuomi`'s stack frame.
const ARENA_BYTES: usize = 256 * 1024;
/// Part of [`ARENA_BYTES`] reserved for environments, which are reclaimed.
///
/// Sizing: a nested call keeps its environment (64 B for one parameter) plus
/// the argument vectors of its own and of the enclosing native call alive, so
/// the deepest recursion [`MAX_DEPTH`] allows needs roughly
/// `MAX_DEPTH / 3 * 128 B` - 64 KiB. This leaves a 2x margin, and the arena is
/// reclaimed whenever no closure escaped the frame.
const ENV_BYTES: usize = 128 * 1024;
/// Longest single source file the command accepts.
const SOURCE_MAX: usize = 6 * 1024;
/// Maximum evaluated arguments of one call.
const MAX_ARGS: usize = 24;
/// Nested-evaluation budget (one function call is roughly four levels).
const MAX_DEPTH: usize = 1500;
/// Bindings a single `let` / parameter list may introduce.
const MAX_BIND: usize = 24;
/// Elements one vector literal or `(vec ...)` may hold.
const MAX_VEC: usize = 64;
/// Scratch buffer for `str` / value formatting.
const FMT_MAX: usize = 1024;

type Res<T> = Result<T, &'static str>;

/* ------------------------------------------------------------------------- */
/* Values                                                                    */
/* ------------------------------------------------------------------------- */

#[derive(Clone, Copy, PartialEq, Eq)]
enum Nat {
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    NumEq,
    Lt,
    Gt,
    Le,
    Ge,
    Not,
    Equal,
    Eq,
    Print,
    Println,
    Draw,
    Cons,
    Car,
    Cdr,
    List,
    Length,
    Nth,
    Get,
    Set,
    VecNew,
    Str,
    Substring,
    StringSearch,
    Last,
    Reverse,
    Append,
    NullP,
    NumberP,
    StringP,
    ListP,
    VectorP,
    FunctionP,
    BoolP,
    MemberP,
    Map,
    Filter,
    Reduce,
    Range,
    Assoc,
    Abs,
    Floor,
    Ceil,
    Round,
    Sqrt,
    Pow,
    Min,
    Max,
    Exists,
    IsDir,
    FileSize,
    ReadFile,
    WriteFile,
    AppendFile,
    Ls,
    Mkdir,
    Rmdir,
    Delete,
    Mv,
    Cp,
    Cls,
    Home,
    ShowCursor,
    Sleep,
    Random,
    Key,
}

const NATIVES: &[(&[u8], Nat)] = &[
    (b"+", Nat::Add),
    (b"-", Nat::Sub),
    (b"*", Nat::Mul),
    (b"/", Nat::Div),
    (b"%", Nat::Mod),
    (b"=", Nat::NumEq),
    (b"<", Nat::Lt),
    (b">", Nat::Gt),
    (b"<=", Nat::Le),
    (b">=", Nat::Ge),
    (b"not", Nat::Not),
    (b"equal?", Nat::Equal),
    (b"eq?", Nat::Eq),
    (b"print", Nat::Print),
    (b"println", Nat::Println),
    (b"draw", Nat::Draw),
    (b"cons", Nat::Cons),
    (b"car", Nat::Car),
    (b"cdr", Nat::Cdr),
    (b"list", Nat::List),
    (b"length", Nat::Length),
    (b"nth", Nat::Nth),
    (b"get", Nat::Get),
    (b"set", Nat::Set),
    (b"vec", Nat::VecNew),
    (b"str", Nat::Str),
    (b"substring", Nat::Substring),
    (b"string-search", Nat::StringSearch),
    (b"last", Nat::Last),
    (b"reverse", Nat::Reverse),
    (b"append", Nat::Append),
    (b"null?", Nat::NullP),
    (b"number?", Nat::NumberP),
    (b"string?", Nat::StringP),
    (b"list?", Nat::ListP),
    (b"vector?", Nat::VectorP),
    (b"function?", Nat::FunctionP),
    (b"bool?", Nat::BoolP),
    (b"member?", Nat::MemberP),
    (b"map", Nat::Map),
    (b"filter", Nat::Filter),
    (b"reduce", Nat::Reduce),
    (b"range", Nat::Range),
    (b"assoc", Nat::Assoc),
    (b"abs", Nat::Abs),
    (b"floor", Nat::Floor),
    (b"ceil", Nat::Ceil),
    (b"round", Nat::Round),
    (b"sqrt", Nat::Sqrt),
    (b"pow", Nat::Pow),
    (b"min", Nat::Min),
    (b"max", Nat::Max),
    (b"exists?", Nat::Exists),
    (b"is-dir?", Nat::IsDir),
    (b"file-size", Nat::FileSize),
    (b"read-file", Nat::ReadFile),
    (b"write-file", Nat::WriteFile),
    (b"append-file", Nat::AppendFile),
    (b"ls", Nat::Ls),
    (b"mkdir", Nat::Mkdir),
    (b"rmdir", Nat::Rmdir),
    (b"delete", Nat::Delete),
    (b"mv", Nat::Mv),
    (b"cp", Nat::Cp),
    (b"cls", Nat::Cls),
    (b"home", Nat::Home),
    (b"show-cursor", Nat::ShowCursor),
    (b"sleep", Nat::Sleep),
    (b"random", Nat::Random),
    (b"key", Nat::Key),
];

#[derive(Clone, Copy)]
enum Val {
    Nil,
    Bool(bool),
    Num(f64),
    /// Identifier; resolved through an environment.
    Sym(*mut Str),
    /// String literal / produced string.
    Str(*mut Str),
    List(*mut Cons),
    Vec(*mut VecObj),
    Func(*mut Func),
    Native(Nat),
}

struct Str {
    len: u32,
    sym: bool,
    pad: [u8; 3],
}

struct Cons {
    car: Val,
    cdr: Val,
}

struct VecObj {
    len: u32,
    pad: u32,
}

struct Env {
    parent: *mut Env,
    n: u32,
    cap: u32,
    keys: *mut Val,
    vals: *mut Val,
}

struct Func {
    arity: u32,
    params: *mut Val,
    body: Val,
    env: *mut Env,
}

/* ------------------------------------------------------------------------- */
/* Arena                                                                     */
/* ------------------------------------------------------------------------- */

struct Arena {
    base: *mut u8,
    len: usize,
    used: usize,
}

impl Arena {
    fn new(buf: &mut [u8]) -> Arena {
        Arena {
            base: buf.as_mut_ptr(),
            len: buf.len(),
            used: 0,
        }
    }

    fn alloc(&mut self, size: usize, align: usize) -> Option<*mut u8> {
        let off = (self.used + align - 1) & !(align - 1);

        if size > self.len || off + size > self.len {
            return None;
        }
        self.used = off + size;
        Some(unsafe { self.base.add(off) })
    }
}

unsafe fn str_data<'a>(s: *mut Str) -> &'a [u8] {
    let p = (s as *const u8).add(core::mem::size_of::<Str>());

    core::slice::from_raw_parts(p, (*s).len as usize)
}

unsafe fn vec_items<'a>(v: *mut VecObj) -> &'a [Val] {
    let p = (v as *const u8).add(core::mem::size_of::<VecObj>());

    core::slice::from_raw_parts(p as *const Val, (*v).len as usize)
}

fn car(v: Val) -> Val {
    match v {
        Val::List(c) => unsafe { (*c).car },
        _ => Val::Nil,
    }
}

fn cdr(v: Val) -> Val {
    match v {
        Val::List(c) => unsafe { (*c).cdr },
        _ => Val::Nil,
    }
}

fn is_nil(v: Val) -> bool {
    matches!(v, Val::Nil)
}

fn is_falsey(v: Val) -> bool {
    match v {
        Val::Nil => true,
        Val::Bool(b) => !b,
        _ => false,
    }
}

fn list_len(mut v: Val) -> usize {
    let mut n = 0;

    while let Val::List(c) = v {
        n += 1;
        v = unsafe { (*c).cdr };
    }
    n
}

/// Length of a list or vector; `None` for anything else.
fn coll_len(v: Val) -> Option<usize> {
    match v {
        Val::Nil => Some(0),
        Val::List(_) => Some(list_len(v)),
        Val::Vec(p) => Some(unsafe { (*p).len as usize }),
        _ => None,
    }
}

/// Element `i` of a list or vector (`Val::Nil` when out of range).
fn coll_at(v: Val, i: usize) -> Val {
    match v {
        Val::List(_) => {
            let mut cur = v;

            for _ in 0..i {
                cur = cdr(cur);
            }
            car(cur)
        }
        Val::Vec(p) => {
            let items = unsafe { vec_items(p) };

            items.get(i).copied().unwrap_or(Val::Nil)
        }
        _ => Val::Nil,
    }
}

/* ------------------------------------------------------------------------- */
/* Interpreter                                                               */
/* ------------------------------------------------------------------------- */

struct Interp<'a> {
    arena: Arena,
    env_arena: Arena,
    env_epoch: u64,
    out: &'a mut dyn Sink,
    shell: &'a Shell,
    depth: usize,
    globals: *mut Env,
}

impl<'a> Interp<'a> {
    fn new(buf: &'a mut [u8], out: &'a mut dyn Sink, shell: &'a Shell) -> Res<Interp<'a>> {
        let split = buf.len().saturating_sub(ENV_BYTES);
        let (values, envs) = buf.split_at_mut(split);
        let mut it = Interp {
            arena: Arena::new(values),
            env_arena: Arena::new(envs),
            env_epoch: 0,
            out,
            shell,
            depth: 0,
            globals: core::ptr::null_mut(),
        };
        let g = it.new_global_env(256)?;

        it.globals = g;
        for (name, id) in NATIVES.iter() {
            let s = it.alloc_str(name, true)?;

            it.env_define(g, s, Val::Native(*id))?;
        }
        Ok(it)
    }

    /* ---- allocation ---------------------------------------------------- */

    fn alloc_str(&mut self, bytes: &[u8], sym: bool) -> Res<*mut Str> {
        let total = core::mem::size_of::<Str>() + bytes.len();
        let p = self.arena.alloc(total, 8).ok_or("out of memory")? as *mut Str;

        unsafe {
            (*p).len = bytes.len() as u32;
            (*p).sym = sym;
            (*p).pad = [0; 3];
            let d = (p as *mut u8).add(core::mem::size_of::<Str>());

            core::ptr::copy_nonoverlapping(bytes.as_ptr(), d, bytes.len());
        }
        Ok(p)
    }

    fn alloc_cons(&mut self, a: Val, b: Val) -> Res<*mut Cons> {
        let p = self
            .arena
            .alloc(core::mem::size_of::<Cons>(), 8)
            .ok_or("out of memory")? as *mut Cons;

        unsafe {
            (*p).car = a;
            (*p).cdr = b;
        }
        Ok(p)
    }

    fn alloc_vec(&mut self, items: &[Val]) -> Res<*mut VecObj> {
        if items.len() > MAX_VEC {
            return Err("vector too long");
        }
        let vs = core::mem::size_of::<Val>();
        let total = core::mem::size_of::<VecObj>() + items.len() * vs;
        let p = self.arena.alloc(total, 8).ok_or("out of memory")? as *mut VecObj;

        unsafe {
            (*p).len = items.len() as u32;
            (*p).pad = 0;
            let d = (p as *mut u8).add(core::mem::size_of::<VecObj>()) as *mut Val;

            core::ptr::copy_nonoverlapping(items.as_ptr(), d, items.len());
        }
        Ok(p)
    }

    fn alloc_list(&mut self, items: &[Val]) -> Res<Val> {
        let mut head = Val::Nil;

        for &v in items.iter().rev() {
            head = Val::List(self.alloc_cons(v, head)?);
        }
        Ok(head)
    }

    fn new_env(&mut self, parent: *mut Env, cap: usize) -> Res<*mut Env> {
        // Environments come from their own stack-disciplined arena so a call
        // can give its frame back on return; values keep the main arena. A
        // closure that captured a frame bumps `env_epoch`, which makes the
        // owner skip the reset (the frame then lives as long as the arena).
        match env_alloc(&mut self.env_arena, parent, cap) {
            Some(p) => Ok(p),
            // Exhausted: fall back to the value arena, which never rewinds.
            None => env_alloc(&mut self.arena, parent, cap).ok_or("out of memory"),
        }
    }

    /// The global environment lives in the value arena: it must never be
    /// rewound, and it is too large to sit in the reclaimable region.
    fn new_global_env(&mut self, cap: usize) -> Res<*mut Env> {
        env_alloc(&mut self.arena, core::ptr::null_mut(), cap).ok_or("out of memory")
    }

    fn env_mark(&self) -> usize {
        self.env_arena.used
    }

    fn env_unwind(&mut self, mark: usize, epoch: u64) {
        if self.env_epoch == epoch {
            self.env_arena.used = mark;
        }
    }

    fn env_define(&mut self, env: *mut Env, key: *mut Str, val: Val) -> Res<()> {
        if env.is_null() {
            return Err("no environment");
        }
        unsafe {
            let n = (*env).n as usize;

            for i in 0..n {
                if let Val::Sym(s) = *(*env).keys.add(i) {
                    if str_data(s) == str_data(key) {
                        *(*env).vals.add(i) = val;
                        return Ok(());
                    }
                }
            }
            if n >= (*env).cap as usize {
                return Err("too many bindings");
            }
            *(*env).keys.add(n) = Val::Sym(key);
            *(*env).vals.add(n) = val;
            (*env).n = (n + 1) as u32;
        }
        Ok(())
    }

    /* ---- reader -------------------------------------------------------- */

    fn read_form(&mut self, src: &[u8], pos: &mut usize, depth: usize) -> Res<Val> {
        if depth > 32 {
            return Err("reader nesting too deep");
        }
        skip_ws(src, pos);
        if *pos >= src.len() {
            return Err("unexpected end of input");
        }
        let c = src[*pos];

        match c {
            b'(' => {
                *pos += 1;
                self.read_seq(src, pos, b')', depth)
            }
            b'[' => {
                *pos += 1;
                self.read_seq(src, pos, b']', depth)
            }
            b')' | b']' => Err("unexpected )"),
            b'"' => {
                *pos += 1;
                let mut buf = [0u8; FMT_MAX];
                let n = read_string(src, pos, b"\"", true, &mut buf)?;

                Ok(Val::Str(self.alloc_str(&buf[..n], false)?))
            }
            b'\'' => {
                *pos += 1;
                let v = self.read_form(src, pos, depth + 1)?;
                let q = Val::Sym(self.alloc_str(b"quote", true)?);

                self.alloc_list(&[q, v])
            }
            0xE2 => {
                // U+275D ❝ opens a string, U+275E ❞ closes it.
                if *pos + 3 <= src.len() && &src[*pos..*pos + 3] == "\u{275d}".as_bytes() {
                    *pos += 3;
                    let mut buf = [0u8; FMT_MAX];
                    let n = read_string(src, pos, "\u{275e}".as_bytes(), false, &mut buf)?;

                    Ok(Val::Str(self.alloc_str(&buf[..n], false)?))
                } else {
                    self.read_atom(src, pos)
                }
            }
            _ => self.read_atom(src, pos),
        }
    }

    fn read_seq(&mut self, src: &[u8], pos: &mut usize, close: u8, depth: usize) -> Res<Val> {
        let mut items = [Val::Nil; MAX_VEC];
        let mut n = 0usize;

        loop {
            skip_ws(src, pos);
            if *pos >= src.len() {
                return Err("unterminated list");
            }
            if src[*pos] == close {
                *pos += 1;
                break;
            }
            if src[*pos] == b')' || src[*pos] == b']' {
                return Err("mismatched bracket");
            }
            if n >= MAX_VEC {
                return Err("list literal too long");
            }
            items[n] = self.read_form(src, pos, depth + 1)?;
            n += 1;
        }
        if close == b']' {
            Ok(Val::Vec(self.alloc_vec(&items[..n])?))
        } else {
            self.alloc_list(&items[..n])
        }
    }

    fn read_atom(&mut self, src: &[u8], pos: &mut usize) -> Res<Val> {
        let start = *pos;

        while *pos < src.len() && !is_delimiter(src[*pos]) {
            // Stop before the UTF-8 closing delimiter so a stray ❞ is not
            // swallowed into the neighbouring atom.
            if src[*pos] == 0xE2
                && *pos + 3 <= src.len()
                && &src[*pos..*pos + 3] == "\u{275e}".as_bytes()
            {
                break;
            }
            *pos += 1;
        }
        if *pos == start {
            return Err("empty atom");
        }
        let tok = &src[start..*pos];

        if tok == b"true" {
            return Ok(Val::Bool(true));
        }
        if tok == b"false" {
            return Ok(Val::Bool(false));
        }
        if tok == b"nil" {
            return Ok(Val::Nil);
        }
        if let Some(n) = parse_num(tok) {
            return Ok(Val::Num(n));
        }
        Ok(Val::Sym(self.alloc_str(tok, true)?))
    }

    /* ---- evaluator ----------------------------------------------------- */

    fn eval(&mut self, node: Val, env: *mut Env) -> Res<Val> {
        if self.depth >= MAX_DEPTH {
            return Err("maximum recursion depth exceeded");
        }
        self.depth += 1;
        let r = self.eval_inner(node, env);

        self.depth -= 1;
        r
    }

    #[inline(never)]
    fn eval_inner(&mut self, node: Val, env: *mut Env) -> Res<Val> {
        match node {
            Val::Nil | Val::Bool(_) | Val::Num(_) | Val::Native(_) | Val::Func(_) => Ok(node),
            Val::Str(_) => Ok(node),
            Val::Sym(s) => {
                let name = unsafe { str_data(s) };

                match env_find(env, name) {
                    Some(slot) => Ok(unsafe { *slot }),
                    None => Err("unbound variable"),
                }
            }
            Val::Vec(v) => self.eval_vector(v, env),
            Val::List(c) => self.eval_list(c, env),
        }
    }

    /// Vector literals evaluate their elements (unlike quoted lists). Kept in
    /// its own frame so the scratch array is not reserved at every recursion
    /// level of the evaluator.
    #[inline(never)]
    fn eval_vector(&mut self, v: *mut VecObj, env: *mut Env) -> Res<Val> {
        let items = unsafe { vec_items(v) };
        let mut tmp = [Val::Nil; MAX_VEC];
        let n = items.len().min(MAX_VEC);

        for i in 0..n {
            tmp[i] = self.eval(items[i], env)?;
        }
        Ok(Val::Vec(self.alloc_vec(&tmp[..n])?))
    }

    #[inline(never)]
    fn eval_body(&mut self, mut cur: Val, env: *mut Env) -> Res<Val> {
        let mut last = Val::Nil;

        while let Val::List(c) = cur {
            let form = unsafe { (*c).car };

            last = self.eval(form, env)?;
            cur = unsafe { (*c).cdr };
        }
        Ok(last)
    }

    #[inline(never)]
    fn eval_list(&mut self, cell: *mut Cons, env: *mut Env) -> Res<Val> {
        let head = unsafe { (*cell).car };
        let args = unsafe { (*cell).cdr };

        if let Val::Sym(s) = head {
            let name = unsafe { str_data(s) };

            match name {
                b"quote" => return Ok(car(args)),
                b"if" => {
                    let c = self.eval(car(args), env)?;

                    if !is_falsey(c) {
                        return self.eval(car(cdr(args)), env);
                    }
                    let alt = cdr(cdr(args));

                    if is_nil(alt) {
                        return Ok(Val::Nil);
                    }
                    return self.eval(car(alt), env);
                }
                b"while" => {
                    let cond = car(args);
                    let body = cdr(args);
                    let mut last = Val::Nil;

                    while !is_falsey(self.eval(cond, env)?) {
                        last = self.eval_body(body, env)?;
                    }
                    return Ok(last);
                }
                b"define" => return self.eval_define(args, env),
                b"set!" => {
                    let target = car(args);

                    if let Val::Sym(ts) = target {
                        let tname = unsafe { str_data(ts) };
                        let v = self.eval(car(cdr(args)), env)?;
                        let slot = env_find(env, tname).ok_or("set!: unbound variable")?;

                        unsafe { *slot = v };
                        return Ok(Val::Nil);
                    }
                    return Err("set!: not a symbol");
                }
                b"lambda" => {
                    let params = car(args);
                    let body = cdr(args);
                    let f = self.make_closure(params, body, env)?;

                    return Ok(Val::Func(f));
                }
                b"let" => return self.eval_let(args, env),
                b"let*" => return self.eval_let_star(args, env),
                b"letrec" => return self.eval_letrec(args, env),
                b"begin" => return self.eval_body(args, env),
                b"and" => {
                    let mut cur = args;
                    let mut last = Val::Bool(true);

                    while let Val::List(c) = cur {
                        last = self.eval(unsafe { (*c).car }, env)?;
                        if is_falsey(last) {
                            return Ok(last);
                        }
                        cur = unsafe { (*c).cdr };
                    }
                    return Ok(last);
                }
                b"or" => {
                    let mut cur = args;
                    let mut last = Val::Nil;

                    while let Val::List(c) = cur {
                        last = self.eval(unsafe { (*c).car }, env)?;
                        if !is_falsey(last) {
                            return Ok(last);
                        }
                        cur = unsafe { (*c).cdr };
                    }
                    return Ok(last);
                }
                _ => {}
            }
        }

        self.eval_call(head, args, env)
    }

    /// Evaluate the operator and the arguments of a normal call. The argument
    /// vector is taken from the reclaimable environment arena, so a call does
    /// not reserve a fixed array in every recursion level's stack frame.
    #[inline(never)]
    fn eval_call(&mut self, head: Val, args: Val, env: *mut Env) -> Res<Val> {
        let callee = match head {
            Val::Sym(s) => {
                let name = unsafe { str_data(s) };

                match env_find(env, name) {
                    Some(slot) => unsafe { *slot },
                    None => return Err("unbound variable"),
                }
            }
            _ => self.eval(head, env)?,
        };

        let argc = list_len(args);

        if argc > MAX_ARGS {
            return Err("too many arguments");
        }
        if argc == 0 {
            return self.apply(callee, core::ptr::null(), 0);
        }
        let mark = self.env_mark();
        let epoch = self.env_epoch;

        match self
            .env_arena
            .alloc(argc * core::mem::size_of::<Val>(), 8)
        {
            Some(buf) => {
                let slot = buf as *mut Val;
                let mut i = 0usize;
                let mut cur = args;

                while let Val::List(c) = cur {
                    let v = self.eval(unsafe { (*c).car }, env)?;

                    unsafe { *slot.add(i) = v };
                    i += 1;
                    cur = unsafe { (*c).cdr };
                }
                let r = self.apply(callee, slot, i);

                self.env_unwind(mark, epoch);
                r
            }
            None => Err("out of memory"),
        }
    }

    fn eval_define(&mut self, args: Val, env: *mut Env) -> Res<Val> {
        let target = car(args);

        match target {
            Val::Sym(s) => {
                let v = self.eval(car(cdr(args)), env)?;

                self.env_define(env, s, v)?;
                Ok(v)
            }
            Val::List(c) => {
                let name = unsafe { (*c).car };
                let params = unsafe { (*c).cdr };
                let body = cdr(args);
                let f = self.make_closure(params, body, env)?;

                if let Val::Sym(s) = name {
                    self.env_define(env, s, Val::Func(f))?;
                } else {
                    return Err("define: bad function name");
                }
                Ok(Val::Func(f))
            }
            _ => Err("define: bad form"),
        }
    }

    fn make_closure(&mut self, params: Val, body: Val, env: *mut Env) -> Res<*mut Func> {
        let mut names = [Val::Nil; MAX_BIND];
        let mut n = 0usize;
        let mut cur = params;

        while let Val::List(c) = cur {
            let p = unsafe { (*c).car };

            if !matches!(p, Val::Sym(_)) {
                return Err("closure: parameter is not a symbol");
            }
            if n >= MAX_BIND {
                return Err("too many parameters");
            }
            names[n] = p;
            n += 1;
            cur = unsafe { (*c).cdr };
        }

        let p = self
            .arena
            .alloc(core::mem::size_of::<Func>(), 8)
            .ok_or("out of memory")? as *mut Func;
        let arr = self
            .arena
            .alloc(n * core::mem::size_of::<Val>(), 8)
            .ok_or("out of memory")? as *mut Val;

        unsafe {
            core::ptr::copy_nonoverlapping(names.as_ptr(), arr, n);
            (*p).arity = n as u32;
            (*p).params = arr;
            (*p).body = body;
            (*p).env = env;
        }
        // A closure may outlive the frame that created it; stop that frame
        // (and every enclosing one) from rewinding its environment storage.
        self.env_epoch = self.env_epoch.wrapping_add(1);
        Ok(p)
    }

    /// `(let ((name init) ...) body...)`: every `init` is evaluated in the
    /// outer scope first, then one environment holds all bindings.
    fn eval_let(&mut self, args: Val, env: *mut Env) -> Res<Val> {
        let bindings = car(args);
        let body = cdr(args);
        let mut names = [Val::Nil; MAX_BIND];
        let mut inits = [Val::Nil; MAX_BIND];
        let mut n = 0usize;
        let mut cur = bindings;

        while let Val::List(c) = cur {
            let pair = unsafe { (*c).car };
            let name = car(pair);

            if !matches!(name, Val::Sym(_)) {
                return Err("let: binding name is not a symbol");
            }
            if n >= MAX_BIND {
                return Err("let: too many bindings");
            }
            names[n] = name;
            inits[n] = self.eval(car(cdr(pair)), env)?;
            n += 1;
            cur = unsafe { (*c).cdr };
        }

        let mark = self.env_mark();
        let epoch = self.env_epoch;
        let inner = self.new_env(env, n.max(1))?;

        for i in 0..n {
            if let Val::Sym(s) = names[i] {
                self.env_define(inner, s, inits[i])?;
            }
        }
        let r = self.eval_body(body, inner)?;

        self.env_unwind(mark, epoch);
        Ok(r)
    }

    /// `(let* ((name init) ...) body...)`: each binding sees the previous.
    fn eval_let_star(&mut self, args: Val, env: *mut Env) -> Res<Val> {
        let bindings = car(args);
        let body = cdr(args);
        let nb = list_len(bindings).min(MAX_BIND);
        let mark = self.env_mark();
        let epoch = self.env_epoch;
        let inner = self.new_env(env, nb.max(1))?;
        let mut cur = bindings;

        while let Val::List(c) = cur {
            let pair = unsafe { (*c).car };
            let name = car(pair);

            if let Val::Sym(s) = name {
                let v = self.eval(car(cdr(pair)), inner)?;

                self.env_define(inner, s, v)?;
            } else {
                return Err("let*: binding name is not a symbol");
            }
            cur = unsafe { (*c).cdr };
        }
        let r = self.eval_body(body, inner)?;

        self.env_unwind(mark, epoch);
        Ok(r)
    }

    /// `(letrec ((name init) ...) body...)`: names exist before their values.
    fn eval_letrec(&mut self, args: Val, env: *mut Env) -> Res<Val> {
        let bindings = car(args);
        let body = cdr(args);
        let nb = list_len(bindings).min(MAX_BIND);
        let mark = self.env_mark();
        let epoch = self.env_epoch;
        let inner = self.new_env(env, nb.max(1))?;
        let mut cur = bindings;

        while let Val::List(c) = cur {
            let name = car(unsafe { (*c).car });

            if let Val::Sym(s) = name {
                self.env_define(inner, s, Val::Nil)?;
            } else {
                return Err("letrec: binding name is not a symbol");
            }
            cur = unsafe { (*c).cdr };
        }
        cur = bindings;
        while let Val::List(c) = cur {
            let pair = unsafe { (*c).car };
            let name = car(pair);
            let v = self.eval(car(cdr(pair)), inner)?;

            if let Val::Sym(s) = name {
                self.env_define(inner, s, v)?;
            }
            cur = unsafe { (*c).cdr };
        }
        let r = self.eval_body(body, inner)?;

        self.env_unwind(mark, epoch);
        Ok(r)
    }

    #[inline(never)]
    fn apply(&mut self, callee: Val, args: *const Val, argc: usize) -> Res<Val> {
        match callee {
            Val::Native(id) => self.native(id, args, argc),
            Val::Func(f) => unsafe {
                let arity = (*f).arity as usize;

                if arity != argc {
                    return Err("arity mismatch");
                }
                let mark = self.env_mark();
                let epoch = self.env_epoch;
                let call_env = self.new_env((*f).env, arity.max(1))?;

                for i in 0..arity {
                    if let Val::Sym(s) = *(*f).params.add(i) {
                        self.env_define(call_env, s, *args.add(i))?;
                    }
                }
                let body = (*f).body;
                let r = self.eval_body(body, call_env)?;

                self.env_unwind(mark, epoch);
                Ok(r)
            },
            _ => Err("not a callable"),
        }
    }

    /* ---- primitives ---------------------------------------------------- */

    #[inline(never)]
    fn native(&mut self, id: Nat, args: *const Val, argc: usize) -> Res<Val> {
        let a: &[Val] = if argc == 0 {
            &[]
        } else {
            unsafe { core::slice::from_raw_parts(args, argc) }
        };

        match id {
            Nat::Add => {
                let mut s = 0.0;

                for v in a {
                    s += num(*v)?;
                }
                Ok(Val::Num(s))
            }
            Nat::Sub => {
                if a.is_empty() {
                    return Err("- expects numbers");
                }
                let first = num(a[0])?;

                if a.len() == 1 {
                    return Ok(Val::Num(-first));
                }
                let mut s = first;

                for v in &a[1..] {
                    s -= num(*v)?;
                }
                Ok(Val::Num(s))
            }
            Nat::Mul => {
                let mut s = 1.0;

                for v in a {
                    s *= num(*v)?;
                }
                Ok(Val::Num(s))
            }
            Nat::Div => {
                if a.is_empty() {
                    return Err("/ expects numbers");
                }
                let mut s = if a.len() == 1 { 1.0 } else { num(a[0])? };
                let from = if a.len() == 1 { 0 } else { 1 };

                for v in &a[from..] {
                    let d = num(*v)?;

                    if d == 0.0 {
                        return Err("division by zero");
                    }
                    s /= d;
                }
                Ok(Val::Num(s))
            }
            Nat::Mod => {
                if a.len() != 2 {
                    return Err("% expects two numbers");
                }
                let x = num(a[0])?;
                let y = num(a[1])?;

                if y == 0.0 {
                    return Err("modulo by zero");
                }
                Ok(Val::Num(x - trunc(x / y) * y))
            }
            Nat::NumEq | Nat::Lt | Nat::Gt | Nat::Le | Nat::Ge => {
                let mut i = 1;

                while i < a.len() {
                    let x = num(a[i - 1])?;
                    let y = num(a[i])?;
                    let ok = match id {
                        Nat::NumEq => x == y,
                        Nat::Lt => x < y,
                        Nat::Gt => x > y,
                        Nat::Le => x <= y,
                        _ => x >= y,
                    };

                    if !ok {
                        return Ok(Val::Bool(false));
                    }
                    i += 1;
                }
                Ok(Val::Bool(true))
            }
            Nat::Not => {
                if a.is_empty() {
                    return Err("not expects 1 arg");
                }
                Ok(Val::Bool(is_falsey(a[0])))
            }
            Nat::Equal => {
                if a.len() < 2 {
                    return Ok(Val::Bool(true));
                }
                for v in &a[1..] {
                    if !value_equal(a[0], *v, 32) {
                        return Ok(Val::Bool(false));
                    }
                }
                Ok(Val::Bool(true))
            }
            Nat::Eq => {
                if a.len() < 2 {
                    return Ok(Val::Bool(true));
                }
                for v in &a[1..] {
                    if !value_eq(a[0], *v) {
                        return Ok(Val::Bool(false));
                    }
                }
                Ok(Val::Bool(true))
            }
            Nat::Print | Nat::Println => {
                for (i, v) in a.iter().enumerate() {
                    if i > 0 {
                        self.out.write(b" ");
                    }
                    fmt_val(self.out, *v, false, 0)?;
                }
                if id == Nat::Println {
                    self.out.write(b"\n");
                }
                Ok(Val::Nil)
            }
            Nat::Draw => {
                for v in a {
                    fmt_val(self.out, *v, true, 0)?;
                }
                Ok(Val::Nil)
            }
            Nat::Cons => {
                if a.len() != 2 {
                    return Err("cons expects 2 args");
                }
                Ok(Val::List(self.alloc_cons(a[0], a[1])?))
            }
            Nat::Car => match a.first() {
                Some(Val::List(_)) => Ok(car(a[0])),
                _ => Err("car: not a pair"),
            },
            Nat::Cdr => match a.first() {
                Some(Val::List(_)) => Ok(cdr(a[0])),
                _ => Err("cdr: not a pair"),
            },
            Nat::List => self.alloc_list(a),
            Nat::Length => {
                if a.len() != 1 {
                    return Err("length expects 1 arg");
                }
                let n = match a[0] {
                    Val::Nil => 0,
                    Val::List(_) => list_len(a[0]),
                    Val::Vec(v) => unsafe { (*v).len as usize },
                    Val::Str(s) => unsafe { (*s).len as usize },
                    _ => return Err("length: bad type"),
                };
                Ok(Val::Num(n as f64))
            }
            Nat::Nth | Nat::Get => {
                if a.len() != 2 {
                    return Err("get expects (coll index)");
                }
                let idx = trunc(num(a[1])?);

                if idx < 0.0 {
                    return Ok(Val::Nil);
                }
                let idx = idx as usize;
                match a[0] {
                    Val::List(_) => {
                        let mut cur = a[0];

                        for _ in 0..idx {
                            cur = cdr(cur);
                        }
                        if matches!(cur, Val::List(_)) {
                            Ok(car(cur))
                        } else {
                            Ok(Val::Nil)
                        }
                    }
                    Val::Vec(v) => {
                        let items = unsafe { vec_items(v) };

                        Ok(items.get(idx).copied().unwrap_or(Val::Nil))
                    }
                    Val::Str(s) => {
                        let bytes = unsafe { str_data(s) };

                        if idx < bytes.len() {
                            Ok(Val::Str(self.alloc_str(&bytes[idx..idx + 1], false)?))
                        } else {
                            Ok(Val::Nil)
                        }
                    }
                    _ => Ok(Val::Nil),
                }
            }
            Nat::Set => {
                if a.len() != 3 {
                    return Err("set expects (vec index val)");
                }
                match a[0] {
                    Val::Vec(v) => {
                        let idx = trunc(num(a[1])?) as usize;
                        let len = unsafe { (*v).len as usize };

                        if idx >= len {
                            return Err("set: index out of range");
                        }
                        let p = unsafe {
                            (v as *mut u8).add(core::mem::size_of::<VecObj>()) as *mut Val
                        };

                        unsafe { *p.add(idx) = a[2] };
                        Ok(a[2])
                    }
                    _ => Err("set: not a vector"),
                }
            }
            Nat::VecNew => Ok(Val::Vec(self.alloc_vec(a)?)),
            Nat::Str => {
                let mut buf = Buf::new();

                for v in a {
                    fmt_val(&mut buf, *v, true, 0)?;
                }
                let n = buf.len;
                let bytes = unsafe { core::slice::from_raw_parts(buf.b.as_ptr(), n) };

                Ok(Val::Str(self.alloc_str(bytes, false)?))
            }
            Nat::Substring => {
                if a.is_empty() || a.len() > 3 {
                    return Err("substring expects (s [start [end]])");
                }
                let bytes = str_of(a[0])?;
                let len = bytes.len() as i64;
                let mut start = if a.len() >= 2 { trunc(num(a[1])?) as i64 } else { 0 };
                let mut end = if a.len() >= 3 { trunc(num(a[2])?) as i64 } else { len };

                if start < 0 {
                    start += len;
                }
                if end < 0 {
                    end += len;
                }
                let start = start.max(0).min(len) as usize;
                let end = end.max(start as i64).min(len) as usize;

                Ok(Val::Str(self.alloc_str(&bytes[start..end], false)?))
            }
            Nat::StringSearch => {
                if a.len() < 2 || a.len() > 3 {
                    return Err("string-search expects (haystack needle [start])");
                }
                let hay = str_of(a[0])?;
                let needle = str_of(a[1])?;
                let mut from = if a.len() == 3 { trunc(num(a[2])?) as i64 } else { 0 };

                if from < 0 {
                    from += hay.len() as i64;
                }
                let from = from.max(0) as usize;
                if from > hay.len() {
                    return Ok(Val::Num(-1.0));
                }
                if needle.is_empty() {
                    return Ok(Val::Num(from as f64));
                }
                let mut i = from;

                while i + needle.len() <= hay.len() {
                    if &hay[i..i + needle.len()] == needle {
                        return Ok(Val::Num(i as f64));
                    }
                    i += 1;
                }
                Ok(Val::Num(-1.0))
            }
            Nat::Last => {
                if a.len() != 1 {
                    return Err("last expects 1 arg");
                }
                match coll_len(a[0]) {
                    Some(0) | None => Err("last expects a non-empty list"),
                    Some(n) => Ok(coll_at(a[0], n - 1)),
                }
            }
            Nat::Reverse => {
                if a.len() != 1 {
                    return Err("reverse expects 1 arg");
                }
                let n = coll_len(a[0]).ok_or("reverse: bad type")?;
                let mut head = Val::Nil;

                for i in 0..n {
                    head = Val::List(self.alloc_cons(coll_at(a[0], i), head)?);
                }
                Ok(head)
            }
            Nat::Append => {
                if a.len() != 2 {
                    return Err("append expects 2 args");
                }
                let na = coll_len(a[0]).ok_or("append expects lists or vectors")?;
                let nb = coll_len(a[1]).ok_or("append expects lists or vectors")?;
                // Build the tail first (b in order), then prepend a's
                // elements, so no scratch array is needed.
                let mut head = Val::Nil;

                for i in (0..nb).rev() {
                    head = Val::List(self.alloc_cons(coll_at(a[1], i), head)?);
                }
                for i in (0..na).rev() {
                    head = Val::List(self.alloc_cons(coll_at(a[0], i), head)?);
                }
                Ok(head)
            }
            Nat::NullP => {
                let n = match a.first() {
                    Some(v) => coll_len(*v),
                    None => Some(0),
                };

                Ok(Val::Bool(matches!(n, Some(0))))
            }
            Nat::NumberP => Ok(Val::Bool(matches!(a.first(), Some(Val::Num(_))))),
            Nat::StringP => Ok(Val::Bool(matches!(a.first(), Some(Val::Str(_))))),
            Nat::ListP => Ok(Val::Bool(matches!(
                a.first(),
                Some(Val::List(_)) | Some(Val::Nil)
            ))),
            Nat::VectorP => Ok(Val::Bool(matches!(a.first(), Some(Val::Vec(_))))),
            Nat::FunctionP => Ok(Val::Bool(matches!(
                a.first(),
                Some(Val::Func(_)) | Some(Val::Native(_))
            ))),
            Nat::BoolP => Ok(Val::Bool(matches!(a.first(), Some(Val::Bool(_))))),
            Nat::MemberP => {
                if a.len() != 2 {
                    return Err("member? expects (x coll)");
                }
                let n = coll_len(a[1]).ok_or("member? expects a list or vector")?;

                for i in 0..n {
                    if value_equal(coll_at(a[1], i), a[0], 8) {
                        return Ok(Val::Bool(true));
                    }
                }
                Ok(Val::Bool(false))
            }
            Nat::Map => {
                if a.len() != 2 {
                    return Err("map expects (fn coll)");
                }
                let f = a[0];
                let n = coll_len(a[1]).ok_or("map expects a list or vector")?;
                let mut head: *mut Cons = core::ptr::null_mut();
                let mut tail: *mut Cons = core::ptr::null_mut();

                for i in 0..n {
                    let item = [coll_at(a[1], i)];
                    let v = self.apply(f, item.as_ptr(), 1)?;
                    let cell = self.alloc_cons(v, Val::Nil)?;

                    if head.is_null() {
                        head = cell;
                    } else {
                        unsafe { (*tail).cdr = Val::List(cell) };
                    }
                    tail = cell;
                }
                if head.is_null() {
                    Ok(Val::Nil)
                } else {
                    Ok(Val::List(head))
                }
            }
            Nat::Filter => {
                if a.len() != 2 {
                    return Err("filter expects (pred coll)");
                }
                let f = a[0];
                let n = coll_len(a[1]).ok_or("filter expects a list or vector")?;
                let mut head: *mut Cons = core::ptr::null_mut();
                let mut tail: *mut Cons = core::ptr::null_mut();

                for i in 0..n {
                    let item = coll_at(a[1], i);
                    let argv = [item];

                    if !is_falsey(self.apply(f, argv.as_ptr(), 1)?) {
                        let cell = self.alloc_cons(item, Val::Nil)?;

                        if head.is_null() {
                            head = cell;
                        } else {
                            unsafe { (*tail).cdr = Val::List(cell) };
                        }
                        tail = cell;
                    }
                }
                if head.is_null() {
                    Ok(Val::Nil)
                } else {
                    Ok(Val::List(head))
                }
            }
            Nat::Reduce => {
                if a.len() < 2 || a.len() > 3 {
                    return Err("reduce expects (fn coll) or (fn init coll)");
                }
                let f = a[0];
                // With three arguments the reference accepts either order and
                // tells them apart by which one is a collection, so scripts
                // written as `(reduce + 100 coll)` and `(reduce + coll 100)`
                // both work. Keep that tolerance.
                let (coll, mut acc) = if a.len() == 2 {
                    (a[1], None)
                } else {
                    let first_coll = coll_len(a[1]).is_some();
                    let second_coll = coll_len(a[2]).is_some();

                    if first_coll && !second_coll {
                        (a[1], Some(a[2]))
                    } else if !first_coll && second_coll {
                        (a[2], Some(a[1]))
                    } else {
                        return Err("reduce: one arg must be a list or vector");
                    }
                };
                let n = coll_len(coll).ok_or("reduce expects a list or vector")?;

                for i in 0..n {
                    let item = coll_at(coll, i);

                    acc = Some(match acc {
                        Some(x) => {
                            let argv = [x, item];

                            self.apply(f, argv.as_ptr(), 2)?
                        }
                        None => item,
                    });
                }
                Ok(acc.unwrap_or(Val::Nil))
            }
            Nat::Range => {
                if a.is_empty() || a.len() > 3 {
                    return Err("range expects (n [end [step]])");
                }
                let (mut i, end, step) = if a.len() == 1 {
                    (0.0, trunc(num(a[0])?), 1.0)
                } else {
                    let s = if a.len() == 3 { num(a[2])? } else { 1.0 };

                    (num(a[0])?, num(a[1])?, s)
                };

                if step == 0.0 {
                    return Err("range: zero step");
                }
                let mut head: *mut Cons = core::ptr::null_mut();
                let mut tail: *mut Cons = core::ptr::null_mut();

                // The arena bounds the result; an enormous range fails with
                // "out of memory" instead of a synthetic element cap.
                while (step > 0.0 && i < end) || (step < 0.0 && i > end) {
                    let cell = self.alloc_cons(Val::Num(i), Val::Nil)?;

                    if head.is_null() {
                        head = cell;
                    } else {
                        unsafe { (*tail).cdr = Val::List(cell) };
                    }
                    tail = cell;
                    i += step;
                }
                if head.is_null() {
                    Ok(Val::Nil)
                } else {
                    Ok(Val::List(head))
                }
            }
            Nat::Assoc => {
                if a.len() < 2 || a.len() > 3 {
                    return Err("assoc expects (list key [val])");
                }
                // Two arguments look the pair up; three return a new alist
                // with the first matching pair replaced (or the pair added at
                // the end), preserving every other entry's order.
                if a.len() == 2 {
                    let mut cur = a[0];

                    while let Val::List(c) = cur {
                        let pair = unsafe { (*c).car };

                        if value_eq(car(pair), a[1]) {
                            return Ok(pair);
                        }
                        cur = unsafe { (*c).cdr };
                    }
                    return Ok(Val::Nil);
                }

                let mut head: *mut Cons = core::ptr::null_mut();
                let mut tail: *mut Cons = core::ptr::null_mut();
                let mut replaced = false;
                let mut cur = a[0];

                while let Val::List(c) = cur {
                    let pair = unsafe { (*c).car };
                    let entry = if !replaced && value_eq(car(pair), a[1]) {
                        replaced = true;
                        self.alloc_list(&[a[1], a[2]])?
                    } else {
                        pair
                    };
                    let cell = self.alloc_cons(entry, Val::Nil)?;

                    if head.is_null() {
                        head = cell;
                    } else {
                        unsafe { (*tail).cdr = Val::List(cell) };
                    }
                    tail = cell;
                    cur = unsafe { (*c).cdr };
                }
                if !replaced {
                    let pair = self.alloc_list(&[a[1], a[2]])?;
                    let cell = self.alloc_cons(pair, Val::Nil)?;

                    if head.is_null() {
                        head = cell;
                    } else {
                        unsafe { (*tail).cdr = Val::List(cell) };
                    }
                }
                if head.is_null() {
                    Ok(Val::Nil)
                } else {
                    Ok(Val::List(head))
                }
            }
            Nat::Abs => Ok(Val::Num(abs(num(first(a)?)?))),
            Nat::Floor => Ok(Val::Num(floor(num(first(a)?)?))),
            Nat::Ceil => Ok(Val::Num(ceil(num(first(a)?)?))),
            Nat::Round => Ok(Val::Num(round(num(first(a)?)?))),
            Nat::Sqrt => Ok(Val::Num(sqrt(num(first(a)?)?))),
            Nat::Pow => {
                if a.len() != 2 {
                    return Err("pow expects 2 args");
                }
                Ok(Val::Num(pow(num(a[0])?, num(a[1])?)))
            }
            Nat::Min | Nat::Max => {
                if a.is_empty() {
                    return Err("min/max expects numbers");
                }
                let mut best = num(a[0])?;

                for v in &a[1..] {
                    let x = num(*v)?;

                    if (id == Nat::Min && x < best) || (id == Nat::Max && x > best) {
                        best = x;
                    }
                }
                Ok(Val::Num(best))
            }
            Nat::Exists | Nat::IsDir | Nat::FileSize => {
                let path = str_of(first(a)?)?;
                let mut abs = [0u8; PATH_MAX];
                let n = self.shell.abs_path(path, &mut abs);

                if n == 0 {
                    return Ok(match id {
                        Nat::FileSize => Val::Num(-1.0),
                        _ => Val::Bool(false),
                    });
                }
                let st = fs::stat(to_str(&abs[..n]));

                match id {
                    Nat::Exists => Ok(Val::Bool(st.is_some())),
                    Nat::IsDir => Ok(Val::Bool(
                        matches!(st, Some(s) if s.mode == fs::MODE_DIR),
                    )),
                    _ => Ok(Val::Num(match st {
                        Some(s) => s.size as f64,
                        None => -1.0,
                    })),
                }
            }
            Nat::ReadFile => {
                let path = str_of(first(a)?)?;
                let mut buf = [0u8; FMT_MAX];

                if !self.exists(path) {
                    return Ok(Val::Nil);
                }
                let n = self.shell.read_file(path, &mut buf);

                Ok(Val::Str(self.alloc_str(&buf[..n], false)?))
            }
            Nat::WriteFile | Nat::AppendFile => {
                if a.len() != 2 {
                    return Err("write-file expects (path text)");
                }
                let path = str_of(a[0])?;
                let text = str_of(a[1])?;

                Ok(Val::Bool(self.shell.write_file(
                    path,
                    text,
                    id == Nat::AppendFile,
                )))
            }
            Nat::Ls => {
                let path = if a.is_empty() { &b"."[..] } else { str_of(a[0])? };
                let mut abs = [0u8; PATH_MAX];
                let n = self.shell.abs_path(path, &mut abs);

                if n == 0 {
                    return Ok(Val::Nil);
                }
                let mut items = [Val::Nil; MAX_VEC];
                let mut count = 0usize;

                if let Some(fd) = fs::opendir(to_str(&abs[..n])) {
                    while let Some(Some(e)) = fs::readdir(fd) {
                        if count >= MAX_VEC {
                            break;
                        }
                        let name = e.name();

                        if name == "." || name == ".." {
                            continue;
                        }
                        items[count] = Val::Str(self.alloc_str(name.as_bytes(), false)?);
                        count += 1;
                    }
                } else {
                    return Ok(Val::Nil);
                }
                self.alloc_list(&items[..count])
            }
            Nat::Mkdir | Nat::Rmdir | Nat::Delete => {
                let path = str_of(first(a)?)?;
                let mut abs = [0u8; PATH_MAX];
                let n = self.shell.abs_path(path, &mut abs);

                if n == 0 {
                    return Ok(Val::Bool(false));
                }
                let p = to_str(&abs[..n]);
                let ok = match id {
                    Nat::Mkdir => fs::mkdir(p),
                    _ => fs::unlink(p),
                };

                Ok(Val::Bool(ok))
            }
            Nat::Mv => {
                if a.len() != 2 {
                    return Err("mv expects (from to)");
                }
                let mut abs_a = [0u8; PATH_MAX];
                let mut abs_b = [0u8; PATH_MAX];
                let na = self.shell.abs_path(str_of(a[0])?, &mut abs_a);
                let nb = self.shell.abs_path(str_of(a[1])?, &mut abs_b);

                if na == 0 || nb == 0 {
                    return Ok(Val::Bool(false));
                }
                Ok(Val::Bool(fs::rename(
                    to_str(&abs_a[..na]),
                    to_str(&abs_b[..nb]),
                )))
            }
            Nat::Cp => {
                if a.len() != 2 {
                    return Err("cp expects (from to)");
                }
                let mut buf = [0u8; FMT_MAX];
                let n = self.shell.read_file(str_of(a[0])?, &mut buf);

                Ok(Val::Bool(self.shell.write_file(
                    str_of(a[1])?,
                    &buf[..n],
                    false,
                )))
            }
            Nat::Cls => {
                self.out.clear_screen();
                Ok(Val::Nil)
            }
            Nat::Home | Nat::ShowCursor | Nat::Key => Ok(Val::Nil),
            Nat::Sleep => {
                let ms = if a.is_empty() { 0.0 } else { num(a[0])? };
                let spins = (ms as u64 / 5).min(200);

                for _ in 0..spins {
                    sys::yield_now();
                }
                Ok(Val::Nil)
            }
            Nat::Random => {
                let n = trunc(num(first(a)?)?) as u64;

                if n == 0 {
                    return Ok(Val::Num(0.0));
                }
                Ok(Val::Num((next_random() % n) as f64))
            }
        }
    }

    fn exists(&self, path: &[u8]) -> bool {
        let mut abs = [0u8; PATH_MAX];
        let n = self.shell.abs_path(path, &mut abs);

        n > 0 && fs::stat(to_str(&abs[..n])).is_some()
    }

}

/* ------------------------------------------------------------------------- */
/* Environment / helpers                                                     */
/* ------------------------------------------------------------------------- */

/// Allocate one environment (header + key/value arrays) from `arena`.
fn env_alloc(arena: &mut Arena, parent: *mut Env, cap: usize) -> Option<*mut Env> {
    let vs = core::mem::size_of::<Val>();
    let p = arena.alloc(core::mem::size_of::<Env>(), 8)? as *mut Env;
    let keys = arena.alloc(cap * vs, 8)? as *mut Val;
    let vals = arena.alloc(cap * vs, 8)? as *mut Val;

    unsafe {
        (*p).parent = parent;
        (*p).n = 0;
        (*p).cap = cap as u32;
        (*p).keys = keys;
        (*p).vals = vals;
    }
    Some(p)
}

fn env_find(env: *mut Env, name: &[u8]) -> Option<*mut Val> {
    let mut e = env;

    while !e.is_null() {
        unsafe {
            let n = (*e).n as usize;

            for i in 0..n {
                if let Val::Sym(s) = *(*e).keys.add(i) {
                    if str_data(s) == name {
                        return Some((*e).vals.add(i));
                    }
                }
            }
            e = (*e).parent;
        }
    }
    None
}

fn first(a: &[Val]) -> Res<Val> {
    a.first().copied().ok_or("missing argument")
}

fn num(v: Val) -> Res<f64> {
    match v {
        Val::Num(n) => Ok(n),
        _ => Err("expects a number"),
    }
}

fn str_of(v: Val) -> Res<&'static [u8]> {
    match v {
        Val::Str(s) => Ok(unsafe { str_data(s) }),
        _ => Err("expects a string"),
    }
}

fn to_str(b: &[u8]) -> &str {
    core::str::from_utf8(b).unwrap_or("<?>")
}

fn value_eq(x: Val, y: Val) -> bool {
    match (x, y) {
        (Val::Nil, Val::Nil) => true,
        (Val::Bool(a), Val::Bool(b)) => a == b,
        (Val::Num(a), Val::Num(b)) => a == b,
        (Val::Sym(a), Val::Sym(b)) => unsafe { str_data(a) == str_data(b) },
        (Val::Str(a), Val::Str(b)) => unsafe { str_data(a) == str_data(b) },
        (Val::Native(a), Val::Native(b)) => a == b,
        (Val::List(a), Val::List(b)) => a == b,
        (Val::Vec(a), Val::Vec(b)) => a == b,
        (Val::Func(a), Val::Func(b)) => a == b,
        _ => false,
    }
}

fn value_equal(x: Val, y: Val, depth: usize) -> bool {
    if depth == 0 {
        return false;
    }
    match (x, y) {
        (Val::List(a), Val::List(b)) => {
            let mut ca = Val::List(a);
            let mut cb = Val::List(b);

            while let (Val::List(pa), Val::List(pb)) = (ca, cb) {
                if !value_equal(unsafe { (*pa).car }, unsafe { (*pb).car }, depth - 1) {
                    return false;
                }
                ca = unsafe { (*pa).cdr };
                cb = unsafe { (*pb).cdr };
            }
            is_nil(ca) && is_nil(cb)
        }
        (Val::Vec(a), Val::Vec(b)) => {
            let ia = unsafe { vec_items(a) };
            let ib = unsafe { vec_items(b) };

            if ia.len() != ib.len() {
                return false;
            }
            for i in 0..ia.len() {
                if !value_equal(ia[i], ib[i], depth - 1) {
                    return false;
                }
            }
            true
        }
        _ => value_eq(x, y),
    }
}

/* ------------------------------------------------------------------------- */
/* Reader helpers                                                            */
/* ------------------------------------------------------------------------- */

fn is_delimiter(c: u8) -> bool {
    matches!(
        c,
        b'(' | b')' | b'[' | b']' | b'\'' | b';' | b' ' | b'\t' | b'\n' | b'\r'
    )
}

fn skip_ws(src: &[u8], pos: &mut usize) {
    while *pos < src.len() {
        let c = src[*pos];

        if c == b' ' || c == b'\t' || c == b'\n' || c == b'\r' {
            *pos += 1;
        } else if c == b';' {
            while *pos < src.len() && src[*pos] != b'\n' {
                *pos += 1;
            }
        } else {
            break;
        }
    }
}

fn read_string(src: &[u8], pos: &mut usize, close: &[u8], escape: bool, out: &mut [u8]) -> Res<usize> {
    let mut n = 0usize;

    loop {
        if *pos >= src.len() {
            return Err("unterminated string");
        }
        if *pos + close.len() <= src.len() && &src[*pos..*pos + close.len()] == close {
            *pos += close.len();
            break;
        }
        if n >= out.len() {
            return Err("string too long");
        }
        let c = src[*pos];

        if escape && c == b'\\' && *pos + 1 < src.len() {
            let e = src[*pos + 1];

            out[n] = match e {
                b'n' => b'\n',
                b't' => b'\t',
                other => other,
            };
            n += 1;
            *pos += 2;
            continue;
        }
        out[n] = c;
        n += 1;
        *pos += 1;
    }
    Ok(n)
}

fn parse_num(b: &[u8]) -> Option<f64> {
    if b.is_empty() {
        return None;
    }
    let mut i = 0usize;
    let mut sign = 1.0;
    let mut any = false;
    let mut v = 0.0f64;

    if b[i] == b'-' {
        sign = -1.0;
        i = 1;
    } else if b[i] == b'+' {
        i = 1;
    }
    while i < b.len() && b[i].is_ascii_digit() {
        v = v * 10.0 + (b[i] - b'0') as f64;
        i += 1;
        any = true;
    }
    if i < b.len() && b[i] == b'.' {
        i += 1;
        let mut scale = 0.1;

        while i < b.len() && b[i].is_ascii_digit() {
            v += (b[i] - b'0') as f64 * scale;
            scale *= 0.1;
            i += 1;
            any = true;
        }
    }
    if !any {
        return None;
    }
    if i < b.len() && (b[i] == b'e' || b[i] == b'E') {
        i += 1;
        let mut esign = 1.0;
        let mut eany = false;
        let mut e = 0.0f64;

        if i < b.len() && (b[i] == b'-' || b[i] == b'+') {
            if b[i] == b'-' {
                esign = -1.0;
            }
            i += 1;
        }
        while i < b.len() && b[i].is_ascii_digit() {
            e = e * 10.0 + (b[i] - b'0') as f64;
            i += 1;
            eany = true;
        }
        if !eany {
            return None;
        }
        let mut k = (e * esign) as i32;

        while k > 0 && v < 1e300 {
            v *= 10.0;
            k -= 1;
        }
        while k < 0 && v > 1e-300 {
            v /= 10.0;
            k += 1;
        }
    }
    if i != b.len() {
        return None;
    }
    Some(sign * v)
}

/* ------------------------------------------------------------------------- */
/* Math without libm                                                         */
/* ------------------------------------------------------------------------- */

fn trunc(v: f64) -> f64 {
    if v >= 0.0 {
        (v as i64) as f64
    } else {
        -((-v as i64) as f64)
    }
}

fn abs(v: f64) -> f64 {
    if v < 0.0 {
        -v
    } else {
        v
    }
}

fn floor(v: f64) -> f64 {
    let t = trunc(v);

    if t > v {
        t - 1.0
    } else {
        t
    }
}

fn ceil(v: f64) -> f64 {
    let t = trunc(v);

    if t < v {
        t + 1.0
    } else {
        t
    }
}

fn round(v: f64) -> f64 {
    if v >= 0.0 {
        trunc(v + 0.5)
    } else {
        trunc(v - 0.5)
    }
}

fn sqrt(v: f64) -> f64 {
    if v < 0.0 {
        return 0.0 / 0.0;
    }
    if v == 0.0 {
        return 0.0;
    }
    let mut x = if v > 1.0 { v } else { 1.0 };

    for _ in 0..60 {
        x = 0.5 * (x + v / x);
    }
    x
}

fn pow(base: f64, e: f64) -> f64 {
    let n = trunc(e);

    if n != e || abs(e) > 4096.0 {
        return 0.0 / 0.0;
    }
    let mut k = abs(n) as u32;
    let mut acc = 1.0;
    let mut b = base;

    while k > 0 {
        if k & 1 != 0 {
            acc *= b;
        }
        b *= b;
        k >>= 1;
    }
    if n < 0.0 {
        1.0 / acc
    } else {
        acc
    }
}

static mut RNG: u64 = 0x2545_F491_4F6C_DD1D;

fn next_random() -> u64 {
    unsafe {
        RNG = RNG
            .wrapping_mul(6364136223846793005)
            .wrapping_add(1442695040888963407);
        RNG >> 16
    }
}

/* ------------------------------------------------------------------------- */
/* Value formatting                                                          */
/* ------------------------------------------------------------------------- */

struct Buf {
    b: [u8; FMT_MAX],
    len: usize,
}

impl Buf {
    fn new() -> Buf {
        Buf {
            b: [0u8; FMT_MAX],
            len: 0,
        }
    }
}

impl Sink for Buf {
    fn write(&mut self, data: &[u8]) {
        let n = (self.len + data.len()).min(FMT_MAX);

        self.b[self.len..n].copy_from_slice(&data[..n - self.len]);
        self.len = n;
    }
}

fn fmt_num(out: &mut dyn Sink, v: f64) {
    if v != v {
        out.write(b"nan");
        return;
    }
    if v == f64::INFINITY {
        out.write(b"inf");
        return;
    }
    if v == f64::NEG_INFINITY {
        out.write(b"-inf");
        return;
    }
    let mut buf = [0u8; 32];

    if v < 0.0 {
        out.write(b"-");
    }
    let a = abs(v);
    let ip = a as u64;
    let frac = a - ip as f64;
    let mut n = 0usize;

    if ip == 0 {
        buf[0] = b'0';
        n = 1;
    } else {
        let mut tmp = [0u8; 20];
        let mut m = 0usize;
        let mut x = ip;

        while x > 0 && m < 20 {
            tmp[m] = b'0' + (x % 10) as u8;
            x /= 10;
            m += 1;
        }
        while m > 0 {
            m -= 1;
            buf[n] = tmp[m];
            n += 1;
        }
    }
    if frac > 1e-9 && a < 1e15 {
        // Six decimals, rounded, trailing zeros trimmed.
        let mut digits = [0u8; 6];
        let mut r = frac;

        for d in digits.iter_mut() {
            r *= 10.0;
            let dg = r as u8;

            *d = dg;
            r -= dg as f64;
        }
        if r >= 0.5 {
            let mut i = 5;

            loop {
                if digits[i] < 9 {
                    digits[i] += 1;
                    break;
                }
                digits[i] = 0;
                if i == 0 {
                    break;
                }
                i -= 1;
            }
        }
        let mut last = 6;

        while last > 0 && digits[last - 1] == 0 {
            last -= 1;
        }
        if last > 0 {
            buf[n] = b'.';
            n += 1;
            for &d in digits[..last].iter() {
                buf[n] = b'0' + d;
                n += 1;
            }
        }
    }
    out.write(&buf[..n]);
}

fn fmt_val(out: &mut dyn Sink, v: Val, raw: bool, depth: usize) -> Res<()> {
    if depth > 16 {
        out.write(b"...");
        return Ok(());
    }
    match v {
        Val::Nil => out.write(b"nil"),
        Val::Bool(true) => out.write(b"true"),
        Val::Bool(false) => out.write(b"false"),
        Val::Num(n) => fmt_num(out, n),
        Val::Sym(s) | Val::Str(s) => {
            let bytes = unsafe { str_data(s) };

            if raw {
                out.write(bytes);
            } else {
                out.write(b"\"");
                out.write(bytes);
                out.write(b"\"");
            }
        }
        Val::List(_) => {
            out.write(b"(");
            let mut cur = v;
            let mut first = true;

            while let Val::List(c) = cur {
                if !first {
                    out.write(b" ");
                }
                fmt_val(out, unsafe { (*c).car }, false, depth + 1)?;
                first = false;
                cur = unsafe { (*c).cdr };
            }
            if !is_nil(cur) {
                out.write(b" . ");
                fmt_val(out, cur, false, depth + 1)?;
            }
            out.write(b")");
        }
        Val::Vec(p) => {
            let items = unsafe { vec_items(p) };

            out.write(b"[");
            for (i, item) in items.iter().enumerate() {
                if i > 0 {
                    out.write(b" ");
                }
                fmt_val(out, *item, false, depth + 1)?;
            }
            out.write(b"]");
        }
        Val::Func(_) => out.write(b"<fn lambda>"),
        Val::Native(n) => {
            out.write(b"<native ");
            out.write(native_name(n).as_bytes());
            out.write(b">");
        }
    }
    Ok(())
}

fn native_name(n: Nat) -> &'static str {
    for (name, id) in NATIVES.iter() {
        if *id == n {
            return core::str::from_utf8(name).unwrap_or("?");
        }
    }
    "?"
}

/* ------------------------------------------------------------------------- */
/* Shell command                                                             */
/* ------------------------------------------------------------------------- */

/// `nuomi <script.nuomi>` or `nuomi -e <expression>`.
pub fn command(shell: &Shell, cmd: &Cmd<'_>, out: &mut dyn Sink) -> i32 {
    if cmd.argc < 2 {
        out.write(b"Nuomi 0.3.0 - a Lisp-flavoured scripting language\n");
        out.write(b"usage: nuomi <script.nuomi>\n       nuomi -e <expression>\n");
        return 0;
    }
    let mut source = [0u8; SOURCE_MAX];
    let src_len;

    if cmd.argv[1] == &b"-e"[..] || cmd.argv[1] == &b"--eval"[..] {
        if cmd.argc < 3 {
            out.write(b"nuomi: -e needs an expression\n");
            return 1;
        }
        let mut n = 0usize;

        for a in cmd.argv.iter().take(cmd.argc).skip(2) {
            if n > 0 && n < SOURCE_MAX {
                source[n] = b' ';
                n += 1;
            }
            for &b in a.iter() {
                if n < SOURCE_MAX {
                    source[n] = b;
                    n += 1;
                }
            }
        }
        src_len = n;
    } else if cmd.argv[1] == &b"-c"[..] || cmd.argv[1] == &b"--compile"[..] {
        out.write(b"nuomi: -c needs the host toolchain (user/Nuomi_pl), not nsh\n");
        return 1;
    } else {
        let mut abs = [0u8; PATH_MAX];
        let n = shell.abs_path(cmd.argv[1], &mut abs);

        if n == 0 {
            out.write(b"nuomi: path too long\n");
            return 1;
        }
        match fs::stat(to_str(&abs[..n])) {
            Some(st) if st.size as usize > SOURCE_MAX => {
                out.write(b"nuomi: script too large (limit 6 KiB)\n");
                return 1;
            }
            None => {
                out.write(b"nuomi: cannot open ");
                out.write(cmd.argv[1]);
                out.write(b"\n");
                return 1;
            }
            _ => {}
        }
        let r = shell.read_file(cmd.argv[1], &mut source);

        if r == 0 {
            out.write(b"nuomi: cannot read ");
            out.write(cmd.argv[1]);
            out.write(b"\n");
            return 1;
        }
        src_len = r;
    }

    let mut arena = [0u8; ARENA_BYTES];

    match run(shell, out, &mut arena, &source[..src_len]) {
        Ok(()) => 0,
        Err(e) => fail(out, e),
    }
}

fn run(shell: &Shell, out: &mut dyn Sink, arena: &mut [u8], src: &[u8]) -> Res<()> {
    let mut it = Interp::new(arena, out, shell)?;
    let global = it.globals;
    let mut pos = 0usize;

    // Read and evaluate one form at a time: a script may hold any number of
    // top-level forms, and only the form being evaluated has to be alive.
    loop {
        skip_ws(src, &mut pos);
        if pos >= src.len() {
            return Ok(());
        }
        let form = it.read_form(src, &mut pos, 0)?;

        it.eval(form, global)?;
    }
}

fn fail(out: &mut dyn Sink, msg: &str) -> i32 {
    out.write(b"nuomi: ");
    out.write(msg.as_bytes());
    out.write(b"\n");
    1
}
