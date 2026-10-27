/* ==========================================================================
 * 模板：给 nsh 加一条内建命令
 *
 * 三个函数粘到 apps/src/shell/cmds.rs 里（放在其它 cmd_* 之后、
 * `const BUILTINS` 之前），再把 `("hello", cmd_hello),` 这样的表项加到
 * BUILTINS 末尾。除此之外不用改任何文件：
 *   - `help` 遍历 BUILTINS 自动列出新命令；
 *   - `which` 通过 is_builtin() 认出新命令；
 *   - 管道、重定向 > >> <、命令替换 $(...) 都由 run() 统一处理，
 *     命令本身只需读 cmd.argv 和 stdin、写 out。
 *
 * 函数签名必须和 Bfn 一致：
 *   fn(&mut Shell, &Cmd, &[u8], &mut dyn Sink) -> i32
 * 返回值就是 $?，也就是 shell.last。
 * ========================================================================== */

/// `hello [名字]`：参数、退出码。
fn cmd_hello(_shell: &mut Shell, cmd: &Cmd, stdin: &[u8], out: &mut dyn Sink) -> i32 {
    if cmd.argc > 2 {
        puts(out, "usage: hello [name]\n");
        return 2; /* 2 = 用法错误，和 git 等工具的习惯一致 */
    }
    puts(out, "hello");
    if cmd.argc == 2 {
        putc(out, b' ');
        putb(out, cmd.argv[1]);
    }
    puts(out, "!\n");
    if !stdin.is_empty() {
        puts(out, "stdin: ");
        putb(out, stdin);
    }
    0
}

/// `where`：读取 shell 自身状态（cwd、sysinfo 端口是否就绪）。
fn cmd_where(shell: &mut Shell, _cmd: &Cmd, _stdin: &[u8], out: &mut dyn Sink) -> i32 {
    puts(out, "cwd: ");
    putb(out, &shell.cwd[..shell.cwd_len]);
    putc(out, b'\n');
    if shell.sys_port != 0 {
        puts(out, "sysinfo: ok\n");
    } else {
        puts(out, "sysinfo: unavailable\n");
    }
    0
}

/// `count`：统计标准输入的行数和字节数，用来演示管道。
///   ls | count
///   cat /etc/hostname | count
fn cmd_count(_shell: &mut Shell, _cmd: &Cmd, stdin: &[u8], out: &mut dyn Sink) -> i32 {
    let mut lines = 0u64;
    for &b in stdin.iter() {
        if b == b'\n' {
            lines += 1;
        }
    }
    put_dec(out, lines);
    puts(out, " lines, ");
    put_dec(out, stdin.len() as u64);
    puts(out, " bytes\n");
    0
}

/* --------------------------------------------------------------------------
 * 注册：追加到 cmds.rs 里的 `const BUILTINS` 末尾。
 * 名字是用户在提示符下敲的字符串，大小写敏感，不要带空格。
 * --------------------------------------------------------------------------
 */
const BUILTINS: &[(&str, Bfn)] = &[
    /* ... 已有的表项保持不动 ... */
    ("hello", cmd_hello),
    ("where", cmd_where),
    ("count", cmd_count),
];
