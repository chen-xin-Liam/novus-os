# Novus OS Debug 级接口手册
> 文档版本：V1.0（对应 Novus OS V1.6-FinalSpec）
> 用途：开发调试手册，每个接口展开到参数级、错误码级、前置/后置条件、调试检查点
> 配套：《全部接口总表》《NUC 格式规范》《NKM 格式规范》
> 状态：冻结

---

## 0. 阅读指南

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
| path | .nuc 文件路径，如 "/system/bin/novus-settings" |
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
  - `/system` 下写操作需要 `fs.full` CAP
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

## 12. 日志接口

### 12.1 `log_print`

```c
void log_print(uint32_t level, const char* fmt, ...);
```

- **level**：
  - 0 = DEBUG（开发调试）
  - 1 = INFO（正常信息）
  - 2 = WARN（警告）
  - 3 = ERROR（错误）
- **行为**：写入环形缓冲区 + 串口输出
- **LowMem**：DEBUG 级别不输出（节省串口带宽）
- **调试检查点**：模块初始化函数开头打 INFO

### 12.2 `panic_handler`

```c
void panic_handler(cpu_state_t* state);
```

- **触发时机**：内核致命错误（双重缺页、致命锁死）
- **行为**：
  1. 关中断
  2. VGA 文本模式打印寄存器转储
  3. HLT 死循环
- **绝不**返回。用户态进程崩溃走 `sched_exception_report`，不走 panic。

---

## 13. 调试通用建议

### 13.1 打印策略
- 模块 init：打 INFO
- 关键错误：打 ERROR + 错误码
- 调试细节：打 DEBUG（LowMem 关闭）
- 不要在中断高频路径打日志（VSync 每帧）

### 13.2 断言
- 指针非 NULL：`assert(ptr != NULL);`
- 对齐：`assert((uint64_t)addr % 16 == 0);`
- 锁持有：调试时可记录当前锁持有者
- 返回值：`int ret = foo(); assert(ret == NOVUS_OK);`

### 13.3 常见 Bug 清单
1. **忘记持锁** → 多进程竞态
2. **锁顺序错误** → 死锁（必须按 超级块→目录→inode→块分配）
3. **双重释放** → 内存 corruption
4. **偏移未对齐** → x86_64 严格对齐异常
5. **CAP 忘记检查** → 越权访问
6. **WAL 写了 PREP 没写 COMMIT** → 崩溃后数据丢失
7. **Merkle 哈希包含了自身 hash 字段** → 校验永远失败
8. **NKM 签名时公钥不匹配** → 模块无法加载

---

## 文档状态

本文档为 Debug 级接口手册，开发时对照此文档逐行检查代码。
所有函数签名、参数、错误码、前置条件已冻结。

文档状态：✅ **冻结，可作为 M0–M10 开发调试基准。**
