# M9 Win32 兼容层原型 — 实施计划

## 1. 目标与范围

README 第 19.1 节里程碑清单第 464 行把 **M9 定义为「Win32 兼容层原型」**（第 14 部分：EXE 解析、Win32 API 子集、注册表模拟、独立权限组）。第 444 行备注中的「M9 已实现：Win11 桌面 / 中文」是另一条已完成的支线，本计划不再改动它，只在文档中区分措辞。

本次交付一个**可被用户看到并验收**的最小原型：

1. 内核内自研 PE32+（x86\_64）解析与加载器，把 EXE 映射进独立地址空间。
2. Win32 x64 ABI 的调用门（thunk），实现 15 个控制台 / 注册表 API。
3. 注册表模拟：扁平键值表，持久化到 NovusFS 根目录的 `/registry.cfg`。
4. 兼容层进程与原生 NUC 进程隔离：`task_t.abi` + 系统调用白名单。
5. 验收入口：文件管理器新增 `/exe` 标签页，列出内嵌的 `hello.exe`，选中后点「运行」。

明确不在本次范围：Win32 窗口 / GDI / 文件 API、从磁盘按路径加载任意 EXE、真实 CAP 权限引擎（`security/` 目前只有 NKM）。

## 2. 现状分析

| 能力             | 现状                                                                    | 位置                                                          |
| -------------- | --------------------------------------------------------------------- | ----------------------------------------------------------- |
| ring-3 任务创建    | 已有 `sched_create_user`，每任务独立 CR3、64 KiB 内核栈                           | `kernel/sched.c:361-408`、`kernel/sched.h:82-83`             |
| 用户地址空间         | `vm_space_create` / `vm_user_map` / `vm_translate` / `vm_user_access` | `kernel/vm.h:53-73`                                         |
| 用户栈 + 返回跳板范例   | NUC 加载器 `map_pages`、`0xA0000000` 栈顶、`0x90000000` 跳板                   | `kernel/nuc.c:36-39`、`115-148`、`271-312`                    |
| 系统调用表          | 编号 1–19、60，`g_syscalls[]`                                             | `kernel/trap.c:30-50`、`649-670`                             |
| trap 帧寄存器      | `trap_frame_t` 含 `rdi/rdx/r8/r9/r10/rip/rsp`                          | `kernel/idt.h:24-30`                                        |
| syscall 入口     | 保存用户 RSP，RCX/R11 被 `syscall` 覆盖                                       | `arch/x86_64/kernel/trap.S:253-267`                         |
| 用户内存拷贝         | `user_copy(cr3, va, buf, len, to_user)`                               | `kernel/trap.c:162-200`                                     |
| 内嵌应用表          | `g_apps[]` 6 项，blob 由 `xxd -i` 生成                                     | `kernel/apps.c:23-30`、`Makefile:275-304`                    |
| sysinfo 虚拟目录   | `/apps` `/dev` `/proc`，每页 3 条                                         | `kernel/sysinfo.h:64-74`、`kernel/sysinfo.c:54-65`、`140-188` |
| 文件管理器          | 4 标签，运行按钮只接受 `KIND_APP`                                               | `apps/src/bin/fileman.rs:154-164`、`212`、`268-292`、`336-348` |
| 注册表 / PE / CAP | **不存在**                                                               | 全仓库无 `pe.c`、`win32.c`、`registry`                            |

结论：不需要新增系统调用或 IPC 协议。复用 `apps_launch_name` → `sched_create_user_ex` 的既有路径，只需在 `apps.c` 增加一个 Win32 表。

## 3. 总体设计

### 3.1 执行路径

```
文件管理器 /exe 标签
  → sinfo::spawn_name("hello.exe")            (REQ_SPAWN_NAME 0x030E，已存在)
  → sysinfo.c req_spawn_name
  → apps_launch_name("hello.exe")
  → apps_find 未命中 → apps_win32_find 命中
  → apps_win32_launch → pe_load()
       ├─ 新建 vm_space，映射头/节/栈/跳板/argv
       ├─ 应用 DIR64 重定位（加载基址 0x50000000）
       ├─ 解析导入表，为每个 API 写入 thunk，填 IAT
       └─ sched_create_user_ex(..., TASK_ABI_WIN32)
  → EXE 在 ring 3 执行；Win32 API 调用走 `syscall` (40)
  → trap.c 白名单 → win32_syscall()
```

### 3.2 Thunk ABI（关键正确性点）

`syscall` 会把返回地址写进 **RCX**、RFLAGS 写进 **R11**，而 Win64 第 1 个参数正在 RCX。所以 thunk 必须在 `syscall` 之前把 RCX 移到 RDI：

```
offset 0 : 48 89 CF                    mov %rcx, %rdi   ; arg0 -> rdi
offset 3 : 41 BA <api_id:imm32>        mov $id, %r10d   ; API 编号
offset 9 : B8 28 00 00 00              mov $40, %eax    ; SYS_WIN32
offset 14: 0F 05                       syscall
offset 16: C3                          ret
```

共 17 字节。参数读取约定：

| 参数   | 来源                                           |
| ---- | -------------------------------------------- |
| arg0 | `trap_frame_t.rdi`（thunk 已从 rcx 移入）          |
| arg1 | `trap_frame_t.rdx`                           |
| arg2 | `trap_frame_t.r8`                            |
| arg3 | `trap_frame_t.r9`                            |
| arg4 | 用户栈 `frame.rsp + 0x28`（返回地址 8 + shadow 0x20） |
| arg5 | 用户栈 `frame.rsp + 0x30`                       |

每个 API 编号只生成一个 thunk（去重），最多 16 个，映射到固定页 `0x60000000`。

`Sleep` 用 park 重放实现：`sched_park(WIN32_SLEEP_PORT, ticks)` 后 `f->rip -= 2`；唤醒时 `wait_err == IPC_ETIMEDOUT`，入口处检查并清空即视为睡眠完成。tick 周期由 `kernel/lapic.c:46` 校准为 10 ms，故 `ticks = max(1, (ms+9)/10)`。

### 3.3 PE 映射与重定位

固定加载基址 `WIN32_IMAGE_BASE = 0x50000000`，栈顶 `WIN32_STACK_TOP = 0xA0000000`（64 KiB），跳板 `WIN32_TRAMPOLINE = 0x90000000`，thunk 页 `WIN32_THUNK_VA = 0x60000000`，argv 页 `WIN32_ARGV_VA = 0x70000000`。

加载基址与首选基址不同，因此**强制要求** **`.reloc`** **存在**（`IMAGE_FILE_RELOCS_STRIPPED` 置位则拒绝），只接受 `IMAGE_REL_BASED_DIR64 (10)` 与 `IMAGE_REL_BASED_ABSOLUTE (0)`。

解析全部按文件偏移进行（不用 packed 结构体），校验清单：

* `MZ`、`e_lfanew` 在 `[0x40, size-24]`、签名 `PE\0\0`；

* `Machine == 0x8664`、`Magic == 0x20B`、`SizeOfOptionalHeader >= 160`、`NumberOfRvaAndSizes >= 6`；

* `SectionAlignment >= 0x1000` 且 4 KiB 对齐；`NumberOfSections ∈ [1, 32]`；

* `SizeOfHeaders <= size`；`WIN32_IMAGE_BASE + SizeOfImage <= 0x60000000`；

* 节：`VirtualAddress` 页对齐、`PointerToRawData + SizeOfRawData <= size`、节区间互不重叠（含头部页）；

* `TLS` 目录 (9) 与 Delay Import (13) 必须为 0；

* 导入按名称，按序号导入直接拒绝；未知 DLL/函数拒绝并打印 `[PE] unknown import dll!func`；

* 入口 RVA 必须落在带 `IMAGE_SCN_MEM_EXECUTE (0x20000000)` 的已映射区间内。

权限：头部与节映射 `VM_PROT_USER | VM_PROT_WRITE`，可执行节再加 `VM_PROT_EXEC`；栈 `USER|WRITE`；跳板 `USER|WRITE|EXEC`。

RVA 读写通过 `pe_read`/`pe_write` 逐字节 `vm_translate`（镜像小，重定位/导入条目有限，代价可接受）。

### 3.4 权限隔离（原型级）

`task_t` 新增 `abi`。`trap.c` 在 `syscall_dispatch` 入口检查：

```c
if (t != NULL && t->abi == TASK_ABI_WIN32 &&
    f->rax != SYS_WIN32 && f->rax != SYS_EXIT) {
  /* 只记录一次，返回 -1 */
}
```

即 Win32 进程只能发 `SYS_WIN32 (40)` 与 `SYS_EXIT (60)`，无法调用 IPC、文件、电源、安装器等任何原生调用。这就是 README 第 14 部分第 5 条「独立权限组」在当前代码基础上的最小可行实现。

### 3.5 注册表模拟

```c
#define WIN32_REG_MAX_ENTRIES 16
typedef struct {
  char   key[48];      /* 例 "HKCU\\Software\\Novus" */
  char   value[40];
  UINT32 type;         /* 1 = REG_SZ, 4 = REG_DWORD */
  UINT32 len;
  UINT8  data[72];
} win32_reg_entry_t;                       /* 168 字节 */

typedef struct {
  UINT32 magic;        /* 'NVRG' */
  UINT32 version;      /* 1 */
  UINT32 count;
  UINT32 pad;
  win32_reg_entry_t e[WIN32_REG_MAX_ENTRIES]; /* 2704 字节 */
} win32_reg_file_t;
```

* 预定义句柄：`HKEY_CURRENT_USER = 0x80000001` → 前缀 `HKCU`，`HKEY_LOCAL_MACHINE = 0x80000002` → `HKLM`。

* `RegOpenKeyExA` 组合「前缀 + `\` + SubKey」存入 8 项打开表，返回 `0x1000 + idx`。

* 首次初始化时若表为空，写入种子：`HKCU\Software\Novus` / `Greeting` = `"Hello from the Novus registry"`（REG\_SZ）。

* `vfs_root_persistent()` 为真时：启动读 `/registry.cfg`（校验 magic/version），每次 `RegSetValueExA` 后写回；live 只读根时保持内存态并打印 `[WIN32] registry: in-memory only`。

* 进程退出时 `win32_owner_exit(pid)` 释放其打开表项。

### 3.6 Win32 API 子集（15 个）

| id | 名称                 | DLL      | 说明                             |
| -- | ------------------ | -------- | ------------------------------ |
| 0  | `ExitProcess`      | kernel32 | `sched_exit_current(code)`     |
| 1  | `GetStdHandle`     | kernel32 | -11→1，-12→2，-10→0              |
| 2  | `WriteFile`        | kernel32 | 经 `[WIN32] `  前缀输出，写回 DWORD 计数 |
| 3  | `WriteConsoleA`    | kernel32 | 同上                             |
| 4  | `Sleep`            | kernel32 | park + RIP 回退重放                |
| 5  | `GetTickCount64`   | kernel32 | `sched_ticks() * 10`           |
| 6  | `GetTickCount`     | kernel32 | 取低 32 位                        |
| 7  | `GetLastError`     | kernel32 | `t->win32_last_error`          |
| 8  | `SetLastError`     | kernel32 | 写入并返回 0                        |
| 9  | `GetCommandLineA`  | kernel32 | 返回 `WIN32_ARGV_VA`             |
| 10 | `GetModuleHandleA` | kernel32 | 返回 `t->win32_image_base`       |
| 11 | `RegOpenKeyExA`    | advapi32 | 5 参数，arg4 读栈                   |
| 12 | `RegQueryValueExA` | advapi32 | 6 参数，arg4/arg5 读栈              |
| 13 | `RegSetValueExA`   | advapi32 | 6 参数，arg4/arg5 读栈              |
| 14 | `RegCloseKey`      | advapi32 | 释放打开表项                         |

输出每次最多 64 字节（受 `kputs` 行缓冲限制），文档中注明。

## 4. 文件级改动（按实施顺序）

### 4.1 新增 `kernel/pe.h` / `kernel/pe.c`

* `int pe_load(const char *name, const UINT8 *img, UINT32 size);`

* `int pe_selftest(void);`

* 内部：`pe_map()`（复用 `nuc.c::map_pages` 的写法，本文件私有）、`pe_read/pe_write`、`pe_img_ptr`（文件偏移 → 常量指针）、`pe_validate_header`、`pe_map_sections`、`pe_relocate`、`pe_imports`、`pe_stack`。

* 错误码沿用负数风格：`PE_ENOTPE -1`、`PE_EMACHINE -2`、`PE_EFORMAT -3`、`PE_ENOMEM -4`、`PE_EIMPORT -5`、`PE_ERELOC -6`。

* 成功日志：`[PE] hello.exe: <n> sections, base 0x50000000, entry 0x..., imports <n>`。

* `pe_selftest()`：构造两个非法镜像（无 PE 签名、Machine=0x14C），断言 `pe_load` 返回负值；通过打印 `[PE] selftest PASS`。校验失败发生在建任务之前，不会占用任务槽。

### 4.2 新增 `kernel/win32.h` / `kernel/win32.c`

`win32.h`：

```c
#define SYS_WIN32 40u
#define WIN32_IMAGE_BASE   0x0000000050000000ULL
#define WIN32_THUNK_VA     0x0000000060000000ULL
#define WIN32_ARGV_VA      0x0000000070000000ULL
#define WIN32_TRAMPOLINE   0x0000000090000000ULL
#define WIN32_STACK_TOP    0x00000000A0000000ULL
#define WIN32_STACK_MIN    0x0000000000010000ULL
#define WIN32_SLEEP_PORT   0xFFFFFFFFFFFFFFFFULL
#define WIN32_THUNK_LEN    17u
#define WIN32_API_COUNT    15u

void win32_init(void);
void win32_syscall(trap_frame_t *f);
void win32_owner_exit(UINT64 pid);
int  win32_api_lookup(const char *dll, const char *func);
void win32_thunk_bytes(UINT32 api_id, UINT8 out[WIN32_THUNK_LEN]);
```

`win32.c`：API 分派、注册表、`win32_init`（加载/种子/保存）、thunk 编码、字符串比较（大小写不敏感）。

### 4.3 修改 `kernel/sched.h` / `kernel/sched.c`

* `SCHED_MAX_TASKS 8 → 10`（现有常驻：idle、spin、5 个桌面应用、session；再留 hello.exe 与余量）。

* 新增 `#define TASK_ABI_NUC 0` / `#define TASK_ABI_WIN32 1`。

* `task_t` 增加 `UINT8 abi; UINT32 win32_last_error; UINT64 win32_image_base;`。

* 新增 `sched_create_user_ex(name, space, entry, user_rsp, abi)`；原 `sched_create_user` 变成 `TASK_ABI_NUC` 的包装（`nuc.c` 无需改动）。

* `sched_exit_current` 在 `abi == TASK_ABI_WIN32` 时调用 `win32_owner_exit(pid)`（`#if CONFIG_NOVUS_WIN32`）。

### 4.4 修改 `kernel/trap.c`、`kernel/kernel.h`

* `trap.c`：`#include "win32.h"`，`#define SYS_WIN32 40u`，`g_syscalls[]` 增加 `{SYS_WIN32, "win32", win32_syscall}`；`syscall_dispatch` 增加 3.4 的 ABI 白名单。

* 把 `user_copy` 的 `static` 去掉，原型放进 `kernel/kernel.h`：

```c
/* 通过任务页表拷贝用户内存（trap.c）。 */
int user_copy(UINT64 cr3, UINT64 va, void *buf, UINT64 len, int to_user);
```

### 4.5 新增 `user/win32compat/samples/hello.c`

* 不包含 `<windows.h>`，自行 `__declspec(dllimport)` 声明 6 个用到的 API（避免 CRT 依赖歧义）。

* 入口 `void novus_win32_main(void)`，流程：

  1. `GetStdHandle(-11)` → `WriteFile("Hello from Win32 on Novus\n")`
  2. `RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Novus", ...)`
  3. `RegQueryValueExA(key, "Greeting", ...)` → 输出读到的字符串
  4. `RegSetValueExA(key, "Runs", REG_DWORD, &runs, 4)`（`runs = GetTickCount()`）
  5. `RegCloseKey`、`Sleep(200)`
  6. `WriteFile("win32 layer done\n")`、`ExitProcess(0)`

* 即使入口返回也会落到跳板 `0x90000000`（等价 `ExitProcess(rax)`）。

### 4.6 修改 `Makefile`、`Kconfig`、`configs/novus_defconfig`

`Makefile`：

```make
WIN32_SRC    := user/win32compat/samples/hello.c
WIN32_BIN    := out/win32/hello.exe
WIN32_BLOB_H := build/win32_blobs.h

$(WIN32_BIN): $(WIN32_SRC) | out
	mkdir -p $(@D)
	$(CC) -O2 -ffreestanding -fno-stack-protector \
	    -fno-asynchronous-unwind-tables -mno-stack-arg-probe \
	    -nostdlib -nostartfiles \
	    -Wl,--subsystem,console -Wl,--image-base,0x400000 \
	    -Wl,--dynamicbase -Wl,--disable-auto-import \
	    -Wl,-e,novus_win32_main -o $@ $< -lkernel32 -ladvapi32

$(WIN32_BLOB_H): $(WIN32_BIN) | build
	@{ echo '/* Generated by the build: M9 Win32 sample images. */'; \
	   echo '#ifndef NOVUS_WIN32_BLOBS_H'; echo '#define NOVUS_WIN32_BLOBS_H'; \
	   xxd -i -n k_app_hello_exe $(WIN32_BIN); \
	   echo '#endif'; } > $@

build/kernel_apps.o: $(APPS_BLOB_H) $(WIN32_BLOB_H)
```

* `SRC` 增加 `kernel/pe.c`、`kernel/win32.c`。

* `CFLAGS` 增加 `-DCONFIG_NOVUS_WIN32=$(if $(filter y,$(CONFIG_NOVUS_WIN32)),1,0)`；`banner` 增加 `win32=$(CONFIG_NOVUS_WIN32)`。

* `Kconfig`：`config NOVUS_WIN32`，`default y`，`depends on NOVUS_SCHED`。

* `configs/novus_defconfig`：`CONFIG_NOVUS_WIN32=y`。

### 4.7 修改 `kernel/apps.h` / `kernel/apps.c`

* 新增：

```c
UINT32       apps_win32_count(void);
const char  *apps_win32_name(UINT32 i);
UINT32       apps_win32_size(UINT32 i);
int          apps_win32_find(const char *name);
int          apps_win32_launch(UINT32 i);
```

* `apps.c` 增加独立表 `g_win32[]`（不并入 `g_apps`，避免改变 `/apps` 与既有 spawn 下标）：

```c
static const app_t g_win32[] = {
    {"hello.exe", k_app_hello_exe, &k_app_hello_exe_len},
};
```

* `apps_win32_launch`：检查登录、按名字防重、打印 `[APPS] launching hello.exe (N bytes)`、调用 `pe_load`。

* `apps_launch_name`：先 `apps_find`，未命中再 `apps_win32_find` → `apps_win32_launch`。

* `apps.h` 的 `APPS_MAX` 保持 6（仅描述 NUC 表）。

### 4.8 修改 `kernel/kmain.c`

在 `install_init();` 之后、`nd_demo_init()` 之前：

```c
#if CONFIG_NOVUS_WIN32
  win32_init();
  (void)pe_selftest();
#endif
```

（`win32_init` 需要 VFS 根已就绪，`install_init` 之后的时序满足。）

### 4.9 修改 `kernel/session.c`

`desktop_app()` 增加 `text_equal(name, "hello.exe")`，使注销时兼容层进程一并结束。`desktop_start()` 保持只启动索引 0–2，不自动拉起 `hello.exe`。

### 4.10 修改 `kernel/sysinfo.h` / `kernel/sysinfo.c`

* 头文件：`#define SYSINFO_DIR_EXE 3u`、`#define SYSINFO_KIND_EXE 4u`。

* `sysinfo.c`：`dir_count()` 增加 `SYSINFO_DIR_EXE → apps_win32_count()`；`req_files` 的 switch 增加分支，填 `apps_win32_name/size`，`kind = SYSINFO_KIND_EXE`。

### 4.11 修改 `apps/src/sinfo.rs` / `apps/src/bin/fileman.rs`

`sinfo.rs`：

```rust
pub const DIR_EXE: u8 = 3;
pub const KIND_EXE: u8 = 4;
```

`fileman.rs`：

* 标签数组改为 `["/apps", "/dev", "/proc", "/exe", "磁盘"]`，5 个：

  * 绘制：`x = 88 + i * 64`，宽 60；`on` 判定中磁盘下标由 3 改为 4。

  * 点击：`for i in 0..5i16`，命中区间 `88+i*64 .. 148+i*64`；`st.disk = i == 4`；其余 `st.dir = i as u8`。

* 行颜色/后缀：`KIND_EXE => ui::accent()`，值列显示 `ui::fmt_size(r.size, ...)`。

* `can_run` 与运行按钮条件改为 `matches!(kind, KIND_APP | KIND_EXE)`（用现有 `==` 写法：`st.rows[st.sel].kind == sinfo::KIND_APP || ... == sinfo::KIND_EXE`）。

* 运行仍走 `sinfo::spawn_name(sys_port, name)`。

### 4.12 文档与里程碑状态

* 新增 `docs/M9 Win32 兼容层原型.md`：架构、API 表、thunk 字节、PE 校验规则、注册表格式、验收步骤、限制（无 GUI/文件 API、无任意路径 EXE、无真实 CAP）。

* `README.md`：第 464 行改为 `- M9：Win32兼容层原型 🆗`；在第 463 行后补一条 `> M9 Win32 兼容层原型已实现：...` 备注，并把第 444 行措辞改为「M9 的 Win11 桌面/中文部分已实现」，消除歧义。

* `changes.md`：在 `# Changes` 下新增 `## M9 Win32 兼容层原型`，列出新增文件、API、注册表与验证结果。

## 5. 验证

### 5.1 构建

```
make            # 期望 0 warning，产出 build/win32_blobs.h 与 out/win32/hello.exe
x86_64-w64-mingw32-objdump -h out/win32/hello.exe
x86_64-w64-mingw32-objdump -p out/win32/hello.exe | sed -n '/Import Tables/,/^$/p'
```

检查：`.reloc` 存在；导入表含 `KERNEL32.dll`（WriteFile/GetStdHandle/Sleep/GetTickCount/ExitProcess）与 `ADVAPI32.dll`（Reg\*）。

### 5.2 串口启动日志（`make run`）

* `[PE] selftest PASS`

* `[WIN32] registry: loaded` 或 `in-memory only`

* 手工在文件管理器切到 `/exe`，选中 `hello.exe` → 运行：

  * `[APPS] launching hello.exe (N bytes)`

  * `[PE] hello.exe: N sections, base 0x50000000, entry 0x..., imports N`

  * `[WIN32] Hello from Win32 on Novus`

  * `[WIN32] Greeting=Hello from the Novus registry`

  * `[WIN32] win32 layer done`

  * `[SCHED] task hello.exe exited with code 0x0`

* 任务管理器在运行期间能看到 `hello.exe`。

* 重复点「运行」显示「已在运行」（APPS\_ERUNNING = -100）。

### 5.3 容错

* 关闭 `CONFIG_NOVUS_WIN32` 后 `make` 仍通过（所有调用点有 `#if` 或空表分支）。

* 在 `hello.exe` 镜像中人工破坏节表（临时测试）→ `[PE]` 负值拒绝且内核不崩。

## 6. 风险与假设

| 项                    | 决定                                                     |
| -------------------- | ------------------------------------------------------ |
| 加载基址                 | 固定 `0x50000000`，与首选基址不同，强制走重定位；无 `.reloc` 的 EXE 拒绝     |
| `syscall` 覆盖 RCX/R11 | thunk 先 `mov %rcx,%rdi`，API 号放 R10，已规避                 |
| 任务槽                  | 8 → 10；内核栈池增加 128 KiB BSS                              |
| 注册表持久化               | 仅 `vfs_root_persistent()` 时落盘；live 只读根为内存态             |
| 权限                   | 未实现真实 CAP 引擎，用 ABI + 系统调用白名单模拟；文档明确说明                  |
| 输出长度                 | 单次最多 64 字节，示例程序每行都短于该限制                                |
| tick 精度              | 10 ms/tick（`lapic.c:46` 校准），`Sleep` 向下取整到 tick         |
| 工具链                  | 复用现有 mingw 交叉编译器；`-nostdlib -nostartfiles` 且显式入口，无 CRT |
| 失败降级                 | 任何 PE 拒绝只影响该次启动，内核继续运行（README 第 17 部分）                 |

## 7. 假设

1. 现有 `x86_64-w64-mingw32-gcc` 能生成带 `.reloc` 与导入表的 PE32+（构建时用 objdump 验证；若 `.reloc` 缺失，则在链接行补 `-Wl,--enable-reloc-section`）。
2. `user/win32compat/` 目录尚不存在，需要新建。
3. 文件管理器窗口宽度 416 可容纳 5 个 60 px 标签（88..404）。
4. 本轮不引入新的 IPC 请求号；沿用 `SYSINFO_REQ_SPAWN_NAME`。

