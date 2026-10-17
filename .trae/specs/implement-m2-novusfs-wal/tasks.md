# Tasks

- [ ] Task 1: 块设备抽象与 ramdisk
  - [ ] SubTask 1.1: 编写 `kernel/blkdev.h/.c`：blkdev 结构（512B 扇区 read/write、扇区数查询、上下文指针）、注册/获取接口
  - [ ] SubTask 1.2: ramdisk 后端：PMM 分配 16MiB 物理内存（经 physmap 可写），实现扇区读写；fs_selftest 扇区模式读回校验
- [ ] Task 2: Rust CRC32 FFI
  - [ ] SubTask 2.1: `lib/novuslib-rs/src/lib.rs` 新增 `novus_rs_crc32(u32 seed, *const u8, usize) -> u32`（IEEE 多项式 0xEDB88320 表驱动，零依赖，无 core::fmt）；`lib/novuslib_rs.h` 同步声明
- [ ] Task 3: NovusFS 磁盘结构与 mkfs
  - [ ] SubTask 3.1: 编写 `fs/novusfs/novusfs.h`：冻结磁盘结构（超级块字段含双副本/版本/TXID/wal_head/wal_tail/区域几何/CRC32；WAL 条目头含魔数/TXID/类型/块号/长度/CRC32；inode 结构；目录项记录）与常量（块 4096、WAL 4MiB、ramdisk 16MiB 容量自洽）
  - [ ] SubTask 3.2: 编写 `fs/novusfs/novusfs.c`：mkfs（超块双副本、根目录 inode 1、空位图、空 WAL）与 mount（超块 CRC 校验、备份恢复优先级1、区域一致性校验）、几何信息串口输出
- [ ] Task 4: inode/目录项/块分配器
  - [ ] SubTask 4.1: 编写 `fs/novusfs/inode.h/.c`：inode 位图与数据块位图（复用 Rust 位图 FFI）分配/释放、inode 读写、12 直接块+单间接块地址翻译、文件块读写（经 blkdev）
  - [ ] SubTask 4.2: 目录项：定格式变长记录的查找/追加/删除（删除用记录 inode=0 或合并空闲）、根目录初始化、多级路径遍历（`/a/b`，以 / 分隔）
- [ ] Task 5: WAL 事务与 fsck 恢复
  - [ ] SubTask 5.1: 编写 `fs/novusfs/wal.h/.c`：循环字节缓冲追加（head/tail 偏移落超块）、满判定 head==tail、满时 tail 推进至下一个完整事务 BEGIN 边界（不截断事务）、条目 CRC32 校验
  - [ ] SubTask 5.2: 事务 API：tx_begin（TXID+1 写 BEGIN）/ tx_write_block（DATA 含目标块镜像）/ tx_commit（COMMIT → checkpoint 按序写目标块）；mount 恢复扫描：有 COMMIT 重做、无 COMMIT 丢弃、CRC 损坏截断并置 wal_corruption 告警（README 4.2.3 优先级2）
- [ ] Task 6: VFS 抽象层与文件操作
  - [ ] SubTask 6.1: 编写 `kernel/vfs.h/.c`：挂载态、fd 表（16）、vfs_mkdir/create/open/read/write/close/stat/unlink/listdir 统一 API（内部转 novusfs 后端），stat 结构（类型/大小/inode 号）
  - [ ] SubTask 6.2: unlink 回收链路：目录项移除 → inode 释放 → 数据块经事务回收，位图计数回归
- [ ] Task 7: 自检与接线
  - [ ] SubTask 7.1: 编写 `fs/fs_selftest.h/.c`：①ramdisk 扇区校验 ②mkfs/mount 几何+CRC ③mkdir/create/write 多块文件(>48KiB)/read 比对/stat/listdir/unlink 回收 ④WAL 原子性：提交事务 + 注入无 COMMIT 事务 → 重新 mount → 已提交保留/未提交丢弃 ⑤回绕：超容量事务流断言 tail 落点为 BEGIN 边界 ⑥WAL 字节损坏 → corruption 标志与告警
  - [ ] SubTask 7.2: `kernel/kmain.c` 在 heap_selftest 后、lapic_init 前插入 fs_selftest()（失败 kputs [FAIL] 但不 panic，符合容错基调）；Makefile SRC 增加 fs 文件与 -Ifs -Ifs/novusfs
- [ ] Task 8: 端到端验证
  - [ ] SubTask 8.1: make clean && make 全量 0 error 0 warning（含 Rust CRC32）
  - [ ] SubTask 8.2: `make run MEM=4G`：串口断言 [FS] 各阶段 PASS（扇区/mkfs/文件生命周期/原子性/回绕/损坏检测）且 M1 双核序列完整（AP online、A核 sleep、[HALT]）
  - [ ] SubTask 8.3: `make run MEM=1G` 回归拒绝启动路径；确认 smp.c 用户调优未被改动（git diff 仅含本次授权文件）

# Task Dependencies
- Task 1、Task 2 无依赖可并行
- Task 3 依赖 Task 1（块设备）；Task 4 依赖 Task 2、3；Task 5 依赖 Task 3、4（checkpoint 写块）；Task 6 依赖 Task 4、5
- Task 7 依赖 Task 6；Task 8 依赖全部
- Makefile 修改（Task 7.2）可由主代理在集成时统一处理，避免与并行代理冲突
