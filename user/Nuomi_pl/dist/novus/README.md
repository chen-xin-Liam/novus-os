# Novus 渠道：预编译示例 .nuc

这些是 **Nuomi 编译器（Route-A M2）** 产出的、符合《NUC 可执行二进制格式规范 V1.0》的示例程序，
可直接交给 novus-os 加载执行（shell 的 exec / `sys_nuc_exec`）。

| 文件 | 源码 | 功能 | 入口返回值（rax） |
|---|---|---|---|
| `demo.nuc` | [src/demo.nuomi](src/demo.nuomi) | 算术演示 | 142 |
| `fact.nuc` | [src/fact.nuomi](src/fact.nuomi) | 递归阶乘 fact(10) | 3628800 |
| `fib.nuc`  | [src/fib.nuomi](src/fib.nuomi)  | 双递归斐波那契 fib(10) | 55 |
| `sum.nuc`  | [src/sum.nuomi](src/sum.nuomi)  | while 循环求和 0..99 | 4950 |

## 校验（宿主机上）

```bash
nuomi nuc validate demo.nuc   # -> [0] valid NUC V1.0
nuomi nuc dump    demo.nuc    # 查看段表 / 符号表 / 重定位表 / entry_rip
nuomi nuc run     demo.nuc    # 宿主加载执行，打印入口返回值
```

## 重新编译

```bash
nuomi -c src/fact.nuomi -o fact.nuc
```

## 格式说明

- NUC V1.0（64 位 / STATIC / LOCAL 符号 / `R_X86_64_PC32` 重定位）
- 入口：`entry_rip = text.vaddr + entry_point`（规范 §9.1）
- 跨函数调用经符号表 + 重定位，由 novus 加载器在装载期解析（规范 §5/§7.3）
- 当前编译子集：整数 + 函数(递归) + 局部变量 + if/while/begin + 比较 + **let / let\* / letrec** + **字符串字面量（RODATA 段）**；**无 I/O**（待 novus 系统调用号表公布）
