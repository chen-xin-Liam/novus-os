# M9 Win32 兼容层原型

M9 的目标是让一个真正由 Windows 工具链产出的 PE32+ `.exe` 在 ring 3
跑起来：不再要求程序按 NUC 格式编译，而是由内核里的 PE 加载器完成映射、
重定位与导入解析，每条 Win32 API 调用落到一次 `syscall`。本轮只做
控制台 / 计时 / 睡眠 / 模拟注册表这一小片 API，够跑通一个自包含样例，
也为后续扩 API 留出固定结构。

---

## 1. 边界

已实现：

* PE32+（x86-64）可执行文件加载，基址 `0x50000000`（无重定位表的镜像按其
  自身的链接基址加载）。
* DIR64 基址重定位；按名称导入，IAT 填 thunk 地址。
* 141 条 API（表里 142 行，`GetLocalTime` 复用 `GetSystemTime` 的 id）：控制台输出、
  tick、`Sleep`、错误码、命令行、模块基址、注册表读写、堆与 `VirtualAlloc`、
  `user32` 窗口、消息循环、投递与定时器、坐标换算、光标位置、窗口移动、焦点、
  `gdi32` 绘制与形状、TEB / TLS、屏幕度量、字体度量、文字测量与排版（含
  `DT_WORDBREAK` 折行），文件读写、目录枚举、`LoadLibrary` / `GetProcAddress`，
  控件（`STATIC` / `BUTTON` / `EDIT`、`WM_COMMAND`、`GetDlgItem`）、`MessageBoxA`、
  菜单条与下拉菜单（8 条 API）、PE 资源的读取（`FindResource` / `LoadResource` /
  `LockResource` / `SizeofResource` / `LoadStringA`）、
  版本与兼容模式（`GetVersion` / `GetVersionExA` / `IsWow64Process` /
  `GetCurrentProcess`），
  以及 `ws2_32` 的握手、地址转换与错误码（传输调用如实返回 `WSAENETDOWN`）。
* 每进程独立地址空间（`vm_space`）、独立的 Win32 ABI 标记，以及 `%fs` 指向的 TEB。
* 文件管理器 `/exe` 虚拟目录，选中后运行。

未实现：

* 磁盘上任意路径的 `.exe`：镜像来自内核内嵌表（`build/win32_blobs.h`）。
* 32 位（PE32 / `0x014C`）可执行文件的执行。加载器能识别并如实拒绝，但跑不起来：
  兼容执行器（32 位代码段的 `CS.L=0`、`iret` 进入、stdcall 栈 ABI、`int 0x80`
  入口）没有建。
* 16 位 `.COM`（实模式）与 16 位 NE / LE 可执行文件；`pe_probe` 对裸 MZ 头只输出
  `DOS MZ executable (16-bit real mode)` 这一句描述，然后按"不是 PE"拒绝。
* 序号导入、延迟导入、从磁盘映射真实 DLL、PE TLS 目录、多线程、SEH。
* `DialogBox` 与 `DLGTEMPLATE`（模态框只有 `MessageBoxA` 这一种）、`TrackPopupMenu`
  （下拉菜单只从菜单条展开）、`LoadMenuA`（不从资源里的 `MENU` 模板建菜单）、
  菜单项的 `&` 快捷键下划线、子控件自己的窗口过程、`SendMessage` 的同步语义、
  `ws2_32` 的传输部分（本块只起了握手与地址转换）、`crypt32`、`comctl32`、`ole32`、
  `shell32`。

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

1. **无 `.reloc` 的镜像怎么处理。** 样例程序一开始没有任何绝对地址，链接器就不生成
   重定位表，加载器（有意地）拒绝搬移。当时让样例真的持有绝对地址
   （`static const char *const k_body[]`）来产生 3 条 DIR64。后续补上的
   `tlsdemo.c` 又撞上同一件事，于是加载器这一侧也按 Windows 的规则补全了：
   **没有重定位表的镜像只在它自己被链接的基址上加载**，前提是该基址页对齐、
   整段镜像落在 `0x42000000`（TEB）以下；否则报
   `no relocations and no room at its preferred base`。两个样例因此各走一条路径：
   `hello.exe`/`window.exe` 在 `0x50000000` 走重定位，`tlsdemo.exe` 回到 `0x400000`
   原地加载。
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

- 只有 scale=1 的单一字号，`GetTextMetricsA` 报的就是这一种的度量；CJK 字形的覆盖
  按内置子集来。
- `WM_KEYUP` 不投递（WM 只给按下），修饰键（Shift/Ctrl/Alt）不带进 `WM_CHAR`。
- `WM_CHAR` 由本层排进队列，`TranslateMessage` 仍是空实现。
- 没有 `SetWindowPos` / `SetWindowLong` / 菜单 / 对话框 / 控件（`comctl32` 整块缺）。
- 合成器只在创建、`EndPaint`、`WM_EV_RESIZE`、`InvalidateRect` 时请求重绘。
- 没有 `ws2_32` / `crypt32` / `ole32` / `shell32` / SEH / PE TLS 目录 / 多线程；
  微信、QQ 这类大型应用离"能起来"还有相当距离，本层只到"最小窗口程序能跑"。

---

## 7. 第四块：TEB 与 TLS

Windows 程序在第一条指令前就已经有线程信息块，`%fs` 直接指向它。这一块把这件事补齐，
结构按 x64 ABI 的真实偏移填，而不是按本层自造的布局：

| 偏移 | 字段 | 值 |
| --- | --- | --- |
| `0x08` / `0x10` | `StackBase` / `StackLimit` | `0xA0000000` / 减去 64 KiB |
| `0x30` | `Self` | `0x42000000`（指向自己） |
| `0x40` / `0x48` | `ClientId.ProcessId` / `UniqueThread` | 指针宽，取自 `task_t` |
| `0x1480` | `TlsSlots[64]` | 编译器读线程局部变量走的就是这里 |

TEB 占 `0x42000000` 起的**两页**（`0x1480` 在第二页），由 `pe_teb()` 在 `pe_load()`
末尾、进程号已知之后映射并填写。填不进去只打一行
`TEB not mapped, TlsAlloc will fail`，程序照样启动。

### 切换任务时必须跟着换 %fs

`%fs` 是每核寄存器（`IA32_FS_BASE`，MSR `0xC0000100`），所以调度器三处都要写：

```c
sched_start()          msr_write(MSR_FS_BASE, first->teb_va);
sched_switch_frame()   msr_write(MSR_FS_BASE, next->teb_va);
sched_enter_kidle()    msr_write(MSR_FS_BASE, 0u);   /* 内核上下文不留用户基址 */
```

最后一处是安全边界而不只是整洁：内核 idle 用 ring 0 跑，留着上一个进程的 `%fs`
就能让内核意外解引用一个已经被切出去的地址空间。

### 4 个 TLS API

`TlsAlloc` / `TlsFree` / `TlsGetValue` / `TlsSetValue` 直接读写 TEB 的那 64 个槽，
读写走 `user_copy()`，因为这页属于用户空间：

* 槽 0 保留（镜像自己的 TLS 目录还没有支持），`TlsAlloc` 从 1 开始发索引；
* 发不出去就返回 `TLS_OUT_OF_INDEXES`（`0xFFFFFFFF`），这是程序要自己处理的结果；
* `TlsFree` 会把槽清零，否则同一个索引再发出去时，读到的会是上一个持有者的指针；
* 读一个从未分配的索引返回 0，和 Windows 一致。

`TlsAlloc` 的位图是**进程级**的：本层一个 Win32 进程，多线程调度还没做，不说成线程级。

### 验收程序

`user/win32compat/samples/tlsdemo.c`（控制台，无 CRT，入口 `novus_win32_main`）
开机自检一次，逐项打印并给出 PASS / FAIL：

```
[TLS] TEB checks on %fs
  %fs:0x30 self      = 0x42000000
  %fs:0x60 PEB (clear) = 0x0
  ClientId.ProcessId vs API   ok
  ClientId.ThreadId vs API    ok
  StackBase = 0xa0000000  ok
  StackLimit = 0x9fff0000  ok
[TLS] TlsAlloc / TlsSetValue / TlsGetValue
  first index = 1 / second index = 2
  %fs TLS slot 1 = 0xc0ffee  ok
  %fs TLS slot 2 = 0xbeef    ok
[TLS] TlsFree
  free index reads 0 / %fs slot cleared / TlsFree(0) refused  ok
[SM] GetSystemMetrics  screen = 1024 x 768
[TLS] PASS
```

关键一项是 `%fs TLS slot`：它由程序自己用 `movq %fs:(reg)` 读出来，而不是问内核，
所以它同时证明了三件事——TEB 页真的映射了、`%fs` 真的指向它、`TlsSetValue` 写进去的
值和编译器会读到的是同一个位置。

## 8. 第五块：键盘字符、字体度量、重绘

三件事凑成"程序能对输入作出反应"：

* **`WM_CHAR`。** WM 交下来的 `key` 对文本键是 Unicode 码点，对控制键是
  `ND_KEY_BASE` 以上的 `ND_KEY_*`。`vk_for_char()` / `vk_for_special()` 把它翻成
  `WM_KEYDOWN` 的 VK，字符本身再单独排一条 `WM_CHAR`。字符是**本层**排进队列的，
  而不是像 Windows 那样由 `TranslateMessage` 生成——本层没有保存"上一个 keydown"
  的状态，排在这里可以让没调 `TranslateMessage` 的程序也能收到字符。
* **`GetTextMetricsA`。** 按 `TEXTMETRICA` 的 x64 布局整块 56 字节填出：11 个 LONG
  + 9 个 BYTE。字号是这台合成器唯一有的那一种——16 像素字身、13 像素 ascent、
  8 像素半宽格、CJK 16 像素格、`tmPitchAndFamily = 0x30`（FF_MODERN 定宽）、
  `tmCharSet = 134`（GB2312）。程序据此居中、换行都对得上。
* **`InvalidateRect` 合并重绘。** 只置窗口的 `dirty` 标志，`gui_pull` 在队列空了以后
  统一排一条 `WM_PAINT`：Windows 也合并重复失效，否则每敲一个键就排一条 PAINT，
  队列会被自己填满。矩形参数被忽略——显示列表本来就是整块重画。
* **`GetStockObject`。** 与 `CreateSolidBrush` 同一套"高字节置 1 的颜色"编码，
  所以两种 handle 可以混用；`HANDLE 0` 是空心刷，`FillRect` 收到它就什么都不画。

样例 `window.c` 因此能一边显示 `tm 16/13/8` 的实测值，一边把键入的字符打在窗口里，
退格删字、回车清空——这条链路走的是 `WM_CHAR → InvalidateRect → WM_PAINT →
TextOutA`。

## 9. 第六块：画笔与形状

`FillRect` 只能画盒子；一条墨线、一个空心框和一个椭圆，才让"盒子"变成"画"。所以画笔
和画刷并列，每个形状都以显示列表里的一条记录发出：

* **`nd_ellipse` / `nd_fill_ellipse`。** 合成器原来只会画圆（`nd_circle`），椭圆按
  `x = rx*sqrt(1-(y/ry)^2)` 求出每行的半宽，用整数开方 `isqrt_i32()` 算，不查表；
  半径 3 像素的椭圆也是闭合的。
* **WM 的三个新 op。** `NWM_OP_LINE`（(x,y)→(x+w,y+h)）、`NWM_OP_ELLIPSE`（圆心 +
  两个半径）、`NWM_OP_ELLIPSE_FILL`。WM 对不认识的 op 仍然忽略，前向兼容不变。
* **`Rectangle` / `Ellipse` 是"刷 + 笔"两条记录。** Windows 画矩形会先填再用笔画边，
  本层照做：先发 `FILL`/`ELLIPSE_FILL`，再发 `FRAME`/`ELLIPSE`，颜色分别取当前
  画刷和当前画笔。
* **画笔是真的对象。** `CreatePen` 返回 `0xFD000000 | (槽+1)`，`SelectObject` 按句柄
  高字节区分画笔/画刷/空心刷（0），`DeleteObject` 只真的释放画笔槽——别的对象是一个
  颜色，删不删都不占资源。
* **`SetBkMode` / `SetBkColor` / `SetPixel` / `PatBlt`。** 背景模式默认是 TRANSPARENT
  （和 Windows 一致），`PatBlt` 只接受 `PATCOPY`、`WHITENESS`、`BLACKNESS` 三种光栅
  操作，其它返 0 并置 `ERROR_INVALID_PARAMETER`——猜一个近似值等于画错。
* **`PostQuitMessage` 真的结束循环。** 退出码存进进程状态，`GetMessageA` 在队列空时
  把 `WM_QUIT` 连同 `wParam` 写进 `MSG` 再返回 0，所以"看返回值"和"看 msg.message"
  两种循环都能退出。
* **`SetWindowTextA` / `GetWindowTextA`。** 标题写在 WM 的窗口标题栏里（`nwm_owner_title`），
  本地同时留一份，读取不再绕一趟 IPC。

样例 `window.c` 在窗口底部多画一块面板：`PatBlt` 色带、`MoveToEx` + 两段 `LineTo` 的
折线、一个矩形、一个椭圆、16 个 `SetPixel` 点，画笔成对创建/选中/删除。

## 10. 第七块：消息循环的其余部分

前六块让程序"能开窗、能画、能收字符"，这一块让它"像一个 Windows 程序那样跑循环"。
其中一条是**修掉一个真实的兼容错误**：

* **`GetMessageA` 的参数顺序。** Windows 是 `GetMessageA(hwnd, lpMsg, min, max)`，
  之前的层把第一个参数当成 `MSG*`，样例也跟着写成 `GetMessageA(&msg, 0, 0, 0)`——
  在自家样例里自洽，但任何真实 Windows 程序编译出来的调用都会把窗口句柄当地址写坏。
  现在层按 Windows 顺序取参，`hwnd` 非 0 时还按它过滤消息；样例同步改回标准写法。
* **`PostMessageA`。** 消息进本进程队列，调用在消息被处理之前就返回，和 Windows 一样。
* **`SendMessageA`。** 同步调用要求窗口过程在调用者的栈帧里运行，而本层的窗口过程由
  程序**自己的**循环派发（是用户态回调，内核无法代调）。所以 `SendMessageA` 以异步方式
  投递并返回 0，这是**写在文档里的偏差**，而不是悄悄返回一个假结果：`msgdemo.exe`
  专门断言"返回 0 且消息随后到达"。
* **`SetTimer` / `KillTimer` / `WM_TIMER`。** 4 个槽，周期下限 10 ms（一个 tick）。
  队列空时由 tick 计数补一条 `WM_TIMER`，队列满则顺延而不是丢——和 Windows 的合并
  语义一致。`TIMERPROC` 需要从 tick 路径回调用户态，本层暂时做不到，所以带回调的
  `SetTimer` 返回 0 并置 `ERROR_INVALID_PARAMETER`，而不是给一个永远不响的定时器。
* **`WM_KEYUP`。** 输入源只报按下，本层在按下/字符之后立刻补一条 `WM_KEYUP`。
  依赖"按下-抬起成对"的热键程序因此能工作，代价是抬起比真实键盘早一个消息。
* **坐标与指针。** `GetWindowRect` 报的是窗口**被放置**的矩形（`nwm_owner_rect`），
  不是调用者请求的那一个：被夹回屏幕的窗口位置会变，报请求值会让 `ClientToScreen`
  和屏幕上的像素对不上。客户区原点在 `(x+1, y+32)`——左边 1 像素边框、上面 32 像素
  标题，`ClientToScreen` / `ScreenToClient` 互为逆运算。`GetCursorPos` 读的是合成器
  那块软件指针的真实位置，不是猜的。
* **`GetWindowLongPtrA` / `SetWindowLongPtrA`。** 只实现 `GWLP_USERDATA`（-21），
  这是程序唯一按约定存放自己数据的地方；别的索引返回 0，不假装有值。

## 11. 第八块：文字排版

* **`GetTextExtentPoint32A`。** 量的是合成器真正画字时用的那张步进表
  （`nd_text_width_n`）：载入 TrueType 面时按字形步进，没有载入时按内建点阵
  （半角 6 / 全角 16）。先前按 `nd_ttf_advance` 累加，没有字体时量出 0 宽度，
  量出来和画出来不一致——这是 `msgdemo.exe` 抓出来的，样例现在断言
  "M × 4 = 4 格"、"一个字 = 2 格"的**关系**而不是某个常数。
* **`SetTextAlign` / `GetTextAlign`。** `TA_CENTER` / `TA_RIGHT` 会把原点从字符串左边缘
  移到中心或右边缘，`TextOutA` 因此不用先量再画。
* **`DrawTextA`。** 按 `\n` 分行，`DT_CENTER` / `DT_RIGHT` / `DT_VCENTER` 生效；
  **词间换行（`DT_WORDBREAK`）没有实现**——传了这个标志但没有换行符时，得到的是单行，
  文档写明而不是装作支持。
* **`GetTextFaceA`。** 载入了 TrueType 面就返回它的家族名（`nd_ttf_family()`）；
  没有载入时返回 `Novus Bitmap`——那是此刻真正在画字的那套点阵，而不是一个
  并不存在的字体名。
* **`SetTextCharacterExtra`。** 字间距参与测量与绘制，两边用同一个函数，所以量出来和
  画出来永远一致。

## 12. 第九块：文件与库

十三支 API：`CreateFileA`、`ReadFile`、`CloseHandle`、`GetFileSize`、
`GetFileAttributesA`、`DeleteFileA`、`CreateDirectoryA`、`FindFirstFileA`、
`FindNextFileA`、`FindClose`、`LoadLibraryA`、`GetProcAddress`、`FreeLibrary`。

* **句柄怎么编。** 文件句柄是 `0x22000000 | 任务 fd 槽`，枚举句柄是
  `0x23000000 | 槽`，库句柄是 `0x5E000000 | 序号`。三套位段写在同一个
  `trap_frame_t` 返回值里，`WriteFile` 因此可以按高位分辨"这是文件还是控制台"，
  程序也不会把枚举句柄和文件句柄搞混——`fileio.exe` 专门断言这三个前缀。
* **路径。** 相对路径补成 `/name`，绝对路径原样；解析交给 `vfs_resolve_path` /
  `vfs_resolve_parent`，删除与建目录都用"父目录 + 叶子名"的后端签名，和文件系统
  适配层一致。
* **`CreateFileA` 的 creation 语义。** `CREATE_NEW` 撞上已存在的名字**返回失败**
  （`ERROR_FILE_EXISTS`），不是打开旧文件；`CREATE_ALWAYS` / `OPEN_ALWAYS` 在
  父目录里新建；`TRUNCATE_EXISTING` 保留文件、清空字节；`OPEN_EXISTING` 缺文件
  返回 `ERROR_FILE_NOT_FOUND`。第一条是 `fileio.exe` 抓出来的真错误，已修。
* **枚举要先落盘。** 后端把目录块放在写回缓存里，同进程刚建的文件在"块级"
  枚举里还看不见，所以 `FindFirstFileA` / `FindNextFileA` 每次枚举前先
  `fs->sync()`。这是本层的责任——Windows 程序从不自己 flush 就能 `FindNext`。
  通配 `*` / `?` 由本层匹配；枚举自然结束返回 0（Windows 置
  `ERROR_NO_MORE_FILES`），`readdir` 真的报错时才打日志。
* **`LoadLibraryA` 只认四个内建库。** `kernel32.dll` / `user32.dll` / `gdi32.dll` /
  `advapi32.dll` 的入口本来就编在镜像里，给句柄是真话；别的名字返回 0 并置
  `ERROR_MOD_NOT_FOUND`，而不是给一个稍后 `GetProcAddress` 会失败的假句柄。
  **从磁盘映射真实 DLL 没有实现**，文档写明。
* **`GetProcAddress` 复用导入那条路。** 它按 `win32_api_lookup` 查名字、再用
  `win32_api_entry` 拿到同一枚 thunk 地址，所以"导入表拿到的函数"和"运行时
  查到的函数"是同一个入口，不会出现两套实现。
* **`CloseHandle`。** 文件槽、枚举槽都清零；控制台与标准句柄 0/1/2 是
  "关不掉也不算错"，返回 1 而不是报错。

验证：`fileio.exe` 在 QEMU/TCG 上全项 PASS（建目录、`CREATE_ALWAYS` 写 13 字节、
`GetFileSize`=13、关句柄后再关失败、`OPEN_EXISTING` 读回同样 13 字节并逐字节比对、
`CREATE_NEW` 被拒、属性 0x80/0x10/失败 0xFFFFFFFF、`/fio/*` 枚举到 `note.txt`、
删除后再查为 INVALID、`gdi32.dll` 拿到句柄且 `TextOutA` 落在 thunk 页、
`ws2_32.dll` 与不存在的名字都被拒）。

## 13. 实机验证记录

在 QEMU/TCG（`-accel tcg -smp 2 -m 3072 -vga std`，串口转存）上跑了两轮：

* **样例盘。** `tlsdemo.exe` / `msgdemo.exe` / `fileio.exe` 依次启动，串口
  `[TLS] PASS`、`[MSG] PASS`、`[FIO] PASS` 三条全绿，无失败项。`msgdemo`
  抓出两处真缺陷（枚举竞态、`CREATE_NEW` 语义）和一处样例自身写穿栈的问题，
  都已修完复测。
* **目视盘。** 桌面只启 `window.exe`，让它的窗口不被设置页挡住。串口
  `[WIN32] paint: begin hwnd=0x40000003` →
  `paint: submitted shapes=0x19 texts=0xA flush->0`，屏幕上是：标题栏
  `Win32 窗口 · Novus 0` 带最小化/最大化/关闭三个按钮、左侧 4 像素红色标记条、
  五行文字（含中文）、一条蓝色折线、一个 120×40 蓝框矩形、一个点线椭圆、
  底部 16 个 `SetPixel` 点组成的点线。二十五条形状、十条文字全部由窗口过程的
  `WM_PAINT` 经 `FillRect` / `TextOutA` / `MoveToEx` / `LineTo` / `Rectangle` /
  `Ellipse` / `SetPixel` 提交进 Novus-WM——第六到第八块的绘制路径在真机管线上是通的。

这一段最初留下的两个缺口，一个换通道后关闭，另一个是层里的真 bug，已修。

* **输入注入：QEMU monitor 通不了，改走 VNC。** `-display none` + PS/2 下
  `mouse_move` 不驱动客户机光标（截图上光标始终停在 `(0,0)`）。改成
  `-vnc 127.0.0.1:NN` 后用 vncdotool 注入，点击能让 Win32 窗口置顶并获得焦点，
  串口出现 `[NWM] key -> win=3 pid=7`。这说明注入通道本身是好的，之前的结论只
  对 monitor 成立。
* **按键到不了窗口过程，根因在层的收件箱判断上。** `ipc_try_recv` 成功时返回的
  是**载荷长度**（24、64 这样），不是 0；`gui_pump` 写的是
  `while (ipc_try_recv(...) == 0)`，于是它只可能捞到"长度为 0 的消息"——真实事件
  一条也捞不到。串口里 `[WIN32] ipc event` 一次都没打印，就是这个原因；而
  `GetMessageA` 的有界等待把这条空队列误读成"该结束了"，一有点击到达就直接
  `return 0`，窗口的 `while (GetMessageA(...))` 于是退出、进程结束——这也是
  "点一下窗口就没了"的真凶。改成 `>= 0` 后，点击、按键都进了窗口过程：串口里
  `ipc event type=0x202`（鼠标）、`0x203`（按键）、`key event reached the layer`
  连续出现，窗口里把键入的 `abcd` 画了出来（`WM_CHAR` → `TextOutA`）。
* **没有 TTF 时缺字的汉字画成空心框。** 点阵表原先只收源码里出现过的非 ASCII
  字符（541 个），Win32 程序运行时自带的中文不在表里。`tools/fontgen/fallback.py`
  改成按 GB2312 一级字库（`0xB0A1..0xD7F9`，3755 字）加标点符号现算，与源码
  字符取并集，共 **4445 个字形**（`build/nd_font_fallback.h` 942 KB）。截图里
  "提供"等字已经正常，不再画 `口`。代价是 `NOVUS.EFI` 大了约 0.9 MB。

## 14. 第十块：窗口端语义与字形

这一块补的是"程序能改自己的窗口、能拿到键盘、能把一段长文排进窄框"，以及把
上一块暴露的收件箱 bug 一起修掉。

* `MoveWindow(hwnd,x,y,w,h,repaint)`：尺寸按**整窗**传入，客户区在层内记为
  `w-2 × h-NWM_TITLE_H-1`（同 `client_rect`），改完置 `dirty` 触发重绘。
  经 `nwm_owner_move` 落到已有 `NWM_REQ_MOVE`。
* `SetFocus` / `GetFocus`：`SetFocus` 走 `nwm_owner_focus`（WM 侧 raise +
  `focus_set`），并由层给旧窗口投 `WM_KILLFOCUS`、新窗口投 `WM_SETFOCUS`；
  `GetFocus` 返回层里记住的 `focus_hwnd`。用户点击窗口时 WM 发 `NWM_EV_FOCUS`，
  同一条 `gui_translate` 分支把层和程序的焦点视图对齐。
* `DrawTextA` 的 `DT_WORDBREAK`：新增 `wrap_len()` 按像素宽度找断点，优先在
  ASCII 空格断，没有空格就按码点断（中文没有空格，必须能断），行尾跳过多余空格。
  返回行数；不带标志时仍是一条。
* `kernel/win32.c` 的收件箱判断与有界等待：`ipc_try_recv` 一律按 `>= 0` 认成
  "收到"，`GetMessageA` 的等待改成 `sched_park(WIN32_MSG_POLL_TICKS)`——漏一次
  唤醒退化成一次 poll，而不是窗口永久失聪。

验收（QEMU/TCG，串口）：`msgdemo.exe` 全项 `ok`、`FAIL count = 0`：

* `MoveWindow(200,150,400,200)` 后 `GetClientRect` = 398×167，
  `GetWindowRect` = (200,150,600,350)；
* `SetFocus` 返回同一 hwnd，`GetFocus` 随后同值；点击前 `GetFocus` 允许是 0 或
  该窗口——Novus-WM 在窗口打开时就把键盘给它，这是窗口管理器策略，不是
  Windows 语义，样例按"两者皆合法"断言；
* `DrawTextA` 无 `DT_WORDBREAK` 返回 1 行，带标志在同一 60×80 矩形内返回 5 行，
  且行数在 2..8 之间；
* 目视：`window.exe` 被点击后置顶、获得焦点，键入 `abcd` 以红色画在窗口里。

仍未做（别当成已做）：模态对话框、`ws2_32` 的传输部分、`crypt32`、
`comctl32`、`ole32`、`shell32`、SEH、PE TLS 目录、多线程、D3D。微信 / QQ
那一层还没碰。

## 15. 第十一块：`ws2_32` 起头

内核其实已经有 TCP 引擎（`net_tcp.c`）、解析器（`net_dns.c`）与 DHCP，但那套
引擎是**一条连接**，不是描述符表；`ws2_32` 还没有描述符层可以落到它上面。所以
这块只做**不需要连接**的那部分，剩下的如实失败，下一块才把描述符接到
`net_tcp_*` 上：

* `WSAStartup` / `WSACleanup`：握手成功，`wVersion` 与 `wHighVersion` = 0x0202，
  `szDescription` = "Novus ws2_32"，`szSystemStatus` = "socket layer not wired"，
  `iMaxSockets` 与 `iMaxUdpDg` 都是 0——没有描述符层，就不给一个看起来能用的数字。
* `WSAGetLastError` / `WSASetLastError`：沿用每个任务已有的错误槽，读写一致。
* `inet_addr` / `inet_ntoa`：真转换。`inet_addr` 逐段解析、越界与段数不足都回
  `INADDR_NONE`（0xFFFFFFFF）；`inet_ntoa` 按网络字节序展开，文本写进 TEB 第二页
  尾部 `+0x1700` 的 16 字节缓冲并返回该地址（Windows 用它自己的线程缓冲，这里用
  线程块尾部，语义上是同一个东西）。
* `socket` / `connect` / `send` / `recv` / `bind` / `listen` / `accept` / `select` /
  `ioctlsocket`：返回失败值（`INVALID_SOCKET` 或 `SOCKET_ERROR`）并把
  `WSAGetLastError` 置为 `WSAENETDOWN`（10050）。
* `closesocket`：没有句柄被发放过，任何值都是 `WSAENOTSOCK`（10038）。
* `gethostbyname`：回 0，错误 `WSAHOST_NOT_FOUND`（11001）；`getaddrinfo` 回
  `EAI_FAIL`（-4，按 x64 符号扩展返回），`freeaddrinfo` 是空操作。
* `LoadLibraryA("ws2_32.dll")` 现在给得出句柄，`GetProcAddress` 能解析出同名
  thunk——动态加载和导入表走同一条路。

验证：`wsdemo.exe` 在 QEMU/TCG 上 `[WS] PASS`，`FAIL count = 0x0`，29 项 `ok`：
握手与 `WSADATA` 各字段、`inet_addr` 三个拒绝用例、`inet_ntoa` 三组往返字符串、
`socket`/`connect`/`gethostbyname`/`closesocket` 的失败值与错误码、`WSASetLastError(0)`
回读，以及经 `GetProcAddress` 取到的 `inet_ntoa` 指针调用结果。


## 16. 第十二块：控件

Windows 的对话框本质是"带子窗口的窗口"，所以这块先把子窗口做出来，而不是先
做 `DialogBox` 的模板解析：没有控件，对话框只是一个空框。

* `CreateWindowExA` 的类名为 `STATIC` / `BUTTON` / `EDIT`（大小写不敏感）且第 9
  个参数（父窗）非零时，不建顶层窗口，而是在父窗里建一个子控件，句柄从
  `0x41000000` 起按槽位编号。控件不经过 Novus-WM：它不是窗口管理器的对象，
  只占父窗客户区里的一个矩形，与 Windows 的 child window 一致。
* 每个控件有自己的文本缓冲（96 字节），`SetWindowTextA` / `GetWindowTextA` 对
  控件同样有效——这也是 `WM_INITDIALOG` 里填初值的做法。
* 绘制由父窗这一帧统一发出：`EndPaint` 之后、`nwm_owner_flush` 之前把控件矩形
  逐个画进去。`STATIC` 不画底（透出父窗背景），`BUTTON` 圆角填充加描边、文字
  居中，`EDIT` 白底加 1 像素边框、有插入符时画一根竖线。
* 命中测试跳过 `STATIC`（它不接收鼠标），从最上层往下找。按下 `BUTTON` 记下
  按压态，同一个控件上松开才发 `WM_COMMAND`：`wParam` 高 16 位是控件 id
  （约定为 `(hwnd & 0xFF) + 1`），低 16 位是通知码 `BN_CLICKED`（0），`lParam`
  是控件句柄。按下 `EDIT` 则把焦点交给它并画插入符。
* 键盘：焦点在 `EDIT` 上时，字符与退格直接改它的缓冲。**这里是明说的差异**：
  Windows 会把按键先给控件再以 `WM_CHAR` 形式投给控件的过程，这里只有父窗一个
  过程，键消息仍投父窗，父窗看不到子控件的键盘。真正的 `SetFocus` 到子控件、
  控件自绘的 `WM_PAINT`，要等子窗口有自己的过程时才能对齐。
* `GetDlgItem(parent, id)`：按 id 反查子控件句柄，校验父窗归属。

验证：`ctrldemo.exe` 建 520×300 窗口加三个控件，控制台断言
`RegisterClassExA`、三个句柄非零且互异、`GetDlgItem(hwnd, 2)` 等于按钮句柄、
控件上的文本往返；画面上点按钮后标签变成 `btn #1`，在编辑框里键入字符可见。

菜单已实现：`CreateMenu` / `CreatePopupMenu` / `AppendMenuA` / `SetMenu` 维护菜单栏，
`TrackPopupMenu` 在客户区弹出右键菜单并支持 `TPM_RETURNCMD`；右键事件产生
`WM_RBUTTONDOWN` / `WM_RBUTTONUP` 与 `WM_CONTEXTMENU`。
`user/win32compat/samples/menudemo.c` 同时自检菜单栏与右键弹出层。
仍未做：`DialogBox` 与 `DLGTEMPLATE`、`SendMessage` 的同步语义、子控件自己的窗口过程、
`comctl32` 的通用控件。

## 17. 第十三块：`MessageBoxA`

消息框是层自己的窗口：没有注册类、没有窗口过程，因为调用它的是内核里的代码，不是程序里的函数指针。

* 调用进来时，层按文本长度算出一个框（`nwm_owner_create`，屏幕居中），把文本按 320 像素宽折成最多 4 行，每行一个 `STATIC`；底部按 `MB_OK` / `MB_OKCANCEL` / `MB_YESNO` 摆 1 或 2 个 `BUTTON`。
* 框建好之后调用者被 **park** 在 `MessageBoxA` 里：syscall 回退两字节、重放，直到结果落定。点按钮或点关闭盒时写 `msgbox_result`，下一次重放把框销毁并带着 `IDOK(1)` / `IDCANCEL(2)` / `IDYES(6)` / `IDNO(7)` 返回。
* 文本里的 `\n` 和 `\r` 是硬换行，连续换行会得到一个空行——与 Windows 一致。
* 键盘：框在屏幕上时，`Enter` 当默认按钮（`IDOK` / `IDYES`），`Esc` 当取消（`IDCANCEL`），与 Windows 的默认按钮行为同向；`IDNO` 没有快捷键。
* 卡住过一次的原因值得记下来：`GetMessageA` 之外没人抽收件箱，模态期间点击被投进队列后一直躺着。所以 `MessageBoxA` 每次重放先自己 `gui_pump` + `gui_timers`，再判结果——模态循环必须自己驱动消息。
* 按钮编号按"框里第几个 `BUTTON`"数位序（位置 0 是 OK/Yes），不能用全局子控件槽位算，否则框内 OK 会落到别的编号上被当成 Cancel。
* 已知差异：`MB_ICON*`、`MB_DEFBUTTON*`、`MB_TOPMOST` 等标志位只被读取 `MB_OK/OKCANCEL/YESNO` 这一组低 4 位，图标与默认按钮没有画；owner 窗口只是在 WM 里被压到下面（没有禁用），因为 WM 还没有"模态"概念。

验证：`ctrldemo.exe` 在建完控件、跑完断言后弹一个 `MB_OKCANCEL` 框，返回 1 或 2 即通过。

## 18. 第十四块：菜单

Windows 的菜单是"菜单条 + 下拉"两层：`CreateMenu` 建条，每个条项用 `MF_POPUP` 挂一个 `CreatePopupMenu` 建的下拉。这里照这个模型做，句柄从 `0x24000000` 起编号，每个菜单最多 8 项。

* `AppendMenuA` 认 `MF_STRING`、`MF_POPUP`、`MF_SEPARATOR`、`MF_CHECKED`、`MF_DISABLED` / `MF_GRAYED`；`GetMenuItemCount` 返回项数。
* 画：菜单栏画在客户区最上面 20 像素（浅灰底 + 一条分隔线），条项按文字宽度排；展开的下拉是白底 + 1 像素边框，勾选项前面画 `*`，禁用项用灰字，分隔项画一条横线。
* 点：这一带的点击被层吃掉，不投给程序。点条项展开/收起下拉；点下拉项发 `WM_COMMAND`（`wParam` 是菜单项 id，通知码为 0，`lParam` 为 0，与 Windows 发菜单命令的形状一致）；点别处收起下拉。

**这是一处明说的差异**：Windows 把菜单条放在非客户区，并从客户区高度里扣掉；Novus-WM 的标题区下面还没有菜单条，所以层把菜单条盖在客户区顶上，程序的 `GetClientRect` 仍然是整块。等 WM 协议加上菜单条（`NWM_REQ_MENU`）才能对齐。程序的绘制不会被裁掉，只是顶部 20 像素被菜单盖住——这一点在文档和样例注释里都写明了。

验证：`menudemo.exe` 建 File/Edit 两条，File 里有 New、分隔线、Exit，Edit 里有勾选的 Undo；断言句柄非零、三次 `GetMenuItemCount`、`SetMenu`/`GetMenu` 往返、`DrawMenuBar`；画面上点 File 展开下拉、点 New 后串口出现 `[MENU] WM_COMMAND id=0x65`，标题变成 `menu: new`。

仍未做：资源里的菜单（`LoadMenuA` 现在能把 `.rsrc` 翻出来，但还不会把 `MENU` 模板变成菜单对象）、`TrackPopupMenu`、快捷键下划线 `&`、`DialogBox` 与 `DLGTEMPLATE`、子控件自己的窗口过程。

## 19. 第十五块：PE 资源

Windows 程序把自己的字符串、图标、对话框模板、菜单模板都塞在镜像的 `.rsrc` 段里，运行时靠一棵三层字典树找：最高层是类型（`RT_STRING`=6、`RT_RCDATA`=10 ……），第二层是名字 / 编号，第三层是语言，叶子是 `IMAGE_RESOURCE_DATA_ENTRY`（给出数据在镜像里的 RVA 和长度）。

这一块把这条链走通：

* `pe_find_resource()` 从 `base + 0x3C` 取 `e_lfanew`，在可选头的数据目录第 2 项（`opt + 112 + 16`，PE32+）拿到资源目录的 RVA，然后按 类型 → 名字 → 首个语言 的次序三级遍历；`IMAGE_RESOURCE_DIRECTORY` 的命名项和数据项计数在 +12/+14，条目从 +16 起每 8 字节一个（名字，偏移），偏移高位为 1 表示"指向下一层目录"。命名项（字符串名）跳过并如实返回找不到——本层只支持整数 id。
* 句柄从 `0x25000000` 起编号，进程内最多 8 个；同一份数据重复查找返回同一个句柄（表里按 RVA 去重）。
* `LoadStringA` 走 `RT_STRING`：字符串表的块号是 `id/16 + 1`，块内序号 `id % 16`；块是一串 UTF-16（先 2 字节长度，再内容），逐项跳过直到目标项；取到后按码点转成 UTF-8 写回调用者的缓冲区，长度 0 就是"这个 id 没有"，超过 255 截断。

**诚实边界**：`.rsrc` 的解析是按 PE 规范做的，但只认"整数 id + 首个语言"，不认命名项、不认多语言回退；`LoadStringA` 只处理 `RT_STRING` 块，不处理 `STRINGTABLE` 之外的字符串资源。

一个容易踩的坑，第一次实现时正是栽在这里：资源树里**所有偏移都是相对"资源目录起点"**，不是相对当前那一层目录。第一层取到 `0x80000020`（指向下一层），第三层再取到的仍是相对同一个起点的绝对偏移；把它当"相对当前节点"叠加一遍就会读进字符串表中间，`FindResourceA` 报找不到。现在的写法是 `res_dir_find()` 只返回去掉最高位的偏移，调用方统一 `base + res_rva + off`。

验证：`resdemo.exe`（源码 + `resdemo.rc`，`windres` 产出 `.rsrc`，`objdump` 确认 `.rsrc` 在 RVA `0x405000`，镜像 11,665 字节）断言 `RCDATA(1)` 长度 17 且内容就是 `.rc` 里那串 `novus-rcd-payload`、`LoadStringA(101)` = `console says hello`、`LoadStringA(202)` = `second line from the table`，以及缺失的 id / 类型如实返回 0。`windres` 不存结尾 NUL，所以长度是 17 而不是 18——这是样例一开始断言写错的地方。

## 20. 合成顺序：按提交顺序重放

下拉菜单在实机上暴露了一个合成器缺陷：程序先画自己的文字（"menus: File / Edit"），再画菜单下拉的白底，可是画面上白底盖不住那行字，字从下拉里透出来。

根因在 `nwm.c` 的 `paint_window()`：它先把窗口的 `shape[]` 全部画完，再去循环 `text[]`，于是同一帧里"后提交的填充"永远压不住"先提交的文字"。Windows 的合成顺序就是程序调用的顺序，这里也必须一样。

改法：`nwm_shape_t` / `nwm_text_t` 各加一个 `seq`，由合成器在收到 `NWM_REQ_SHAPE` / `NWM_REQ_TEXT` 时自增盖章（内核侧 wrapper 不用填，结构体大小由同一份头文件保证一致）；`paint_window()` 改成两个游标归并绘制，谁的 `seq` 小先画谁。这样一帧的绘制次序就等于程序提交的次序，层画的下拉能真正遮住底下程序的字。

验证：`menudemo.exe` 实机点 File，下拉白底把整块区域盖干净（只在弹层右侧还能看到被弹层边界切开的 `item` 字样，那是弹层之外的部分）；点 New 仍是 `[MENU] WM_COMMAND id=65`、标题变 `menu: new`。

## 21. 编辑框的插入点与 Tab 次序

控件能拿文字还不够：编辑框的核心是**插入点**。这一块把 `caret` 从"画不画一根竖线"变成"竖线画在哪"。

* 每个 `EDIT` 控件多一个 `ins`（按字节数索引的插入点）。`SetWindowTextA` 整串替换时把 `ins` 落到末尾，并把越界的 `ins` 夹回字符串长度。
* 点击编辑框时按落点算插入点：从 0 开始累加每个前缀的宽度（`measure_text`），第一个宽度超过 `客户区 x - 文本起点` 的位置就是插入点。点在哪，竖线就站在哪，后面敲的字插在那。
* 键：`←` `→` 移动一格，`Home` / `End` 到两端，`Delete` 删插入点右边一个，`Backspace` 删左边一个，可打印 ASCII 插在插入点（后面的字节整体后移）。
* 这些键被控件吃掉，父窗口的 `WM_KEYDOWN` / `WM_CHAR` 不再到达——与 Windows 的编辑框一致（真正差异是这里没有"子控件自己的窗口过程"，所以是层在替控件处理，而不是控件自己处理）。
* `Tab` 在窗口内可聚焦的控件之间循环（`BUTTON` 与 `EDIT`；`STATIC` 不可聚焦），`Shift+Tab` 反向。次序按创建顺序，与 Windows 的对话框 Tab 次序同向。切走时原控件的竖线熄灭，切到编辑框时竖线亮起并落到文本末尾。
* 老版本把字符一律追加在末尾，`Delete` / `Home` / 方向键全无效果，插入点只有"末尾"这一个位置；`insert` 现在真的能在中间插入。

验证：`editdemo.exe`（窗口 62,93 起 520×300，编辑框、读取按钮各一）。实机点编辑框（插入点落在点中的位置）→ 敲 `d` `e`；`←` 两次 → 敲 `Z` 得到 `abcZde`；`Delete` 删掉后面的 `d` 得 `abcZe`；`Home` 把插入点移到 0；再敲 `xyz` `1` 得 `abcZexyz1`，串口 `[EDIT] read len=9 text=abcZexyz1` 与屏幕标签一致；两次 `Tab` 先走到按钮（竖线熄灭）再回到编辑框（竖线亮起在末尾）。

## 22. `ws2_32` 的描述符层

第十一块只是把 API 面接通：`socket` 一律返回 `INVALID_SOCKET`、传输调用一律 `WSAENETDOWN`。这一块把描述符真的接上内核的 TCP 引擎（`net_tcp.c`）。

* 句柄是 `0x26000000 | 槽位`，进程内 4 个槽；每个槽记着所有者 pid、目标地址和是否已连接。句柄带槽位、并且校验 pid，所以别的进程手里的旧句柄只会得到 `WSAENOTSOCK`。
* `socket(af, type, proto)`：只接受 `AF_INET` + `SOCK_STREAM`，其他族/类型返回 `WSAEAFNOSUPPORT` / `WSAEPROTOTYPE`——不给出用不了的句柄。
* `connect(s, name, len)`：读 16 字节 `sockaddr_in`（端口与地址网络序），调 `net_tcp_open` 发起握手，然后**让调用者 park**、由调度器驱动 engine 的 tick，最多等 80 × 50 ms；成功记 `connected`，失败按原因返回 `WSAENETDOWN`（没有活动网卡或本机没地址）或 `WSAETIMEDOUT`（有链路但对端不应答）。
* `send` 按 256 字节一段从用户空间拷进内核再交给引擎；`recv` 从引擎的接收缓冲拷出并 `consume`，没有数据时返回 `WSAEWOULDBLOCK`；对端 `FIN` 之后返回 0（有序关闭）。
* `closesocket` 关引擎、释放槽位；对已释放的句柄返回 `WSAENOTSOCK`。
* `WSADATA.szSystemStatus` 写的是 "one TCP connection, no listening"，`iMaxSockets` 报 1——引擎就是一条连接，报个好看的数只会骗程序去开它拿不到的 socket。

**诚实边界**：引擎只有一条连接，所以第二个想连的 socket 得到 `WSAENOBUFS`（不是伪造一条连接共享）；`bind` / `listen` / `accept` / `select` / `ioctlsocket` 返回 `WSAEOPNOTSUPP`，因为引擎没有监听路径；`gethostbyname` 仍是空（DNS 解算器存在但还没接进 `HOSTENT` 的返回路径）；UDP 没有。

验证：`wsdemo.exe` 在 `-net none` 的机器上跑完整个自测，`[WS] FAIL count = 0x0`、`[WS] PASS`：句柄落在 `0x26000000`、`SOCK_DGRAM` 被 `10041` 拒、`AF_UNIX` 被 `10047` 拒、未知句柄的 `connect` 得 `10038`、127.0.0.1:80 的 `connect` 得 `0x2742`(=10050，无网卡)、`send`/`recv` 在未连接句柄上得 `10057`、关一次成功再关得 `10038`、`inet_addr`/`inet_ntoa` 与动态 `GetProcAddress` 路径仍通过。
**数据通路没有在实机上验证过**：QEMU 这轮是 `-net none`，没有网卡就没有握手可看；能证的是"API 接上了引擎、失败码说得准确"，不能证"字节真的走通了"。那需要一台有 e1000/virtio 的机器把 `net_tcp` 的握手日志打出来。

## 23. 版本矩阵与兼容模式

这一块不是"多支持几个 API"，是回答一个问题：这份清单上的程序，能不能在这台机器上跑起来，跑不起来的那部分系统有没有说实话。

**`pe_probe` / `pe_describe`（`kernel/pe.c`）**：在正式加载前先把镜像读一遍头，得出机型、魔数、子系统版本、是不是 DLL、入口 RVA。`pe_describe` 把结果写成一句人话，例如：

* `PE32+ x86-64 console EXE, subsystem 6.1 (Windows 7)`
* `PE32 i386 console EXE, subsystem 6.1 (Windows 7)`
* `DOS MZ executable (16-bit real mode)`（只有 MZ 头、没有 `PE\0\0` 签名）

`pe_load` 的拒绝日志统一带这行描述，所以串口里看到的是"被拒的是什么"，不只是"被拒了"。

**清单逐条对应的处理**：

| 清单项 | 当前状态 |
| --- | --- |
| Windows 11 / 10 / 8.1 / 8（64 位） | 可执行：`PE32+` + `AMD64`，这层默认就是 Win11（10.0 / build 22631） |
| Windows 7 / Vista（64 位） | 可执行；旧程序按 `GetVersion` 分支时用兼容模式把版本报成 6.1 / 6.0 |
| Windows 11 … Vista（32 位） | 识别 + 精确拒绝（`PE_E32`），说明"32 位需要 WOW 执行器，没建" |
| Windows XP / 2000 / NT4 / NT3.51 的 32 位 | 同上：识别 + 拒绝；版本号本身可以用兼容模式报成 5.1 / 5.0 / 4.0 / 3.51 |
| 16 位 `.COM` | 识别为 DOS 实模式，按"不是 PE"拒绝 |
| `.dll` 从磁盘加载 | 只认内置 5 个（`kernel32` / `user32` / `gdi32` / `advapi32` / `ws2_32`）；`LoadLibraryA` 对别的名字打印一句"磁盘 DLL 加载没实现"并返回 `ERROR_MOD_NOT_FOUND` |

32 位不是"暂时没空做"，是差一整套执行器：长模式里跑 32 位代码要有兼容代码段（`CS.L=0`、`D=1`）、`iret` 下沉到 32 位栈、stdcall/cdecl 的栈传参与被调者清栈、以及用 `int 0x80` 而不是 `SYSCALL` 进内核（32 位代码下 `SYSCALL` 的 `RCX`/`R11` 语义也不一样）。少任何一样，跑起来的都不是 32 位程序。所以这里的选择是：**认得出来、拒绝得清楚**，而不是拿 64 位路径去糊一个"看起来能跑"。

**兼容模式（注册表两个值）**：`HKLM\Software\Novus\Compat`

* `WinVersion`（`REG_SZ`）：`"10.0"` / `"6.1"` / `"5.1"` / `"4.0"` / `"3.51"`
* `WinBuild`（`REG_DWORD`）：例如 `7601`（Win7 SP1）、`2600`（XP）

`GetVersion` 按 NT 的打包方式返回（低字主版本、高字次版本），`GetVersionExA` 按 `OSVERSIONINFOA` 的四个 DWORD 填（主 4、次 8、build 12、platform 16、CSD 20，总 148 字节）；两处读同一份值，所以同一台机器上不会出现"一个函数说 6.1、另一个说 10.0"。`IsWow64Process` 报 FALSE（这层只跑 64 位镜像，没有 WOW 可言），`GetCurrentProcess` 返回 `-1` 这个进程伪句柄。

**这一块抓到的两个真 bug**：

1. `OSVERSIONINFOA` 的四个字段按 2 字节写：主版本写在 4，次版本写在 6——6 是主版本的高字节。于是调用者读到的次版本是 build 的低字节、build 读到 platform、platform 读到 0。改成按 4 字节各占一个 DWORD。
2. `RegSetValueExA` 的第二参数是**值名**，不是子键（子键由已经打开的 key 句柄携带）。原实现按 `RegQueryValueExA` 的样子把 `R8`（保留字段）当值名读，写进去的名字是空串，于是"写成功"但谁都查不到。样例因此改成先 `RegOpenKeyExA` 再把句柄传给 `RegSetValueExA`。

**32 位样例怎么来的**：这台机器的 mingw 没有 32 位目标（`gcc -m32` 静默返回 1，`C:\msys64\mingw32` 是空壳），所以 `tools/mkpe32.py` 按字节写出两张镜像：`hello32.exe`（PE32、`0x014C`、子系统 6.01、一个 `.text` 里 `jmp $`）和 `dosdemo.com`（16 位实模式裸代码）。它们不是"假装跑得起来"的道具，正是要被拒绝的输入。

**验证（`/tmp/serial-t42.log`）**：

* `[PE] hello32.exe: rejected (0xFFFFFFFFFFFFFFF9) PE32 i386 console EXE, subsystem 6.1 (Windows 7) - 32-bit needs the WOW executor, not built`
* `[PE] dosdemo.com: rejected (0xFFFFFFFFFFFFFFFF) not a PE image`
* `verdemo.exe` 全部通过：默认 `10.0 build 22631 platform 2 GetVersion=0xa`；改成 `6.1` 后 `6.1 build 22631 GetVersion=0x10006`；改成 `5.1` + build 2600 后 `5.1 build 2600 GetVersion=0x10005`；恢复默认后又回到 `10.0 / 22631`；`IsWow64Process = 0`；最后 `[VER] PASS`。
* 同一轮里 `[PE] selftest PASS`：合成镜像（裸 MZ 头、i386 PE）也在自测里被拒过一遍。

**仍然没做到的**（照旧照实写）：

* 32 位执行器（WOW）：没有。
* 从磁盘加载真实 DLL、导出表解析、跨模块 IAT 绑定：没有。
* 16 位 NE/LE 与 `.COM` 的执行：没有；`.COM` 只被识别成"不是 PE"。
* `GetVersionExW`、`VerifyVersionInfo`、manifest 里的 `<compatibility>` 段：没有；兼容模式走的是注册表这两个值。

