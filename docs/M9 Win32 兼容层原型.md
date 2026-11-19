# M9 Win32 兼容层原型

M9 的目标是让一个真正由 Windows 工具链产出的 PE32+ `.exe` 在 ring 3
跑起来：不再要求程序按 NUC 格式编译，而是由内核里的 PE 加载器完成映射、
重定位与导入解析，每条 Win32 API 调用落到一次 `syscall`。本轮只做
控制台 / 计时 / 睡眠 / 模拟注册表这一小片 API，够跑通一个自包含样例，
也为后续扩 API 留出固定结构。

---

## 1. 边界

已实现：

* PE32+（x86-64）可执行文件加载，固定基址 `0x50000000`。
* DIR64 基址重定位；按名称导入，IAT 填 thunk 地址。
* 15 个 API：控制台输出、tick、`Sleep`、错误码、命令行、模块基址、
  注册表读写。
* 每进程独立地址空间（`vm_space`）与独立的 Win32 ABI 标记。
* 文件管理器 `/exe` 虚拟目录，选中后运行。

未实现：

* GUI（`user32`/`gdi32`）、文件 API、堆管理、线程、异常、TLS。
* 磁盘上任意路径的 `.exe`：镜像来自内核内嵌表（`build/win32_blobs.h`）。
* 序号导入、延迟导入、DLL、真实 Windows 注册表语义。

---

## 2. 目录

```
kernel/
  pe.h / pe.c            PE32+ 校验、映射、重定位、导入、启动
  win32.h / win32.c      API 表、thunk 编码、注册表
user/win32compat/
  samples/hello.c        样例源码（不依赖 <windows.h> 与 CRT）
```

构建期产物：

```
out/win32/hello.exe      mingw 交叉编译
build/win32_blobs.h      xxd -i，内嵌为 k_app_hello_exe
```

`Makefile` 用 `x86_64-w64-mingw32-gcc` 以 `-nostdlib -nostartfiles
-Wl,-e,novus_win32_main` 链接，`-Wl,--dynamicbase
-Wl,--enable-reloc-section` 保证 `.reloc` 存在。样例入口不是 `main`，而是
`void novus_win32_main(void)`；`ExitProcess` 之外的返回路径落到
`0x90000000` 的跳板（`mov %rax,%rdi; mov $60,%eax; syscall`），把返回值
当退出码交给 `SYS_EXIT`。

---

## 3. 从 `.exe` 到进程

```
文件管理器 /exe → 选择 hello.exe → 运行
  → SYSINFO_REQ_SPAWN_NAME
  → apps_launch_name：apps_find 未命中 → apps_win32_find
  → apps_win32_launch → pe_load()
       ├─ pe_validate：只读校验，失败返回负错误码
       ├─ vm_space_create
       ├─ 映射头部页、各节、thunk 页、命令行页、64 KiB 栈、跳板页
       ├─ pe_relocate：应用 DIR64
       ├─ pe_imports：写 thunk，填 IAT
       └─ sched_create_user_ex(..., TASK_ABI_WIN32)
  → ring 3 执行；API 调用走 syscall 40
```

固定地址：

| 地址         | 用途                          |
| ---------- | --------------------------- |
| `0x50000000` | PE 镜像（从首选基址重定位）             |
| `0x60000000` | 一页 thunk（32 字节步长）           |
| `0x70000000` | 命令行 / 镜像名（`"hello.exe"`）    |
| `0x90000000` | 返回跳板                        |
| `0xA0000000` | 用户栈顶（向下 64 KiB）              |

---

## 4. PE32+ 校验

全部按文件偏移读取，不用 packed 结构体。拒绝即返回负值，不创建任务：

| 检查                                                       | 错误码       |
| -------------------------------------------------------- | --------- |
| `MZ`、`e_lfanew`、`PE\0\0`                                  | `PE_ENOTPE` |
| `Machine` 必须 `0x8664`                                     | `PE_EMACHINE` |
| `Magic` 必须 `0x20B`，`SizeOfOptionalHeader >= 160`           | `PE_EFORMAT` |
| `Characteristics` 不得含 `RELOCS_STRIPPED` / `DLL`           | `PE_EFORMAT` |
| `SectionAlignment >= 0x1000` 且 4 KiB 对齐                   | `PE_EFORMAT` |
| `SizeOfHeaders`、`SizeOfImage` 边界（镜像不越过 `0x60000000`）      | `PE_EFORMAT` |
| 数据目录必须落在可选头内（PE32+ 目录起点为字节 112）                          | `PE_EFORMAT` |
| TLS / 延迟导入目录非零                                            | `PE_EFORMAT` |
| `NumberOfSections ∈ [1, 32]`，节表在文件内                       | `PE_EFORMAT` |
| 节 `VirtualAddress` 页对齐、原始数据在文件内、页区间互不重叠                | `PE_EFORMAT` |
| 入口 RVA 落在可执行节内                                            | `PE_EFORMAT` |

权限：头部与各节 `USER|WRITE`，可执行节再加 `EXEC`；栈 `USER|WRITE`；
thunk 页与跳板 `USER|WRITE|EXEC`。节超出原始数据部分（`.bss`）由映射时
清零。

重定位只接受 `IMAGE_REL_BASED_ABSOLUTE (0)` 与 `DIR64 (10)`，上限 4096
条；没有重定位目录视为拒绝（`PE_ERELOC`），因为加载基址与首选基址不同。

导入按名称解析，序号导入拒绝。DLL 名大小写不敏感；未收录的
`dll!func` 打印一行 `[PE] unknown import dll!func` 并以 `PE_EIMPORT`
失败。

---

## 5. thunk 与 ABI

Windows x64 用 RCX 传第一个参数，而 `syscall` 会把返回地址写进 RCX；
RDI 在该 ABI 里是**被调用者保存**，不能拿来顶替。因此每条 thunk 先把
RDI 存进调用方预留的 shadow space，再走两次 `mov`：

```
48 89 7C 24 08   mov %rdi,0x8(%rsp)     ; 保存 RDI（shadow space）
48 89 CF         mov %rcx,%rdi          ; 第一个参数 → RDI
41 BA imm32      mov $id,%r10d          ; API id
B8 28 00 00 00   mov $40,%eax           ; SYS_WIN32
0F 05            syscall
48 8B 7C 24 08   mov 0x8(%rsp),%rdi     ; 恢复 RDI
C3               ret
```

共 27 字节，每条 thunk 占 32 字节槽位（其余补 `NOP`）。第 5 个及以后
的参数仍按 Win64 约定在 `[rsp+0x28+(n-4)*8]`，写 `[rsp+8]` 不会碰到它们。

`SYS_WIN32 (40)` 入口核对 `task_t.abi == TASK_ABI_WIN32`，参数取自帧里的
寄存器，返回值写回 RAX。

`Sleep(ms)` 用 park 重放实现：`ticks = max(1, (ms+9)/10)`，调用
`sched_park()` 后把 `f->rip -= 2` 回到 `syscall`；下一次被调度时
`wait_err == IPC_ETIMEDOUT`，入口处直接当作睡眠完成（RAX=0）。

---

## 6. API 表

| id | DLL        | 函数                     | 行为                                  |
| -- | ---------- | ---------------------- | ----------------------------------- |
| 0  | kernel32   | `ExitProcess`          | `sched_exit_current(code)` 并回收句柄 |
| 1  | kernel32   | `GetStdHandle`         | −11 / −12 返回伪句柄                     |
| 2  | kernel32   | `WriteFile`            | 最多 64 字节，`[WIN32] ` 前缀，回写已写字节数   |
| 3  | kernel32   | `WriteConsoleA`        | 同 `WriteFile`                        |
| 4  | kernel32   | `Sleep`                | 见第 5 节                              |
| 5  | kernel32   | `GetTickCount64`       | `sched_ticks() * 10` ms               |
| 6  | kernel32   | `GetTickCount`         | 同上，截断为 32 位                         |
| 7  | kernel32   | `GetLastError`         | 读进程私有错误码                            |
| 8  | kernel32   | `SetLastError`         | 写进程私有错误码                            |
| 9  | kernel32   | `GetCommandLineA`      | `0x70000000` 的 `"name"`              |
| 10 | kernel32   | `GetModuleHandleA`     | `0x50000000`                        |
| 11 | advapi32   | `RegOpenKeyExA`        | 打开/创建键，返回不透明句柄                      |
| 12 | advapi32   | `RegQueryValueExA`     | 读 `REG_SZ` / `REG_DWORD`             |
| 13 | advapi32   | `RegSetValueExA`       | 写 `REG_SZ` / `REG_DWORD`             |
| 14 | advapi32   | `RegCloseKey`          | 释放句柄                                |

写入是同步的：`WriteFile` 直接进串口，不排队。

---

## 7. 注册表

模型是扁平的 `(key, value) → (type, data)` 表：

* 最多 16 项；键路径 ≤ 47 字节，值名 ≤ 39 字节，数据 ≤ 72 字节。
* 仅 `REG_SZ (1)` 与 `REG_DWORD (4)`；其它类型 `RegSetValueExA` 返回 87。
* 键句柄为进程私有的不透明整数（基址 `0x1000`，最多 8 个），进程退出时
  由 `win32_owner_exit` 统一关闭。
* 首次启动写入种子项 `HKCU\Software\Novus` / `Greeting`。

Windows 路径前缀的用户目录为 `/Users/alice/.winprefix/`（其下含
`Program Files`、`ProgramData`、`users/alice`、`Windows/system32`）。

持久化文件 `/Novus/wincompat/system32/registry.cfg`：

```
UINT32 magic   = 'NVRG' (0x4752564E)
UINT32 version = 1
UINT32 count
UINT32 pad
entry[16] { char key[48]; char value[40]; UINT32 type; UINT32 len; UINT8 data[72]; }
```

总长 2704 字节，`_Static_assert` 固定了布局。只在根卷可写
（`vfs_root_persistent()`）时落盘；live 只读根下仅内存态，启动日志为
`[WIN32] registry: in-memory only`。

---

## 8. 权限隔离

`task_t.abi` 区分 `TASK_ABI_NUC` 与 `TASK_ABI_WIN32`。调度器从
`syscall` 回来时，若当前任务是 Win32 ABI，只放行 40 与 60（`SYS_EXIT`）；
其余调用记录 `[TRAP] win32 ABI denied syscall 0x...` 并返回 −1。这样
`.exe` 拿不到 NUC 应用那套文件 / IPC 调用。

---

## 9. 文件管理器 `/exe`

sysinfo 服务的虚拟目录多了一档 `SYSINFO_DIR_EXE (3)`，条目类型
`SYSINFO_KIND_EXE (4)`，数据来自 `apps_win32_name/size`。文件管理器标签
变为 `/apps`、`/dev`、`/proc`、`/exe`、`磁盘`；`KIND_EXE` 行与内置应用
同色显示，可选中后点「运行」。

---

## 10. 构建与验收

```
make
x86_64-w64-mingw32-objdump -h out/win32/hello.exe
x86_64-w64-mingw32-objdump -p out/win32/hello.exe
scripts/run.sh
```

期望：

* `.reloc` 节存在，重定位条目为 `DIR64`。
* 导入表含 `KERNEL32.dll`（`WriteFile`/`GetStdHandle`/`Sleep`/
  `GetTickCount`/`ExitProcess`）与 `ADVAPI32.dll`（`Reg*`）。
* 串口启动日志：`[PE] selftest PASS`、
  `[WIN32] registry: loaded ...` 或 `in-memory only`。
* 在文件管理器切到 `/exe`，选中 `hello.exe`，点「运行」：

```
[APPS] launching hello.exe (N bytes)
[PE] hello.exe: 5 sections, base 0x50000000, entry 0x50001000, imports 9
[WIN32] Hello from Win32 on Novus
[WIN32] Greeting=Hello from the Novus registry
[WIN32] win32 layer done
[SCHED] task hello.exe exited with code 0x0000000000000000
```

* `CONFIG_NOVUS_WIN32=n` 时仍可构建：`apps.c` 的 Win32 分支整体编译掉，
  sysinfo 的 `/exe` 目录为空。

## 第二批 API：程序启动阶段真正会调的十几个

第一批只有 15 个入口，够证明"PE 能加载、导入表能填、系统调用能回到内核"，
但任何一个真实程序在画第一个像素之前都要先问三件事：**我能分多少内存、我在哪个路径
下运行、现在几点**。第二批补的正是这些，合计 30 个入口：

| 组 | API | 实现口径 |
| --- | --- | --- |
| 模块 | `GetModuleFileNameA` | 返回加载时的镜像名（不含挂载点，层里还没有路径概念） |
| 内存 | `VirtualAlloc` / `VirtualFree` | 进程独占 `0x20000000–0x30000000` 的 256 MiB 窗口，逐页映射新物理页；地址参数被忽略 |
| 堆 | `GetProcessHeap` / `HeapAlloc` / `HeapFree` | 和 VirtualAlloc 同一个窗口；簿记表放在内核（64 项），不放用户内存 |
| 时钟 | `GetSystemTime` / `GetLocalTime` / `QueryPerformanceCounter` / `QueryPerformanceFrequency` | 日期来自 RTC，计数来自 LAPIC 10 ms 节拍，频率固定 10 MHz |
| 身份 | `GetCurrentProcessId` / `GetCurrentThreadId` / `IsDebuggerPresent` | 进程 id 即任务 pid（本层没有多线程）；调试器恒为否 |
| 机器 | `GetSystemInfo` | 48 字节 SYSTEMINFO，页 4 KiB、粒度 64 KiB、CPU 数为当前任务数 |
| 兜底 | `SetUnhandledExceptionFilter` | 接受并返回旧过滤器，但异常仍然终止进程（不假装有 SEH） |

三条设计取舍，写在这里免得下次又踩：

1. **分配器簿记不放用户内存。** Windows 的堆会在每个块前留头，本层内核不能直接解引用
   用户地址（要过 `user_copy`），把 64 项 `{owner, va, size}` 表放内核更省事，也更安全。
2. **释放不还页。** `VirtualFree`/`HeapFree` 只清表项，页继续映射。层里还没有 unmap 路径，
   而"指针可能还活着"时回收页比泄漏更糟；地址空间随进程退出一起销毁。
3. **RDI 保存约定不变。** 新 API 都走同一个 thunk（RDI 暂存在 shadow space 里），
   第 4 个及以后的参数仍从 `rsp + 0x28 + (n-4)*8` 读。

验收：样例程序 `hello.exe` 在退出前会打印这一段，全部来自第二批 API：

```
[WIN32] image: hello.exe
[WIN32] virtual: block ok, pagesum=...
[WIN32] heap: alloc/free ok
[WIN32] date: 2026
[WIN32]   month 11  day 19  hour 20  minute 5x
[WIN32] qpf: 10000000
[WIN32] qpc: ...
[WIN32] page: 4096
[WIN32] cpus: ...
[WIN32] pid: ...
```
