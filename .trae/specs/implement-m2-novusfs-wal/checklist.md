# Checklist

- [ ] blkdev 抽象接口存在，ramdisk 16MiB 后端扇区读写模式校验通过（串口 [FS] 行）
- [ ] NovusFS 冻结磁盘结构头：超级块（魔数 NOVUFS1/双副本/版本/TXID/wal_head/tail/区域几何/CRC32）、WAL 条目头（魔数/TXID/类型/块号/长度/CRC32）、inode、目录项
- [ ] mkfs 产出双副本超级块、根目录 inode 1、空 WAL；mount CRC 校验通过并打印区域几何
- [ ] 超级块主副本损坏时从备份副本恢复（README 4.2.3 优先级1）
- [ ] inode 位图与数据块位图经 Rust 位图 FFI 分配/释放；12 直接块 + 单间接块路径被自检覆盖（>48KiB 文件）
- [ ] VFS 全链路：mkdir → create → write → read 逐字节一致 → stat 大小正确 → listdir 可见 → unlink 后 stat 不存在
- [ ] unlink 后 inode 与数据块回收，位图 free_count 回归分配前值
- [ ] WAL 事务顺序 BEGIN→DATA→COMMIT→checkpoint；条目 CRC32 实际调用 Rust FFI
- [ ] 崩溃恢复：含 COMMIT 事务重做保留、无 COMMIT 事务丢弃，串口打印 redo/丢弃事务数
- [ ] WAL 回绕：tail 推进落点全部为完整事务 BEGIN 边界，已提交数据读回正确
- [ ] WAL 条目字节损坏被检测：截断、wal_corruption 置位、[FS] 告警出现且挂载成功
- [ ] 多级路径（/system/…）遍历正确
- [ ] make clean 全量构建 0 error 0 warning（C + Rust）
- [ ] `make run MEM=4G` fs 自检全 PASS 且 M1 双核序列（AP online/cpu_count=2/A核 sleep/[HALT]）完整无回归
- [ ] `make run MEM=1G` 低于 2GiB 拒绝启动回归通过
- [ ] kernel/smp.c 用户调优常量未被改动；README.md/LICENSE 未修改
- [ ] Merkle 区域仅预留未实现；构建产物不入库（.gitignore 已覆盖 build/）
