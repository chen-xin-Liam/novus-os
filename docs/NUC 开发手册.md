# NUC 开发手册

> 面向：要往 Novus-OS 里放一个自己的可执行程序的开发者
> 配套：
>
> `docs/NUC 可执行二进制格式规范 V1.0.md`
>
> （格式冻结稿，本文只讲 “怎么用”）
> 实现：
>
> `kernel/nuc.c`
>
>  / 
>
> `kernel/nuc.h`
>
> 、
>
> `kernel/apps.c`
>
> 、
>
> `kernel/sysapps.c`
>
> 、
>
> `apps/link.ld`

## 1. 先分清三条路线



| 路线     | 输入                       | 产出                                  | 适合                        |
| ------ | ------------------------ | ----------------------------------- | ------------------------- |
| A 工具链  | `apps/src/bin/<name>.rs` | 内核内嵌 blob + `/Novus/bin/<name>.nuc` | 官方内置应用（fileman、settings…） |
| B 内核打包 | 一段扁平机器码 `UINT8[]`        | `nuc_pack_blob()` 生成的 NUC 镜像        | 单元自检、手写小客户端               |
| C 外部自制 | 任意扁平镜像                   | 自己按第 3 节的字节表拼出 `.nuc`               | 从别的系统搬程序、脚本化生成            |

三条路线产出的是**同一种字节**，加载器不分来源。

## 2. 容器长什么样

一个 `.nuc` = 48 字节文件头 + 段表（每段 32 字节）+ 符号表 + 重定位表 + 段数据，按文件偏移顺序排。



```
0x00  Header (48B)
0x30  Segment Table (32B × seg_count)
      Symbol Table  (24B × sym_count)
      Relocation    (16B × rel_count)
      String Table
      Segment payload（各段按 file_offset 指向这里）
```

文件头字段（与 `nuc_hdr_t` 一致）：



| 偏移   | 大小 | 字段             | 说明                                       |
| ---- | -- | -------------- | ---------------------------------------- |
| 0x00 | 4  | magic          | `4E 55 43 21`（"NUC!"），不符 → `-3`          |
| 0x04 | 2  | abi\_version   | 高字节 = major，必须为 1；当前写 `0x0100`           |
| 0x06 | 2  | compile\_flag  | bit0 必须是 1（64 位）；官方镜像写 `0x05`（64 位 + 静态） |
| 0x08 | 4  | entry\_point   | **段内**偏移，入口 = text 段 vaddr + 它（官方镜像恒为 0） |
| 0x0C | 2  | seg\_count     | 1\~16，0 或 >16 → `-1`                     |
| 0x0E | 2  | symtab\_count  | 符号条数，静态镜像写 0                             |
| 0x10 | 4  | strtab\_offset | 字符串表文件偏移，且必须 ≥ 重定位表末尾                    |
| 0x14 | 4  | rel\_offset    | 重定位表文件偏移，且必须 ≥ 符号表末尾                     |
| 0x18 | 4  | stack\_size    | 用户栈字节数，加载器夹到 64 KiB \~ 4 MiB             |
| 0x1C | 4  | rel\_count     | 重定位条数，纯静态写 0                             |
| 0x20 | 16 | reserved       | 全 0                                      |

段表条目（`nuc_seg_t`，32 字节）：



| 偏移   | 大小 | 字段           | 说明                               |
| ---- | -- | ------------ | -------------------------------- |
| 0x00 | 4  | type         | 0=text（可执行）1=data 2=bss 3=rodata |
| 0x04 | 4  | file\_offset | 段数据在文件中的偏移；bss 用 0               |
| 0x08 | 8  | vaddr        | **必须 4 KiB 对齐**，否则 `-1`          |
| 0x10 | 8  | size         | 字节数，映射时按页向上取整；0 → `-1`           |
| 0x18 | 4  | flags        | 权限位（见下）                          |
| 0x1C | 4  | reserved     | 0                                |

`flags` 按规范是 `bit0 R / bit1 W / bit2 X / bit3 USER`，官方 TEXT 段写 `0x0D`、DATA/BSS `0x0B`、RODATA `0x09`。**但现在的加载器不读这个字段**，它按 `type` 定权限：text 段拿 R+W+X+USER，其它文件段拿 R+W+USER（不可执行），bss 段直接清零后 R+W+USER。写 `0x0D` 只是为了和规范对齐，不会让 data 段变成可执行。

## 3. 加载器实际做了什么（和规范不完全一样）

`nuc_load(img, size, name)` 的判定顺序，照这个写就不会踩坑：



1. `size < 48` → `-6`；magic 不对 → `-3`。

2. `seg_count` 不在 1..16 → `-1`；`compile_flag` 没有 bit0 → `-1`；`abi_version >> 8 != 1` → `-1`。

3. 符号表、重定位表、字符串表的偏移必须落在文件内且互不重叠，否则 `-11` / `-1`。

4. 每段：`vaddr` 页对齐、`size > 0`；文件段还要 `file_offset + size <= 文件长度`。

5. 建页表、逐段映射；每页先清零再拷数据，所以**最后一页尾部自动是零**，段内 `file_offset` 没必要自己补齐。

6. 重定位：只认 `1 = ABS32` 和 `2 = PC32`，符号只能在镜像内部解析（`shndx` 必须是某个段）；`3` 及以后 → `-1`。**本实现没有导入、没有动态链接**，`compile_flag` 的 PIC / 动态位写了也没用。

7. 入口：找第一个 `type == 0` 的段，`entry = vaddr + entry_point`；没有 text 段 → `-1`。

8. 用户栈：`stack_size` 夹到 \[64 KiB, 4 MiB]，顶端固定在 `0xA0000000`，初始 `RSP = 0xA0000000 - 8`，那 8 个字节放的是 `0x90000000`（跳板地址）。

9. 建任务（ring 3），`[NUC] <name>: N segments, entry 0x…, stack 0x… bytes` 打进串口。

跳板在 `0x90000000`，内容是 `mov rax,rdi; mov eax,60; syscall`：**程序&#x20;**`ret`**&#x20;或从&#x20;**`_start`**&#x20;返回就等于&#x20;**`exit(RAX)`，不会跳到野地址。

地址空间约定（不要和镜像抢）：



| 区域           | 地址           |
| ------------ | ------------ |
| 镜像基址（官方链接地址） | `0x400000`   |
| 跳板页          | `0x90000000` |
| 用户栈顶         | `0xA0000000` |

## 4. 运行时契约



* **no\_std、无堆**：没有 `brk`/`mmap` 系统调用，也没有分配器。所有缓冲区都是静态数组（`static mut`）或栈上。内置应用最大的一个（nsh）也就 148 KB 镜像。

* **单实例**：加载器用 `sched_create_user(name, …)` 建任务，同名任务已在跑 → `apps_launch` 直接返回 “已在运行”。想再开一个窗口请自己开多窗，不要指望再启动一份。

* **必须让出 CPU**：事件循环末尾要 `sys::yield_now()`（调用号 1），否则单核会被你这一个任务占满。

* **panic 要收尾**：官方 runtime 的 `#[panic_handler]` 是 `write("[APP] panic\n")` 再 `exit(3)`；自己写就照抄，别让 panic 掉进 `loop {}`。

* **失败返回负值**：所有系统调用返回 `i64`，负数是错误码；`sys::exit(code)` 只写 `u64`，非零退出码会显示在任务管理器里。

## 5. 系统调用 ABI

`rax` = 调用号，`rdi / rsi / rdx / r10` = 参数 1\~4，返回值在 `rax`；`syscall` 会改写 `rcx`、`r11`。用户态实现只有一条汇编（`apps/src/sys.rs` 的 `call()`）。



| 号       | 名字                                        | 参数要点                                                                                                    |
| ------- | ----------------------------------------- | ------------------------------------------------------------------------------------------------------- |
| 1       | yield                                     | 无                                                                                                       |
| 2       | write                                     | `(fd=1, buf, len)` 内核日志行；写普通字符串用它                                                                       |
| 3       | getpid                                    | 无                                                                                                       |
| 4       | ipc\_send                                 | `(port, type 与 flags<<16 打包成一个 u64, buf, len)`                                                          |
| 5       | ipc\_recv                                 | `(port, buf, len, flags)`，返回 payload 长度                                                                 |
| 6 / 7   | ipc\_lookup / ipc\_stats                  | 端口查表 / 统计                                                                                               |
| 8\~12   | open / read / fs\_write / close / stat    | 文件，路径上限 256，单次 IO 上限 4096                                                                               |
| 13\~15  | opendir / readdir / mkdir                 | 目录                                                                                                      |
| 16 / 17 | power\_off / reboot                       | 无                                                                                                       |
| 18      | session                                   | `(op, &SessionRequest)`，结构 100 字节                                                                       |
| 19      | setup                                     | 安装器专用，需要内核授权                                                                                            |
| 20\~24  | unlink / rename / mount / umount / mounts | `/Novus` 前缀会被 VFS 拒绝（-1）                                                                                |
| 25      | update                                    | 更新服务控制 ABI，336 字节请求                                                                                     |
| 26      | https                                     | HTTPS 客户端控制 ABI                                                                                         |
| 27      | console                                   | `(0=写,1=读, buf, len)` 串口，nsh 的 stdout                                                                   |
| 28\~37  | Mini-DWM                                  | vmem\_alloc/free/map/grant、surface\_present、wait\_vsync、set\_exclusive、set\_cursor、dwm\_query、set\_mode |
| 40      | win32                                     | Win32 兼容层（受限制，只允许 SYS\_WIN32 与 SYS\_EXIT）                                                               |
| 60      | exit                                      | `(code)`，不返回                                                                                            |

未知调用号会被记一次并拒绝（串口 `[SYS] unknown call …`），所以打错号不会 “没反应”，而是有日志。

图形界面怎么画不在这里讲，看 `docs/应用图形化开发指南.md`（`wm` 客户端 + `ui` 组件）。

## 6. 路线 A：用现成工具链做一个内置应用



```
# apps/Cargo.toml
[[bin]]
name = "hello2"
path = "src/bin/hello2.rs"
```



```
# Makefile
APPS_NAMES := fileman taskman settings netman session setup nsh devmgmt novusd dwm hello hello2
```



```
/* kernel/apps.c 表尾 */
{"hello2", k_app_hello2, &k_app_hello2_len},
```

源码要求（照 `apps/src/bin/hello.rs` 抄骨架）：



* `#![no_std]`、`#![no_main]`，自己导出 `#[no_mangle] extern "C" _start`，没有 `main` 包装、没有栈初始化；

* 链接脚本固定 `apps/link.ld`（`. = 0x400000`，`KEEP(.text._start)` 保证入口就是文件第 0 字节），`relocation-model=static` —— 所以**镜像里不留重定位，绝对地址直接可用**；

* 构建产物 `out/apps/<name>.bin` 是 `objcopy -O binary` 出来的扁平镜像，`xxd -i` 进 `build/apps_blobs.h`；

* 开机 `sysapps_seed()` 再把同一份 blob 打成 `.nuc` 写进 `/Novus/bin/<name>.nuc`，启动时优先跑文件。

两条硬上限（超了是构建期 / 加载期报错，不是运行时崩溃）：



* **单个镜像 ≤ 256 KiB**：`nuc_spawn_blob()` 的内嵌打包缓冲和 `sysapps_spawn()` 的读入缓冲都是 `48+32+262144`；变大会返回 `NUC_EINVAL`，而不是截断。nsh 已经用到 148 KB，剩的余量不多。

* **应用表最多 12 项**：`kernel/apps.h` 的 `APPS_MAX 12`，当前已占 11 个名字。

构建与验证：



```
cmd /c C:\msys64\tmp\build_main.cmd          # 或 make disk-img
# 串口应出现
[SYSAPP] wrote /Novus/bin/hello2.nuc (0x… bytes)
[APPS] image from /Novus/bin
[USER] [APP] hello2 up
```

## 7. 路线 C：不靠编译器造一个 `.nuc`

官方镜像的布局极其简单：**48 字节头 + 1 条段表 + 扁平镜像**（`nuc_pack_blob()` 就干这件事）。任何扁平镜像（objcopy 出来的、手写的机器码、别的系统的代码段）都能这样包起来。



```
# mk_nuc.py <flat.bin> <out.nuc> [vaddr] [stack]
import struct, sys

src = open(sys.argv[1], 'rb').read()
vaddr = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x400000
stack = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x400000

hdr_size, seg_size = 48, 32
payload = hdr_size + seg_size           # 段表紧接文件头，数据紧接着段表

hdr = b'NUC!'                            # magic
hdr += struct.pack('<H', 0x0100)         # abi_version: major 1, minor 0
hdr += struct.pack('<H', 0x05)           # compile_flag: 64-bit + static
hdr += struct.pack('<I', 0)              # entry_point: 段内偏移 0
hdr += struct.pack('<H', 1)              # seg_count
hdr += struct.pack('<H', 0)              # sym_count
hdr += struct.pack('<I', payload)        # strtab_offset（空表就指向数据开头）
hdr += struct.pack('<I', payload)        # rel_offset
hdr += struct.pack('<I', stack)          # stack_size
hdr += struct.pack('<I', 0)              # rel_count
hdr += b'\0' * 16                        # reserved
assert len(hdr) == hdr_size

seg = struct.pack('<I', 0)               # type = TEXT
seg += struct.pack('<I', payload)        # file_offset
seg += struct.pack('<Q', vaddr)          # vaddr，必须页对齐
seg += struct.pack('<Q', len(src))       # size
seg += struct.pack('<I', 0x0D)           # flags: R|X|USER（加载器按 type 定权限）
seg += struct.pack('<I', 0)
assert len(seg) == seg_size

open(sys.argv[2], 'wb').write(hdr + seg + src)
print("nuc bytes:", payload + len(src))
```

约束：`vaddr` 必须 4 KiB 对齐，`size > 0`，镜像内不能有指向段外的绝对地址（除非你自己补重定位表，类型只认 1/2）。入口就是镜像第一字节，所以**扁平镜像的第 0 字节必须是可以执行的指令**（`nuc_selftest` 就是这么手搓 `mov eax,2; …; syscall; ret` 的）。

## 8. 部署、升级与介质



* 跑起来的位置是 `/Novus/bin/<name>.nuc`，由 `apps_launch()` 先 `sysapps_spawn()` 再退回内嵌镜像。串口 `[APPS] image from /Novus/bin` 表示这次跑的是文件。

* seed 会读回已装文件逐字节比对：一致 → `kept`；不一致 → `unlink` + 重写。所以升级程序不用手工删。

* seed 用 `novusfs_write_direct` 直写（绕开 256 KiB 的 WAL 环），目录项仍走日志事务。

* 只读介质（ISO、刻死介质）：`[SYSAPP] cannot write …` 或 `… is not writable`，应用回落到内嵌镜像，功能不缺。

* `/Novus` 是系统树：VFS 的 `unlink` / `rename` 对 `/Novus` 前缀一律返回 -1；安装器走 `novusfs_*` 直调不受影响。**别手改系统树里的&#x20;**`.nuc`，下次开机比对不一致会被覆盖。

## 9. 排错对照



| 现象                    | 原因 / 看哪里                                                                       |
| --------------------- | ------------------------------------------------------------------------------ |
| `-3`（`ENOTFOUND`）     | magic 不是 `NUC!`，或者文件根本不是 NUC                                                   |
| `-1`（`EINVAL`）        | 段数为 0 / >16、`compile_flag` 缺 bit0、`abi` 主版本不是 1、`vaddr` 没页对齐、没有 text 段、重定位类型非法 |
| `-11`（`EOVERFLOW`）    | `file_offset + size` 超过文件长度，或表偏移越界                                             |
| `-2`（`ENOMEM`）        | 页分配失败（机器内存不足，或段 size 写得离谱）                                                     |
| `-6`（`EIO`）           | 文件短于 48 字节 / 读不出来                                                              |
| 跑起来立刻退出               | `_start` 直接 `ret` 了，跳板拿 `RAX` 当退出码；或栈上取了未初始化的值                                 |
| 一启动就抢满一个核             | 循环里漏了 `yield`（调用号 1）                                                           |
| `[APPS] spawn failed` | 加载器返回了非 0，往上看 `[NUC] …` 之前那行错误                                                 |
| 窗口空白                  | 不是 NUC 的问题，见图形化指南第 8 节 “画了但不显示”                                                |

## 10. 现在做不到的



* **没有&#x20;**`exec`**&#x20;系统调用**：第三方 `.nuc` 目前只能通过 `kernel/apps.c` 的内置表按名字启动（`apps_launch_name`）。要做成 “文件管理器里双击运行”，得先加一个 `SYS_EXEC(path)` 并接上权限。

* **没有动态链接 / 导入符号**：`compile_flag` 的 PIC / 动态位保留未实现，重定位只能指向镜像内部。

* **没有堆**：想要 `Vec`/`Box` 就得自己带一个静态 arena（nsh 里的 Nuomi arena 就是这种用法）。

* **没有 ASLR**：链接地址即运行地址，`0x400000` 固定。