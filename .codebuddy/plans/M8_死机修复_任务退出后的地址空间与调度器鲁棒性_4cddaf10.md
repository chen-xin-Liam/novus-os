---
name: M8 死机修复：任务退出后的地址空间与调度器鲁棒性
overview: 修复 M8 中"关闭应用窗口后系统停机"的问题：反复启动/退出应用会破坏用户地址空间（idle 与新建应用在 0x90000000 / 0x400000 触发保护性缺页），最后一个可运行任务退出后内核在中断上下文里 cpu_halt 死锁。计划先补诊断，再修地址空间回收的根因，最后让调度器在无任务可运行时退回内核空闲上下文而不是停机。
todos:
  - id: audit-page-tables
    content: "[subagent:code-explorer] 复核 vm.c/vm.h/nuc.c 的页表私有化与释放路径，列出共享表写入点"
    status: completed
  - id: add-diagnostics
    content: 在 pmm_free_page 加重复释放告警、[REAP] 补 cr3/pages、halt 前打印任务表快照
    status: completed
  - id: fix-space-ownership
    content: vm_user_map 改为写时克隆 PT/PD，alloc_table 溢出即失败，free_user_leafs 只遍历私有表
    status: completed
    dependencies:
      - audit-page-tables
  - id: sched-idle-fallback
    content: task_pick_next 改按槽位扫描，sched_switch 无任务时切内核 idle 兜底并自愈 idle 任务
    status: completed
    dependencies:
      - add-diagnostics
  - id: churn-regression
    content: 编写 scripts/wm-churn.sh，20 轮启停+关闭全部窗口，断言无缺页且 tick 持续
    status: completed
    dependencies:
      - fix-space-ownership
      - sched-idle-fallback
  - id: rebuild-verify-iso
    content: 全量重建、自检回归、重建 ISO 并从 CD-ROM 跑同一压力脚本
    status: completed
    dependencies:
      - churn-regression
---

## 用户需求

用户运行 M8 桌面后点关闭文件管理器窗口，串口停在：

```
[SCHED] task fileman exited with code 0x0000000000000000
[SCHED] no runnable task, halting
```

之后 `[SCHED] ticks` 不再增长、桌面完全冻结（“死机”）。要求定位并彻底修掉，后续同类问题也必须随发现随修。

## 已复现的故障链

在本地 QEMU 用「关闭 fileman 窗口 → 点任务栏 Files 重启」循环，第 4 轮必现，且日志显示不是简单的“退出处理没写对”：

```
[APPS] launching fileman (0x1668 bytes)
[NUC] fileman: 1 segments, entry 0x400000, stack 0x10000 bytes
[TRAP] #PF err=0x5  rip=0x90000007      ← 已存在的 idle 在跳板页上保护性缺页
[SCHED] task idle exited with code 0xFFFFFFFF
[REAP] idle: 0x29 pages back
[TRAP] #PF err=0x6  rip=0x400000        ← 新 fileman 的代码页不可用
[SCHED] task fileman exited with code 0xFFFFFFFF
[SCHED] no runnable task, halting
```

单次关闭三个窗口（不重启应用）不崩，只有反复 spawn/reap 才触发，说明是**用户地址空间生命周期**缺陷；而“停机”这一步又把一个可恢复的进程故障放大成整机死机。

## 修复目标（缺一不可）

1. **地址空间不再互相踩踏**：重复创建/销毁进程后，任何任务都不得在 `0x400000`（代码）或 `0x90000000`（跳板）上出现保护性缺页；应用可以无限次启停。
2. **调度器永不因进程退出而停机**：没有 READY 任务时退回内核空闲上下文，继续跑时钟（WM、sysinfo、任务栏仍然可用），并能再次拉起应用；`idle` 任务丢失时自动重建。
3. **同类问题可观测**：页表/物理页的重复释放、地址空间泄满、任务表异常都要在串口留下明确告警，而不是静默忽略或直接 halt。
4. 保持既有行为不变：`[IPC]`/`[NWM]`/`[ND]`/NKM 自检仍全 PASS，启动即进桌面，M5/M6/M8 交互不受影响。

## 结论：三个叠加缺陷

### 1（根因）页表克隆只到 PD 层，PT 与内核共享

`kernel/vm.c: vm_space_create()` 拷贝内核 PML4，然后只克隆 `pdpt[0]` 和 `pdpt[0..3]` 里的四个 PD。于是克隆出来的 PD 中，**凡是指向 4 KiB 页表的 `pd[i2]` 项仍指向内核自己的 PT**。

`vm_user_map()` 只在两种情况下动结构：

```c
if (pd[i2] 是 2MiB 大页) { split 成私有 PT }        // 私有 ✓
else if (pd[i2] 不存在)  { alloc_table 私有 PT }     // 私有 ✓
// 缺一条分支：pd[i2] 是"已存在的 4KiB 表指针"时直接写进去 → 改的是内核 PT
```

同一函数里 `pml4[i4] |= PTE_U`、`pdpt[i3] |= PTE_U` 也有同样问题：`pdpt[4..511]` 是共享的内核项。结果是某个进程映射 `0x90000000` 时把**内核 PT 的那一项**改成了自己的跳板页，所有进程（含 idle）都被改；等这个进程退出，`free_user_leafs()` 又遍历所有 `i4<256` 的叶子，把共享内核表里 `PTE_U` 的页 `pmm_free_page()` 并清零——于是 **idle 的跳板页被释放并复用**，它下一次执行就 `#PF err=0x5`（P=1、U 权限不足），随后新建进程的代码页也被同一批被污染的物理页占据，`#PF err=0x6`。

### 2 `alloc_table()` 静默泄漏

`npages >= VM_MAX_TABLE_PAGES(32)` 时只打一句 WARN 就继续使用该表、但不登记，销毁空间时漏放；父项被释放后这些表页仍被引用，之后被 PMM 重新分配即产生别名。应改成失败返回 0（让装载失败并回收），而不是继续跑。

### 3 停机点过于激进

`kernel/sched.c: sched_switch()` 在 `task_pick_next()==NULL && current->state==TASK_EXITED` 时直接 `for(;;) cpu_halt()`，且这一步发生在定时器 IRQ 上下文里（IF 已清），于是时钟停摆、WM/sysinfo 全部停更。即使任务确实全没了，内核也应当退回一个内核态 idle 上下文继续 `sti; hlt`，让 tick 继续跑——这样“最后一个应用退出”不会变成整机死机，任务栏还能把应用再拉起来。另外 `task_pick_next()` 依赖环形链表，一旦链表与槽位不一致就会漏看 READY 任务，应改为按槽位顺序扫描（公平轮转 + 环形链表只作提示）。

## 实施方案

### A. 地址空间：写时克隆（copy-on-write at table level）

在 `kernel/vm.c` 增加 `static UINT64 table_own(vm_space_t *sp, UINT64 phys)`（在 `sp->pages[]` 里查），`vm_user_map()` 每一级都保证“要写的表是本空间私有的”：

- PML4 项存在但指向的表不在 `sp->pages[]` → `clone_table()` 后替换；
- PDPT/PD 同理（覆盖 `pdpt[4..511]` 这类共享内核项）；
- PD 项是 4 KiB 表指针且非私有 → `clone_table()` 后再写叶子；
- 保持“2 MiB 大页拆分”为私有 PT 的现有逻辑；
- `alloc_table()` 溢出处返回 0。

`free_user_leafs()` 因此只需遍历本空间私有结构（共享内核表不再被触碰），并在函数头加注释说明这一前提。`nuc_load()` 的错误路径保持“成功交给任务、失败才 `vm_space_free()`”的现状，不得双释放。

### B. 调度器：内核空闲兜底 + idle 自愈

- 新增 `static void sched_kernel_idle(void)`：在一个静态内核栈上 `sti; hlt` 循环，只让定时器继续跑；`sched_switch()` 在 `task_pick_next()==NULL` 时切到它（`g_current = NULL` 语义要处理干净：tick 里已有 `g_current == NULL` 分支），**不再 halt**；同时打印一次任务表快照（名字/状态/pid/cr3）。
- `task_pick_next()` 改为从 `g_task_seq` 索引开始按槽位扫描 `TASK_READY`，环形链表仅用于 `g_first` 维护；消除“链表与槽位不一致”导致的漏看。
- `sched_tick()` 检测 `idle` 槽位：若 `nuc_create_idle()` 创建过的任务已不在（名字 `idle` 且状态非 FREE/READY 连续 N 个 tick），调用一次 `nuc_create_idle()` 重建，并打日志。

### C. 可观测性

- `pmm_free_page()`：命中 `bitmap==0`（重复释放）时计数并限流打印 `[PMM] WARN double free ...`，退出码不变。
- `[REAP]` 行追加 `space=cr3 pages=n`，便于对齐“释放了谁的页”。

### D. 验证

1. 新建 `scripts/wm-churn.sh`（宿主机驱动）：无头 QEMU + monitor，循环「关 fileman → 任务栏重启」≥20 轮，断言串口无 `#PF`（除启动期 NX 探针）、`[SCHED] ticks` 持续增长、任务表稳定在 5 个、`[PMM] double free` 为 0；
2. 关闭全部三个窗口后仍能继续点击任务栏重启任意应用（这是用户报障场景）；
3. `make` 全量 0 warning，`[IPC]/[NWM]/[ND]` 自检 PASS，`make iso` 后从 CD-ROM 启动复测同一脚本；
4. 回归 M5/M6 交互：shell Apply 点击、WM 标题栏拖动、主题色切换。

### 目录与文件

```
kernel/vm.c        [修改] vm_user_map 写时克隆 PT/PD、table_own 辅助、alloc_table 失败即返回、
                          free_user_leafs 加“只遍历私有表”的注释与断言
kernel/pmm.c       [修改] pmm_free_page 增加重复释放告警（限流）
kernel/sched.c     [修改] sched_kernel_idle 兜底、task_pick_next 按槽位扫描、
                          tick 内 idle 自愈、halt 分支改为快照+兜底
kernel/sched.h     [修改] 声明内核空闲栈/兜底入口（如需）
kernel/nuc.c       [修改] 仅在装载失败时释放空间（保持），日志补 cr3/pages
scripts/wm-churn.sh [新增] 应用启停压力回归脚本
```

## Agent Extensions

### SubAgent

- **code-explorer**
- Purpose: 在动手前用语义检索复核 `vm_space_create` / `vm_user_map` / `free_user_leafs` / `task_reap` 的全部调用点与 `sp->pages[]` 的登记口径，确认没有遗漏的共享表写入路径（例如 HAL 对 framebuffer、NUC 对 trampoline 的映射）。
- Expected outcome: 输出一份“哪些页表项可能被跨空间写入/释放”的清单，作为 A 阶段改动的验收对照。