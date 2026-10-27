# shell-command：给 nsh 加内建命令

`hello_cmd.rs` 是**片段**（不会被主仓构建），里面有三个可直接抄的例子：

| 命令 | 演示 |
|------|------|
| `hello [名字]` | 读 `cmd.argv`、返回退出码 |
| `where` | 读 `Shell` 状态（`cwd`、`sys_port`） |
| `count` | 读管道喂进来的 `stdin`（`ls \| count`） |

## 接入

1. 把函数粘进 `apps/src/shell/cmds.rs`（其它 `cmd_*` 之后、`const BUILTINS` 之前）。
2. 在文件末尾的 `BUILTINS` 表追加 `("hello", cmd_hello),`。
3. `make`。

`help` 会列出新命令，`which` 认得它，管道 / 重定向 / `$(...)` / `; && ||`
都自动可用。

## 签名与约束

```rust
fn cmd_xxx(shell: &mut Shell, cmd: &Cmd, stdin: &[u8], out: &mut dyn Sink) -> i32
```

- 参数：`cmd.argv[0..cmd.argc]`，已展开变量 / glob / 引号，上限 40 个。
- 输入：`stdin` 是完整缓冲（≤ 4 KiB），非流式。
- 输出：只写 `out`，不要 `println!`——同一份命令要能进管道和 `$(...)`。
- 返回：`$?`；`0` 成功、`127` 未找到、其它自定义。

## 试一下

```bash
user@novus:/$ hello world
hello world!
user@novus:/$ ls | count
7 lines, 61 bytes
user@novus:/$ where
cwd: /
sysinfo: ok
```
