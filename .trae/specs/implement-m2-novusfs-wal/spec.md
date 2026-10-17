# M2 NovusFS 最小版本与 WAL 基础实现 Spec

> 对应 README 第19部分里程碑 **M2：NovusFS最小版本，WAL基础实现**。
> 基于 M1（implement-m1-memory-paging-smp，已完成）成果：PMM/页表/内核堆/Rust 库可用。
> 冻结依据：README 第4部分（4.1 核心能力、4.2 磁盘分区布局、4.2.1 WAL 日志区、4.2.3 fsck 修复优先级、4.3 VFS API）。Merkle 树属 M3，本 spec 仅预留区域不实现。

## Why
M1 后系统有内存管理与双核运行环境但无持久存储抽象。M2 交付最小可用的自研 NovusFS：超级块 + 事务 WAL + inode/目录项/块分配器 + VFS 统一接口，具备崩溃后已提交事务不丢、未提交事务回滚的原子性保证，为 M3 Merkle 校验与后续真实存储驱动接入打基础。

## What Changes
- **块设备抽象层**：新增 `kernel/blkdev.h/.c`——统一 `read/write (扇区 512B)` 接口；M2 提供 **ramdisk 后端**（PMM 分配 16MiB 物理内存模拟磁盘）。真实 AHCI/NVMe 驱动属后续驱动里程碑，经同一抽象接入，M2 不实现
- **NovusFS 磁盘布局**（新增 `fs/novusfs/`，逻辑块 4096B，README 4.2）：
  - 块0/块1：**超级块双副本 + 版本计数器**（README 4.2.2 双副本框架 M2 先行：魔数 "NOVUFS1"、版本、块大小、总块数、各区域起始块/块数[WAL 区/inode 位图/inode 表/Merkle 预留区/数据块区]、根 inode 号=1、当前 TXID、wal_head/wal_tail 字节偏移、结构 CRC32）
  - WAL 日志区（4MiB，**严格按 README 4.2.1 冻结**）：循环字节缓冲；条目 = 头部（魔数、TXID、事务类型 BEGIN/DATA/COMMIT、目标块号、数据长度、头部+载荷 CRC32）+ 4KiB 载荷；满判定 `wal_write_ptr == wal_tail_ptr`；满时推进 `wal_tail_ptr` 至**完整事务边界**，不截断事务；校验失败判定为日志损坏
  - inode 位图 + inode 表（inode 结构：类型[文件/目录]、大小、块计数、12 个直接块指针 + 1 个单间接块、创建/修改时间戳序号）；数据块位图分配器（**复用 M1 Rust 位图 FFI**）
  - 目录项：目录 inode 数据块内变长记录（inode 号、记录长度、名称长度、名称，最长 255），根目录 inode 1
  - Merkle 树存储区：仅在超级块登记起始/大小并预留，**不写数据结构**（M3）
- **WAL 事务与恢复**（README 4.1/4.2.3）：`tx_begin/tx_write_block/tx_commit`——写数据块前先把块镜像写入 WAL（redo 物理日志），COMMIT 落盘后 checkpoint 写入目标块；挂载时执行**最小 fsck 恢复**：①超级块损坏加载备份副本（4.2.3 优先级1）；②扫描 WAL，含 COMMIT 的事务重做（redo），无 COMMIT 的事务直接丢弃（"放弃未提交事务，回滚到上一个稳定状态"，优先级2）；③WAL 条目 CRC 损坏：截断至损坏点、放弃未提交事务并置 `wal_corruption` 标志，挂载继续但 kputs 告警（README 4.2.1"严重损坏拒绝 Full 模式"的策略消费方属后续内存模式模块，M2 仅产出标志与告警，不实现模式切换）
- **VFS 抽象层**：新增 `kernel/vfs.h/.c`——挂载单一 NovusFS 实例，统一 API：`vfs_mkdir / vfs_create / vfs_open / vfs_read / vfs_write / vfs_close / vfs_stat / vfs_unlink / vfs_listdir`（README 4.3 集合的 M2 最小子集）；路径遍历支持一级/多级 `/a/b`；fd 表（16 项）；无用户态/系统调用（属 M4），内核内直接调用
- **Rust 扩展**：`lib/novuslib-rs` 新增零依赖 CRC32（表驱动，标准 IEEE 多项式）FFI：`novus_rs_crc32(seed, ptr, len)`，供超级块与 WAL 条目校验
- **自检接线**：`kmain` 在堆初始化后、SMP 唤醒前执行 `fs_selftest()`；Makefile SRC 扩展 fs/novusfs/*.c；不修改 smp.c（用户调优等待常量）

## Impact
- Affected specs: implement-m1-memory-paging-smp（消费 PMM/heap/Rust 库；初始化时序插入 fs_selftest，不改 M1 任何已验证行为）
- Affected code:
  - 新增 `kernel/blkdev.h/.c`、`kernel/vfs.h/.c`、`fs/novusfs/novusfs.h`（磁盘结构冻结头）、`fs/novusfs/wal.h/.c`、`fs/novusfs/inode.h/.c`、`fs/novusfs/novusfs.c`（mkfs/mount/fsck 恢复/VFS 后端）、`fs/fs_selftest.h/.c`
  - 修改 `kernel/kmain.c`（插入 fs_selftest 与串口阶段行）、`lib/novuslib-rs/src/lib.rs`（CRC32 FFI）、`Makefile`（SRC/头文件搜索路径 -Ifs/novusfs -Ifs）
  - 不修改 `README.md`、`LICENSE`、`kernel/smp.c`、`scripts/run.sh`（-smp 2 不变）

## ADDED Requirements

### Requirement: 块设备抽象与 ramdisk
系统 SHALL 提供统一 512B 扇区块设备接口（read/write/容量查询），M2 以 PMM 背书的 16MiB ramdisk 为唯一后端；NovusFS 仅依赖该抽象，不假设后端介质。

#### Scenario: ramdisk 读写一致
- **WHEN** fs 自检对 ramdisk 写入扇区模式并读回
- **THEN** 全部字节一致，串口输出块设备容量行

### Requirement: NovusFS 磁盘布局与 mkfs
系统 SHALL 按冻结布局格式化：超级块双副本（版本计数器）、4MiB WAL 区、inode 位图/表、Merkle 预留区、数据块区，根目录为 inode 1；超级块含 CRC32 校验。

#### Scenario: 格式化与挂载
- **WHEN** kmain 对 ramdisk 执行 mkfs 后 mount
- **THEN** 串口输出各区域几何（起始块/块数）、超级块魔数与 CRC 校验通过、根目录存在且为空

### Requirement: WAL 事务原子性
系统 SHALL 通过 WAL 提供事务：所有数据块修改先写日志（BEGIN→DATA×N→COMMIT）再 checkpoint；循环缓冲满时 tail 仅推进到完整事务边界（README 4.2.1 冻结）；每个条目含 CRC32。

#### Scenario: 已提交数据崩溃后保留
- **WHEN** 自检写入文件 A（含 COMMIT）后，人为向 WAL 追加一条无 COMMIT 的事务（模拟断电崩溃）并重新挂载
- **THEN** fsck 重做已提交事务、丢弃未提交事务；文件 A 内容完整可读，文件系统元数据一致，串口输出恢复摘要（redo/丢弃事务数）

#### Scenario: 循环回绕不截断事务
- **WHEN** 自检制造超过 WAL 区容量的事务流
- **THEN** tail 推进落点均为事务边界（断言落点条目为 BEGIN），所有已提交数据读回正确，无越界写入

#### Scenario: 损坏检测
- **WHEN** 自检破坏 WAL 区中一个条目的字节
- **THEN** 恢复在损坏点截断、wal_corruption 置位、串口出现 [FS] WAL corruption 告警且挂载成功

### Requirement: VFS 文件与目录操作
系统 SHALL 经 VFS 抽象提供 mkdir/create/open/read/write/close/stat/unlink/listdir，支持多级路径；数据块经位图分配器分配/释放（Rust 位图）。

#### Scenario: 文件生命周期自检
- **WHEN** fs_selftest 执行 mkdir("/system") → create+write 多块文件（>48KiB，覆盖单间接块）→ close → open 读回比对 → stat → listdir("/system")
- **THEN** 内容逐字节一致、stat 大小正确、目录列表含该文件；unlink 后 stat 返回不存在、数据块与 inode 已回收（位图 free_count 恢复）

### Requirement: Rust CRC32 FFI
系统 SHALL 在 novuslib-rs 以零依赖实现 CRC32 并经 FFI 供超级块/WAL 校验复用，串口自检打印校验值证明 FFI 链路。

## 边界说明（冻结决策）
1. Merkle 树（README 4.2.2 节点布局/树高公式）完全属于 M3；M2 仅在超级块登记预留区
2. 真实存储驱动（AHCI/NVMe/PCI）属驱动里程碑；M2 仅 ramdisk，镜像不落宿主文件、无跨启动持久化
3. 用户态系统调用（README 第8部分）属 M4；M2 全部验证在内核态直接调用 VFS
4. 不做：多挂载点、路径缓存、硬链接/符号链接、权限位（CAP 在安全里程碑）、独立用户态 fsck 工具、稀疏文件、并发文件锁；时间戳为自增序号非实时时钟
5. 文件最大尺寸 = (12 + 1024) × 4KiB ≈ 4MiB（12 直接块 + 单间接），M2 自检覆盖至单间接路径即可
6. fsck 仅实现 README 4.2.3 优先级 1（超级块备份）与 2（WAL 恢复）；inode 隔离（优先级3）属 M3
