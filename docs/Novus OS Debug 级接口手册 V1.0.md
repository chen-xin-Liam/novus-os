# Novus OS Debug 级接口手册
> 文档版本：V1.1（对应 Novus OS V1.6-FinalSpec；V1.1 为日志与手工调试全量增补版）
> 用途：开发调试手册，每个接口展开到参数级、错误码级、前置/后置条件、调试检查点；并收录**当前代码库中全部日志的完整解释**与**全部手工 debug 方法**
> 配套：《全部接口总表》《NUC 格式规范》《NKM 格式规范》
> 状态：接口基线冻结；V1.1 新增第 12、13 章（日志全解 + 手工调试全解），新增内容均为对源码现状的实测归纳
> 变更说明：V1.1 增补 12.0「规格与实现差异声明」、12.3「全量日志清单」、13「手工 Debug 方法」；原 V1.0 的接口条目保持不变，便于对照冻结基线

---

## 0. 阅读指南

本手册分三部分：

- **第 1–11 章**：冻结的接口规格（参数、错误码、前置/后置条件、调试检查点）。
- **第 12 章**：日志系统**实现现状**与**全量日志逐条解释**——每一条日志对应的源文件、行号、级别、触发条件、含义、以及看到它之后应该做什么。
- **第 13 章**：**手工 debug 方法全解**——从 `make run` 到串口抓取、QEMU monitor、共享内存快照、自检入口、nsh 诊断命令、主机侧 `.nuc` 工具链、分场景排错流程。

> 重要：**第 12.0 节先读**。规格中描述的 `log_print` / `panic_handler` 等接口与当前源码的实现并不完全一致，本手册同时保留规格定义与实现真相，避免调试时误判。

每个接口条目包含：
1. **原型**：C 函数签名
2. **所属子系统**
3. **调用方 / 被调用方**
4. **前置条件**：调用前必须满足什么（锁、CAP、中断状态、内存）
5. **参数表**：逐字段类型、取值范围、单位、默认值、说明
6. **返回值**：逐情况说明
7. **错误码对照表**：什么情况返回什么错误码
8. **后置条件**：返回后系统状态变化
9. **调试检查点**：开发时应该打日志/断言的位置
10. **使用示例**：最小调用代码
11. **常见 Bug**：易犯错误

---

## 1. 通用错误码（全局复用）

| 值 | 宏名 | 含义 | 触发场景举例 |
|---|---|---|---|
| 0 | NOVUS_OK | 成功 | 一切正常 |
| -1 | NOVUS_EINVAL | 参数无效 | 指针为NULL、长度为0、枚举值越界 |
| -2 | NOVUS_ENOMEM | 内存不足 | 页分配失败、缓冲区不够 |
| -3 | NOVUS_ENOTFOUND | 对象不存在 | 路径不存在、符号未找到、设备不存在 |
| -4 | NOVUS_EPERM | 权限不足 | cap_check() 返回 false |
| -5 | NOVUS_EBUSY | 资源忙 | 锁超时、文件被占用、模块被依赖 |
| -6 | NOVUS_EIO | IO错误 | 磁盘读失败、PCI读写错误 |
| -7 | NOVUS_ECORRUPT | 数据损坏 | SHA256不匹配、CRC32校验失败 |
| -8 | NOVUS_EMODULE | 模块错误 | 签名校验失败、ABI不兼容 |
| -9 | NOVUS_EFULL | 队列满 | IPC队列满、WAL日志满 |
| -10 | NOVUS_EAGAIN | 重试 | 资源暂时不可用 |
| -11 | NOVUS_EOVERFLOW | 溢出 | 段大小越界、文件偏移超出文件长度 |
| -12 | NOVUS_EDEP | 循环依赖 | 模块依赖图成环 |

---

## 2. SMP / AB 双核接口

### 2.1 `smp_shmem_write`

```c
void smp_shmem_write(uint32_t offset, uint64_t val);
```

- **所属**：SMP 启动
- **调用方**：A核(BSP)
- **前置条件**：
  - 共享内存物理页已预留（A核早期分配）
  - 调用者已持有 `smp_shmem_lock()`（如多核心并发写）
  - 本地中断状态：允许开中断
- **参数**：

| 参数 | 类型 | 范围 | 说明 |
|---|---|---|---|
| offset | uint32_t | 0 ~ 4095 | 共享内存内偏移，必须 8 字节对齐 |
| val | uint64_t | 任意 | 要写入的值 |

- **返回值**：无（void）
- **后置条件**：共享内存指定偏移处值更新；CPU 缓存一致性由 `sfence` 保证
- **调试检查点**：
  - 写入后立即读回比对：`assert(smp_shmem_read(offset) == val);`
  - 打印：`log_print(DEBUG, "shmem write off=%u val=%lx\n", offset, val);`
- **常见 Bug**：offset 未对齐导致原子性破坏；忘记持锁导致竞态

### 2.2 `smp_shmem_read`

```c
uint64_t smp_shmem_read(uint32_t offset);
```

- **调用方**：B核(AP)
- **前置条件**：共享内存已映射到 AP 页表
- **参数**：offset 同上
- **返回值**：指定偏移处的值
- **调试检查点**：读取 `bsp_state` 时循环等待，超时 200ms 判定启动失败

### 2.3 `smp_shmem_lock` / `smp_shmem_unlock`

```c
int smp_shmem_lock(void);
void smp_shmem_unlock(void);
```

- **锁类型**：自旋锁
- **自旋上限**：1,000,000 次
- **超时返回**：`NOVUS_EBUSY (-5)`
- **调试检查点**：
  - 加锁前后打日志
  - 超时触发时打印当前锁持有者信息（如实现了锁调试）
- **注意**：锁内禁止调用可能睡眠的函数（此时调度器未启动）

### 2.4 `acore_enter_sleep`

```c
void acore_enter_sleep(void);
```

- **调用方**：A核自检完成后
- **前置条件**：
  - 所有硬件信息已写入共享内存
  - IPI 已发送给所有 AP
  - `bsp_state` 已置 2
- **行为**：进入 `cli; hlt;` 无限循环
- **后置条件**：BSP 不再执行任何指令
- **调试检查点**：进入前打印 "Acore entering sleep"

### 2.5 `smp_wake_ap`

```c
int smp_wake_ap(uint64_t ap_id);
```

- **前置条件**：
  - `bsp_state` 已置 2
  - 已执行 `sfence`
  - AP 启动栈地址已写入共享内存
- **参数**：ap_id = AP 核心编号
- **行为**：发送 IPI 向量 0x80
- **返回值**：0 成功；-ENOMEM 失败
- **调试检查点**：发送后等待 `ap_ready` 标志，超时 200ms → 重启

### 2.6 `smp_ap_signal_ready`

```c
void smp_ap_signal_ready(void);
```

- **调用方**：AP 完成 GDT/IDT/页表初始化后
- **后置条件**：共享内存 `ap_ready = 1`；BSP 检测到后休眠

---

## 3. 内存管理接口

### 3.1 `mm_alloc`

```c
void* mm_alloc(size_t pages);
```

- **所属**：物理内存管理
- **前置条件**：
  - 物理内存管理器已初始化（M1 之后）
  - 调用者不在中断上下文（或使用 GFP_ATOMIC 标志）
- **参数**：

| 参数 | 类型 | 单位 | 说明 |
|---|---|---|---|
| pages | size_t | 4KiB/页 | 要分配的连续物理页数 |

- **返回值**：
  - 成功：内核虚拟地址（已映射）
  - 失败：`NULL`
- **错误码**：返回 NULL 即失败（不通过返回值传错码）
- **后置条件**：
  - 物理页已保留（从空闲位图标记为已占用）
  - 已映射到内核虚拟地址空间
- **调试检查点**：
  - 分配后打印：`log_print(DEBUG, "mm_alloc %zu pages -> %p\n", pages, ptr);`
  - 检查返回值是否对齐：`assert((uint64_t)ptr % 4096 == 0);`
- **常见 Bug**：
  - pages=0 直接 panic
  - 分配后忘记 `mm_free` → 内存泄漏
  - 在中断上下文调用可睡眠版本 → 死锁

### 3.2 `mm_free`

```c
void mm_free(void* vaddr, size_t pages);
```

- **参数**：
  - vaddr：`mm_alloc` 返回的地址
  - pages：必须与分配时一致
- **后置条件**：物理页归还空闲位图；虚拟映射解除
- **调试检查点**：释放后打印地址；检查双重释放（位图标记）

### 3.3 `vm_map`

```c
int vm_map(uint64_t va, uint64_t pa, uint64_t len, uint32_t flags);
```

- **参数**：

| 参数 | 类型 | 单位 | 说明 |
|---|---|---|---|
| va | uint64_t | 字节 | 虚拟起始地址，4KiB 对齐 |
| pa | uint64_t | 字节 | 物理起始地址，4KiB 对齐 |
| len | uint64_t | 字节 | 长度，4KiB 对齐 |
| flags | uint32_t | 位掩码 | bit0=READ bit1=WRITE bit2=EXEC bit3=USER |

- **返回值**：0 成功；负数错误码
- **错误码**：
  - -EINVAL：va/pa/len 未对齐
  - -ENOMEM：页表分配失败
- **调试检查点**：映射后读 CR3 验证 TLB；打印映射范围
- **W^X 原则**：flags 同时置 WRITE 和 EXEC → 拒绝（返回 -EINVAL）

### 3.4 `vm_unmap`

```c
int vm_unmap(uint64_t va, uint64_t len);
```

- 解除映射，释放页表项
- 后置条件：访问该地址触发页错误

### 3.5 `oom_scan`

```c
int oom_scan(proc_info_t* out);
```

- **前置条件**：内存水位 < 10%
- **行为**：遍历所有进程，按评分公式计算分数
- **评分公式（冻结）**：
  ```
  score = rss_bytes * front_weight * age_weight * type_weight
  front_weight:  前台=1.8, 后台=1.0
  age_weight:    <30s=1.0, 30s~5min=0.7, >5min=0.4
  type_weight:   普通=1.0, 系统辅助=0.2
  ```
- **保护列表**（永不参与评分）：init、novus-wm、novus-settings、novus-taskmgr
- **返回值**：0 找到候选进程，写入 out；-ENOMEM 无候选进程可杀
- **调试检查点**：
  - 打印每个进程的 score
  - 打印选中的 victim
- **常见 Bug**：误杀保护列表进程 → 系统崩溃

### 3.6 `oom_reclaim`

```c
int oom_reclaim(uint32_t level);
```

- **参数**：level
  - 0 = 温和：只回收页缓存、目录项缓存
  - 1 = 紧急：回收离屏 FB、NovusFS 冗余缓存
- **回收顺序（冻结）**：文件页缓存 → 目录项缓存 → 图形离屏缓冲 → NovusFS 缓存
- **防抖**：1 秒内最多触发一次全局回收

---

## 4. 调度器接口

### 4.1 `sched_spawn`

```c
int sched_spawn(const char* path, uint64_t caps, pid_t* out_pid);
```

- **前置条件**：
  - 调用者拥有 `sys.proc.ctrl` CAP（创建系统进程）
  - path 指向的 .nuc 文件存在
- **参数**：

| 参数 | 说明 |
|---|---|
| path | .nuc 文件路径，如 "/Novus/bin/novus-settings" |
| caps | 进程初始 CAP 掩码（低64位） |
| out_pid | 输出参数，成功写入新 PID |

- **返回值**：0 成功；负数错误码
- **错误码**：
  - -ENOENT：文件不存在
  - -EINVAL：.nuc 头部校验失败
  - -ENOMEM：无法分配进程地址空间
- **调试检查点**：打印新进程 PID、入口地址、初始栈
- **后置条件**：进程加入就绪队列，等待调度

### 4.2 `sched_ctl`

```c
int sched_ctl(pid_t pid, uint32_t op, void* arg);
```

- **参数 op**：
  - 0 = 暂停进程（SIGSTOP）
  - 1 = 恢复进程（SIGCONT）
  - 2 = 终止进程（SIGKILL）
  - 3 = 调试 attach
- **CAP**：`sys.proc.ctrl`
- **调试检查点**：打印 pid、op、返回值

### 4.3 `sched_exception_report`

```c
int sched_exception_report(pid_t pid, exc_info_t* exc);
```

- **触发时机**：用户态进程触发页错误/非法指令/除零
- **行为**：
  1. 收集寄存器状态到 exc_info_t
  2. 不 panic，不杀进程
  3. 通过 IPC 发消息给 novus-wm
  4. WM 弹出错误对话框
- **exc_info_t 字段**：

```c
typedef struct {
    uint64_t pid;
    uint64_t rip;        // 出错指令地址
    uint64_t cr2;        // 页错误地址
    uint32_t err_code;   // 错误码
    char proc_name[64];
    uint8_t  cause;      // 0=页错误 1=非法指令 2=除零 3=断点
} exc_info_t;
```

---

## 5. 安全/CAP 接口

### 5.1 `cap_check`

```c
int cap_check(proc_cap_t* p, uint32_t bit);
```

- **参数**：
  - p：进程的 CAP 结构指针
  - bit：要检查的位号（0~31）
- **返回值**：
  - 1：拥有该权限
  - 0：没有该权限
- **实现**：
  ```c
  return (p->cap_mask_low >> bit) & 1;
  ```
- **调试检查点**：权限拒绝时打印进程名、请求的 bit 号
- **常见 Bug**：bit 号写错（off-by-one）；忘记检查导致越权

### 5.2 `cap_drop`

```c
int cap_drop(proc_cap_t* p, uint32_t bit);
```

- **行为**：清除指定位（不可逆）
- **用途**：进程初始化完成后主动降权（最小权限）
- **返回值**：0 成功；-EINVAL bit 越界

### 5.3 `nkm_load`

```c
int nkm_load(const char* path);
```

- **前置条件**：调用者拥有 `mod.load` CAP
- **执行步骤（debug 级）**：
  1. 打开文件，读前 128 字节 Header
  2. 校验 magic == "NKM!" → 失败返回 -ENOENT
  3. 校验 abi_version 主版本 == 1 → 否则 -EINVAL
  4. 校验 min_kernel_ver <= 当前内核版本 → 否则 -EMODULE
  5. 拷贝整个文件到内核内存
  6. **哈希校验**：
     - 临时副本中偏移 0x50 起 96 字节置 0
     - 计算 SHA256
     - 与头部 module_hash 比对 → 不匹配 -ECORRUPT
  7. **签名校验**：
     - 遍历 kernel_trusted_keys[0..key_count-1]
     - 对 module_hash 做 Ed25519 verify
     - 全部失败 → -EMODULE
     - （管理员放行：传 NKM_FLAG_ALLOW_UNSIGNED 跳过此步）
  8. 递归加载依赖
  9. 映射段到内核地址空间
  10. 应用重定位
  11. 调用 nkm_init() → 非0返回则回滚
- **调试检查点**：每一步打日志，失败时打印具体步骤
- **常见 Bug**：
  - 哈希计算时忘记置零 hash+sig 字段 → 永远不匹配
  - 重定位时外部符号没找到 → 内核 oops

---

## 6. VFS 接口

### 6.1 `vfs_open`

```c
int vfs_open(const char* path, uint32_t flags, uint32_t mode);
```

- **参数 flags**：
  - bit0 = O_RDONLY
  - bit1 = O_WRONLY
  - bit2 = O_RDWR
  - bit3 = O_CREAT
  - bit4 = O_TRUNC
- **路径检查**：
  - `/Novus` 下写操作需要 `fs.full` CAP
  - 路径解析失败 → -ENOENT
- **返回值**：文件描述符（正整数）；负数错误码
- **调试检查点**：打印 path、flags、返回的 fd

### 6.2 `vfs_read` / `vfs_write`

```c
int vfs_read(int fd, void* buf, uint64_t len);
int vfs_write(int fd, const void* buf, uint64_t len);
```

- **参数**：
  - fd：vfs_open 返回的描述符
  - buf：缓冲区指针
  - len：要读/写的字节数
- **返回值**：实际读/写字节数；负数错误码
- **调试检查点**：打印 fd、len、实际传输量
- **常见 Bug**：buf 为 NULL；fd 越界；文件指针未推进

---

## 7. NovusFS 接口

### 7.1 `novusfs_alloc_block`

```c
int novusfs_alloc_block(uint64_t* out_block);
```

- **前置条件**：持有块分配器锁
- **行为**：扫描块位图找第一个空闲位
- **返回值**：0 成功，*out_block 填入块号
- **错误码**：
  - -ENOSPC：磁盘满
  - -ECORRUPT：位图标记了坏块但块号仍被分配
- **调试检查点**：打印分配的块号；验证位图已置 1
- **锁顺序**：超级块锁 → 目录锁 → inode锁 → **块分配器锁**（最内层）

### 7.2 WAL 事务

#### `wal_begin_tx`
```c
int wal_begin_tx(uint32_t type, uint64_t block_no, wal_tx_t* out);
```
- type：0=块写 1=inode修改 2=目录修改
- 写入 PREP 记录到 WAL 区
- 返回 0 成功；-EFULL 日志区满

#### `wal_commit`
```c
int wal_commit(wal_tx_t* tx, const void* data, uint32_t len);
```
- 写数据 + COMMIT 记录
- 后置：事务持久化

#### `wal_rollback`
```c
void wal_rollback(wal_tx_t* tx);
```
- 丢弃 PREP 记录（不写 COMMIT）

#### WAL 满判定
```c
// 满条件：write_ptr == tail_ptr
if (wal_write_ptr == wal_tail_ptr) {
    // 推进 tail 到下一个完整事务边界
    wal_advance_tail();
}
```

---

## 8. Merkle 树接口

### 8.1 `merkle_verify_root`

```c
int merkle_verify_root(dev_t dev, const uint8_t expect[32]);
```

- **调用方**：A核开机自检
- **行为**：
  1. 读取超级块中的 merkle_root_primary
  2. 如果 primary 损坏，读 backup
  3. 比对 expect（A核自己计算的哈希）与存储的根
  4. 不一致 → 返回 -ECORRUPT
- **错误码**：
  - -ECORRUPT：哈希不匹配
  - -EIO：磁盘读失败
- **调试检查点**：打印 expect 和 stored 两个哈希的前 8 字节

### 8.2 节点磁盘映射

```c
// idx: 节点索引（根=1）
uint64_t node_to_block(uint64_t idx) {
    return merkle_start_block + (idx / 64);
}
uint64_t node_offset_in_block(uint64_t idx) {
    return (idx % 64) * 64;
}
```

- **边界检查**：
  - leaf_count == 0 → 拒绝挂载
  - leaf_count == 1 → 树高=2，根=1，叶子起始=2

---

## 9. 图形栈系统调用

### 9.1 `sys_gfx_create_surface`

```c
int sys_gfx_create_surface(uint32_t w, uint32_t h, uint32_t fmt, fb_handle_t* out);
```

- **参数**：
  - w/h：宽高（像素）
  - fmt：0=BGRA8888, 1=BGR24
  - out：输出帧缓冲句柄
- **LowMem 限制**：最多 4 个离屏 FB
- **Full 限制**：最多 16 个
- **调试检查点**：打印 w、h、分配的 fb 地址
- **错误码**：-ENOMEM 分配失败

### 9.2 `sys_gfx_submit_dirty`

```c
int sys_gfx_submit_dirty(fb_handle_t h, const rect_t* rects, uint32_t n);
```

- **参数 rect_t**：
  ```c
  typedef struct {
      uint32_t x, y, w, h;
  } rect_t;
  ```
- **限制**：n 最大 8（Full）/ 4（LowMem）
- **行为**：标记窗口脏区域，触发合成器重绘
- **调试检查点**：打印 rect 数量和坐标

---

## 10. IPC 接口

### 10.1 `ipc_send`

```c
int ipc_send(novus_ipc_hdr_t* hdr, const void* payload);
```

- **前置条件**：
  - 调用者已通过 `ipc_bind` 注册通道
  - 拥有对应窗口的归属权（或 gui.override CAP）
- **hdr 必须字段**：
  - magic = 0x4E574D49
  - sender_pid = 当前进程 PID
  - msg_type = 有效消息类型
  - payload_len = 实际 payload 长度
- **错误码**：
  - -EFULL：目标队列满
  - -EINVAL：magic 错误
  - -EPERM：无权限
- **调试检查点**：打印 msg_type、target_pid、payload_len

---

## 11. 系统调用 ABI（debug 要点）

### 11.1 寄存器约定

| 用途 | 寄存器 |
|---|---|
| 调用号 | rax |
| 参数1 | rdi |
| 参数2 | rsi |
| 参数3 | rdx |
| 参数4 | rcx |
| 参数5 | r8 |
| 参数6 | r9 |
| 返回值 | rax |
| 被调用者保存 | rbp, rbx, r12, r13, r14, r15 |

### 11.2 常见调试技巧

1. **系统调用号打印**：在 syscall 入口打印 rax 和参数
2. **返回值检查**：用户态库包装后检查 rax 是否为负
3. **栈对齐**：进入内核时检查 rsp % 16 == 0
4. **权限拒绝**：syscall 入口先 cap_check，拒绝时打印 bit 号

---

## 12. 日志系统：实现现状与全量日志解释

### 12.0 规格与实现差异声明（调试前必读）

V1.0 冻结规格按"理想接口"描述了 `log_print(level, fmt, ...)` 与 `panic_handler(cpu_state_t*)`。对当前源码逐文件核实后，实现与规格存在以下差异。**调试时必须以"实现真相"一列为准**，否则会在错误的地方找日志、误判"日志没打出来"。

| 项目 | 规格（V1.0 原文） | 实现真相（当前源码） | 调试含义 |
|---|---|---|---|
| 日志 API | `log_print(level, fmt, ...)` 四级（DEBUG/INFO/WARN/ERROR） | **不存在**该函数。统一原语只有 `kputs(const char*)` 与 `kputs_hex(UINT64)`（`kernel/kmain.c:78–99`），"格式化"由多段 `kputs` / 本地 `kputs_dec` 拼接 | 不要搜索 `log_print` 调用点；搜 `kputs(`、`kputs_hex(`、`[TAG]` 前缀 |
| 级别过滤 | DEBUG/INFO/WARN/ERROR 数值级别，LowMem 下 DEBUG 不输出 | **无级别枚举、无运行时过滤、无 LowMem 日志档位**。级别仅靠文本前缀 `[WARN]/[ERROR]/[FATAL]/[PANIC]/[CHECK]` 区分 | LowMem 不会"吞掉"日志；串口队列溢出才是丢失原因 |
| 环形缓冲 | `log_print` 写环形缓冲 + 串口 | 唯一环形缓冲是共享内存 `log_buf[512]` @ `0x080`（`kernel/shared_mem.h:76–77`），仅由 `shared_mem_log()` 写入，只用于 A/B 核启动阶段，不承载普通内核日志 | "历史日志"只能靠 QEMU `-serial file:` 落盘；512B 共享缓冲看不到 `kputs` 内容 |
| 持久化 | 规格提到"持久化写入 NovusFS 日志分区" | **未实现**。无磁盘 crash dump、无日志文件落盘 | 崩溃现场只能用串口重定向保存 |
| panic | `panic_handler()`：关中断 → VGA 打印寄存器转储 → HLT | **不存在** `panic_handler`。三条致命路径：① Rust panic → `novus_rs_panic_hook()`（`kernel/pmm.c:297–315`）打 `[PANIC] novuslib-rs:` 后 `cli;hlt`；② CPU 异常 → `trap_exception()`（`kernel/trap.c:1133–1155`）打 `[TRAP] ... err/rip/cs/rsp` 后按 ring0/ring3 分流；③ 引导期 `[FATAL]` 返回 `EFI_LOAD_ERROR` | 寄存器转储只有 **err/rip/cs/rsp 四个值**，无完整 GPR、无 backtrace |
| assert / BUG_ON | 调试检查点建议 `assert(...)` | 源码中**无运行时 assert / BUG_ON / oops / ud2 / int3**。只有编译期 `_Static_assert`（`kernel/shared_mem.h:136–165`）冻结共享内存布局 | 断言只在编译期生效；运行时靠日志 + selftest 的 PASS/FAIL |
| 用户态崩溃 | 走 `sched_exception_report`，弹 WM 对话框 | 用户态 Rust panic → `[APP] panic\n` + `exit(3)`（`apps/src/lib.rs:40–44`）；用户态 CPU fault → `fault in user code: task terminated`；`sched_exception_report` 未出现在日志链路 | 任务管理器退出码 3 = Rust panic；`[TRAP] fault in user code` = 用户态异常 |
| LowMem | 独立日志档位 | **无日志档位**。相关只有 <2GiB 引导 FATAL、`0x7E000` 保留共享页、`0x8000` AP trampoline、低 4GiB identity map 约束 | 低内存表现为"拒绝启动"或"分配失败日志"，不是"少打日志" |

> 设计哲学（源码注释反复申明）：`report, refuse, never panic`（`fs/novusfs/novusfs.h:260`）、`Failure is always a returned NOVUS_* code, never a halt`（`security/nkm/nkm.h:23`）、`degrade, never deadlock, never panic`（`kernel/shared_mem.h:192`）。因此多数错误是**返回错误码 + 一条 WARN/ERROR 日志**，而非崩溃。

### 12.1 日志输出基础设施（实现真相）

| 设施 | 源文件（路径前缀 `ROOT = //wsl.localhost/Debian/home/dev/novus-os/novus-os/`） | 关键函数/位置 | 细节 |
|---|---|---|---|
| 统一输出 | `kernel/kmain.c:78–99` | `kputs()` / `kputs_hex()` | `kputs` 在 `irq_save()/irq_restore()` 内同时写 VGA 与 COM1，可被 ISR 重入；`kputs_hex` 固定输出 `0x` + 16 位大写十六进制 |
| VGA 文本 | `kernel/vga.c` / `vga.h` | `vga_init/clear/putc/puts/set_color` | 80×25，显存 `0xB8000`，属性字节 `0x07`，`vga_scroll()` 上滚，光标端口 `0x3D4/0x3D5` |
| 串口 | `kernel/serial.c:1–9,82–153` / `serial.h` | `serial_init/irq_init/putc/puts/getc` | 16550 COM1 `0x3F8`，115200 8N1；`SERIAL_QUEUE=1024`，rx/tx 各 1024B 环形缓冲；`g_rx_dropped` 记溢出；`serial_puts()` 把 `\n` 展开为 `\r\n`；队列满/关中断时 `serial_putc_sync()` 轮询回退；IRQ4 名 `"com1"` |
| 帧缓冲控制台 | `kernel/con.c:23–25,305–309` / `con.h` | `con_init/con_ready/con_out/con_in_read` | `CON_MAX_ROWS=300`、`CON_ROW_BYTES=256`、`CON_IN_CAP=128`；解析 ANSI SGR 与 `ESC[2J`；**每个输出字节镜像到 `serial_putc()`**（源码注释：session can be captured/debugged）；无桌面时经 `SYS_CONSOLE` 供 nsh |
| 共享内存环形日志 | `kernel/shared_mem.h:63–77,118–126` / `shared_mem.c:84–125` | `log_buf[512]`、`log_head`、`shared_mem_log()`、`shared_mem_lock()` | 物理 `0x7E000`，8 KiB 保留页；CAS 自旋锁上限 `1<<20` spins，超时打 `[WARN] shared_mem lock timeout, degrading (spec 3.4)` 并降级；`dbg_tmp/dbg_live/dbg_route/dbg_fat/dbg_ktest` 为引导期 M-debug 快照 |
| 引导期输出 | `arch/x86_64/boot/main.c:59–70,96–149` | `boot_puts/boot_quiet/boot_serial_putc/boot_put_hex/boot_put_dec` | UEFI SimpleTextOutput(UTF-16) + COM1 双写；`g_quiet=1` 后只走串口（splash 接管屏幕）；无 UART 时读状态口得 `0xFF` 不挂死 |
| 引导期日志助手 | `arch/x86_64/boot/stage3.c:1056–1104` | `say()/say_line()/say_ascii()/hex8()` | `hex8` 故意只打印哈希摘要前若干字节 |
| TLS 编译期宏 | `security/tls/tls13_client.c:14–39`、`tls12_client.c:22–24` | `KLOG/KLOG_HEX`、`DBG/DBG_HEX` | `TLS13_KERN_DEBUG` → `kputs` 输出 `[TLS13]`；`TLS13_HOST_DEBUG`/`TLS12_HOST_DEBUG` → `fprintf(stderr,"[tls13]/[tls12] ")`，可转储 transcript 到 `/tmp` |
| 用户态输出 | `apps/src/sys.rs:20–22,169–181` | `sys::write` → `SYS_WRITE`(nr=2,fd=1)；`sys::console_write` → `SYS_CONSOLE`(nr=27,op=0) | `sys::write` 内核侧加 `[USER]` 前缀并镜像串口；`console_write` 是裸串口（nsh stdout） |

**输出目标汇总**：VGA 文本、`COM1`(115200)、帧缓冲控制台（镜像串口）、UEFI 控制台、共享内存 512B 环形缓冲。没有内核日志文件落盘——保存日志必须用 QEMU `-serial file:<path>`（见 13.3）。

### 12.2 日志级别、前缀与限流约定

没有数值级别，级别由**文本前缀**表达：

| 前缀 | 视作级别 | 典型含义 | 看到后的第一动作 |
|---|---|---|---|
| （无前缀，`... PASS` / `... up` / `selftest PASS`） | INFO | 正常里程碑 | 确认启动链继续推进 |
| `[CHECK]` | INFO | 引导期硬件自检（CPU/内存映射） | 核对 CPU/内存容量 |
| `[WARN]` / `warning` / `WARN` / `WARNING` | WARN | 降级、可选特性不可用、资源不足但可继续 | 判断是否影响功能；降级往往非致命 |
| `[ERROR]` / `FAIL` / `FATAL` / `PANIC` / `refused` / `denied` | ERROR | 操作被拒、校验失败、致命停机 | 立即定位对应子系统 |
| `[USER]` | INFO | 用户态 `sys_write` 内容镜像 | 区分应用输出与内核日志 |
| `[APP]` | INFO/ERROR | 用户态 Rust 应用启动标记或 panic | `xxx up` = 成功；`panic` = 用户态崩溃（退出码 3） |
| `[TRAP]` | ERROR | CPU 异常 / 未知系统调用 / ABI 越界 | 结合 err/rip/cs/rsp 与向量名判断 |

**限流规则（日志"消失"通常是限流，不是没触发）**：

| 限流点 | 位置 | 上限 | 表现 |
|---|---|---|---|
| 未知系统调用 | `kernel/trap.c:1124` `g_unknown_calls` | 8 条 | 第 9 次起静默 |
| Win32 ABI 越界 syscall | `kernel/trap.c:1108` `g_abi_denials` | 8 条 | 第 9 次起静默 |
| WM 事件投递 | `wm/novus-wm/nwm.c:231` `g_evt_log` | 12 条 | `[NWM] event ...` 只打前 12 条 |
| WM display list 满 | `wm/novus-wm/nwm.c:978` `g_full_log` | 8 条 | `[NWM] WARN display list full` 只打前 8 条 |
| 应用 display list 预算 | `apps/src/wm.rs:280,326` | 每帧一次 | `[APP] display list over ... budget` |
| 字体/图标/壁纸"缺失"提示 | `graphics/nd_icon.c:314`、`nd_wallpaper.c:169` | 一次性 | `g_said_png` / `g_said_no_pkg` 后不再提示 |
| 输入首字节 | `graphics/nd_input.c:612–613` | 一次性 | `g_irq_logged` 位 |
| PMM 双重释放 | `kernel/pmm.c:254–258` | 一条后抑制 | `[PMM] WARN further double frees suppressed` |
| 调度心跳 | `kernel/sched.c:565–569` | 每 40 tick | `[SCHED] ticks=... current=...` |
| WM 健康统计 | `wm/novus-wm/nwm.c:1409–1415` | 每 1000 tick（约 10 s） | `[NWM] stats req/ev/drop` |

### 12.3 全量日志清单（逐条解释）

下表覆盖当前源码中**全部**日志/打印调用点。列说明：**日志文本**（`{...}` 为 `kputs_hex`/`kputs_dec` 填入的动态值）、**位置**（`ROOT = //wsl.localhost/Debian/home/dev/novus-os/novus-os/`）、**触发条件**、**含义与应对**。多段 `kputs` 拼接算一条；`kputs_hex` = `0x`+16 位大写 hex，`kputs_dec` = 十进制，`kputs_hex_prefix(buf,n)` = n 字节小写 hex（无 `0x`）。

#### 12.3.1 引导阶段：`arch/x86_64/boot/`

`main.c`（UEFI 引导入口）：

| 日志文本 | 位置 | 触发条件 | 含义与应对 |
|---|---|---|---|
| `[CHECK] CPU vendor: {字符串}` / `[CHECK] CPU brand: {字符串}` | `main.c:248–250 / 275–277` | CPUID 读取后 | 确认 CPU 厂商型号；SMP 异常时先核对 |
| `[CHECK] Logical processors: {dec}` | `main.c:297–299` | 同上 | 应 ≥2 才启动 AP |
| `[CHECK] Memory map entries: {dec}` / `[CHECK] Usable memory: {dec} MiB` / `[CHECK] Usable bytes: {hex}` | `main.c:338–346` | UEFI 取内存映射后 | 可用内存 <2048 MiB 会触发 FATAL |
| `[FATAL] Usable memory below 2GiB. Boot refused.` / `[FATAL] Usable memory below 2GiB minimum (Novus OS spec 1.2). Boot refused.` | `main.c:351 / 353–354` | 可用内存不足 | 加大 QEMU `-m`；置 `SHM_STATUS_FAIL` 并返回 `EFI_LOAD_ERROR` |
| `=====...` / `      Novus OS - Boot Stage` / `=====...` / `[BOOT] UEFI boot started` | `main.c:386–389` | 引导横幅 | 看不到说明固件没找到 `BOOTX64.EFI` |
| `[FATAL] Memory map retrieval failed. status={hex}` | `main.c:401–403` | `GetMemoryMap` 失败 | UEFI 固件异常；查 OVMF 版本 |
| `[BOOT] NovusDisplay framebuffer: {w}x{h}` | `main.c:428–432` | GOP 拿到线性帧缓冲 | 记录分辨率；图形问题先核对 |
| `[BOOT] boot splash unavailable, text console stays` | `main.c:439` | splash 不可用 | 降级文本控制台，非致命 |
| `[BOOT] NovusDisplay: no linear framebuffer, VGA fallback` | `main.c:442` | GOP 无线性帧缓冲 | 图形栈走 VGA；见 `[ND] no linear framebuffer` |
| `[BOOT] Exiting boot services, entering BSP kernel stage...` | `main.c:451` | `ExitBootServices` 前 | **关键分界**：之后无固件服务；此行后无输出 = 内核入口崩溃 |
| `[FATAL] Failed to fetch final memory map.` / `[FATAL] kernel_entry returned.` | `main.c:462 / 477` | 最终映射失败 / 内核异常返回 | 引导 abort；内核不该返回 |

`stage3.c`（ESP/镜像读取）：

| 日志文本 | 位置 | 触发条件 | 含义与应对 |
|---|---|---|---|
| `[BOOT] reading the ESP via {via} on {dev}` | `stage3.c:1092–1098` | 首次读 ESP | `via` 为固件 FS 或 fat32；live-root 损坏时对照 `dbg_route` |
| `[BOOT] block devices: ` / `[BOOT]   {entry}` | `stage3.c:1742/1752` | 枚举块设备 | 确认引导介质被识别 |
| `[BOOT]   none: the firmware offered no block device at all` | `stage3.c:1774` | 无块设备 | 检查 QEMU `-drive`/`-cdrom` |
| `[BOOT] EFI system partition opened` / `[BOOT] WARN no EFI system partition: live root unavailable` | `stage3.c:1789/1791` | ESP 打开成功/失败 | 失败则 live root 不可用 |
| `[BOOT] live root: {…}` / `[BOOT] WARN live root not loaded` | `stage3.c:1829/1835` | live root 发布成功/失败 | 失败对照 `dbg_tmp/dbg_live` M-debug 快照 |
| `[BOOT] install ESP template: {…}` / `[BOOT] WARN install ESP template not staged` | `stage3.c:1846/1850` | 安装模板暂存 | 缺失则安装器报 `[INST] no install payload staged` |
| `[BOOT] live medium is on USB` | `stage3.c:1877` | 从 USB 启动 | 安装器拒绝覆盖该介质 |

#### 12.3.2 启动主流程：`kernel/kmain.c`

| 日志文本 | 位置 | 触发条件 | 含义与应对 |
|---|---|---|---|
| `Novus OS M1 - A-core (BSP) kernel stage` | `kmain.c:206` | `kmain()` 入口 | 内核已接管；此前问题在引导阶段 |
| `[SHM] shared memory verified (NVSH v1.1)` / `[WARN] shared memory verify FAILED, continuing degraded` | `kmain.c:225/227` | 共享内存校验 | 失败则 SMP/硬件信息可能不可用 |
| `[BLK] idle spin-down after {hex}s` | `kmain.c:269–271` | 设置空闲降速超时 | — |
| `[WARN] NovusFS selftest failed` / `[WARN] VFS selftest failed` / `[WARN] ext4 selftest failed` | `kmain.c:300/315/319` | 对应 selftest 非 0 | 对照 `[NOVS]/[VFS]/[EXT4] FAIL` 逐项定位 |
| `[M3] integrity: PASS` | `kmain.c:329–347` | Merkle 校验通过 | 系统分区与根哈希一致 |
| `[M3] integrity: MISMATCH - system partition does not match its root hash` + `[M3] continuing degraded (README 18)` | 同上 | 哈希不匹配 | 完整性受损，降级运行；检查镜像是否被改 |
| `[M3] integrity: NOT BUILT - no root hash stored yet` / `UNREADABLE - device or tree could not be read` / `UNKNOWN - no system partition was checked` | 同上 | 未构建/不可读/无分区 | 分别对应首次启动、存储驱动故障、缺系统分区 |
| `[WARN] live root could not be adopted` / `[WARN] live root mount failed` | `kmain.c:363/369` | 内存镜像/挂载失败 | 对照 `[NOVS]` 超级块/WAL 日志 |
| `[M3] live root mounted: {hex} bytes, integrity ...` | `kmain.c:373–379` | 挂载成功 | **冒烟测试 marker** |
| `[M3] no live root published by the boot stage` | `kmain.c:391` | 未发布 live root | 冒烟 no-root 模式 marker |
| `[NKM] staged module loaded: {name}` / `[NKM] staged module refused: {nkm_err_text}` | `kmain.c:422–431` | 模块加载 | `nkm_err_text` 来自 `nkm_strerror()` |
| `[WARN] NKM selftest failed, continuing degraded` | `kmain.c:437` | selftest 非 0 | 对照 `[NKM] FAIL ...` |
| `[NET] https cabundle: vendor roots={hex}` / `[NET] https fail-closed: no cabundle` | `kmain.c:451–456` | 根证书就绪/缺失 | 缺失则 HTTPS 拒连；E2E 断言点 |
| `[E2E] QEMU test anchor added, total roots={hex}` | `kmain.c:462–464` | 仅 `NOVUS_HTTPS_E2E_PROBE` | E2E 断言点 |
| `[NOVU] update format hdr={n}B, verify+queue ready` | `kmain.c:467–480` | 更新服务就绪 | — |
| `A-core asleep` | `kmain.c:489` | A 核休眠前，`shared_mem_log()` | 写共享环形缓冲，不进串口；SMP 调试用 |
| `[SCHED] ERROR: idle task could not be created` / `[NUC] self test could not load its own image` | `kmain.c:507/510` | idle/自测镜像失败 | 严重；对照 `[APPS] spawn failed` |
| `[BOOT] no-graphics mode: on-screen text console, window manager not started` / `[BOOT] install mode: starting the graphical installer` / `[BOOT] boot stage chose no-desktop: skipping the menu` / `[BOOT] no-desktop mode: launching the shell` | `kmain.c:554/599/607/618` | 启动模式分支 | 之后应看到 `[APPS] launching nsh` |
| `[HALT] entering the scheduler (the A core stays parked)` | `kmain.c:635` | 调度器接管前 | **冒烟测试关键 marker**；之后 B 核运行 |
| `[HALT] System halted.` | `kmain.c:638` | 未编译 `CONFIG_NOVUS_SCHED` | 随后 `cli;hlt`；检查 defconfig |
| `[STORAGE] NovusFS entry: {name}` / `[STORAGE] NovusFS file read PASS: {name}` | `kmain.c:117–129` | 根目录列表（前 8 项）/读测试 | — |
| `[STORAGE] NovusFS partition registration failed` / `mount failed: {name}` / `mounted: {name}` / `root listing failed` / `has no readable regular file in root` | `kmain.c:173/178–196` | 存储挂载流程 | 最后一条说明镜像未正确构建；`make check-rootfs` |

#### 12.3.3 内存与页表：`pmm.c` / `heap.c` / `mm.c` / `vm.c`

| 日志文本 | 位置 | 触发条件 | 含义与应对 |
|---|---|---|---|
| `[PMM] WARNING: init lock timeout; continuing unlocked` | `pmm.c:63` | 初始化锁超时 | 降级无锁；早期单核通常无害 |
| `[PMM] ERROR: descriptor size too small` / `empty memory map` | `pmm.c:108/112` | 固件映射异常 | 引导 FATAL 通常先触发 |
| `[PMM] WARN double free of {hex16}` / `[PMM] WARN further double frees suppressed` | `pmm.c:254–258` | 重复释放 | 定位 `mm_free` 配对；后续抑制 |
| `[PANIC] novuslib-rs: {msg}` | `pmm.c:309–311` | Rust panic 经 FFI | **致命**：最多 64 字节，随后 `cli;hlt` |
| `[PMM] selftest SKIPPED (not initialized)` / `bitmap 256KiB, pages: total/reserved/free = {hex}/{hex}/{hex}` / `novuslib-rs v0.1 FFI active` / `ERROR: novuslib-rs version mismatch` | `pmm.c:328/332–338/343/345` | selftest/版本检查 | 版本不匹配 = Rust/C ABI 不同步 |
| `[PMM] ERROR: alloc returned NULL` / `alloc handed out a reserved page` / `duplicate allocation` / `free count not restored` / `statistics inconsistent` | `pmm.c:356/361/367/381/385` | selftest 断言失败 | 按文本定位不变量 |
| `[PMM] selftest PASS (alloc/free regression)` / `[PMM] selftest FAIL` | `pmm.c:390/392` | selftest 结尾 | 冒烟等待 PASS |
| `[HEAP] virtually contiguous page heap ready (256 MiB window)` | `heap.c:32` | 堆就绪 | — |
| `[HEAP] warning: allocation size too large` / `allocation table full, kmalloc failed` / `out of memory, kmalloc failed` / `kfree of unknown pointer` | `heap.c:44/56/75/100` | 分配/释放异常 | 检查尺寸、泄漏、跨堆释放 |
| `[HEAP] selftest FAIL (kmalloc)` / `(pattern)` / `(physical alias)` / `(leak)` / `[HEAP] selftest PASS` | `heap.c:115/125/133/144/142` | selftest | 冒烟等待 PASS |
| `[MM] ERROR: translating unmapped heap address` / `out of pages for page tables` | `mm.c:99/118` | 页表缺项/耗尽 | 常伴随 `#PF` |
| `[MM] paging enabled, CR3={hex}` | `mm.c:204–206` | 分页启用 | 基线信息 |
| `[MM] ERROR: invalid heap unmap range` / `heap page already unmapped` / `invalid heap map range` / `heap mapping overlaps an allocation` / `no physical page for heap mapping` | `mm.c:220/228/240/246/255` | heap 映射守卫 | 范围/重叠错误 |
| `[MM] physmap readback PASS` / `FAIL` | `mm.c:272/274` | 读回自检 | FAIL = identity map 有问题 |
| `[MM] PAT not advertised by CPUID: write-combining needs an MTRR` / `firmware left CR4.PAT clear: ... using an MTRR WC range instead` / `PAT enabled: ...` | `mm.c:365/375–376/383–384` | PAT 能力检测 | 图形性能相关 |
| `[MM] no variable MTRR with WC support (MTRRCAP=0x{hex}): framebuffer mapped uncached` / `framebuffer already inside a WC MTRR range, write-combining is on` / `no free MTRR slot for the framebuffer: mapped uncached` / `WC MTRR programmed: 0x{hex} for 0x{hex} bytes` | `mm.c:426–428/458–459/472/486–490` | MTRR 编程 | 未缓存仅影响性能 |
| `[VM] ERROR: table page budget exhausted, refusing to map` / `[VM] ERROR: refusing user mapping in the kernel half` | `vm.c:42/154` | 映射被拒 | 安全检查触发；检查调用方地址 |

#### 12.3.4 调度、异常与 IPC：`sched.c` / `trap.c` / `ipc.c`

| 日志文本 | 位置 | 触发条件 | 含义与应对 |
|---|---|---|---|
| `[REAP] {name}: {hex} pages back, pml4={hex}, free={hex}` | `sched.c:175–183` | 任务回收 | 确认资源释放 |
| `[SCHED]   task {name} pid={hex} state={hex}` | `sched.c:251–257` | 任务表转储 | 判断阻塞/就绪 |
| `[SCHED] no runnable task: kernel idle, desktop keeps running` / `no task to run` | `sched.c:302/909` | 无就绪任务 | 桌面仍在通常无害 |
| `[SCHED] NX enabled: data pages are not executable` / `syscall gate at {hex}` | `sched.c:349/359–361` | 安全基线 | — |
| `[SCHED] WARN no LAPIC: no timer, no preemption` / `LAPIC calibrated count {hex}` / `timer armed: vector 32, initial count {hex}` | `sched.c:365/371–373/379–381` | LAPIC 初始化 | 无 LAPIC 则调度测试异常 |
| `[SCHED] no free task slot for {name}` / `ERROR: no kernel stack slot` | `sched.c:391–393/399` | 任务/栈槽耗尽 | 应用无法启动；后者严重 |
| `[SCHED] switch -> {name}` | `sched.c:507–509` | 上下文切换 | 高频，谨慎全量抓取 |
| `[SCHED] ticks={hex} current={name}` / `current=(kernel idle)` | `sched.c:565–569` | 每 40 tick | **稳态心跳**：长时间不变说明调度卡死 |
| `[SCHED] idle task missing, recreating` | `sched.c:593` | 每 200 tick 检查 | idle 被意外杀死，重建 |
| `[SCHED] task {name} exited with code {hex}` / `task {name} killed (pid {hex})` | `sched.c:655–659/782–786` | 退出/被杀 | 退出码 3 = `[APP] panic`；127 = 命令未找到 |
| `[SCHED] foreground selftest: PASS (80:40 ticks)` / FAIL | `sched.c:898` | 前台调度自测 | 时间片正确性 |
| `[SCHED] entering ring 3: {name}` | `sched.c:923–925` | 首次进用户态 | 之后应用输出才应出现 |
| `[USER] {内容}` + `[TRAP] sys_write: user buffer ended early` | `trap.c:163–165` | 用户指针越界 | 内核已拒绝，不崩溃 |
| `[USER] {内容}` | `trap.c:172–174` | 正常 `sys_write` | 应用输出镜像 |
| `[POWER] user requested shutdown` / `user requested reboot` | `trap.c:659/667` | 用户请求 | — |
| `[TRAP] win32 ABI denied syscall {hex}` / `[TRAP] unknown syscall {hex}` | `trap.c:1110–1112/1126–1128` | ABI 越界/未知号 | 前 8 条可见；用户库与内核 ABI 不同步 |
| `[TRAP] {trap_name} err={hex} rip={hex} cs={hex} rsp={hex}` | `trap.c:1134–1144` | 任意 IDT 异常 | **核心调试信息**；向量名见 `trap_name()`（`trap.c:105–120`）：`#DE divide error`、`#DB debug`、`#BP breakpoint`、`#PF page fault`、`#GP general protection` 等。仅 4 个值，无完整 GPR/backtrace |
| `[TRAP] fault in user code: task terminated, kernel continues` / `[TRAP] kernel fault: halting` | `trap.c:1147/1151` | ring3 / ring0 异常 | 前者只杀进程；后者**致命**，随后 `cpu_halt()` |
| `[IPC] message ports ready: queue {hex}, payload {hex} bytes` / `[IPC] warning: no inbox for pid {hex}` / `[IPC] FAIL: {what}` / `[IPC] selftest: PASS\|FAIL` | `ipc.c:60–64/133–135/244–246/323–325` | IPC 初始化/投递/自检 | 无 inbox 则消息丢失 |

#### 12.3.5 SMP / AB 双核：`smp.c` / `lapic.c` / `shared_mem.c`

| 日志文本 | 位置 | 触发条件 | 含义与应对 |
|---|---|---|---|
| `[SMP] disabled by config` | `smp.c:164` | 未编译 `CONFIG_NOVUS_SMP` | 单核运行 |
| `Novus AP (B-core) online` / `[AP] CPU: {vendor} / usable mem {dec} MiB` | `smp.c:130–135` | AP 启动成功（AP 侧直写串口） | 与 `[SMP] AP online` 对应 |
| `[SMP] WARNING: local APIC not supported` / `[SMP] LAPIC at {hex}` | `lapic.c:165/186–188` | LAPIC 检测 | 无 LAPIC 则 SMP 不可用 |
| `[WARN] shared_mem lock timeout, degrading (spec 3.4)` | `shared_mem.c:90` | 自旋超 `1<<20` | 强制降级不 panic；共享数据可能不完整 |
| `[SMP] WARNING: bad trampoline blob size, continuing uniprocessor (spec 3.4)` | `smp.c:184–186` | trampoline 尺寸异常 | 退化为单核 |
| `[SMP] WARNING: no local APIC, continuing uniprocessor (spec 3.4)` | `smp.c:191–193` | 无 LAPIC | 同上 |
| `[SMP] WARN no MADT CPUs; trying legacy APIC ID 1` / `[SMP] MADT enabled xAPIC IDs: {hex}` | `smp.c:201/208–210` | MADT 解析 | 回退探测/成功 |
| `[SMP] WARN no contiguous stack for AP ID {hex}` | `smp.c:243–245` | AP 栈分配失败 | AP 无法启动 |
| `[SMP] Waking AP ID {hex}` / `[SMP] AP online, ID {hex}` / `[SMP] WARN AP wake timeout, ID {hex}` / `[SMP] online AP count {hex}` | `smp.c:258–260/296–298/300–302/306–308` | SIPI/就绪/超时/汇总 | 超时检查 `-smp 2` 与 LAPIC 日志；正常 count=1 |

#### 12.3.6 存储、设备与网络驱动

**块设备 / GPT / PCI / 设备表**：

| 日志文本 | 位置 | 触发条件 | 含义与应对 |
|---|---|---|---|
| `[BLK] unsupported block size, rejected {name}` / `[BLK] device table full, rejected {name}` | `blkdev.c:84–86/102–104` | 设备被拒 | 检查设备块大小/设备表容量 |
| `[BLK] spin-down refused by {name}` / `[BLK] spun down {name} after {hex}s idle` | `blkdev.c:249–251/261–265` | 空闲降速 | 被拒说明设备忙，非致命 |
| `[GPT ] {dev}: cannot read the header block` | `gpt.c:134` | GPT 头读失败 | 检查设备可读性 |
| `[GPT ] {dev}: no GPT signature (blank or not partitioned)` | `gpt.c:138` | 无 GPT 签名 | 磁盘空白/未分区 |
| `[GPT ] {dev}: header size out of range` / `header CRC mismatch` / `entry array does not fit the device` | `gpt.c:144/153/166` | GPT 结构/CRC/越界错误 | GPT 损坏，重新分区 |
| `[GPT ] partition not aligned to the device block size` | `gpt.c:390` | 分区未对齐 | 兼容性/性能警告 |
| `[DEVTAB] {hex} nodes, {hex} pci devices` | `devtab.c:1276–1280` | 设备树建立 | 冒烟测试 grep 标记 |
| `[PCI] {hex} devices` / `[PCI]   {bus}:{dev}.{func} id {vendor}:{device} class {class}:{subclass} bar0 {hex}` | `pci.c:239–259` | PCI 枚举 | 确认网卡/GPU/存储控制器被发现 |

**NVMe / AHCI / IDE / USB**：

| 日志文本 | 位置 | 触发条件 | 含义与应对 |
|---|---|---|---|
| `[NVMe] LBA 0 read PASS\|FAIL` / `namespace {name} blocks {hex} logical block bytes {hex}` | `nvme.c:415/417/419–425` | 首块读取/命名空间识别 | FAIL → 控制器不可用 |
| `[NVMe] BAR0 not mapped below 4 GiB` / `unsupported queue depth, page size, or command set` / `unsupported doorbell stride` / `controller disable timed out` / `controller enable timed out` / `admin identify failed` / `I/O queue setup failed` / `no usable namespace found` | `nvme.c:579/589/594/602/635/640/645/650` | 初始化失败各分支 | `qtest.sh` 测 NVMe 时逐条对照 |
| `[AHCI] LBA 0 DMA read PASS\|FAIL` / `disk {name} controller {hex} capacity {hex} sectors, block {hex} bytes` | `ahci.c:513/515/519–527` | 首块 DMA/磁盘识别 | FAIL → AHCI 不可用 |
| `[AHCI] controller BAR5 unavailable` / `BAR5 unmappable above 4 GiB` / `controller reset timed out` / `controller cap {hex} ports {hex}` | `ahci.c:554/562/573/577–581` | 控制器初始化 | — |
| `[IDE] disk {name} capacity {hex} sectors, lba48={hex}` | `ide.c:458–464` | IDE 盘识别 | — |
| `[USB] ep0 rc {hex} code {hex}` / `bulk transfer failed, code {hex}` | `usb.c:428–432/472–474` | USB 传输失败 | USB 启动介质问题 |
| `[USB] HID device attached` / `[USB] HID device ready` / `[USB] BAR0 moved to {hex}` | `usb.c:1190/1363/1280–1282` | HID/xHCI | 键鼠可用 |
| `[USB] no reachable BAR0` / `no DMA memory for xHCI` / `no DMA memory for device` / `enumeration failed` | `usb.c:1286/1314/1352/1358` | 初始化失败 | USB 存储/HID 不可用 |
| `[USB] disk {name} is the boot medium` / `disk {name} capacity {hex} sectors` | `usb.c:1414–1416/1427–1431` | USB 盘识别 | 安装器拒绝覆盖启动介质 |

**IRQ / IDT / GDT / GPU / 电源**：

| 日志文本 | 位置 | 触发条件 | 含义与应对 |
|---|---|---|---|
| `[IRQ] no LAPIC: device IRQs unavailable` | `irq.c:182` | 无 LAPIC | 键鼠/网卡中断不可用 |
| `[IRQ] ACPI MADT: IOAPIC + ISA overrides` / `[IRQ] WARN no usable MADT: QEMU fallback IOAPIC 0xFEC00000, GSI 0` | `irq.c:190/195` | MADT 解析 | 回退 QEMU 默认 IOAPIC |
| `[IRQ] IOAPIC base={hex} gsi={hex} pins={hex}` / `[IRQ] registered {name} line={hex} gsi={hex} vector={hex}` | `irq.c:212–218/260–268` | IOAPIC/中断注册 | 检查 `com1`/`kbd`/`mouse` 注册 |
| `[IDT] installed: exceptions 0-31, timer 32, IRQ 33-254, spurious 255` / `[GDT] installed, TSS at {hex}` | `idt.c:69/gdt.c:130–132` | 安装完成 | — |
| `[GPU] controller {v}:{d} bus={hex} bar0={hex} bar0_sz={hex} bar2={hex} bar2_sz={hex}` (+` (scanout)`) / `[GPU] no display controller on the PCI bus` | `gpu.c:172–201/248` | GPU 枚举 | 独显 VRAM 超 4GiB 触发 splash 跳过 |
| `[GPU] {name} vendor=.. device=.. mmio=.. size=.. reg0=.. (reachable\|all ones!)` / `... has no reachable BAR 0` / `mmio=.. aperture=.. aper_bytes=.. bar0_64bit=.. stolen=..` | `gpu.c:300–338` | BAR 探测 | BAR 不可达则回退 |
| `[GPU] framebuffer={hex} size={hex} inside BAR {hex} (gpu aperture {hex})` / `[GPU] scanout is above the 4 GiB identity map: ...` / `outside every BAR (firmware linear framebuffer)` | `gpu.c:377–393` | 显存位置 | 高位显存走映射 aperture |
| `[GPU2D] Intel 2D engine: registers reachable, command stream not written yet (needs ring + GTT)` / AMD `(needs ring + IH)` / NVIDIA `needs GSP firmware + a negotiated channel; falling back to the CPU over the firmware framebuffer` / `[GPU2D] backend={name} (64-bit span stores; no vendor engine wired up)` | `gpu2d.c:145–156/170–171/200–202` | 2D 引擎探测 | 回退 CPU 绘制，非致命 |
| `[POWER] ACPI FADT unavailable` / `PM1a control register unavailable` / `valid ACPI _S5_ sleep types unavailable` | `power.c:265/280/295` | ACPI 信息缺失 | 关机功能受限 |
| `[POWER] ACPI shutdown ready, PM1a={hex} S5={hex}/{hex}` / `ACPI reset register ready at {hex}` / `[POWER] ERROR: shutdown unavailable: ACPI _S5_ not initialized` | `power.c:299–316/332` | 关机就绪/失败 | 无 S5 无法软关机 |

**网络**：

| 日志文本 | 位置 | 触发条件 | 含义与应对 |
|---|---|---|---|
| `[NET] arp reply for {hex}` | `net.c:220–222` | ARP 回复 | — |
| `[NET] ping send failed (no route/arp)` / `ping timeout` / `ping retry {hex}` / `ping {hex} rtt={hex}ms` | `net.c:399/427/432–434/451–455` | ping 诊断 | 网络连通性检查 |
| `[NET] ip {o}.{o}.{o}.{o} gw {g}.{g}.{g}.{g}` | `net.c:562–578` | 地址配置完成 | — |
| `[NET] {name} ({model}) link=<..> state=<..> mac=<hex:..>` / `active {name}` / `no usable network device` | `net.c:591–615` | 网卡枚举 | 无网卡则 HTTPS/DNS 不可用 |
| `[NET] tcp connect` / `tcp timeout` / `tcp reset` / `tcp established` / `tcp closed` | `net_tcp.c:168/239/297/314/359` | TCP 状态迁移 | E2E 时对照 |
| `[TCP] in seq={hex} ack={hex} rcv={hex} plen={hex} fl=0x{hex}` / `[TCP] OOO drop seq={hex} expect={hex} seen={hex}` | `net_tcp.c:326–331/348–351` | 每入站包/乱序 | 高频；乱序丢包 |
| `[DNS] no link or no server` / `bad name` / `query {name}` / `timeout` / `server error` / `answer {ip}` / `no A record` | `net_dns.c:94/113/123–125/137/200/231–233/239` | DNS 流程 | HTTPS/NCSI 依赖 DNS |
| `[NET] dhcp discover` / `dhcp timeout, using the emulated-network defaults` / `dhcp offer` | `net_dhcp.c:106/118/180` | DHCP | 超时使用 10.0.2.x 默认值 |
| `[NCSI] probe http://{host}/connecttest.txt` / `connected: {preview}` / `... - not connected` / `http {code} - not connected` / `no link - not connected` / `dns unavailable - not connected` | `net_ncsi.c:65–195` | 连通性探测状态机 | 判断"网络是否真通" |
| `[NET] virtio: rings must live below 4 GiB` / `virtio 1.0 (0x1041) has no legacy BAR; skipped` / `BAR0 is not an I/O window` / `no memory for queues` / `no memory for buffers` / `virtio up, {model}, MAC ...` | `net_virtio.c:151/284/292/348/353/368–377` | virtio 网卡 | `NOVUS_NIC=virtio-net-pci` 时对照 |
| `[NET] igb up, {model}, MAC ...` | `net_igb.c:300–309` | igb 成功 | — |
| `[NET] e1000_send BUSY slot={n}` / `e1000_send OK len={n}` / `e1000 RX from IRQ queue` | `net_e1000.c:217–218/231/272` | e1000 收发调试 | 高频 DEBUG |
| `[NET] e1000: no MMIO BAR` / `firmware did not assign an INTx IRQ line` / `IRQ controller unavailable` / `no usable MAC in RAL/RAH or EEPROM` / `no memory for descriptors` / `INTx route unavailable` / `e1000 up, {model}, MAC ...` | `net_e1000.c:306/310/314/364/379/425/445–454` | e1000 初始化 | 默认网卡；失败则网络不可用 |
| `[NET] rtl8139 RX up` / `rtl8139 up, {model}, MAC ...` / `no I/O window` / `no memory for the receive ring` / `no memory for TX buffers` | `net_rtl8139.c:134/240–249/169/212/223` | rtl8139 初始化 | — |
| `[NET] {name}: {model} ({vendor}:{device}) 无线，需要固件与 802.11/WPA 支持，暂不可用` / `[NET] wireless: 802.11 association is not implemented` | `net_wifi.c:65–73/78` | 无线网卡 | 功能未实现 |

#### 12.3.7 文件系统：`fs/`

**VFS（`fs/vfs.c`，全部在 `vfs_selftest()`）**：

| 日志文本 | 位置 | 触发条件 | 含义与应对 |
|---|---|---|---|
| `[VFS] selftest skipped (no root)` | `vfs.c:428` | `vfs_root()` 为空 | 无挂载根，非失败 |
| `[VFS] selftest FAIL (write)` / `(read)` / `(stat)` / `(readdir)` / `(unlink)` | `vfs.c:435/441/451/462/474` | 对应分派失败 | 立即 `return -1`；定位具体 VFS 操作 |
| `[VFS] selftest: PASS ({kind})` | `vfs.c:479–481` | 全部通过 | — |

> `vfs_lookup_path()` / `vfs_parent_path()` 与 `fs/vfs_mount.c` 全文**无日志**；路径解析失败只通过 selftest 的 FAIL 间接体现。

**NovusFS（`novusfs.c` / `super.c` / `merkle.c` / `wal.c` / `rootfs.c`）**：

| 日志文本 | 位置 | 触发条件 | 含义与应对 |
|---|---|---|---|
| `[NOVS] ERROR root inode allocation failed` / `root directory init failed` / `root layout seed failed` | `novusfs.c:126/133/144` | 格式化失败 | 检查 PMM/磁盘 |
| `[NOVS] wal: replayed {dec} entries` | `novusfs.c:161–163` | 挂载时 WAL 恢复 | 重放条数 |
| `[NOVS] === M3 selftest: NovusFS + WAL + Merkle ===` | `novusfs.c:673` | selftest 入口 | 冒烟等待前缀 |
| `[NOVS] FAIL ramdisk_init (no PMM pages?)` / `[NOVS] FAIL format` | `novusfs.c:678/687` | 自检早期失败 | PMM 页不足/格式化失败 |
| `[NOVS] ramdisk: {dec} sectors` | `novusfs.c:681–683` | ramdisk 创建成功 | — |
| `[NOVS] wal: used={dec} bytes, entries={dec}` | `novusfs.c:751–755` | WAL CRC 校验统计 | — |
| `[NOVS] merkle: leaves={dec} height={dec}` | `novusfs.c:805–809` | 建树/校验后 | — |
| `[NOVS] PASS\|FAIL {what}` | `novusfs.c:647–651`，30+ 调用点 | 每项自检 | `what` 包括 `format + root inode`、`wal entries verify (CRC-32)`、`merkle detects tampering`、`merkle invalidated by a committed write`、`remount: contents intact` 等（`:692,699,701,708,710,712,716,719,723,724,730,739,742,749,750,760,763,766,770,785,798,802,803,814,816,819,832,835,837,844`） |
| `[NOVS] === selftest PASSED\|FAILED ===` | `novusfs.c:850–852` | selftest 结尾 | 冒烟等待 |
| `[NOVS] WARN image too small for NovusFS` / `out of memory for FS buffers` | `super.c:199/203` | 超级块读取 | — |
| `[NOVS] WARN primary superblock bad, trying backup` / `mounted from backup superblock` / `[NOVS] ERROR both superblock copies unusable, mount refused` | `super.c:218/229/234` | 双副本校验 | 双副本都坏则拒绝挂载 |
| `[NOVS] WARN cannot format: image too small` / `out of memory for FS buffers` / `cannot format: no usable data area` / `WAL zeroing failed` / `bitmap init failed` / `inode table init failed` | `super.c:255/259/296/307/317/327` | 格式化各步骤 | 按步骤定位 |
| `[NOVS] layout: not mounted` / `[NOVS] layout total=<hex> wal@<hex> merkle@<hex> bmap@<hex> inode@<hex> data@<hex>` | `super.c:353/356–368` | `novusfs_dump_layout()` 手工调用 | 布局转储，见 13.8 |
| `[MRKL] WARN region too small: need <hex> nodes, have <hex>` / `out of memory for tree buffers` / `read failed at block <hex>` / `superblock flush failed` | `merkle.c:221–225/232/240–242/310` | 建树失败 | 预留区不足/读块失败/根未落盘 |
| `[WAL ] WARN redo failed` / `recover pass1 stopped on bad entry` / `verify: bad entry (magic/CRC) - log damaged` | `wal.c:354/434/499` | WAL 重放/恢复/校验 | 后两者置 `fs->wal_corrupt=1` |
| `[NOVS] ERROR seed_text: parent` / `seed_text: creat` / `seed_text: write` / `root layout mkdir failed` / `root layout metadata failed` | `rootfs.c:124/135/143/158/166` | 根目录种子写入 | 镜像构建问题 |

> **静默路径（重要）**：`wal_begin`/`wal_log_block`/`wal_commit`/`wal_abort` 的失败、`novusfs_merkle_verify()`、`novusfs_merkle_invalidate()`、`balloc.c`/`dir.c`/`inode.c` 的分配失败，**都不打日志**，只返回错误码。遇到"操作失败但无日志"时，应调用 selftest 或逐步加 `kputs` 插桩。

**ext4（`fs/ext4/`）**：

| 日志文本 | 位置 | 触发条件 | 含义与应对 |
|---|---|---|---|
| `[EXT4] volume too small` | `format.c:103` | `blocks < 64` | — |
| `[EXT4] formatted: {hex} blocks, {hex} inodes, {hex} groups` | `format.c:254–260` | 格式化成功 | — |
| `[EXT4] too many block groups` / `group descriptor read failed` | `mount.c:46/51` | 挂载失败 | — |
| `[EXT4] mounted: {hex} blocks, {hex} inodes, {hex} byte blocks, {hex} groups` | `mount.c:56–64` | 挂载成功 | — |
| `[EXT4] metadata checksums present, not verified yet` | `mount.c:66` | 特性位 | **校验未实现的提示** |
| `[EXT4] volume has a journal, not replayed yet` | `mount.c:69` | 特性位 | **journal 未重放提示** |
| `[EXT4] selftest FAIL (ramdisk\|format\|mount\|creat persist\|write persist\|mkdir\|creat temp\|write temp\|readdir\|lookup\|unlink\|sync\|remount\|persist lost\|dir lost\|unlink not persisted\|data not persisted)` | `mount.c:147–257` | 对应检查失败 | 按括号项定位 |
| `[EXT4] selftest: PASS` | `mount.c:265` | selftest 通过 | — |

> `fs/fat/*`、`fs/iso9660/*` 全文**无任何日志**。

#### 12.3.8 安全：`security/`

**CAP 与 NKM**：

| 日志文本 | 位置 | 触发条件 | 含义与应对 |
|---|---|---|---|
| `[NKM] WARNING: signature check skipped by request` | `nkm.c:596` | `NKM_FLAG_ALLOW_UNSIGNED` | 管理员放行，跳过 Ed25519；非错误 |
| `[NKM]  PASS\|FAIL {what}` | `nkm.c:823–827`，见下 | selftest 各项 | 格式：`[NKM]  PASS <what>`（两个空格） |
| `[NKM] === NKM loader selftest ===` | `nkm.c:862` | selftest 入口 | — |
| `[NKM]  loaded: name={name} type={hex} segs={hex} digest={8字节hex}` | `nkm.c:975–983` | 签名模块加载成功 | digest 前 8 字节 |
| `[NKM] === selftest PASSED\|FAILED ===` | `nkm.c:1012–1014` | selftest 结尾 | — |

`nkm_selftest()` 的检查项（`fails += nkm_check(...)` 的 `what` 实参，行号即日志位置）：`reject truncated image`(:865)、`reject bad magic`(:873)、`reject incompatible ABI version`(:880)、`reject wrong header size`(:889)、`reject reserved bytes non-zero`(:896)、`reject bad name`(:903)、`reject kernel version too new`(:914)、`reject bad layout`(:920)、`reject tampered digest`(:927)、`accept unsigned module`(:931)、`reject tampered signature`(:946)、`reject missing dependency`(:951)、`reject dependency cycle`(:957)、`accept signed module`(:969)、`relocation resolved a kernel symbol`(:993)、`module found by name`(:995)、`unload frees the slot`(:999)、`no module left over from the selftest`(:1009)。

> `nkm_load()` 本身**除 UNSIGNED 警告外全静默**：magic/ABI/header/kernel 版本/布局/槽位/摘要(SHA-256)/签名(Ed25519)/依赖/段映射/重定位/init 的失败都是 `return NOVUS_*` 或 `goto fail`，无日志。排错靠 `nkm_selftest()` 的 PASS/FAIL 与 `nkm_strerror()`（`nkm.c:803`，`NOVUS_EPERM → "capability required"`）。WM 的 CAP 检查 `owned_by()`（`wm/novus-wm/nwm.c:676–689`）也**无日志**，失败只 `ack(NWM_EDENIED)`。

**TLS 1.3（`security/tls/tls13_client.c`）**：

KLOG（定义 `TLS13_KERN_DEBUG` 后经 `kputs` 输出）：

| 日志文本 | 位置 | 触发条件 |
|---|---|---|
| `[TLS13] ServerHello parse fail` | `:285` | ServerHello 解析失败 |
| `[TLS13] cert chain verify FAIL` | `:306` | 证书链校验失败 |
| `[TLS13] certverify FAIL` | `:316` | CertificateVerify 失败 |
| `[TLS13] server Finished mismatch` | `:336` | Finished 32 字节不一致 |
| `[TLS13] read_record fail` | `:422` | 握手期读记录失败 |
| `[TLS13] EE/hs record open fail seq={hex}` | `:437–439` | 用 hs key 解记录失败 |
| `[TLS13] handle_msg fail (msgtype={hex})` | `:456–458` | 消息处理失败 |

DBG（定义 `TLS13_HOST_DEBUG` 后 `fprintf(stderr,"[tls13] ")`）：`:117–119` c_hs_secret/s_hs_secret/master HEX、`:192` `cv scheme=%04x siglen=%u`、`:198` `cv pubkey extract fail`、`:201` `cv rsa n_len=%u e=%u`、`:204` `cv pss rc=%d`、`:269–270` c_ap/s_ap HEX、`:284` `SH parse fail`、`:288` `SH suite=%04x`、`:293` `shared` HEX、`:305` `cert chain fail`、`:309` `cert chain ok`、`:315` `certverify fail`、`:319` `certverify ok`、`:335` `finished mismatch`、`:339` `finished ok`、`:342` `th4_c` HEX、`:351` `transcript len=%u`、`:352` `th4_rehash` HEX、`:414` `sent CH %u`、`:421` `read_record fail`、`:425` `record type=%u len=%u`、`:448` `unexpected record type=%u b0=%02x b1=%02x`、`:517` `app read_record fail`、`:520` `app record type=%u len=%u`、`:528` `app open fail seq=%llu`、`:532` `alert decodes with HS key ctype=%u tl=%u b0=%02x b1=%02x`、`:536` `record decodes with neither key`、`:541` `app ctype=%u ptlen=%u`。定义 `TLS13_HOST_DEBUG` 时握手 transcript 写 `/tmp/tls_transcript.bin`（`:348–353`）。

**TLS 1.2（`security/tls/tls12_client.c`，`TLS12_HOST_DEBUG` 后 `[tls12]` 前缀）**：

| 日志文本 | 位置 | 触发条件 |
|---|---|---|
| `hs rec_read fail (enc=%d)` | `:203` | 加密阶段读失败 |
| `sent CH %u` / `SH suite=%04x` | `:411/431` | ClientHello/ServerHello |
| `cert chain FAIL` / `cert chain ok n=%u` | `:457/460` | 证书链 |
| `SKE sigalg=%04x len=%u` | `:489` | ServerKeyExchange |
| `leaf parse FAIL` / `leaf pubkey FAIL` / `leaf rsa n_len=%u e=%u` / `pkcs1 verify FAIL` / `pss verify FAIL` / `ske signature ok` | `:517/524/527/531/537/562` | 叶子证书与签名校验 |
| `expected SHD, got type=%u` / `SHD ok` | `:570/574` | ServerHelloDone |
| `CKE sent` / `client finished sent` | `:587/609` | 客户端发送 |
| `expected server Finished, got type=%u` / `server Finished length %u` / `server Finished verify_data mismatch` | `:615/624/631` | 服务端 Finished |

定义 `TLS12_HOST_DEBUG` 时写 `/tmp/tr.bin`、`/tmp/ske.sig`（`:508–515`）。

**HTTPS E2E 探针（`security/novu/novu_service.c`，`NOVUS_HTTPS_E2E_PROBE`）**：

| 日志文本 | 位置 | 触发条件 |
|---|---|---|
| `[E2E] OK len={dec}` + 回包首行 | `:31–46` | `https_get(10.0.2.2:8443,/v1/updates) > 0` |
| `[E2E] FAIL: https_get returned {dec}` | `:49–58` | `n <= 0` |
| `[E2E] link up, cabundle ready: starting HTTPS probe` | `:99` | DHCP BOUND + HTTP ready + NCSI 终态 + TCP IDLE/CLOSED/FAIL，最多 3 次 |

> `security/novu/` 其余文件无 `kputs`；错误只通过 `set_err()` 写入调用方缓冲区。

#### 12.3.9 图形栈：`graphics/`

**显示 HAL（`nd_hal.c`）**：

| 日志文本 | 位置 | 触发条件 | 含义与应对 |
|---|---|---|---|
| `[ND] no linear framebuffer captured, VGA text stays active` | `nd_hal.c:347` | `!g_disp.available` | 图形降级为 VGA 文本 |
| `[ND] primary framebuffer unmapable` / `[ND] mirror framebuffer unmapable, skipped` | `nd_hal.c:366/407` | `mm_map_phys` 失败 | 主/镜像帧缓冲不可用 |
| `[ND] NovusDisplay: {backend} {hex}x{hex}, pitch {hex}, base {hex}, RGB\|BGR` | `nd_hal.c:430–440` | 显示初始化成功 | 后端/分辨率/pitch/基址/颜色序 |
| `[ND] output {hex} primary\|mirror {hex}x{hex} digital\|analog[, preferred {hex}x{hex}]` | `nd_hal.c:444–458` | 每个输出 | 多屏诊断 |
| `[ND] compositor: off-screen buffer\|direct to framebuffer, vsync available\|unavailable` | `nd_hal.c:467–470` | 合成器模式 | — |
| `[ND] boot splash kept until the desktop presents` | `nd_hal.c:475` | 合成缓冲可用 | — |

**Splash / 输入 / 字体 / TTF**：

| 日志文本 | 位置 | 触发条件 | 含义与应对 |
|---|---|---|---|
| `[ND] boot splash skipped: framebuffer at {hex} is above the 4 GiB identity map` | `nd_boot.c:73–75` | 独显 VRAM >4GiB | 跳过 splash，非致命 |
| `[ND] WARN unsupported absolute pointer version; using PS/2` / `[ND] absolute pointer ready (VMware mouse protocol)` | `nd_input.c:111/116` | VMware 鼠标探测 | 回退 PS/2 |
| `[ND] PS/2 keyboard + mouse IRQ enabled (no hardware polling)` / `[ND] WARN PS/2 IRQ unavailable: input disabled` | `nd_input.c:275/279` | IRQ1/IRQ12 注册 | 失败则键鼠不可用；检查 `[IRQ] registered` |
| `[ND] WARN absolute pointer queue error; using PS/2` | `nd_input.c:520` | 队列错误 | 同上 |
| `[ND] mouse byte from IRQ queue` / `[ND] keyboard byte from IRQ queue` | `nd_input.c:614–615` | 首次从 IRQ 队列取字节 | 一次性；冒烟 marker 之一 |
| `[FONT] no system filesystem: embedded UI glyphs active` | `nd_font.c:244` | `fs == NULL` | 使用内建字形 |
| `[FONT] /Novus/fonts/font.bin missing in system partition` / `stat ... failed` / `invalid font size: using embedded UI glyphs` / `alloc ... failed` / `read ... failed` | `nd_font.c:249/253/257/262/267` | 字体加载失败 | 检查镜像/字体文件 |
| `[FONT] loaded {hex} bytes from system partition` | `nd_font.c:272–274` | 加载成功 | — |
| `[FONT] {path}: size out of range, skipped` / `no memory for {hex} bytes` / `read failed` | `nd_font.c:589–592/596–600/605–607` | 单字体文件失败 | — |
| `[FONT] outline face {name}: {hex} bytes, {hex} glyphs, {hex}px cell` | `nd_font.c:620–628` | TTF 加载成功 | — |
| `[FONT] self-check U+{hex}: {hex}px advance, {hex} covered pixels` | `nd_font.c:654–660` | 对 U+4E2D/U+0041 光栅化 | 字体渲染自检 |
| `[FONT] {dir}: no .ttf/.otf present, bitmap font in use` | `nd_font.c:688` | 目录无字体 | 回退位图字体 |
| `[FONT] ttc collection is empty` / `ttc offset outside the file` / `{name}: OpenType/CFF outlines are not supported, use a TrueType (.ttf)` / `not a TrueType font` / `missing head/maxp` / `implausible unitsPerEm` / `missing hhea/hmtx/cmap/loca/glyf` / `no Unicode cmap (format 4 or 12)` | `nd_ttf.c:931/936/947–949/953–955/973–975/988–990/1015–1017/1041–1043` | TTF 解析失败 | 按文本判断字体格式问题 |

**壁纸 / 图标 / JPEG / IME / 自检**：

| 日志文本 | 位置 | 触发条件 | 含义与应对 |
|---|---|---|---|
| `[WP] no wallpaper package staged: flat desktop colour` | `nd_wallpaper.c:171` | 无壁纸包 | 一次性；纯色桌面 |
| `[WP] wallpaper package: bad magic` / `unsupported version` / `header does not fit the staged bytes` / `entry out of range` | `nd_wallpaper.c:177/181/189/200` | 包格式错误 | `wpkg` 打包问题 |
| `[WP] no SOF marker in wallpaper` / `wallpaper surface allocation failed` / `wallpaper ready: {name}` | `nd_wallpaper.c:233/254/270–272` | JPEG 解码 | 冒烟 marker：`wallpaper ready` |
| `[WP] wallpaper index out of range` / `wallpaper selection deferred: display not initialized` | `nd_wallpaper.c:297/305` | 索引/显示未就绪 | — |
| `[ICON] unsupported bpp` / `compressed DIB unsupported` | `nd_icon.c:184/188` | ICO DIB 格式 | — |
| `[ICON] PNG-compressed ICO entry skipped (need a PNG decoder)` | `nd_icon.c:316` | ICO 内 PNG | 一次性；跳过 |
| `[ICON] loaded {name}.ico from /Novus/icons` / `builtin decode failed` / `[ICON] image icons ready` | `nd_icon.c:402–404/417/513` | 图标加载 | — |
| `[JPEG] bad huffman table` / `multiple SOF markers` / `unsupported SOF` / `out of memory` / `SOS before SOF` / `bad SOS` / `unsupported SOF type` / `no image data` | `nd_jpeg.c:859–912`，宏 `JP_FAIL` (`:20`) | JPEG 解码失败 | 壁纸/图标图片损坏 |
| `[IME] pinyin up: {hex} syllables, {hex} characters` | `nd_ime.c:151–155` | 输入法初始化 | — |
| `[ND] FAIL: {what}` | `nd_demo.c:47`（`CHECK` 宏，调用点 `:124–360`） | 自检断言失败 | — |
| `[ND] selftest FAIL (no surface)` / `[ND] selftest FAIL` | `nd_demo.c:117/222` | surface/widget 上下文失败 | — |
| `[ND] selftest: {hex} checks passed, {hex} failed` + `[ND] selftest PASS\|FAIL` | `nd_demo.c:367–372` | selftest 结尾 | 冒烟 marker：`[ND] selftest PASS` |
| `[ND] GUI: Apply pressed` / `[ND] GUI: window closed` | `nd_demo.c:402/458` | 演示窗口交互 | — |
| `[ND] app launch failed: {name}` | `nd_demo.c:797–799` | `apps_launch_name` 失败 | 对照 `[APPS] spawn failed` |
| `[ND] demo: GUI init failed` / `[ND] demo desktop painted` | `nd_demo.c:1397/1413` | 桌面绘制 | **冒烟 marker**：`demo desktop painted` |
| `[ND] theme applied: dark\|light` | `nd_demo.c:1454–1455` | 主题切换 | — |
| `[ND] GUI disabled, no widget self test` | `nd_demo.c:1535` | 未编译 `CONFIG_NOVUS_GUI` | — |

#### 12.3.10 窗口管理：`wm/novus-wm/`

| 日志文本 | 位置 | 触发条件 | 含义与应对 |
|---|---|---|---|
| `[NWM] event {hex:type} -> pid {hex:owner}` | `nwm.c:233–237` | 事件投递成功 | 限流 12 条 |
| `[NWM] {what} win={hex} from pid={hex}` | `nwm.c:618–625`（`log_request()`） | 通用请求日志 | `what` 见下 |
| `[NWM] create win={id} from pid={sender}` / `destroy win={id} from pid={sender}` / `first frame win={id} from pid={owner}` | `nwm.c:672/718/1040` | 窗口创建/销毁/首帧 | 经 `log_request` |
| `[NWM] WARN display list full (text) win={hex}` | `nwm.c:980–982` | 文本槽满 | 限流 8 条；随后 `ack(NWM_EFULL)` |
| `[NWM] key -> win={hex} pid={hex}` | `nwm.c:1168–1172` | 按键派发 | 每次按键 |
| `[NWM] stats req={hex} ev={hex} drop={hex}` | `nwm.c:1409–1415` | 每 1000 tick | 运行时健康；drop 持续增长说明投递失败 |
| `[NWM] owner gone, closing win={hex}` | `nwm.c:1425–1427` | 客户端进程死亡 | 强制关窗 |
| `[NWM] ERROR no IPC port for the window manager` | `nwm.c:1564` | `ipc_port_create` 失败 | **WM 不启动** |
| `[NWM] compositor: double buffered\|direct to framebuffer` / `Novus-WM up: protocol {hex}, port {hex}, max {hex} windows` | `nwm.c:1583–1594` | 初始化成功 | 冒烟 marker |
| `[NWM] FAIL: {what}` / `[NWM] selftest: PASS\|FAIL` | `nwm.c:1618–1620/1813–1815` | selftest | 启动时自动调用 |
| `[NWM] could not start the ring-3 client` / `ring-3 client started, it will ask for a window over IPC` | `nwm_demo.c:206/209–210` | 演示客户端 | — |

> `req_move/resize/raise/state/focus/draw/clear/shape/title/effect`、`handle_request()`、`route()`、`pump()`、`paint()` **无逐请求日志**；所有权拒绝静默 `ack()`。

#### 12.3.11 用户态应用与 nsh

**`[APP]` 前缀（经 `sys::write`，可在串口看到）**：

| 日志文本 | 位置 | 触发条件 | 含义与应对 |
|---|---|---|---|
| `[APP] panic` | `apps/src/lib.rs:42` | 任意 apps Rust panic | **致命**：随后 `exit(3)`，任务管理器显示退出码 3 |
| `[APP] display list over shape budget` / `display list over text budget` | `apps/src/wm.rs:280/326` | 一帧内 shape/text 达上限 | 每帧一次；图形负载过高，检查应用绘制逻辑 |
| `[APP] settings up` / `netman up` / `setup wizard up` / `fileman up` / `session manager up` / `devmgmt up` / `taskman up` | `apps/src/bin/{settings.rs:564,netman.rs:215,setup.rs:712,fileman.rs:872,session.rs:97,devmgmt.rs:12,taskman.rs:308}` | 应用启动 | **冒烟断言点**：缺少说明应用没起来 |

**nsh 控制台（`apps/src/bin/nsh.rs`）**：

| 日志文本/字节串 | 位置 | 触发条件 | 含义与应对 |
|---|---|---|---|
| ANSI 着色输出（`SerialSink::flush_line`） | `nsh.rs:421` | 每行刷出 | 绿色=`pass/ok/success/done`，红色=`error/fail/fatal`，黄色=`warn/warning/caution`（`color_for()` `:456–474`）；视觉信号 |
| `\x1b[2J\x1b[H` | `nsh.rs:448/495` | `clear` / 控制台模式初始化 | 清屏 |
| `\x1b[36mNovus OS nsh (console)\x1b[0m - type 'help'.` | `nsh.rs:496` | 控制台模式横幅 | 无桌面 shell 起点 |
| `user@novus:{cwd}$ ` | `nsh.rs:500–502` | 每条命令前 | 提示符 |
| `\r\n` / `\x08 \x08` / `^C\r\n` / 回显 | `nsh.rs:516/530/534/548` | 回车/退格/Ctrl+C/可打印字符 | 行编辑反馈 |
| `Novus OS nsh - type 'help'.` | `nsh.rs:609` | GUI 终端欢迎语 | 写 scrollback，非串口 |
| 右键菜单：清屏/命令帮助/命令历史/当前用户/磁盘列表 | `nsh.rs:772–785` | GUI 右键 | 触发 `help`/`history`/`whoami`/`drives` |
| GUI Ctrl+C → `{prompt}{input}^C` / Ctrl+L → `scroll.clear()` | `nsh.rs:737–759` | GUI 快捷键 | — |

**nsh 内建命令错误/诊断输出（`apps/src/shell/cmds.rs`）**——全部 33 个内建：`ls, cd, pwd, cat, echo, mkdir, rmdir, rm, cp, mv, stat, env, export, unset, clear, help, history, true, false, whoami, which, touch, wc, head, tail, tree, alias, drives, mount, umount, nuomi, update`。输出辅助 `puts/putb/putc/put_dec/put_size`（`:17–47`）。逐条文本（按命令分组）：

| 命令 | 文本（完整拼接） | 位置 | 触发条件 |
|---|---|---|---|
| ls | `ls: {path}: path too long` / `ls: {path}: no such file or directory` / `{path}:` / `d\|- {size}{name}` / `- {size} {path}` | `cmds.rs:143–212` | 路径过长/不存在/多路径标题/目录项/文件 |
| cd | `cd: path too long` / `cd: not a directory: {target}` / `cd: no such file or directory: {target}` | `:230–250` | 对应失败 |
| pwd / cat / echo | `{cwd}` / `cat: {file}: no such file` / 文件内容 / stdin / echo 参数 + `\n` | `:258–301` | — |
| mkdir | `mkdir: path too long` / `mkdir: cannot create directory: {path}` / `...: file exists` | `:328–380` | `-p` 失败/已存在/其他失败 |
| rm | `rm: cannot remove: {path}: no such file or directory` / `rm: cannot remove directory: {path}` / `rm: path too long` / `rm: cannot remove '{path}': is a directory (use -r)` / `rm: cannot remove: {path}` | `:391–482` | 对应失败；注意 `-f` 抑制 |
| rmdir | `rmdir: path too long` / `failed to remove '{a}': no such file or directory` / `not a directory` / `directory not empty` | `:499–521` | — |
| cp | `cp: cannot open for read` / `cp: cannot stat source` / `cp: cannot create directory: {dst}` / `cp: missing file operand` | `:534–607` | — |
| mv / stat | `mv: missing destination` / `mv: cannot move '{src}'` / `stat: missing operand` / `  File: {a}  Size: {n}  Type: directory\|regular file` / `stat: cannot stat '{a}'` | `:654–717` | — |
| env / clear | `{name}={value}`（每行）/ `out.clear_screen()` | `:727–781` | `export` 无参数也走 env |
| help / history / whoami / which | `nsh built-ins:` + 内建列表 / 管道重定向 `$(...)` `$VAR` 说明 / `{序号} {历史行}` / `user` / `{name} is a shell builtin` / `{name} is /bin/{name}` / `{name}: not found` | `:786–837` | — |
| touch / wc / head / tail / tree / alias | `touch: cannot touch '{a}'` / `{行} {词} {字节}`（+文件/汇总）/ 原始文件行 / 缩进+名字 / `='{val}'` / `alias: {a}: not found` | `:851–1090` | — |
| drives / mount / umount | `mounts: kernel rejected the request` / `MOUNT POINT DEVICE FSTYPE MODE` / `(no mounts)` / 挂载表行 / `mount: cannot open /.novusfs/mounts.json` / `mount: cannot read the mount table` / `mount: {n} volume(s) not mounted` / `usage: mount ...` / `mount: path too long` / `mount: failed (unknown device, volume or mount point)` / `umount: ...` | `:1149–1247` | `drives` 列出真实挂载表，是排挂载问题的首选 |
| 未知命令 | `{name}: command not found`（退出码 127） | `:1299–1300` | 非内建且 `spawn_name` 失败 |

> `cmd.exe` 兼容别名默认关闭（`shell.cmd_aliases`）：`dir→ls, copy→cp, del→rm, move→mv, type→cat, cls→clear`（`:1274–1288`）。`run_subst` 嵌套 ≥3 层返回 -1 无提示（`mod.rs:322–330`）；非法 UTF-8 显示 `<?>`（`mod.rs:453–455`）。

#### 12.3.12 update 与 Nuomi 的日志

**`update` 命令（`apps/src/shell/update.rs`）**：

| 文本 | 位置 | 触发条件 |
|---|---|---|
| `update: cannot open {dir}` | `:155–157` | 扫描目录失败 |
| `[OK]   {full}  {level}  {type}` / `[FAIL] {full}  {err}` | `:188–201` | 校验通过/失败；level=`optional/recommend/preview/urgent/dev`，type=`.novu/.npth/.apth/.hotpth` |
| `update: cannot read package` / `update: verify failed: {err}` / `update: manifest offset out of range` / `update: cannot mkdir {parent}` / `update: write failed {dst}` / `  put {dst}` / `  del {path} (unlink not yet wired)` / `update: applied {name}` | `:237–304` | apply 流程 |
| `update: cannot read {file}` / `VERIFIED {name} level={level} uuid={hex16}` / `REJECTED: {err}` | `:402–420` | `--verify` |
| `update root pubkey: {hex64}` / `service: {state} ({n})` / `check requested` / `updates: {n}` + `  {uuid16} {name} type={n} level={n} state={n}` / `deferred` / `update: bad uuid` / `rolled back\|rollback failed` / `update: unknown option {a}` / usage | `:429–573` | 各选项 |

`verify()` 返回错误串（最终由 `[FAIL]`/`REJECTED:`/`verify failed:` 打印）：`truncated header`(:112)、`bad magic`(:115)、`unsupported header version`(:118)、`uuid mismatch`(:122)、`target OS version too new`(:125)、`min compatible version too new`(:128)、`SHA-256 mismatch`(:136)、`bad signature length`(:141)、`Ed25519 signature rejected`(:144)。

**Nuomi（`apps/src/nuomi.rs`，nsh 内版本）**：

| 文本 | 位置 | 触发条件 |
|---|---|---|
| `Nuomi 0.3.0 - a Lisp-flavoured scripting language` + usage | `:2308–2309` | 无参数 |
| `nuomi: -e needs an expression` / `nuomi: -c needs the host toolchain (user/Nuomi_pl), not nsh` / `nuomi: path too long` / `nuomi: script too large (limit 6 KiB)` / `nuomi: cannot open {path}` / `nuomi: cannot read {path}` | `:2317–2364` | 命令行/加载 |
| `nuomi: {msg}` | `:2397–2399` | 统一由 `fail()` 加前缀输出 |

运行时/求值错误串（约 60 条，全部经 `fail()` 输出）：解析/环境 `vector too long`(:435)、`no environment`(:490)、`too many bindings`(:504)、`reader nesting too deep`(:517)、`unexpected end of input`(:521)、`unexpected )`(:534)、`unterminated list`(:572)、`mismatched bracket`(:579)、`list literal too long`(:582)、`empty atom`(:609)、`maximum recursion depth exceeded`(:632)、`unterminated string`(:1918)、`string too long`(:1925)；求值/绑定 `unbound variable`(:651/789)、`set!: unbound variable`(:727)、`set!: not a symbol`(:732)、`too many arguments`(:798)、`out of memory`(:827 等)、`define: bad function name`(:850)、`define: bad form`(:854)、`closure: parameter is not a symbol`(:867)、`too many parameters`(:870)、`let: binding name is not a symbol`(:914)、`let: too many bindings`(:917)、`let*: binding name is not a symbol`(:959)、`letrec: binding name is not a symbol`(:985)、`arity mismatch`(:1014)、`not a callable`(:1031)、`missing argument`(:1814)、`expects a number`(:1820)、`expects a string`(:1827)；算术/比较 `- expects numbers`(:1056)、`/ expects numbers`(:1080)、`division by zero`(:1089)、`% expects two numbers`(:1097)、`modulo by zero`(:1103)、`not expects 1 arg`(:1130)、`pow expects 2 args`(:1596)、`min/max expects numbers`(:1602)；列表/向量/字符串/集合 `cons expects 2 args`(:1176)、`car: not a pair`(:1182)、`cdr: not a pair`(:1186)、`length expects 1 arg`(:1191)、`length: bad type`(:1198)、`get expects (coll index)`(:1204)、`set expects (vec index val)`(:1244)、`set: index out of range`(:1252)、`set: not a vector`(:1261)、`substring expects (s [start [end]])`(:1278)、`string-search expects (haystack needle [start])`(:1298)、`last expects 1 arg`(:1326)、`last expects a non-empty list`(:1329)、`reverse expects 1 arg`(:1335)、`reverse: bad type`(:1337)、`append expects 2 args`(:1347)、`append expects lists or vectors`(:1349–1350)、`member? expects (x coll)`(:1385)、`member? expects a list or vector`(:1387)、`map expects (fn coll)`(:1398)、`map expects a list or vector`(:1401)、`filter expects (pred coll)`(:1425)、`filter expects a list or vector`(:1428)、`reduce expects (fn coll) or (fn init coll)`(:1455)、`reduce: one arg must be a list or vector`(:1473)、`reduce expects a list or vector`(:1476)、`range expects (n [end [step]])`(:1494)、`range: zero step`(:1505)、`assoc expects (list key [val])`(:1531)；文件/进程 `write-file expects (path text)`(:1652)、`mv expects (from to)`(:1710)、`cp expects (from to)`(:1727)。

输出原语：`Print/Println` 参数间加空格、println 末尾换行（`:1156–1166`）；`Draw` 原样（`:1168–1172`）；`fmt_num` → `nan/inf/-inf/-`（`:2144–2228`）；`fmt_val` → `...`（深度>16）/`nil/true/false`/数字/`"..."` /`raw`/`(...)` /`(a . b)`/`[...]`/`<fn lambda>`/`<native NAME>`（`:2230–2290`）。

#### 12.3.13 构建工具与主机工具链

**镜像构建工具（构建期调试）**：

| 工具 | 输出（路径前缀 `ROOT = ...`） |
|---|---|
| `tools/mkdisk/mkdisk.c` | `novuslib-rs panic: `(:95)；`mkdisk: cannot open/cannot size/is empty/failed writing the ESP/failed writing the system partition/GPT write failed/cannot flush`(:202–337)；usage(:234)；成功 `mkdisk: {file} {n} MiB: ESP at LBA ...`(:330) |
| `tools/mkfat/mkfat.c` | `mkfat: {msg}`(:74)；`mkfat: {src} -> {dest} {n} bytes (cluster ..., FAT..)`(:654)；usage(:811)；`mkfat: wrote {out} ({n} MiB, {u} clusters, FAT..)`(:901) |
| `tools/mkiso/mkiso.c` | `mkiso: {msg}`(:114)；`Joliet root is missing ...`(:1029)；`  {src} ... {n} bytes`(:1052–1055)；usage(:1071)；`unexpected argument '{s}'`(:1094)；`{out} {n} sectors`(:1199) |
| `tools/mkfsnovus/mkfsnovus.c` | `novuslib-rs panic: `(:221)；`mkfsnovus: cannot open/size/out of memory/short read/bad destination path/mkdir failed/empty destination file name/creat failed/write failed`(:109–189)；`injected {src} -> {dest} ({n} bytes)`(:194)；usage(:309) |
| `tools/wpkg/wpkg.c` | `wpkg: {msg}`(:39)；`cannot open/{f} is empty`(:57–65)；usage(:110)；`{out} {n} images, {n} bytes`(:163)；`  [{i}] {name} {n} bytes`(:165) |

**更新/密钥/证书工具**：`tools/updatesrv/updatesrv.py` 日志前缀 `[updatesrv] `（`:99/157`，`--quiet` 静音）；`selftest.py` 输出 `<name> OK/FAIL`、`ALL PASS (n failures)`（`:21/76`）；`tools/novupkg/src/main.rs` 输出 `novupkg: ...`、`PASS/FAIL <name>`、`novupkg selftest: OK / N FAILED`、usage（`:142–381`）；`tools/nkmgen/src/main.rs` 输出 `[NKMGEN] SHA-256 / SHA-512 known answers`、`[NKMGEN] Ed25519 RFC 8032 vectors`、`PASS/FAIL <name>`、`seed:/pub:/pub C array:`（`:52–582`）；`tools/tlstest/host_tls13.c` 输出 `[io] ...`/`[host] ...`（`:38–91`）。

**Nuomi 主机工具链 `user/Nuomi_pl/`**：

| 文件 | 输出 |
|---|---|
| `src/eval.c` | `Runtime error: %s`(:13)；`-DNUOMI_DEBUG` 时 `  [define] %s -> val seq=%lu objtype=%d ...`(:68)、`  [eval] LIST/OBJ/scalar ...`(:395–400)、`  [apply] ...`(:467–502) |
| `src/value.c` | `-DNUOMI_DEBUG` 时 `NUOMI_DEBUG: %s @ '%s' on object NOT LIVE ...`(:48–52)、`  [release] @%p rc=%d`(:87)、`[FATAL] over-release: ...`(:93)、`[FREE] seq=%lu type=%d @%p phase='%s'`(:108)、`  [free #%lu] ...`(:117–119)；用于"释放后再访问"取证 |
| `src/main.c` | `cannot open %s`/`read error`(:30/33)、`Parse error: %s`(:43/98)、`Execution stopped: recursion too deep.`(:58)、`  [runFile] release(form) seq=%lu`(:62)、REPL 提示 `Nuomi ... 输入表达式回车求值...`/`nuomi> `/`=> `(:74–115)、编译模式 `compiled %s -> %s (%zu bytes)`(:248) |
| `src/nucutil.c` | `build failed`/`write failed`/`wrote %s (%zu bytes)`(:48–50)、`reloc type %u unsupported by host runner (skipped)`(:101)、`[run] 该产物含系统调用...可用 nucrun.py 验证`(:196–198)、`entry returned: %lld (0x%LLX)`(:206)、`parse failed`/`validate: %s`/`no TEXT segment`(:213–217)、`VirtualAlloc failed`/`mmap(RWX) failed`/`run: 当前平台暂不支持真机执行`(:238–274)、`usage: nuomi nuc <dump\|validate\|emit-demo\|run> <file>`(:284) |
| `src/nucfmt.c` | `.nuc` 二进制 dump：`=== NUC Header ===`、`magic : NUC!`、`abi_version`、`compile_flag (64bit=... pic=... static=... stackprot=... debug=...)`、`entry_point`、`seg_count`、`symtab_count`、`strtab_offset`、`rel_offset`、`stack_size`、`rel_count`、`=== Segment Table ===`、`[i] type file_off vaddr size flags`、`=== Symbol Table ===`、`=== Relocation Table ===`、`entry_rip = text.vaddr + entry_point`（`:408–456`） |
| `src/update.c` | `Nuomi %s`、`未知选项`、`用法: nuomi update ...`、`当前版本`/`最新版本`、`已是最新版本`、`有可用更新`、`正在下载`/`下载失败`、`更新完成`等（`:179–303`） |
| `tools/nucrun.py` | `不是 .nuc 文件`(:60)、`不支持的重定位类型 %d`(:116)、`=== 段表 ===`/`=== 符号 ===`/`=== 重定位 ===`(:127–136)、`未处理的中断 int 0x%X @ rip=0x%X`(:170)、**`  [syscall] write(fd=, buf=, len=) -> `**(:181)、`  [syscall] exit(%d)`(:186)、`[nucrun] 执行异常: %s (rip=0x%X)`(:196) |
| `user/cppcc/` | `cppcc: {:#}`(main.rs:5)、`-E` 预处理输出/`--dump-ast`/`--check` 成功/`wrote {path}`/编译诊断/usage（driver.rs:51–150）；C ABI 侧靠 `cppcc_last_error()` 与 `STATUS_OK/DIAGNOSTICS/ERROR`(:16–30/229–234) 取错 |

---

## 13. 手工 Debug 方法全解

> 本章全部来自对当前仓库脚本、源码与文档的实测归纳。路径前缀 `ROOT = //wsl.localhost/Debian/home/dev/novus-os/novus-os/`。

### 13.0 总览：调试链路与工具选择

```
[源码] kputs() ──┬──> VGA 文本 (0xB8000)      ── 肉眼看屏幕
                 ├──> COM1 串口 (0x3F8,115200) ── -serial stdio（终端）/ -serial file:xxx.log（落盘）
                 └──> con_out() ──> 帧缓冲控制台 + 镜像串口 ── nsh 会话可完整抓取

[引导阶段] boot_puts() ──> UEFI 控制台 + COM1（g_quiet 后只留 COM1）
[跨阶段]   shared_mem_log() ──> 0x7E000 的 512B 环形缓冲（A/B 核启动取证）
[主机侧]   QEMU monitor / QMP / VNC ──> 注入按键鼠标、screendump、quit
[离线]     selftest 入口 / nucrun.py / nuc dump / cppcc --check
```

| 症状 | 首选手段 | 次选手段 |
|---|---|---|
| 完全没输出 / 固件阶段卡死 | `-serial stdio` 看 `[BOOT]`/`[FATAL]`/`[CHECK]` | 检查 `out/NOVUS.EFI` 与 OVMF 路径 |
| 内核入口后卡死 | 看最后一条 `kputs`；对照 12.3 找"下一条本应出现"的日志 | 共享内存 `selfcheck_status`、QEMU monitor `info registers` |
| 驱动/设备没识别 | 串口 grep `[PCI]/[DEVTAB]/[AHCI]/[NVMe]/[USB]/[IRQ]` | `qtest.sh` 多控制器矩阵 |
| 文件系统挂载/完整性失败 | `[NOVS]/[M3]/[WAL ]/[MRKL]` + `novusfs_dump_layout()` | `novusfs_selftest()` 逐项 PASS/FAIL |
| 应用没起来 | `[APPS] launching` / `[APP] xxx up` / `[SCHED] task ... exited` | nsh `which` / `stat` / 任务管理器 |
| 图形黑屏/闪退 | `[ND] NovusDisplay` / `[ND] demo desktop painted` | QEMU monitor `screendump` + 截图放大（`docs/ui-design.md:168–185`） |
| 键鼠无响应 | `[IRQ] registered` / `[ND] PS/2 ...` / `[NWM] key ->` | VNC + vncdotool 注入（`docs/M9 Win32 兼容层原型.md:640–644`） |
| 网络/HTTPS 不通 | `[NET]`/`[DNS]`/`[NCSI]`/`[E2E]` | `tools/updatesrv/e2e-run.sh` |
| `.nuc` 程序跑飞 | `nucrun.py -v` 看 `[syscall]` 轨迹 | `nuomi nuc dump/validate` |

### 13.1 构建与启动

**make 目标**（`ROOT/Makefile:19–32,320–322,579–584`）：

| 命令 | 作用 |
|---|---|
| `make` | 默认 `all`，产出 `out/NOVUS.EFI` |
| `make run` | 构建 + 壁纸包 + `hello.nuc`→`esp/DEMO.NUC`，然后 `scripts/run.sh "$(MEM)"`；`MEM ?= 4G`（`:66–67`） |
| `make iso` / `make iso-force` / `make run-iso` | 生成/强制重建 live ISO 并从 ISO 启动 |
| `make disk-img` / `make install DEVICE=/dev/sdX` | 系统分区镜像 / 安装到物理盘 |
| `make check-rootfs` | 检查 rootfs 内容（镜像缺文件时先跑这个） |
| `make cppcc` / `cppcc-tools` / `cppcc-demo` | C++ 编译器工具链与演示 |
| `make clean` | 清理 |

**`scripts/run.sh [MEM]`**（`ROOT/scripts/run.sh:1–65`，`make run` 实际执行者）：

```bash
MEM="${1:-4G}"
mkdir -p esp/EFI/BOOT out
cp out/NOVUS.EFI esp/EFI/BOOT/BOOTX64.EFI
[ -f out/wallpapers.pkg ] && cp out/wallpapers.pkg esp/WPAPERS.PKG
[ -f out/OVMF_VARS.fd ] || cp /usr/share/OVMF/OVMF_VARS_4M.fd out/OVMF_VARS.fd
# 有 DISPLAY -> -display gtk；否则 -display none
qemu-system-x86_64 -machine q35 -smp 2 -m "$MEM" \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=out/OVMF_VARS.fd \
  -drive format=raw,file=fat:rw:esp \
  -serial stdio -no-reboot $DISPLAY_ARGS \
  -netdev user,id=novusnet -device "$NIC",netdev=novusnet
```

- `-smp 2`：vCPU0 = A 核(BSP)，vCPU1 = B 核(AP)。
- `-serial stdio`：COM1 直连当前终端，所有 `kputs` 实时可见。
- 网卡模型用 `NOVUS_NIC` 选择：`e1000`（默认）、`e1000e`、`virtio-net-pci`、`rtl8139`（`:57–59`）。
- `out/OVMF_VARS.fd` 首次复制后保留，固件状态跨运行持久化；异常时可删除让其重建。

**其它启动脚本**：

| 脚本 | 用途 / 参数要点 |
|---|---|
| `scripts/run-iso.sh [MEM] [DISK]` | live ISO 启动；可选第二块 IDE 盘作安装目标；`-serial stdio -net none`（`:41–57`） |
| `qtest.sh` | 存储驱动矩阵：依次启动 IDE/AHCI/NVMe/USB，各写 `/tmp/b_{ide,ahci,nvme,usb}.log`，最后 grep `[MOUNT]`（`:1–31`） |
| `scripts/run-install-test.sh [SIZE_MB]` | 安装器 E2E；`-serial file:/tmp/novus-install-serial.log -monitor unix:...`；输出 `.ppm` 截图 |
| `scripts/wm-churn.sh [MEM] [ROUNDS] [--iso]` | WM 反复开关压力；`/tmp/novus-churn.serial` + monitor socket |
| `scripts/font_smoke.sh` | 字体/启动标记冒烟；`-serial file:out/smoke.log`，grep `[FONT]/[M3]/[MOUNT]/DEVTAB/panic/assert` |
| `scripts/rebuild-kernel.sh` | 改内核后强制重建并跑 `font_smoke.sh` |
| `scripts/build-img.sh` | 重建 NovusFS 系统分区 + `inspect_img.py` |
| `tools/updatesrv/e2e-run.sh [SECONDS]` | HTTPS E2E；`make E2E=1 iso` + 更新服务器 + 串口 grep `[E2E]` |

### 13.2 串口日志抓取（最核心手段）

**驱动侧**：`kernel/serial.c` 16550 COM1 `0x3F8`，115200 8N1；`serial_puts()` 自动 `\n → \r\n`；`g_rx_dropped` 记录接收溢出（发送队列满时轮询回退）。

**两种抓取方式**：

| 方式 | 用法 | 适用 |
|---|---|---|
| 终端直出 | `-serial stdio`（`run.sh`/`run-iso.sh`） | 交互式观察、手动敲 nsh |
| 落盘抓文件 | `-serial file:<path>` | 自动化、回归、事后分析 |

**已知日志落盘位置**：

| 脚本 | 路径 |
|---|---|
| `qtest.sh` | `/tmp/b_{ide,ahci,nvme,usb}.log` |
| `scripts/font_smoke.sh` | `out/smoke.log` |
| `scripts/qemu_smoke.py` | `<output>/serial.log`（默认 `out/qemu-smoke/serial.log`） |
| `scripts/run-install-test.sh` | `/tmp/novus-install-serial.log` |
| `scripts/wm-churn.sh` | `/tmp/novus-churn.serial` |
| `tools/updatesrv/e2e-run.sh` | `$LOGDIR/serial.log`（默认 `/tmp/novus-e2e`） |

**过滤与分析**：

```bash
# 简单正则过滤，打印行号+内容（默认 out/smoke.log）
python3 scripts/grep_log.py [path]
# 其内置正则：FONT|M3|panic|assert|NOVUS OS|DEVTAB|font|novusfs|stat|error|fail|dbg（忽略大小写）

# 常用手工 grep
grep -E '\[(FATAL|PANIC|ERROR|WARN|TRAP)\]' out/qemu-smoke/serial.log
grep -E 'selftest (PASS|FAIL)|\[NOVS\] (PASS|FAIL)' out/qemu-smoke/serial.log
tail -n 80 out/qemu-smoke/serial.log     # 看最后停在哪里
```

**`scripts/qemu_smoke.py`**（包装器 `scripts/qemu-smoke.sh`）：全自动串口解析 + RFB 截图比对。

```bash
bash scripts/qemu-smoke.sh
bash scripts/qemu-smoke.sh --no-root --output out/qemu-smoke-no-root
python3 scripts/qemu_smoke.py --timeout 180 --output out/qemu-smoke \
  --baseline scripts/baselines/qemu/disk --tolerance 8 --max-changed 0.002
```

- 失败正则（`:20–22`）：`[FATAL]`、`[TRAP] kernel fault`、`selftest...FAIL`、`[ND] FAIL:`、`display list over`、`app launch failed`、`[SCHED] task ... exited`。
- 等待 marker（`:246–342`）：`[ND] demo desktop painted`、`[HALT] entering the scheduler`、`[ND] absolute pointer ready`、`[ND] selftest PASS`、`[HEAP] selftest PASS`、`[M3] live root mounted` 或 `[M3] no live root published`、`[FONT] no system filesystem`、`[WP] wallpaper ready`、`[APP] session manager up`/`settings up`/`netman up`、`[APPS] launching nsh`、`[ND] keyboard byte from IRQ queue`。
- 输出：`out/qemu-smoke/{serial.log,stderr.log,result.json,*.png}`。
- 前置：Python 3.8+、2 核 3GiB、VNC 仅本地 5901（`README.md:507–524`）。

**回归入口**（`CONTRIBUTING.md:185–219`、`README.md:497–505`）：

```bash
bash scripts/display-selftest.sh
bash scripts/irq-selftest.sh
python3 -m unittest discover -s scripts -p qemu_smoke_test.py
python3 -m unittest discover -s scripts -p novusimg_test.py
make disk-img
```

按改动类型：`irq-selftest.sh`（内核启动）、`bash qtest.sh`（存储驱动）、`scripts/run-install-test.sh 256`（安装器，`EN=1` 英文、`FLOW=desktop`）、`scripts/wm-churn.sh 4G 20`（GUI）。

### 13.3 QEMU monitor（HMP）

**启用**：`-monitor unix:<sock>,server,nowait`（`run-install-test.sh:61`、`wm-churn.sh:79`）；QMP 用 `-qmp tcp:...`（`qemu_smoke.py:398`）。

**常用命令**：

| 命令 | 用途 | 范例位置 |
|---|---|---|
| `screendump <file.ppm>` | 抓帧缓冲截图 | `run-install-test.sh:117,135,150,157` |
| `sendkey <key>` | 注入按键（`3`/`ret`/`spc`） | `:124,140–153` |
| `mouse_move X Y` / `mouse_button 1/0` | 注入鼠标 | `:130–133` |
| `info registers` | 查寄存器（配合 `[TRAP]` 诊断） | 通用 HMP |
| `quit` | 退出 QEMU | `:119,159` |

**两种发命令方式**：

```bash
# Python socket 连接 UNIX socket（完整范例：scripts/run-install-test.sh:102–160）
# cmd() 发送文本命令并读回响应

# socat 一行式（scripts/wm-churn.sh:82–85）
printf '%s\n' "screendump /tmp/shot.ppm" | socat - "UNIX-CONNECT:$MON"
```

**已知限制**：`-display none` 下 `mouse_move` 不驱动客户机光标，需改走 VNC + vncdotool（`docs/M9 Win32 兼容层原型.md:640–644`）；headless 下 `sendkey` 不可靠（`docs/核查-2026-11-13-BUG与UI.md:112–113`）。

### 13.4 VNC 与截图验证

- `scripts/qemu_smoke.py` 使用 `-vnc :1`（本地 5901）+ RFB 截图，与 `scripts/baselines/qemu/{disk,no-root}` 比对。
- 截图观察流程：`docs/ui-design.md:168–185` —— "验证：截图 + 放大，而不是我觉得行"，用 PIL 放大后人工检查。
- 安装器截图：`/tmp/novus-install*.ppm`（`run-install-test.sh`）。
- 判断图形是否真起来：**先看串口 `[ND] demo desktop painted`，再看截图**，二者缺一都算未通过。

### 13.5 GDB / gdbstub（仓库空白 + 补法）

**结论**：全库搜索 `gdbstub` / `-gdb tcp::` / `-s -S` / `target remote`，**没有任何启用 gdbstub 的脚本或文档**（命中的 "gdb" 全是 PEM 证书 base64 误匹配）。本仓库的"调试检查点"是源码级 assert/log 建议，非 GDB 流程。

**如需源码级调试，手动补上**：

```bash
# 1) 在 scripts/run.sh 的 qemu 命令追加（gdbstub 监听 tcp::1234，-S 暂停待连）
-s -S

# 2) 另一个终端
gdb out/NOVUS.EFI          # 或带符号的内核 ELF（若有）
(gdb) target remote :1234
(gdb) break kmain
(gdb) continue
```

注意：UEFI 应用是位置无关可执行，需确认 gdb 加载的符号地址与运行时基址一致；否则断点不命中。在没有符号表的构建里，优先用 12.2 的 `[TRAP] rip=` + `kputs` 插桩定位。

### 13.6 共享内存日志与 M-debug 快照读取

**结构**（`kernel/shared_mem.h:63–129`，物理 `0x7E000`，8 KiB）：

| 偏移 | 字段 | 用途 |
|---|---|---|
| `0x000` | `magic`/`version` | `NVSH v1.1` 校验 |
| `0x008` | `selfcheck_status` | `SHM_STATUS_*`；启动失败时首查 |
| `0x00C` | `cpu_cores` | 逻辑处理器数 |
| `0x010/0x020` | `cpu_vendor[16]`/`cpu_brand[64]` | CPU 信息 |
| `0x060/0x068` | `usable_mem_bytes`/`ram_top_bytes` | 内存布局 |
| `0x070` | `ap_online_count` | AP 上线数 |
| `0x078` | `ap_command` | `SHARED_MEM_CMD_*`（休眠命令） |
| `0x07C` | `spinlock` | CAS 锁（超时 WARN 降级） |
| `0x080` | `log_buf[512]` | 启动期环形日志（`shared_mem_log` 写入） |
| `0x280` | `log_head` | 下一写入索引 |
| `0x284` | `merkle_status` / `merkle_root[32]` / `merkle_height` / `merkle_leaf_count` | M3 完整性结果 |
| `0x2B0/0x2B8` | `live_root_base`/`live_root_size` | live root 内存镜像 |
| `0x2C0` | `boot_mode` | `SHM_BOOT_MODE_*`（no-desktop/install/普通） |
| `0x2C4–0x2D8` | `nkm_status`/`nkm_loaded`/`nkm_img_base`/`nkm_img_size` | 模块加载结果 |
| `0x2E0–0x2E8` | `app_img_base`/`app_img_size` | 暂存的 NUC 应用 |
| `0x2F0–0x310` | `esp_img_base`/`esp_img_size`/`install_flags` | 安装载荷 |
| `0x304` | `boot_usb_port` | 启动 USB 口（安装器保护） |
| `0x308–0x310` | `wpkg_img_base`/`wpkg_img_size` | 壁纸包 |
| `0x318/0x348/0x378/0x380/0x388/0x3A8` | `dbg_tmp[6]`/`dbg_live[6]`/`dbg_route`/`dbg_tmp_addr`/`dbg_fat[4]`/`dbg_ktest` | **M-debug**：live-root 损坏取证 |

**冻结布局断言**：`_Static_assert` 保证 `spinlock@0x7C`、`log_head@0x280`、`merkle@0x284`、`boot_mode@0x2C0` 等不漂移（`:136–165`）。漂移会在**编译期**报 `shared_mem frozen layout drift: ...`。

**手工读取方式**：QEMU monitor 的 `xp` 命令可读物理内存，例如：

```
(qemu) xp/4gx 0x7e000        # magic/version/selfcheck_status/cpu_cores
(qemu) xp/16gx 0x7e318       # dbg_tmp[6] + dbg_live[6]
(qemu) xp/1gx 0x7e378        # dbg_route: 1=固件FS, 2=fat32
```

`dbg_tmp` 是 ESP 读缓冲块 0/130/162，`dbg_live` 是保留副本；对比二者可区分"固件读"还是"内存副本"是 live-root 损坏源（`shared_mem.h:118–120`，引导侧写入 `stage3.c:277/312/345–351,897–924`）。`dbg_ktest` 当前**只有声明无写入**。

### 13.7 自检入口速查（无需改代码即可跑）

| 目的 | 调用 | 位置 | 观察日志 |
|---|---|---|---|
| NovusFS 全链路（格式/根目录/WAL/重挂载/Merkle 篡改） | `novusfs_selftest()` | `fs/novusfs/novusfs.c:655` | `[NOVS] === M3 selftest` + 逐项 PASS/FAIL |
| 打印 FS 布局（WAL/Merkle/位图/inode/data 起始块） | `novusfs_dump_layout(fs)` | `fs/novusfs/super.c:351` | `[NOVS] layout total=...` |
| VFS 分派（写/读/stat/readdir/unlink） | `vfs_selftest()` | `fs/vfs.c:420` | `[VFS] selftest ...` |
| ext4 持久化与 remount | `ext4_selftest()` | `fs/ext4/mount.c:135` | `[EXT4] selftest ...` |
| NKM 验签全步骤 | `nkm_selftest()` | `security/nkm/nkm.c:856` | `[NKM] PASS/FAIL ...` |
| 图形栈（像素/裁剪/widget） | `nd_selftest()` | `graphics/nd_demo.c:99` | `[ND] selftest: ...` |
| 窗口协议（create/destroy/未知请求） | `nwm_selftest()`（启动时自动） | `wm/novus-wm/nwm.c:1636` | `[NWM] selftest: ...` |
| IPC 队列/payload | `ipc_selftest()` | `kernel/ipc.c:323` | `[IPC] selftest: ...` |
| 物理页分配回归 | `pmm_selftest()` | `kernel/pmm.c:321–392` | `[PMM] selftest ...` |
| 堆（kmalloc/pattern/别名/泄漏） | `heap_selftest()` | `kernel/heap.c:115–144` | `[HEAP] selftest ...` |
| PE 加载器 | `pe_selftest()` | `kernel/pe.c:817` | `[PE] selftest PASS\|FAIL` |

> `kmain.c` 启动时会调用其中多项；若日志中出现 `[WARN] xxx selftest failed`，先找到对应 `[TAG] FAIL <what>` 那一行——它才是具体失败点。

### 13.8 nsh 手工诊断命令

无需 GUI，控制台模式或 GUI 终端里均可使用：

| 命令 | 诊断用途 |
|---|---|
| `stat <path>` | 查看文件/目录大小与类型（验证镜像内容） |
| `drives` | 列出**真实**挂载表：挂载点、设备、卷类型、只读标志 |
| `mount [-a]` / `umount <path>` | 重放 `/.novusfs/mounts.json` / 卸载；失败文本见 12.3.11 |
| `which <name>` | 区分 shell 内建 / `/bin/<name>` / 未找到 |
| `env` / `export` / `unset` | 查看/修改环境变量（影响 `$VAR` 展开） |
| `help` / `history` | 命令清单 / 历史（确认命令是否可用） |
| `nuomi -e <expr>` / `nuomi <file>` | 快速验证脚本/表达式；错误串见 12.3.12 |
| `update --verify <file>` / `--status` / `--list` | 更新包校验/服务状态/列表 |
| `ls -l` / `tree` / `cat` / `head` / `tail` / `wc` | 文件与目录内容检查 |
| 重定向 `< > >>`、管道 `\|`、`$(...)`、`$VAR`、glob | 组合诊断（如 `drives \| grep Novus`） |

交互快捷键：Ctrl+C 中断（`^C`）、Ctrl+L 清屏、上下历史。GUI 终端右键菜单：清屏/命令帮助/命令历史/当前用户/磁盘列表。系统设置里有"开发者选项：内核调试、日志输出、模块加载开关"（`README.md:354–360`）。

### 13.9 主机侧 `.nuc` / Nuomi / cppcc 调试

当客户机无法启动时，在主机上直接验证产物：

```bash
# .nuc 二进制结构 dump（头/段/符号/重定位表）
nuomi nuc dump <file.nuc>

# 校验 .nuc 合法性
nuomi nuc validate <file.nuc>

# 在 Unicorn 上模拟内核执行，打印 [syscall] 轨迹（-v 详细模式）
python3 user/Nuomi_pl/tools/nucrun.py -v <file.nuc>

# C 解释器开启调试插桩（[eval]/[apply]/[free]/[FATAL] over-release 检测）
gcc -DNUOMI_DEBUG ...

# C++ 编译器前端检查 / AST dump / 预处理
cppcc --check input.cc
cppcc --dump-ast input.cc
cppcc -E input.cc
```

`nucrun.py` 的 `[syscall] write(fd=..., buf=0x..., len=...) -> ...` 与 `[syscall] exit(...)` 是判断 `.nuc` 程序是否按预期调用内核的关键证据。`nuomi nuc dump` 输出 `entry_rip = text.vaddr + entry_point`，可与 `[TRAP] rip=` 对比定位跑飞地址。

### 13.10 断言、panic、寄存器转储与静态断言

| 机制 | 位置 | 行为 | 调试用法 |
|---|---|---|---|
| Rust panic hook | `kernel/pmm.c:297–315` | `[PANIC] novuslib-rs: {msg}` + `cli;hlt` | 消息最多 64 字节；定位 Rust 侧 `panic!` |
| CPU 异常寄存器转储 | `kernel/trap.c:1133–1155` | `[TRAP] {name} err/rip/cs/rsp` | ring3 → 杀任务；ring0 → `cpu_halt()`；**仅 4 个值，无 backtrace** |
| 系统调用 trace（受限） | `kernel/trap.c:1108–1128` | `[TRAP] unknown syscall {rax}` / `win32 ABI denied syscall {rax}` | 各限流 8 条；无全量 tracer |
| 内核 idle 停机 | `kernel/sched.c:270–274` | `sti;hlt`，每秒扫磁盘降速 | 正常空转 |
| AP 停放 | `kernel/smp.c:147–149` | `cli;hlt` 等 `ap_command=SLEEP` | 正常 |
| 引导 FATAL | `arch/x86_64/boot/main.c:349–357,401,462,477` | `[FATAL]` + `SHM_STATUS_FAIL` + `EFI_LOAD_ERROR` | 不 HLT，交回固件 |
| 编译期断言 | `kernel/shared_mem.h:136–165` | `_Static_assert` 布局漂移 | 编译失败即发现，不进运行时 |
| 栈探测 | `arch/x86_64/kernel/chkstk.S` | `___chkstk_ms` 逐页 `testl` 提交栈 | 无打印 |

**运行时 assert 的替代方案**：源码无 `assert()`。调试时可临时在关键点插入 `kputs("[DBG] <tag>\n")` + `kputs_hex(value)`，重新 `make iso` 后从串口观察；定位后移除。**不要**在中断高频路径（VSync 每帧、每包 TCP）长期保留日志——会冲爆 1024B 串口队列。

### 13.11 配置与编译开关

| 开关 | 位置 | 作用 |
|---|---|---|
| `CONFIG_NOVUS_DEBUG_SERIAL`（default y） | `Kconfig:28–33`、`configs/novus_defconfig:15–19` | COM1 调试输出；**最直接的调试开关** |
| `CONFIG_NOVUS_BOOT_VGA`（default y） | `Kconfig:22–26`、`defconfig:15–16` | VGA 文本输出 |
| `CONFIG_NOVUS_SMP`（default y） | `Kconfig:51–57`、`defconfig:24–32` | SMP；`make run` 用 `-smp 2` |
| `CONFIG_NOVUS_A_CORE_DISABLE`（default n） | `Kconfig:41–49`、`defconfig:24–32` | B 核跑主内核 |
| `CONFIG_NOVUS_MIN_MEMORY_MB=2048` | `defconfig:11–13` | <2GiB 引导 FATAL |
| `CONFIG_NOVUS_IRQ` / `DISPLAY` / `IPC` / `WM` / `NET` | `Kconfig:67–73,149–207` | 功能开关，间接影响可观察面 |
| `E2E=1`（`make E2E=1 iso`） | `Makefile:69–71,202–206,240–241` | 追加 `-DNOVUS_HTTPS_E2E_PROBE`、`-DTLS13_KERN_DEBUG=1`；开机做 HTTPS 探测 |
| `TLS13_KERN_DEBUG` / `TLS13_HOST_DEBUG` / `TLS12_HOST_DEBUG` | `tls13_client.c:14–39`、`tls12_client.c:22–24` | TLS 关键失败日志 / 主机侧全量调试 + transcript 转储 |
| `NOVUS_NIC=<model>` | `scripts/run.sh:57–59` | 切换网卡模型 |
| `NUOMI_DEBUG`（主机 C 工具链） | `user/Nuomi_pl/src/value.c:16` | `[eval]/[apply]/[free]/[FATAL]` 插桩 |

### 13.12 分场景排错流程（按日志逐段收敛）

**A. 开机完全无输出**

1. 确认 `out/NOVUS.EFI` 存在（否则 `make`）。
2. 确认 `scripts/run.sh` 的 OVMF 路径（`/usr/share/OVMF/OVMF_CODE_4M.fd`、`out/OVMF_VARS.fd`）。
3. 有 UEFI 横幅但无 `[BOOT] UEFI boot started` → EFI 应用未进入。
4. 有 `[BOOT] Exiting boot services...` 后静默 → 内核入口崩溃；看共享内存 `selfcheck_status`。

**B. 内存不足 FATAL**

`[CHECK] Usable memory: ... MiB` < 2048 → 加大 `make run MEM=4G` 或检查 QEMU `-m`。

**C. 卡在 SMP 启动**

依次检查 `[SMP] LAPIC at`、`[SMP] MADT enabled xAPIC IDs`、`[SMP] Waking AP ID`、`[SMP] AP online, ID` / `[SMP] WARN AP wake timeout`。有 `degrading` 说明按 spec 3.4 降级为单核——确认 `-smp 2` 是否生效。

**D. 存储/文件系统失败**

1. `[PCI]`/`[DEVTAB]` 是否列出控制器。
2. `[AHCI]/[NVMe]/[USB]` 的 `LBA 0 read PASS`。
3. `[GPT ]` 是否是 `no GPT signature`。
4. `[NOVS] WARN primary superblock bad` → 是否 `mounted from backup`。
5. `[WAL ] WARN ...` → WAL 损坏，`wal_corrupt=1`。
6. `[M3] integrity: MISMATCH` → 镜像被改动。
7. 仍不明 → `novusfs_dump_layout()` + `novusfs_selftest()`。

**E. 应用启动失败**

`[APPS] launching <name>` 是否出现 → `[APPS] spawn failed: <hex>` 的返回码 → `[SCHED] task ... exited with code`（3 = Rust panic；127 = 命令未找到）→ `[APP] panic`。GUI 应用还需看 `[NWM] ERROR no IPC port` 与 `[NWM] Novus-WM up`。

**F. 图形黑屏 / 无桌面**

`[ND] NovusDisplay` 是否存在 → `[ND] compositor: ...` → `[ND] demo desktop painted`。若 `[BOOT] no-desktop mode: launching the shell` 出现，说明走的是无桌面模式，检查共享内存 `boot_mode`。最后用 QEMU monitor `screendump` 截图确认。

**G. 键鼠无响应**

`[IRQ] registered kbd/mouse` → `[ND] PS/2 keyboard + mouse IRQ enabled` → `[NWM] key -> win=... pid=...`。headless 下 `sendkey`/`mouse_move` 不可靠，改用 VNC + vncdotool。

**H. 网络 / HTTPS 不通**

`[NET] ... link=...` → `[NET] dhcp ...` → `[DNS] ...` → `[NCSI] ... - not connected` → `[NET] https fail-closed: no cabundle` → `[E2E] FAIL`。需要真实更新服务器时用 `tools/updatesrv/e2e-run.sh`。

**I. NKM 模块被拒**

`[NKM] staged module refused: {nkm_err_text}` → 用 `nkm_strerror()` 对照错误串；`nkm_load()` 本身静默，运行 `nkm_selftest()` 看每步 PASS/FAIL；检查 SHA-256 摘要字段是否在计算时清零。

**J. 日志"缺失"**

先查 12.2 的限流表：未知 syscall/ABI 各 8 条、WM 事件 12 条、display list 满 8 条、字体/图标/壁纸一次性提示。再确认对应路径是否**本来就静默**（WAL commit、Merkle verify、块分配、NKM 校验细节、VFS 路径解析均无日志）。

### 13.13 打印策略（按实现修正）

- 模块初始化成功：直接 `kputs("[TAG] ... up/ready\n")`，不要找不存在的 `log_print(INFO, ...)`。
- 关键错误：`kputs("[TAG] ERROR ...")` + 错误码（`kputs_hex` 十六进制 / 本地 `kputs_dec` 十进制）。
- 高频路径（VSync、每包网络、每次上下文切换）**不要**长期打日志；用一次性标记（如 `g_irq_logged`）或周期心跳（如 `g_ticks % 40`）。
- 需要动态值：`kputs_hex(v)` 输出 `0x`+16 位大写 hex；十进制自己拼；字符串直接 `kputs`。
- 不要在 `cli` 关中断区间依赖 `kputs`——它会 `irq_save/irq_restore`，但串口队列满时会退化为轮询写，可能拖慢路径。

### 13.14 常见 Bug 清单（扩展版）

原 V1.0 八条保留并补充实现层面的高频坑：

1. **忘记持锁** → 多进程竞态；共享内存锁超时会降级（不 panic），数据可能不完整。
2. **锁顺序错误** → 死锁；必须按 超级块→目录→inode→块分配（最内层）。
3. **双重释放** → 内存 corruption；`[PMM] WARN double free` 只打一条后抑制，后续同类静默。
4. **偏移未对齐** → x86_64 严格对齐异常；共享内存字段有 `_Static_assert` 冻结，新增字段只能 carve-out 尾部。
5. **CAP 忘记检查** → 越权；WM `owned_by()` 拒绝是**静默** `ack(NWM_EDENIED)`，日志里看不到。
6. **WAL 写了 PREP 没写 COMMIT** → 崩溃后数据丢失；`wal_commit()` 的 flush 失败是静默 `-1`。
7. **Merkle 哈希包含自身 hash 字段** → 校验永远失败；`novusfs_merkle_verify()` 本身无日志，要靠 selftest 的 `merkle detects tampering` 项。
8. **NKM 签名公钥不匹配** → 模块无法加载；`nkm_load()` 静默返回 `NOVUS_EMODULE`，只有 selftest 暴露。
9. **日志"没打出来"** → 先查限流（12.2）和静默路径（12.3.7/12.3.8 的"静默路径"注记）。
10. **串口队列溢出** → `SERIAL_QUEUE=1024`；高频日志会丢行，`g_rx_dropped` 只统计接收侧。
11. **PID 与任务对不上** → `[SCHED] task ... pid=... state=...` 与 `[NWM] owner gone, closing win=` 联查。
12. **退出码 3 / 127** → 分别是 Rust panic 与命令未找到；不要误判为内核崩溃。
13. **高位显存 >4GiB** → splash 被跳过、framebuffer uncached；性能问题不等于功能失败。
14. **`kputs_hex` 读数误判** → 固定 16 位大写 hex（如 `0x00000000000003E8`），不是十进制。

### 13.15 相关调试文档索引

本手册不替代以下已有文档，遇到专门流程时按表跳转（路径前缀 `ROOT = ...`）：

| 文档 | 可引用的调试流程 |
|---|---|
| `docs/终端与命令解释器设计.md` | nsh 命令清单（`:85–119`）、交互快捷键（`:80`）、构建接线（`:135–142`） |
| `docs/中断体系.md` | 宿主定向测试 + QMP 验收（`:121–141`）、`irq-qemu-test.ps1` 用法 |
| `docs/ui-design.md` | "验证：截图 + 放大"（`:168–185`）、QEMU monitor `screendump` + PIL 放大检查 |
| `docs/核查-2026-11-13-BUG与UI.md` | 真编译 + QEMU 真启动 + 截图核查（`:3–6,18–22,129–135`）、headless 注入限制（`:112–113`） |
| `docs/M9 Win32 兼容层原型.md` | 输入注入从 monitor 改走 VNC 的经验（`:640–644`）、键盘到窗口过程根因分析 |
| `docs/存储驱动与HDD休眠-2026-11-18.md` | 关机/重启、USB power_state 验证缺口（`:63–65`） |
| `docs/图形安装向导设计与实现.md` | 安装调试产物：`/tmp/novus-install-serial.log`、`/tmp/novus-install*.ppm`（`:213–214`） |
| `docs/Novus OS 自研分级增量更新体系完整规范.md` | E2E 验证：串口 `[NET]/[E2E]` 标记（`:534–536`） |
| `README.md` | 冒烟/回归流程（`:497–524`）、错误容错与日志子系统（`:434–439`）、共享内存规范（`:109–111`）、开发者选项（`:354–360`） |
| `CONTRIBUTING.md` | 按改动类型的测试矩阵（`:185–197`）、标准回归命令（`:199–219`）、QEMU 冒烟约定（`:235`） |
| `SECURITY.md` | 安全相关回归命令（`:188–191`） |

---

## 文档状态

本文档为 Debug 级接口手册，开发时对照此文档逐行检查代码。

- V1.0：第 1–11 章接口定义（函数签名、参数、错误码、前置/后置条件、调试检查点）**冻结**，可作为 M0–M10 开发调试基准。
- V1.1：新增第 12 章「日志系统：实现现状与全量日志解释」与第 13 章「手工 Debug 方法全解」。新增内容为对当前源码与脚本的实测归纳，不改变冻结的接口基线；当规格与实现不一致时，以 12.0「规格与实现差异声明」为准。

文档状态：✅ **接口基线冻结；日志与调试章节为 V1.1 增补，可直接使用。**
