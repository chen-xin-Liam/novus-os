# Novus OS 开发模板

这个目录放最小可用的骨架，**复制一份就能开始写自己的东西**，不用从零读内核。
按「你要做什么」选一个：

| 目标 | 模板 | 需要注册 | 工具链 |
|------|------|----------|--------|
| 桌面应用（窗口、控件、鼠标键盘） | `gui-app/` | 4 处（见 §1） | Rust + `x86_64-unknown-none` |
| 给 `nsh` 加一条命令 | `shell-command/` | 1 个文件（见 §2） | Rust |
| 写个脚本小工具 | `nuomi/` | 不用注册 | 无，`nsh` 自带解释器 |

三个模板都**在真实构建链上验证过**：GUI 模板单独编译过（入口 0x400000），也临时
接进 `apps/` + `Makefile` + `kernel/apps.c` 完整 `make` 过（blob 30,700 字节，
`NOVUS.EFI` 链接通过）；命令模板临时粘进 `cmds.rs` 编译过；两个 `.nuomi` 脚本用
同一份 `apps/src/nuomi.rs` 在 host 上跑通。

---

## 0. 先决条件

```bash
make                                       # 需要 x86_64-w64-mingw32-gcc 与 cargo
rustup target add x86_64-unknown-none      # 用户态应用的编译目标
```

`apps/` 是 `no_std`、**没有分配器**的用户态：缓冲自己开数组，不要用
`Vec`/`String`/`Box`——一旦分配就 panic。栈按 4 MiB 映射（`NUC_STACK_MAX`），
够放数组和递归。

调试输出走 COM1：

```rust
sys::write(b"[APP] 到这一步了\n");
```

`panic` 只打印 `[APP] panic` 并 `exit(3)`，没有回溯——定位问题靠分段落点。

---

## 1. 桌面应用（`gui-app/`）

一个窗口 + 页面标题 + 卡片 + 按钮 + 点击计数 + Esc 关闭 + 跟随主题/语言刷新。
用到的 API 都在 `apps/src/{wm,ui,sys,fs,sinfo,ipc}.rs` 的文档注释里。

### 1.1 先单独跑一遍（不改主仓）

```bash
cd templates/gui-app
bash verify.sh     # cargo build + 检查入口地址 0x400000 + 打印 blob 大小
```

产物是扁平镜像，符合 `nuc_spawn_blob()` 的加载约定。

### 1.2 接进系统（4 处）

假设应用叫 `hello_app`。**名字里不要有 `-`**：Makefile 用它拼 C 符号
`k_app_hello_app`，连字符会成为非法标识符。

1. 复制源码：

   ```bash
   cp templates/gui-app/src/main.rs apps/src/bin/hello_app.rs
   ```

2. `apps/Cargo.toml` 加一项（名字与文件名一致）：

   ```toml
   [[bin]]
   name = "hello_app"
   path = "src/bin/hello_app.rs"
   ```

3. `Makefile` 的 `APPS_NAMES` 追加名字（生成 `out/apps/hello_app.bin` 和
   `k_app_hello_app` 符号）：

   ```make
   APPS_NAMES  := fileman taskman settings netman session setup nsh hello_app
   ```

4. `kernel/apps.c` 的 `g_apps[]` **末尾**追加一行（不要插在中间：索引被 `/apps`
   页面复用；`spawn_name` 按名查找）：

   ```c
   {"hello_app", k_app_hello_app, &k_app_hello_app_len},
   ```

然后 `make`。启动方式：`nsh` 里直接敲应用名（内置命令未命中时会用
`sinfo::spawn_name` 启动已注册的 app）：

```bash
user@novus:/$ hello_app
```

要在**开始菜单/桌面磁贴**出现，需另外改 `graphics/nd_demo.c`（菜单项 + 磁贴 +
回调）和 `graphics/nvgui.c` 的图标，不在本模板范围内。

### 1.3 写界面时的硬约束

- **每帧预算**：一次提交最多 40 个图形、48 段文字，超出部分被丢弃。`draw()` 里
  按需绘制，别每帧堆控件。
- **重绘时机**：只在事件/主题变化后 `draw()` + `win.invalidate()`；空闲时
  `sys::yield_now()`（任务不会被抢占）。
- **主题色**：只用 `ui::bg()/card_bg()/row_sel()/text()/dim()/accent()/line()`，
  不要写字面颜色值，否则深色主题下会糊。
- **事件**：`wm::Ev::{Close, Mouse, Key(u32, u8), Focus, Other}`；`Key` 的第二个
  字段是修饰键（`MOD_CTRL` 等）。
- **文件**：走 `crate::fs::*`（`stat/opendir/readdir/mkdir/unlink/rename`）。GUI
  应用没有 cwd，路径自己拼绝对路径。

---

## 2. nsh 内建命令（`shell-command/`）

模板 `hello_cmd.rs` 里有三个可直接抄的例子：`hello`（参数 + 退出码）、
`where`（读 shell 状态）、`count`（读管道输入）。

### 2.1 两步接入

1. 把函数粘进 `apps/src/shell/cmds.rs`（放在其它 `cmd_*` 之后、
   `const BUILTINS` 之前）。
2. 在 `cmds.rs` 末尾的 `BUILTINS` 表里追加一行：

   ```rust
   ("hello", cmd_hello),
   ```

`make` 即可。`help`、`which`、管道 `|`、重定向 `> >> <`、`$(...)`、`; && ||`
自动生效——它们由 `run()` 和 `parse.rs` 统一处理，命令本身只读 `cmd.argv` 和
`stdin`、往 `out` 写。

### 2.2 命令的约定

```rust
fn cmd_xxx(shell: &mut Shell, cmd: &Cmd, stdin: &[u8], out: &mut dyn Sink) -> i32
```

- `cmd.argv[0..cmd.argc]` 是已展开（变量、glob、引号处理完）的 `&[u8]` 参数，
  **上限 40 个**（`MAX_ARGS`），单个参数 256 字节（`WORD_MAX`）。
- `stdin` 是管道 / 重定向 / `$(...)` 喂进来的完整输入，**上限 4 KiB**
  （`Capture.buf`）。不是流式的，别写循环读。
- 输出只走 `out.write(..)`，不要 `println!`：同一套命令要能写进终端，也要能被
  管道和命令替换捕获。
- 返回值就是 `$?`，即 `shell.last`：`0` 成功、`127` 未找到、其它自定义（模板用
  `2` 表示用法错误）。
- 要改环境用 `shell.set_var(..)`；要启动别的程序用
  `sinfo::spawn_name(shell.sys_port, "name")`（没有 fork/exec）。

---

## 3. Nuomi 脚本（`nuomi/`）

`nsh` 内置 Nuomi 解释器，脚本不用编译、不用注册：

```bash
user@novus:/$ nuomi /templates/nuomi/hello.nuomi
user@novus:/$ nuomi -e "(+ 1 2)"
```

- `hello.nuomi`：定义函数、递归、`map/reduce`、向量、闭包、`while`。
- `files.nuomi`：`mkdir` / `write-file` / `read-file` / `append-file` / `cp` /
  `mv` / `delete` / `rmdir`，路径语义与 nsh 一致（单根 `/`，相对路径基于 cwd）。

```nuomi
; 注释；字符串可用 "..." 或 ❝...❞
(define (f n) (* n 2))     ; 定义函数
(if cond a b)              ; 特殊形式：if while let let* letrec begin and or
(let ((a 1)) (+ a 1))      ; 调用时才开始求值
(set! x 3)                 ; 改变已有绑定（闭包状态靠它）
[1 2 3]  '(1 2 3)          ; 向量 / 列表字面量
(draw "裸文本\n")           ; 原样输出（println 会给字符串加引号）
```

内置函数：`+ - * / % = < > <= >= not equal? eq?`、`print println draw str
substring string-search`、`list car cdr cons length nth get set last reverse append
map filter reduce range assoc member? null? number? string? list? vector?
function? bool?`、`abs floor ceil round sqrt pow min max`、`exists? is-dir?
file-size read-file write-file append-file ls mkdir rmdir delete cp mv`、
`cls sleep random`。

### 3.1 限制（都是刻意的）

| 限制 | 值 | 超出时的表现 |
|------|----|--------------|
| 单个脚本 | 6 KiB | `nuomi: script too large (limit 6 KiB)` |
| 对象总量 | 256 KiB | `out of memory`（无 GC，循环里别疯狂 `cons`） |
| 递归 | 约 500 层函数调用 | `maximum recursion depth exceeded` |
| 单次调用实参 | 24 个 | `too many arguments` |
| 列表/向量字面量 | 64 个元素 | `list literal too long` / `vector too long`，长序列用 `range`/`append` 生成 |
| 交互 | 无 `input`，`key` 恒为 `nil` | 脚本不能阻塞等键盘 |

---

## 4. 提交前

- `make` 必须 0 error；新增 app 后确认 `out/apps/<name>.bin` 小于 128 KiB
  （`nuc_spawn_blob()` 的镜像缓冲上限，nsh 已到 69 KiB）。
- `build/apps.stamp` 只跟踪 `apps/src/**`、`apps/Cargo.toml`、`apps/link.ld` 和
  `apps/.cargo/config.toml`；改了其中任何一样再 `make`。
- 命令模板和脚本模板都是**片段**，主仓构建不会检查它们；接入后请实际敲一遍
  命令、跑一遍脚本。
