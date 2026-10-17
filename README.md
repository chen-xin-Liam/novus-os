# novus-os
Innovative new operating system
Novus OS V1.6-FinalSpec（修订版，可进入M0开发）
修订说明：

1. 修复NKM哈希规则前后矛盾，统一采用「跳过hash+sig字段」方案；

2. 补充Merkle树tree_height公式边界兜底；

3. 补充NUC abi_version与compile_flag兼容性判定规则；

4. 其余所有规范保持不变；

5. 修订完成，文档满足M0开发准入条件，可启动M0：UEFI入口、长模式切换、临时页表、BSP/AP握手原型开发
协议：GPLv2 | 平台：x86_64 UEFI
变更点只修改对应章节，其余章节原文保留，下面展示全文，仅修改三处冲突/缺失定义
总目录

第一部分 系统顶层架构与设计铁律（冻结）

第二部分 四层标准化系统层级

第三部分 AB双核SMP两阶段启动架构（最终精准定义）

第四部分 智能内存自适应双模式体系（阈值+策略+回收+OOM完全锁定）

第五部分 NovusFS 自研文件系统（布局+锁+WAL+fsck+容错完整冻结）

第六部分 Merkle树源码防篡改校验体系（磁盘映射+节点格式+原子更新终极锁定）

第七部分 NovusDisplay GOP图形栈（双缓冲+脏矩形+权限边界冻结）

第八部分 NovusWM & IPC 通信协议

第九部分 128位CAP能力权限系统（位定义+校验点+权限边界完全锁定）

第十部分 NUC 原生可执行生态（完整头部+段表+符号+重定位+ABI冻结）

第十一部分 NKM 内核动态模块系统（签名+密钥+验证流程冻结）【本次修订】

第十二部分 Win32有限兼容层（API白名单+实现边界冻结）

第十三部分 SMP多核启动 & A/B核握手协议（内存地址+栈+IPI时序终极锁定）

第十四部分 系统调用ABI完整规范（寄存器+错误码+栈对齐冻结）

第十五部分 桌面系统套件功能终稿

第十六部分 构建系统 Kconfig/Makefile

第十七部分 精细化开发里程碑 M0–M10

第十八部分 工程可行性分级与风险闭环

第十九部分 最终定稿声明
第一部分 系统顶层架构 & V1.6 终极设计铁律

1.1 核心不可变更原则

1. 全程从零自研，不借用 Linux/Windows/BSD 任何内核源码与核心逻辑。

2. 内核底层、驱动层使用 C；图形合成、UI、EDID 适配使用 Rust(no_std)。

3. AB核为 SMP两阶段启动模型，非并行双核、非异构核。

4. 双内存模式严格分层：低配可用、高配满血，代码路径隔离。

5. 用户态进程彻底隔离，进程崩溃永不宕机内核。

6. 所有高难度组件（DRM驱动、完整兼容层、多屏）全部可选.nkm动态模块，不阻塞基础系统启动。

7. 所有底层二进制格式、协议、时序、哈希、锁策略 永久冻结。

8. 安全机制不牺牲性能：Merkle增量校验替代全局哈希。

1.2 四层系统层级（标准化冻结）

1. 硬件层：UEFI、GOP、PCIe、NVMe/SATA、PS2

2. 内核Ring0层：内存管理、调度、中断、IDT、文件系统、IPC、权限、模块加载

3. 系统运行层：窗口管理器、NUC虚拟机、SDK、HAL、可选兼容层

4. 应用层：.nuc桌面应用、.npy脚本、有限Win32程序
第二部分 AB双核 SMP 两阶段启动架构【终极冻结】

2.1 精准定义（彻底消除歧义）

• A核 = BSP 引导主核心：上电唯一初始运行核心
职责：硬件扫描、内存探测、GOP初始化、NovusFS超级块读取、Merkle树校验、开机日志输出
自检完成后永久休眠，不参与调度、不运行业务逻辑。

• B核 = AP 应用核心池：A核唤醒后接管全部系统
职责：初始化完整内核、页表、GDT/IDT、调度器、文件系统、图形栈、用户进程

2.2 SMP 握手完整协议

共享内存结构（64B对齐，固定4KiB预留物理页）
typedef struct {
    uint8_t  bsp_state;    // 0=init 1=verify_ok 2=sleeping
    uint8_t  ap_ready;
    uint16_t handshake_flag;
    uint64_t mem_barrier;
    uint64_t reserved[6];
} smp_shmem_t;
AP启动栈规范

• 每颗AP独立16KiB私有栈，物理内存动态分配

• 栈顶地址由BSP写入共享内存

• AP唤醒后直接使用私有栈，绝不复用BSP栈

IPI唤醒时序（绝对固定）

1. BSP 关闭本地中断

2. 写入 bsp_state=2

3. sfence 内存屏障

4. 发送 IPI 向量 0x80 唤醒所有AP

5. AP初始化GDT/IDT/页表

6. AP写入 ap_ready=1

7. BSP永久休眠

启动容错

AP唤醒超时200ms → 内核软重启
第三部分 智能内存自适应双模式体系【完整锁定】

3.1 硬件阈值（永久固定）

• <2GiB：拒绝启动（最低硬件门槛）

• 2GiB ~ 6GiB：LowMem 精简模式

• ≥6GiB：Full 满血模式

3.2 LowMem 强制禁用项

窗口阴影、透明、动画、抗锯齿、VSync、多屏、WAL日志、文件快照、Python脚本、图形高级特效

3.3 Full 满血开启项

4K/120Hz、双缓冲VSync、窗口特效、WAL事务日志、fsck、碎片整理、多屏扩展、完整API

3.4 内存水位线 & 动态回收

三级水位线

1. 空闲>30%：正常运行，不回收

2. 10%~30%：温和回收（页缓存、目录缓存）

3. <10%：紧急回收（离屏FB、所有可释放缓存）

固定回收顺序

文件页缓存 → 目录项缓存 → 图形离屏缓冲 → NovusFS冗余缓存

防抖动策略

1秒内最多触发一次全局回收

3.5 OOM 杀手终极规范

归一化评分公式（永久固定）
score = rss_bytes * front_weight * age_weight * type_weight
• front_weight：前台1.8 / 后台1.0

• age_weight：<30s=1.0 / 30s~5min=0.7 / >5min=0.4

• type_weight：普通程序1.0，系统辅助进程0.2

系统保护列表（永不被杀）

init、novus-wm、novus-settings、novus-taskmgr

OOM兜底行为

无可用可杀进程 → 冻结新进程创建、打印告警、不panic、不宕机
第四部分 NovusFS 自研文件系统【终极工程冻结】

4.1 基础参数

• 固定块大小：4096Byte

• 磁盘布局：GPT/MBR → 超级块 → 块位图 → Inode表 → 数据块 → WAL日志区

4.2 核心结构体
#define NOVUSFS_MAGIC 0x4E6F767573465300ULL
#define NOVUSFS_BLOCK_SIZE 4096
#define NOVUSFS_MAX_FILENAME_LEN 255

typedef struct __attribute__((packed)) {
    uint64_t magic;
    uint64_t total_blocks;
    uint64_t free_blocks;
    uint64_t block_bitmap_start;
    uint64_t inode_table_start;
    uint64_t data_start;
    uint64_t root_inode_id;
    uint8_t  volume_name[64];
    uint8_t  sha256_root_hash[32];
    uint8_t  journal_enable;
    uint8_t  reserved[479];
} novusfs_superblock_t;
4.3 多级锁防死锁体系（永久固定顺序）

超级块锁 → 目录锁 → inode锁 → 块分配器锁

• 单锁自旋上限：1000000

• 超时不panic，降级为「只读查询、阻塞写入」单写保护模式

4.4 并发读写规则

• 多进程并行读完全允许

• 任意写入独占事务，阻塞其他写、不阻塞读

4.5 WAL 写前日志完整规范

日志区参数

• 大小：分区5%，下限16MiB，上限2GiB

• LowMem禁用，仅Full模式启用

日志头结构
typedef struct {
    uint32_t magic;      // 0x4E565357 NOVSWAL
    uint32_t txid;
    uint8_t  tx_type;    // 0块写 1inode 2目录
    uint8_t  state;      // 0PREP 1COMMIT
    uint16_t reserved;
    uint64_t block_no;
    uint32_t data_len;
    uint32_t crc32;
} wal_hdr_t;
循环覆盖满条件

wal_write_ptr == wal_tail_ptr 即为日志已满

• 满时自动将wal_tail_ptr推进至下一完整事务边界

• 禁止截断半事务

崩溃恢复规则

• PREP状态事务：直接回滚丢弃

• COMMIT状态事务：补全落地

• 日志区整体损坏：强制降级LowMem只读模式

4.6 fsck修复优先级

1. 超级块损坏 → 双副本恢复

2. 未完成WAL事务回滚/补全

3. Inode位图修复

4. 孤儿inode移入 /lost+found

5. 坏块隔离冻结

4.7 扩展能力

• xattr扩展属性存储权限标签、系统只读标记

• 离线碎片整理

• 智能坏块隔离
第五部分 Merkle树增量防篡改校验体系【终极冻结】

5.1 基础规格

• 哈希算法：SHA256

• 节点大小：64Byte

• 每4KiB块存放64个节点

• 完全二叉树结构

5.2 节点结构
// 叶子节点
typedef struct {
    uint8_t  hash[32];
    uint64_t block_idx;
    uint64_t reserved[4];
} merkle_leaf_t;

// 中间节点
typedef struct {
    uint8_t  hash[32];
    uint64_t left_idx;
    uint64_t right_idx;
    uint64_t reserved[2];
} merkle_node_t;
5.3 磁盘映射算法（绝对固定）
uint64_t node_to_block(uint64_t idx) {
    return merkle_start_block + (idx / 64);
}
uint64_t node_offset_in_block(uint64_t idx) {
    return (idx % 64) * 64;
}
5.4 树计算规则【本次补充边界兜底】

• 树高度公式：tree_height = ceil(log2(leaf_count)) + 1

◦ leaf_count == 0：非法树，拒绝挂载分区

◦ leaf_count == 1：树高强制为2；根节点索引=1；叶子层起始索引=2

• 叶子层起始索引：leaf_start_idx = 1 << (tree_height - 1)

• 奇数叶子：复制最后有效叶子补齐完美二叉树

5.5 原子更新机制

超级块双副本根哈希：primary / backup
更新顺序：

1. 先写backup根哈希+版本

2. sfence屏障

3. 写primary根哈希+版本

4. 版本号自增

5.6 故障回退

primary损坏自动回退backup；双损坏进入不信任模式、关闭完整校验
第六部分 NovusDisplay 图形栈【落地版永久冻结】

6.1 阶段1基础能力（默认GOP兜底）

✅ 单/双缓冲自动适配
✅ 脏矩形局部更新
✅ 窗口合成、圆角、透明、动画（满血）
✅ 完整2D UI渲染
✅ EDID由UEFI读取，不裸驱硬件

6.2 阶段1不支持（不虚假承诺）

❌ 硬件VSync、硬件光标
❌ 原生高刷
❌ 多屏扩展
❌ GPU硬件加速

6.3 帧缓冲权限规范

• front/back buffer 内核物理分配、内核管控

• WM只读映射front

• back buffer可写，但必须通过内核syscall提交脏矩形

• LowMem强制单缓冲

6.4 渲染屏障

所有显存写入强制sfence，杜绝乱序撕裂
第七部分 128位CAP权限系统【完全落地锁定】

7.1 结构
typedef struct {
    uint64_t cap_mask_low;  // 基础32位有效权限
    uint64_t cap_mask_high; // 96位预留扩展
} proc_cap_t;
7.2 前32位标准权限定义（唯一标准）
bit0:  sys.core        // 仅内核只读调试（日志/符号/内存布局，无写权限）
bit1:  sys.config      // 修改系统全局配置
bit2:  sys.proc.read   // 读取全部进程信息
bit3:  sys.proc.ctrl   // 杀死/调试进程
bit4:  sys.disk.read   // 原始磁盘读
bit5:  sys.disk.ctrl   // 磁盘写/分区/格式化
bit6:  sys.log.read    // 读取内核日志
bit7:  hw.display.ctrl // 全局图形控制
bit8:  fs.full         // 绕过/system只读锁
bit9:  gui.override    // 窗口劫持、截屏覆盖
bit10: runtime.python  // 启用脚本运行时
bit11: acore.ctrl      // 修改A核校验策略
bit12: mod.load        // 加载内核模块
bit13: mod.unload      // 卸载内核模块
bit14: net.raw         // 原始网络（预留）
bit15: time.set        // 修改系统时间
bit16~31: 预留扩展
7.3 强制校验点

所有磁盘写、进程控制、模块操作、图形覆盖、系统配置修改 必须前置cap_check()

7.4 权限继承规则

• 子进程默认继承权限

• 进程可主动drop权限

• 禁止主动提权
第八部分 NUC 可执行格式 & ABI【终极冻结，新增兼容性判定】

8.1 头部完整结构
typedef struct {
    uint8_t  magic[4];        // NUC!
    uint16_t abi_version;     // bit[0~7] = minor次版本; bit[8~15]=major主版本
    uint16_t compile_flag;
    uint32_t entry_point;     // .text段内字节偏移
    uint16_t seg_count;
    uint16_t symtab_offset;
    uint32_t strtab_offset;
    uint32_t rel_offset;
    uint32_t stack_size;
    uint64_t reserved[4];
} nuc_hdr_t;
compile_flag 位定义
bit0: 0=32bit 1=64bit
bit1: PIC位置无关
bit2: 0=动态 1=静态
bit3: 栈保护开启
bit4: 调试符号开启
bit5~15: 保留(0)
ABI版本 & compile_flag 兼容性判定【本次新增】

1. abi_version 主版本号不同 → 直接拒绝加载

2. abi_version 次版本号不同 → 向前兼容，允许加载

3. compile_flag bit0 与当前内核位宽不一致 → 拒绝加载

8.2 段表、符号表、重定位表
typedef struct {
    uint32_t type;
    uint32_t offset;
    uint64_t vaddr;
    uint64_t size;
    uint32_t flags;
    uint32_t reserved;
} nuc_segment_t;

typedef struct {
    uint32_t name_off;
    uint8_t  type;
    uint8_t  bind;
    uint16_t reserved;
    uint64_t value;
    uint64_t size;
} nuc_sym_t;

typedef struct {
    uint64_t r_offset;
    uint32_t r_type;
    uint32_t sym_idx;
} nuc_rel_t;
8.3 入口地址计算公式
entry_rip = text_segment.vaddr + nuc_hdr.entry_point
8.4 系统调用ABI

• 调用号：rax

• 参数：rdi rsi rdx rcx r8 r9

• 返回：rax（0成功、负数错误码）

• 保存寄存器：rbp rbx r12 r13 r14 r15

• 用户态入口强制16字节栈对齐
第九部分 NKM内核动态模块系统（签名+密钥+验证流程冻结）【修订NKM哈希规则】

9.1 加密套件

• 哈希：SHA256

• 签名：Ed25519 RFC8032（32B公钥、64B签名）

9.2 内核密钥池

• 最多内置4把可信公钥

• 仅编译阶段可更新，运行时不可篡改

9.3 哈希覆盖规则【统一修订，消除冲突】

NKM模块验证时，SHA256覆盖除头部module_hash[32]与module_sig[64]之外的全部模块字节。
验证逻辑：将模块头部module_hash、module_sig两个字段临时按0填充，计算SHA256；将计算结果与头部module_hash字段比对。
// NKM头部布局
typedef struct {
    uint8_t module_hash[32];   // 模块本体SHA256
    uint8_t module_sig[64];    // Ed25519签名
    uint64_t min_kernel_ver;
    // ...其余模块头部字段
} nkm_hdr_t;
验证伪代码：
// 临时拷贝一份模块数据，将hash、sig区域置0
temp_buf = copy(nkm_file_data)
memset(temp_buf + offsetof(nkm_hdr_t, module_hash), 0, 32 + 64);
computed_hash = sha256(temp_buf, module_file_length);
if(computed_hash != nkm_hdr.module_hash) fail_hash_check;
// 校验Ed25519签名：签名原文为 module_hash
if(!ed25519_verify(nkm_hdr.module_hash, nkm_hdr.module_sig, pubkey)) fail_sig_check;
9.4 验证流程

1. 拷贝模块数据，将头部hash、sig区域清零，计算本体SHA256

2. 比对计算哈希与头部module_hash字段，不匹配拒绝加载

3. 轮询内核内置公钥池，使用module_hash原文校验Ed25519签名

4. 内核版本不匹配/哈希错误/签名错误直接拒绝加载

9.5 权限策略

普通用户禁止未签名模块；管理员可单次临时放行
删除旧冲突描述：删除原文第八部分“模块全部字节参与SHA256，无例外”的矛盾文字。
第十部分 Win32 有限兼容层【边界冻结】

10.1 支持范围（最小可行集）

✅ PE加载器、虚拟注册表
✅ KERNEL32：文件、进程、内存、时间、线程基础API
✅ GDI2D：基础绘图、矩形、线条、文本、BitBlt

10.3 模块属性

可选.nkm动态模块，可一键关闭节省内存
第十一部分 桌面套件终稿
