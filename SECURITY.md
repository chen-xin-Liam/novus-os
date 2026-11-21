# Novus OS 安全政策

本文档说明 Novus OS 的安全边界、已知限制、漏洞报告方式和贡献者必须遵守的安全规则。它与 [CONTRIBUTING.md](CONTRIBUTING.md) 配套使用；涉及实现细节时，以当前源码和冻结格式规范为准。

## 1. 支持范围

| 对象 | 状态 |
|---|---|
| 当前主分支 / V1.6-FinalSpec | 当前开发基线，安全问题按优先级修复 |
| M0～M11 历史里程碑镜像 | 仅作历史参考，不保证向后移植安全修复 |
| 自定义分支、外部镜像、私有签名密钥 | 不在支持范围内 |
| 物理硬件部署 | 项目目前以 QEMU/UEFI 验证为主，不承诺生产环境安全 |

本项目是实验性操作系统，不提供商业支持、漏洞赏金或固定 SLA。

---

## 2. 如何报告漏洞

**请不要在公开 Issue、PR、`changes.md` 或讨论区中直接贴出漏洞细节、可利用 PoC、私钥或种子。**

如果仓库托管平台提供了私有漏洞报告 / Security Advisory 功能，请使用它提交报告。若当前没有私有渠道，请先提交一个不含技术细节的 Issue，标题类似“Request for private security contact”，等维护者给出私下联系方式后再提供完整信息。

报告中请尽量包含：

- 受影响的提交、镜像版本和构建命令
- 涉及子系统：`kernel/`、`security/nkm`、`security/novu`、`security/tls`、`fs/`、IPC/WM、更新服务等
- 攻击前提：是否需要 Ring 0、本地用户、特定镜像、QEMU 配置或网络位置
- 实际影响：提权、越界读写、签名绕过、信息泄露、拒绝服务等
- 可复现步骤、串口日志或截图；如涉及 PoC，请通过私有渠道发送
- 建议修复方案或相关代码位置

维护者会先确认问题、评估影响并协调修复；在修复或缓解措施发布前，请避免公开细节。修复发布后，可在 PR 或 `changes.md` 中致谢，但不要泄露私下提供的攻击材料。

---

## 3. 安全模型与信任边界

### 3.1 启动与完整性

信任链起点是当前 UEFI 固件与 `out/NOVUS.EFI`：

1. UEFI 入口初始化串口/控制台和块设备，并检查最低 2 GiB 内存。
2. A 核在早期自检阶段校验系统文件完整性；篡改会阻止启动。
3. NovusFS 使用双超级块、CRC-32 和 Merkle 树检测数据/元数据篡改。
4. WAL 记录带 CRC，恢复时只重放带 commit 标记的事务，然后重算 Merkle root 并与超级块比较。

当前实现**不是 Secure Boot**：没有签名固件链、没有 UEFI 安全启动集成，也无法抵御固件层或已经取得 Ring 0 的攻击者。

Merkle 的覆盖范围不包括 WAL 区和 Merkle 区本身；超级块依靠双副本和 CRC，而不是 Merkle 叶。WAL 重放结果目前也不是超级块刷新的严格门控，贡献者不要把这描述成完整的崩溃一致性或防回滚机制。

### 3.2 Ring 0 / Ring 3 隔离

- 内置应用是 Rust `no_std` 扁平 NUC 镜像，在 ring 3、独立 CR3 和独立页表中运行。
- NUC 不要求签名；隔离依赖页表和系统调用边界，而不是镜像签名。
- 系统调用只有一张编号表 `kernel/trap.c:g_syscalls[]`，编号稀疏且冻结。
- 用户指针不能直接解引用：`user_read_byte()` / `user_copy()` 会检查地址范围、按任务 CR3 翻译并校验用户访问权限。
- 路径最长 256 字节，单次 I/O 最长 4096 字节；未知系统调用返回错误并记录，不会静默成功。
- NUC 入口、segment、重定位和权限位都有边界检查；仅 `TEXT` 可执行，其余 segment 为 NX。
- 用户栈被限制在 64 KiB～4 MiB；栈越界必须失败，不能回退到内核地址。

### 3.3 IPC、窗口与 GUI

IPC/WM 是功能协议，**目前不应视为强安全边界**：

- IPC 消息头有 magic、长度、校验和，端口表、队列和载荷都有固定上限。
- 端口 `"nwm"` 使用版本化协议，显示列表每条消息都有长度/数量限制。
- 当前没有基于 CAP 的端口授权；普通用户态可以调用 IPC 系统调用。
- 客户端不应向内核传递内核地址、未初始化长度或超出协议的显示项。
- 修改 IPC/WM 时必须保持消息布局、magic、版本号和长度检查一致；详见 [Novus-WM 与 IPC 协议规范](<docs/Novus-WM 与 IPC 协议规范 V1.0.md>)。

### 3.4 文件系统与挂载

- VFS 只有一个 syscall 可见根；挂载表在路径前缀切换后端。
- `fs/novusfs/rootfs_layout.h` 是根目录树的唯一真源。
- 挂载路径必须命中白名单，挂载表固定 8 项；`/` 不允许卸载，跨挂载 `rename` 被拒绝。
- FAT 和 ISO9660 后端只读；写、删除、重命名必须失败。
- `/.novusfs/mounts.json` 运行时不回写，否则会破坏启动时的 Merkle 校验。
- VFS 不做 `..` 穿越消除；目录实现不应通过特殊条目扩大可访问范围。

### 3.5 Win32 兼容层

Win32 兼容层是原型，不是完整沙箱：

- PE32+ 固定映射到 `0x50000000`，按 DIR64 重定位，按名称填充 IAT。
- `TASK_ABI_WIN32` 任务只允许 `SYS_WIN32(40)` 和 `SYS_EXIT(60)`；其它系统调用直接拒绝。
- `win32_syscall` 会二次校验任务 ABI 类型。
- 注册表只是持久化到普通文件的模拟实现。
- 尚未实现完整进程隔离、DLL 安全加载、异常处理、TLS、线程和文件权限模型。

---

## 4. 密码学、密钥与更新

### 4.1 NKM 内核模块

NKM 是 Ring 0 模块，必须签名：

- SHA-256 覆盖整文件，但计算前先把 `[0x50,0xB0)` 的摘要/签名区置零。
- Ed25519 签的是 32 字节摘要，不是整文件。
- 签名使用编译期固化在内核中的可信公钥。
- 校验顺序为：边界检查 → digest → signature → dependencies → map → relocate → init。
- 偏移、长度和 ABI major 由 `_Static_assert` 与头部检查固定。
- NUC 用户程序不签名；NKM 签名不能用于替代 Ring 3 隔离。

禁止行为：

- 默认启用 `NKM_FLAG_ALLOW_UNSIGNED`
- 在发布镜像中编译测试公钥或测试模块
- 只改签名工具或只改内核加载器而不更新规范和静态断言
- 用固定开发种子生成“生产”签名密钥
- 通过降低校验顺序、跳过依赖检查或放宽段权限来“修复”加载失败

> 当前实现仅有一把开发公钥，且没有运行时导入/轮换公钥的接口。`NKM_FLAG_ALLOW_UNSIGNED` 的“仅管理员本次会话有效”仍是规范目标；当前代码没有完整的管理员鉴权，不能把它当作已落实的安全控制。

### 4.2 更新包与 TLS

- 更新包在用户态经过魔数、版本、UUID 双向绑定、区间、SHA-256 和 Ed25519 校验。
- 开发签名根公钥固化在 `security/novu/novu_verify.c`；打包器的 `--key` 是开发/测试种子，不是生产密钥管理方案。
- TLS 链验证支持有效期、`is_ca`、签名算法子集、根有效期和主机名检查；X.509 只解析必要字段。
- 生产根集合来自 `tools/cabundle/roots.pem`，生成 `security/novu/cabundle.c`；根集合为空时 fail-closed。
- E2E 测试锚只生成 `security/novu/cabundle_e2e.c`，并且仅在 `E2E=1` 时编入内核。生产构建绝不能包含测试锚。
- 当前 TLS 实现不是完整 WebPKI：没有 CRL/OCSP 吊销、名称约束、路径长度、keyUsage 全面校验，也不支持客户端证书、HTTP/2、QUIC 或 0-RTT。
- 随机数由调用方注入种子的 hash-DRBG 提供，未播种时有确定性回退；内核种子来自调度 tick 等材料。不要宣称硬件熵或高熵 TRNG。

### 4.3 测试密钥与私钥规则

- `tools/updatesrv/` 下已有的 `*.key` 资产只用于 QEMU E2E 测试，**不是生产密钥**，绝不能用于生产签名或编入 OS 镜像。
- `.gitignore` 禁止新增 `private/*.pem|key|crt`、`tools/updatesrv/*.key`、`security/tls/*.key`、`user/ssl/*.key` 等私钥文件；历史遗留的测试私钥不应被复制到新位置或扩大用途。
- `NKM_SEED`、`novupkg --key` 和 `nkmgen keygen` 使用的固定种子只适用于自测和开发。
- 不要在文档、日志、串口输出、截图或 PR 描述中打印私钥、种子或完整证书私钥内容。
- 生产 CA 集合变更必须用 `tools/cabundle/import_roots.py --dry-run` 审查，再由离线生成脚本产出；测试 CA 不得写入生产 `cabundle.c`。
- 如果疑似生产私钥泄露，应立即停止使用该密钥、轮换签名根和已签发模块/更新包，并重新生成受影响的镜像。

---

## 5. 当前能力与规范目标的区分

| 项目 | 当前状态 |
|---|---|
| NUC Ring 3 页表隔离、用户指针校验 | 已实现 |
| NKM SHA-256 + Ed25519 签名校验 | 已实现；仅开发公钥，无运行时轮换 |
| 更新包 SHA-256 + Ed25519 校验 | 已实现 |
| TLS 链校验与主机名检查 | 部分实现，非完整 WebPKI |
| NovusFS Merkle + WAL CRC | 已实现；非完整防回滚/崩溃一致性方案 |
| 挂载路径白名单、只读卷保护 | 已实现 |
| Win32 ABI 白名单 | 已实现，但兼容层仍是原型 |
| CAP 最小权限模型 | **规范目标，当前未落地**；不要依赖它做授权 |
| IPC 端口鉴权 | 未实现强鉴权 |
| Secure Boot / 签名固件链 | 未实现 |
| 运行时公钥导入/轮换、证书吊销 | 未实现 |
| 安全随机数 / 硬件 TRNG | 未实现 |
| 灰度推送、定时调度、失败自愈 | 规范中部分定义，不应视为已实现能力 |

README 和部分规范中保留了早期目标描述；若与本节或当前源码冲突，应提交文档勘误，而不是据此放宽实现检查。

---

## 6. 贡献者安全要求

修改涉及安全的代码时，必须遵守以下硬规则：

1. 不绕过、不降级、不条件化 NKM 或更新包签名校验；调试开关不能进入默认构建。
2. 系统调用必须显式检查长度、边界和用户指针；新增调用要同步 `g_syscalls[]`、用户头文件和规范。
3. 不在中断上下文睡眠、分配内存、打印日志或调用协议栈。
4. 不扩大挂载白名单，不回写 `mounts.json`，不把只读后端改成可写。
5. 不把 Win32 兼容层的 API 白名单当作通用进程隔离机制。
6. 不提交私钥、种子、测试 root 到生产 CA 集合，或机器特定的绝对路径。
7. 冻结格式（NUC/NKM/NUP/INS/IPC）发生变化时必须升版本、同步工具链、静态断言、测试夹具和文档。
8. 修复安全问题时优先添加回归测试；测试应验证“错误输入被拒绝”，而不是只验证正常路径。
9. 不在公开日志中输出内核地址、栈回溯、密钥材料或可复现的攻击 payload。
10. 新依赖必须先评估许可证和供应链风险；项目当前坚持自研、不引入外部开源项目。

---

## 7. 安全相关验证

与安全改动至少执行：

```sh
make
make check-rootfs
make nkm-test-module
build/tools/novupkg selftest
(cd tools/nkmgen && cargo run --quiet -- selftest)
python3 tools/updatesrv/selftest.py

bash scripts/display-selftest.sh
python3 -m unittest discover -s scripts -p qemu_smoke_test.py
make disk-img
bash scripts/qemu-smoke.sh
bash scripts/qemu-smoke.sh --no-root --output out/qemu-smoke-no-root
```

更新/TLS 改动还需运行：

```sh
bash tools/updatesrv/e2e-run.sh
make
```

最后一条 `make` 用于确认生产构建（`E2E=0`）没有混入 E2E 测试锚。所有相关 `make` 调用必须一致带 `E2E=1` 或都不带，避免对象文件混用。

详细测试矩阵见 [CONTRIBUTING.md](CONTRIBUTING.md)；NKM 与更新格式细节见 [NKM 格式规范](<docs/NKM 内核模块二进制格式规范 V1.0.md>) 和 [分级增量更新体系规范](<docs/Novus OS 自研分级增量更新体系完整规范.md>)。

## 8. 开源许可

Novus OS 使用 GPLv2。安全修复同样必须遵循 GPLv2 分发；提交者保留版权，所有修改源码按项目条款开源。详见 [LICENSE](LICENSE) 与 [COPYING](COPYING)。
