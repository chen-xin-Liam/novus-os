# NKM 内核模块二进制格式规范
> 文档版本：V1.0（对应 Novus OS V1.6-FinalSpec）
> 适用对象：NKM 加载器开发者、驱动开发者、模块签名工具链开发者
> 平台：x86_64 UEFI
> 状态：**格式冻结**；哈希/签名规则已修复并固化，不得变更

---

## 1. 概述

NKM（Novus Kernel Module）是 Novus OS 的内核动态模块格式，扩展名 `.nkm`。
用途：驱动、文件系统扩展、Win32 兼容层、GPU DRM 驱动等可加载组件。

与 NUC（用户态程序）的本质区别：

| 维度 | NUC（用户态） | NKM（内核模块） |
|---|---|---|
| 加载位置 | 用户虚拟地址空间 | 内核地址空间 |
| 权限 | 用户态页表，Ring3 | 内核页表，Ring0 |
| 签名 | 无要求 | **必须 Ed25519 签名** |
| 入口 | `entry_point` 用户函数 | `nkm_init()` + `nkm_exit()` 双入口 |
| 卸载 | 进程退出自动回收 | 需 `nkm_unload` 显式调用 |
| 访问范围 | 仅自己地址空间 | 可访问全部内核符号 |

---

## 2. 文件整体布局

```
偏移 0x0000 ┌──────────────────────────────┐
            │  NKM Header (nkm_hdr_t)      │   固定 128 字节
偏移 0x0080 ├──────────────────────────────┤
            │  Module Dependencies         │   dep_count × nkm_dep_t (16B/条)
偏移 ...    ├──────────────────────────────┤
            │  Segment Table               │   seg_count × nkm_segment_t (32B/条)
偏移 ...    ├──────────────────────────────┤
            │  Symbol Table                │   sym_count × nkm_sym_t (24B/条)
偏移 ...    ├──────────────────────────────┤
            │  Relocation Table           │   rel_count × nkm_rel_t (16B/条)
偏移 ...    ├──────────────────────────────┤
            │  String Table               │
偏移 ...    ├──────────────────────────────┤
            │  段数据（.text/.data/.rodata）│   4KiB 对齐
            └──────────────────────────────┘
```

---

## 3. NKM Header（文件头）

固定长度 **128 字节**，位于文件偏移 `0x0000`，packed。

### 3.1 结构体定义

```c
#define NKM_MAGIC 0x4E4B4D21   // "NKM!"

typedef struct __attribute__((packed)) {
    uint8_t  magic[4];           // 0x00  "NKM!" = 4E 4B 4D 21
    uint16_t abi_version;        // 0x04  bit[0..7]=minor, bit[8..15]=major
    uint16_t header_size;        // 0x06  = 128 (0x80)，便于扩展
    uint64_t min_kernel_ver;     // 0x08  最低兼容内核版本
    uint32_t module_type;        // 0x10  模块类型（驱动/FS/兼容层...）
    uint16_t seg_count;           // 0x14  段数量
    uint16_t dep_count;           // 0x16  依赖模块数量
    uint32_t symtab_count;        // 0x18  符号数量
    uint32_t rel_count;           // 0x1C  重定位数量
    uint32_t strtab_offset;       // 0x20  字符串表偏移
    uint32_t rel_offset;          // 0x24  重定位表偏移
    uint32_t init_offset;         // 0x28  nkm_init() 在 .text 内偏移
    uint32_t exit_offset;         // 0x2C  nkm_exit() 在 .text 内偏移
    uint8_t  module_name[32];     // 0x30  模块名（'\0' 结尾）
    uint8_t  module_hash[32];     // 0x50  模块本体 SHA256（签名校验字段）
    uint8_t  module_sig[64];     // 0x70  Ed25519 签名
    uint8_t  reserved[0x20];      // 0xB0  预留 32 字节
} nkm_hdr_t;                        // 总大小 128 字节 (0x80)
```

### 3.2 字段逐字节定义

| 偏移 | 大小 | 字段 | 取值规则 |
|---|---|---|---|
| 0x00 | 4 | magic | 固定 `NKM!`，不匹配拒绝加载 |
| 0x04 | 2 | abi_version | 高字节=major，低字节=minor。当前 `0x0100` |
| 0x06 | 2 | header_size | 固定 128（0x80），未来扩展用 |
| 0x08 | 8 | min_kernel_ver | 加载时校验 `内核版本 >= min_kernel_ver` |
| 0x10 | 4 | module_type | 见 §3.3 |
| 0x14 | 2 | seg_count | 段数，1~16 |
| 0x16 | 2 | dep_count | 依赖模块数，0=无依赖 |
| 0x18 | 4 | symtab_count | 符号数量 |
| 0x1C | 4 | rel_count | 重定位数量 |
| 0x20 | 4 | strtab_offset | 字符串表文件偏移 |
| 0x24 | 4 | rel_offset | 重定位表文件偏移 |
| 0x28 | 4 | init_offset | `nkm_init()` 在 .text 段内字节偏移 |
| 0x2C | 4 | exit_offset | `nkm_exit()` 在 .text 段内字节偏移 |
| 0x30 | 32 | module_name | 模块名，`\0` 结尾，最长 31 字符 |
| 0x50 | 32 | module_hash | 模块本体 SHA256（见 §6） |
| 0x70 | 64 | module_sig | Ed25519 签名（见 §6） |
| 0xB0 | 32 | reserved | 必须全 0 |

### 3.3 module_type 取值

| 值 | 名称 | 说明 |
|---|---|---|
| 0 | `NKM_TYPE_DRIVER` | 硬件驱动（VGA/DP/存储/PCI） |
| 1 | `NKM_TYPE_FS` | 文件系统驱动 |
| 2 | `NKM_TYPE_COMPAT` | 兼容层（Win32） |
| 3 | `NKM_TYPE_CRYPTO` | 加密模块 |
| 4 | `NKM_TYPE_OTHER` | 其他扩展 |

---

## 4. 模块依赖表

紧接 Header 之后，共 `dep_count` 条，每条 **16 字节**。

```c
typedef struct __attribute__((packed)) {
    uint32_t dep_name_off;      // 0x00  依赖模块名在 String Table 的偏移
    uint64_t min_dep_ver;       // 0x04  依赖模块最低版本
    uint32_t reserved;           // 0x0C  必须为 0
} nkm_dep_t;                     // 16 字节
```

加载规则：
1. 加载器按依赖表顺序，**先加载被依赖的模块**；
2. 依赖模块未加载 → 自动递归加载；
3. 依赖模块版本不满足 `min_dep_ver` → 拒绝加载本模块；
4. 依赖表出现循环依赖 → 拒绝加载，返回 `NOVUS_EDEP`。

---

## 5. 段表 / 符号表 / 重定位表

格式与 NUC 完全一致（见《NUC 二进制格式规范》第 4/5/7 节），差异点：

| 项 | NKM 差异 |
|---|---|
| 段 vaddr | 加载到**内核地址空间**（高地址区，非用户区） |
| 段权限 | 全部 Ring0，不要求 `VM_USER`；TEXT 可执行，DATA 可写 |
| 外部符号 | 解析到**内核导出符号表**（`nksym`），而非系统 SDK |
| 重定位类型 | 仅 `R_X86_64_64`（64 位绝对地址），内核模块不使用 PC32 |

内核导出符号示例（`nksym` 表，由内核 vmlinux 提供）：
```
mm_alloc, mm_free, vm_map, vm_unmap
sched_spawn, sched_exit
vfs_register_fs, vfs_read, vfs_write
hal_display_probe, hal_display_flush
cap_check, log_print, err_report
pci_enumerate, blk_read, blk_write
```

---

## 6. Ed25519 签名与哈希（核心，已冻结）

### 6.1 密码套件

- 哈希：SHA256
- 签名：Ed25519（RFC 8032）
- 公钥：32 字节
- 签名：64 字节

### 6.2 内核公钥池

```c
// 编译时固化在内核镜像中，运行时不可写、不可改
static const ed25519_pubkey_t kernel_trusted_keys[4] = {
    /* 公钥1 */, /* 公钥2 */, /* 公钥3 */, /* 公钥4 */
};
static const uint8_t trusted_key_count = 1; // 初始仅1把，后续版本追加
```

- 最多支持 **4 把**公钥；
- 仅编译时可更新（密钥轮换 = 发新版内核）；
- **运行时不接受用户导入公钥**；
- 验证时轮询所有公钥，任一通过即合法。

### 6.3 哈希计算范围（已修复冲突，冻结）

> **关键规则**：SHA256 计算范围 = 整个模块文件，**但跳过 `module_hash[32]` 和 `module_sig[64]` 两个字段**。

具体操作（签名工具与加载器必须一致）：

```
1. 拷贝整个模块文件到内存 temp_buf
2. 将 temp_buf 中偏移 0x50 起共 96 字节（32+64）置 0
   —— 即把 module_hash 和 module_sig 两个字段清零
3. computed_hash = SHA256(temp_buf, 文件总长度)
4. 比对 computed_hash 与头部原始 module_hash：
     不一致 → 哈希校验失败，拒绝加载
```

### 6.4 签名验证流程

```
1. 计算出 computed_hash（同 §6.3）
2. 比对 computed_hash == nkm_hdr.module_hash
3. 取头部 module_sig（64 字节）作为 Ed25519 签名
4. 取 nkm_hdr.module_hash（32 字节）作为签名原文
5. 遍历 kernel_trusted_keys[]：
     if ed25519_verify(module_hash, module_sig, pubkey[i]):
         → 校验通过，跳出
6. 全部公钥校验失败 → 拒绝加载
```

### 6.5 为什么跳过 hash+sig 字段

如果哈希包含自身签名，会造成"鸡生蛋"问题：签名依赖哈希，哈希依赖签名字节。
置零后计算哈希，使哈希与签名解耦：哈希覆盖"除自描述字段外的全部内容"，签名再对这个哈希做签名。

---

## 7. 模块入口函数约定

每个 NKM 模块必须导出两个函数，通过 `init_offset` / `exit_offset` 指向：

```c
// 模块初始化（加载时调用一次）
// 返回 0 成功，负数错误码
int nkm_init(void);

// 模块卸载（卸载时调用一次）
// 返回 0 成功
int nkm_exit(void);
```

调用约定：
- 与系统调用 ABI 一致（x86_64 SysV）；
- `nkm_init` 返回非 0 → 加载失败，已分配资源自动回滚；
- `nkm_exit` 必须释放所有申请的内核资源（内存、IRQ、设备注册），否则卸载后系统状态不一致。

---

## 8. 加载完整流程（冻结）

```
1. nkm_load(path) 被调用
2. 校验调用者 cap_check(mod.load)，无权限 → NOVUS_EPERM
3. VFS 打开文件，读取前 128 字节（Header）
4. 校验 magic == "NKM!"
5. 校验 abi_version 主版本号
6. 校验 min_kernel_ver <= 当前内核版本
7. 拷贝整个文件到内核内存
8. 执行哈希校验：
   a. 临时副本中 0x50..0xB0 置 0
   b. 计算 SHA256
   c. 与 module_hash 比对
   失败 → NOVUS_ECORRUPT
9. 执行 Ed25519 签名校验（轮询公钥池）
   失败 → NOVUS_EMODULE
   （管理员单次临时放行：跳过第 9 步，打警告日志）
10. 按依赖表递归加载依赖模块
11. 映射段到内核地址空间
12. 解析重定位（外部符号查内核 nksym 表）
13. 调用 nkm_init()
14. init 返回 0 → 注册到模块列表，返回成功
15. init 失败 → 回滚段映射，返回错误
```

---

## 9. 卸载流程

```
1. nkm_unload(name) 被调用
2. 校验 cap_check(mod.unload)
3. 检查模块是否被其他模块依赖（dep_count 反向引用）
   被依赖 → 拒绝卸载，NOVUS_EBUSY
4. 调用 nkm_exit()
5. 解除段映射
6. 从模块列表移除
7. 释放模块内存
```

---

## 10. 管理员临时放行策略

- 普通用户：未签名模块**一律拒绝加载**；
- 管理员（拥有 `mod.load` CAP）：可加载未签名模块，但：
  - 必须在加载时显式传入 `NKM_FLAG_ALLOW_UNSIGNED`；
  - 内核打印 WARNING 日志，记录模块名、PID、时间；
  - **仅本次会话有效**，不持久保存信任；
  - 重启后该模块不会自动加载。

---

## 11. 错误码

| 错误码 | 含义 |
|---|---|
| `NOVUS_OK (0)` | 加载成功 |
| `NOVUS_EPERM (-4)` | 无 `mod.load` / `mod.unload` CAP |
| `NOVUS_EINVAL (-1)` | 头部字段非法 |
| `NOVUS_ECORRUPT (-7)` | SHA256 哈希不匹配 |
| `NOVUS_EMODULE (-8)` | Ed25519 签名验证失败 |
| `NOVUS_EBUSY (-5)` | 模块被依赖，无法卸载 |
| `NOVUS_ENOTFOUND (-3)` | 依赖模块缺失 |
| `NOVUS_EDEP (-12)` | 循环依赖 |
| `NOVUS_EIO (-6)` | 文件读取失败 |
| `NOVUS_ENOMEM (-2)` | 内核内存不足 |

---

## 12. 完整示例：一个最小驱动模块

### 12.1 Header 关键字段

```
magic:          4E 4B 4D 21  ("NKM!")
abi_version:    00 01        (major=1, minor=0)
header_size:    80 00        (128)
min_kernel_ver: 00 00 00 00 00 00 00 01   (要求内核 >= 1.0)
module_type:    00 00 00 00  (DRIVER)
seg_count:      02 00         (2段)
dep_count:      00 00         (无依赖)
symtab_count:   01 00 00 00
rel_count:      02 00 00 00
strtab_offset:  00 02 00 00   (0x200)
rel_offset:     C0 01 00 00   (0x1C0)
init_offset:    10 00 00 00   (.text+0x10)
exit_offset:    30 00 00 00   (.text+0x30)
module_name:    "nvme.ko" 00 00...
module_hash:    <32字节SHA256>
module_sig:     <64字节Ed25519签名>
```

### 12.2 模块源码骨架（C）

```c
#include <novus/nkm.h>
#include <novus/mm.h>
#include <novus/log.h>

static void* buf = NULL;

int nkm_init(void)
{
    log_print(LOG_INFO, "nvme driver init\n");
    buf = mm_alloc(1);
    if (!buf) return NOVUS_ENOMEM;
    // 注册 NVMe 控制器
    return NOVUS_OK;
}

int nkm_exit(void)
{
    log_print(LOG_INFO, "nvme driver exit\n");
    if (buf) mm_free(buf, 1);
    return NOVUS_OK;
}
```

编译后由 `cppcc` / `nkm-sign` 工具：
1. 编译目标文件；
2. 按本规范拼出 NKM 文件；
3. 将 hash+sig 字段清零；
4. 计算 SHA256 填入 `module_hash`；
5. 用内核私钥对 `module_hash` 做 Ed25519 签名，填入 `module_sig`。

---

## 13. 文档冻结声明

本规范为 NKM 内核模块格式唯一基准文档。约束：

- Header 128 字节布局；
- 哈希跳过 0x50..0xB0（hash+sig 字段）后计算 SHA256；
- Ed25519 签名原文 = `module_hash`（32 字节）；
- 内核公钥池最多 4 把，编译时固化；
- 入口固定 `nkm_init` / `nkm_exit`；
- 依赖表递归加载，禁止循环依赖。

任何变更必须走 ABI 变更 RFC，升版本号。

文档状态： **可作为 M3（NKM 加载器）与 M10（驱动生态）的实现基准。**
