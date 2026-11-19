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
   第 5 个及以后的参数从 `rsp + 0x28 + (n-5)*8` 读（`n` 是真实参数序号，第 5 个是第一
   个真正落到栈上的参数；`0x28 = 返回地址 8 字节 + 四个寄存器参数槽 0x20`）。

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

## 第三批：user32 / gdi32，窗口真的开出来

前两批让程序能跑起来并自己说话；这一批让它有窗口。范围是"能开一个窗口、能画、能收
消息"的最小闭环，一共 18 个 API：

| 分类 | API | 做法 |
| --- | --- | --- |
| 窗口 | `RegisterClassExA` / `CreateWindowExA` / `DestroyWindow` / `ShowWindow` | 窗口就是 Novus-WM 的窗口，owner 记为进程 pid；z-order、焦点、拖拽、关闭按钮全部复用 |
| 消息 | `GetMessageA` / `PeekMessageA` / `DispatchMessageA` / `TranslateMessage` / `DefWindowProcA` | 每个进程一条 16 条的环形队列，货从 WM 事件翻译而来 |
| 客户区 | `GetClientRect` | 客户区 = `(w-2) x (h-33)`，边框和标题栏由 WM 画 |
| 绘制 | `BeginPaint` / `EndPaint` / `GetDC` / `ReleaseDC` / `FillRect` / `TextOutA` / `SetTextColor` / `CreateSolidBrush` | 不是像素缓冲，是**显示列表**：每次调用往 WM 追加一条图元，`EndPaint` 时整体提交 |

### 为什么要有"内核直调"通道

Win32 进程在 ring3 只有 `SYS_WIN32` 一个系统调用入口，它不可能去说 WM 的 IPC 协议
（那是 Rust 侧应用的私有格式）。所以 `nwm.c` 里加了一条内核侧入口：

```c
static int g_kernel_call;      /* 置位时 ack() 不发 reply，只记状态 */
static INT32 g_kernel_status;
static UINT32 g_kernel_win;

int nwm_kernel_request(owner, type, payload, len, out_win);   /* 造一个 sender=owner 的假报文 */
int nwm_owner_create(...); nwm_owner_destroy(...); nwm_owner_clear(...);
int nwm_owner_shape(...);  nwm_owner_text(...);   nwm_owner_flush(...);
```

内核侧调用是同步的：`handle_request()` 走完，`ack()` 看到 `g_kernel_call` 就只把
状态码填进 `g_kernel_status` 然后返回。没有新线程、没有额外队列，也没有把 WM 的
协议暴露给用户态。

### DispatchMessageA 必须回到程序自己的 ring

`WndProc` 是程序里的函数指针，只能由程序自己的 ring3 代码调用。`DispatchMessageA`
因此不是 thunk，而是一页手写机器码（43 字节）：

```
mov rax,[rcx]        ; MSG.hwnd
and eax,0xF
mov r11,[0x60003F00+rax*8]   ; 表项：该窗口的 WndProc
test r11,r11 / jz .none
mov r8,[rcx+0x10]    ; wParam / lParam 按 Win64 顺序摆好
mov r9,[rcx+0x18]
mov edx,[rcx+8]      ; message
mov rax,rcx
mov rcx,[rax]        ; hwnd 回填 rcx
sub rsp,8 / call r11 / add rsp,8 / ret
.none: xor eax,eax / ret
```

HWND 编码是 `0x40000000 | winid`，表索引 `hwnd & 0xF`（16 槽，对应 10 个窗口足够）。
`pe.c` 的 IAT 填充从"一律写 thunk"改成"问 `win32_api_entry(id)`"，只有返回值和
常规 thunk 地址完全相等时才写 thunk，否则直接写 stub 页地址——这样"这个 API 需要
用户态代码"这件事由 `win32.c` 一处决定。

### 等待不能再忙等

`GetMessageA` 空队列时的第一版是 `sched_yield()` 循环，结果整机冻结：
syscall 进入时中断是关的，WM 任务永远上不了 CPU。改成 `ipc_recv(pid, &in, 1)` 真
阻塞，并在 `IPC_ERETRY` 时按 `sys_ipc_recv` 同一套机制处理：

```c
sched_park(pid, 500);
f->rax = SYS_WIN32;
f->rip -= WIN32_SYSCALL_LEN;   /* 回到 syscall，唤醒后原地重放 */
```

唤醒后 `gui_pull` 在 wait 分支返回 -1，`api_get_message` 负责把 rip 卷回去，重放时
队列里已经有消息了。

### 这一批真正修掉的 bug（按发现顺序）

1. **无 `.reloc` 的镜像被拒。** 样例程序没有任何绝对地址，链接器不生成重定位表，
   加载器（有意地）拒绝搬移。解法不是放宽加载器，而是让样例真的持有绝对地址
   （`static const char *const k_body[]`），`Entry 5` 就有了 3 条 DIR64。
2. **`FillRect` 的 DLL 名对不上。** Windows 里它是 USER32 导出，多数编译器按 GDI32
   导入。`win32_api_lookup()` 改成先精确匹配 DLL+函数，再按函数名兜底——DLL 只是
   记账，不是契约。
3. **stub 页只给了 `USER|EXEC`。** `user_copy(to_user=1)` 要求写权限，表项写不进
   去，`WndProc` 永远不被调用（`rc = 0xFFFF...`）。补 `VM_PROT_WRITE` 后
   `DefWindowProc reached` 立刻出现。
4. **栈参数偏移整体错一格。** 旧公式 `0x28 + (n-4)*8` 对第 5 个参数读到 `rsp+0x30`。
   实机栈 dump 是决定性证据：`rsp+0x28` 处正好是 `0x1B`/`0x2D`/`0x27`，和各行文本
   长度吻合。改成 `0x28 + (n-5)*8`（`n >= 5`）后，同一处调用点
   `TextOutA(hdc, x, y, str, len)` 立刻拿到 `len`。顺带把三个 `Reg*` 调用点从"为
   旧公式编的号"改回真实参数号。
   影响面不止文字：`CreateWindowExA` 的 x/y/w/h 也整体错位，窗口被摆到屏外。

### 验收

样例 `user/win32compat/samples/window.c`（无 CRT，入口 `novus_win32_main`）在 QEMU
实机里跑出的串口：

```
[NWM] create win=3 from pid=7
[WIN32] paint: submitted shapes=2 texts=8 flush->0
[NWM] first frame win=3 from pid=7
```

`shapes=2` 是白底 + 左侧 4px 红条，`texts=8` 是 `WndProc` 里八次 `TextOutA`——
文字第一次真的上了屏。这意味着链路已经闭合：

```
CreateWindowExA → WM 窗口 → GetMessageA(park) → DispatchMessageA(stub)
→ 程序自己的 WndProc → BeginPaint → FillRect/TextOutA → EndPaint → 显示列表提交
```

### 还没做的（别当成已做）

- 只有 scale=1 的单一字号，没有 `GetTextMetrics`；CJK 字形的覆盖按内置子集来。
- 没有 `WM_CHAR` 翻译（`TranslateMessage` 目前只处理 `WM_KEYDOWN`）。
- 没有 `SetWindowPos` / `SetWindowLong` / 菜单 / 对话框 / 控件（`comctl32` 整块缺）。
- 合成器不会主动请求重绘，`WM_PAINT` 只在创建、`EndPaint`、`WM_EV_RESIZE` 时投递。
- 没有 `ws2_32` / `crypt32` / `ole32` / `shell32` / SEH / TLS / 多线程；
  微信、QQ 这类大型应用离"能起来"还有相当距离，本层只到"最小窗口程序能跑"。
