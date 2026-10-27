# nuomi：脚本模板

`nsh` 内置 Nuomi 解释器，脚本不用编译、不用注册：

```bash
user@novus:/$ nuomi hello.nuomi
user@novus:/$ nuomi files.nuomi
```

`nuomi` 按 nsh 的 cwd 解析脚本路径，脚本里的相对路径也基于 cwd。

## hello.nuomi

语言特性一站式示例：`define` 函数与递归、`map` / `reduce` / `range`、
向量与 `set`、闭包捕获 `let` 的绑定、`while`、`draw` 原样输出。

实际输出（host 上用同一份解释器跑出来的）：

```
"hello, novus"
"fact(10) =" 3628800
"squares =" (1 4 9 16 25)
"sum =" 55
"v =" [99 20 30] "len =" 3
"counter:" 1 2 3
"0+1+2+3+4 =" 10
raw text, no quotes
```

> `println` 给字符串加引号是 C 版 Nuomi 的既有语义；要裸文本用 `draw`
> （不自动换行，`\n` 自己写）。

## files.nuomi

在 `/` 下建 `/nuomi-demo`，依次 `mkdir` → `write-file` → `read-file` →
`append-file` → `ls`/`file-size` → `cp` → `mv` → `delete` → `rmdir`，最后用
`exists?` 确认清理干净。可以照着改成读写自己的配置文件或数据文件。

## 限制

见 `../README.md` §3.1：单脚本 6 KiB、对象 256 KiB（无 GC）、递归约 500 层、
单次调用 24 个实参、字面量 64 个元素、无 `input`。
