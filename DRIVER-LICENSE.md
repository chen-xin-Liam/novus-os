# Novus OS 闭源驱动许可例外（Driver License Exception）

> 版本：v1.0（2026-11-27）
> 适用对象：希望为 Novus OS 编写**闭源 / 专有设备驱动**的第三方开发者与厂商
> 配套：[LICENSE](LICENSE)、[COPYING](COPYING)、[SECURITY.md](SECURITY.md)、[COMPATIBILITY.md](COMPATIBILITY.md)

---

## 1. 为什么需要这份文件

Novus OS 主体采用 **GPLv2**（见 [LICENSE](LICENSE) 与 [COPYING](COPYING)）。GPLv2 第 10 节允许版权持有人授予**额外权限**（additional permissions），在不修改 GPLv2 正文的前提下，为特定使用场景放宽条款。

本文件就是这样的**额外权限声明**：它允许第三方在 Novus OS 上分发**闭源设备驱动**，只要驱动遵守本文件规定的边界。它**不修改、不替代、不削弱** GPLv2 本身；本文件与 GPLv2 冲突时，以对本文件授权范围更有利的方式解读，但不得扩大到本文件未授权的场景。

> [!NOTE]
> 本文件是项目 license 的一部分，不是法律意见。商业分发前请自行评估目标市场的合规要求，必要时咨询法务。

---

## 2. 授权范围（允许什么）

在下列条件下，你可以**以专有许可证（包括闭源）编写、分发和销售设备驱动**，而无需公开该驱动的源代码：

1. **驱动是独立创作的第三方作品**，不包含、不复制、不派生自 Novus OS 的 GPLv2 代码（`kernel/`、`fs/`、`security/`、`graphics/`、`wm/`、`apps/`、`tools/`、`lib/` 等）。
2. **驱动只通过公开的、冻结的 ABI 与 Novus OS 交互**，包括：
   - 系统调用 ABI（[ABI.md](ABI.md) §2）
   - NKM 内核模块格式与 `nksym_table` 中声明的内核符号（[ABI.md](ABI.md) §4）
   - Novus-WM / IPC 协议（[ABI.md](ABI.md) §5）
   - VFS / 块设备公开接口（[ABI.md](ABI.md) §6）
3. **驱动以 NKM 模块、NUC 应用或两者组合的形式分发**，不修改内核源码，不静态链接内核代码到驱动二进制中。
4. **驱动使用项目认可或厂商自行管理的签名密钥完成签名**，按 [SECURITY.md §4.1](SECURITY.md) 走校验流程；闭源不等于免签名。
5. **驱动明确声明**：名称、版本、支持的 ABI 版本与内核版本、厂商/作者、专有许可证名称，以及"本驱动不是 Novus OS 项目官方组件"。
6. **驱动不得把 GPLv2 代码重新许可为闭源**，也不得把项目商标、Logo、文档内容一并闭源化。

满足以上条件时：

- 驱动二进制可以只包含机器码与数据，**无需附带源代码**。
- 可以对驱动收费、捆绑销售或作为产品固件的一部分分发。
- 项目的 GPLv2 不延伸到驱动本身；驱动适用你选择的许可证。

---

## 3. 明确不覆盖的范围（不允许什么）

以下行为**不**在本例外授权范围内，仍须遵守 GPLv2：

| 行为 | 说明 |
|---|---|
| 修改、 fork、重新编译 Novus OS 内核或任何 GPLv2 组件 | 派生作品必须按 GPLv2 分发 |
| 把 GPLv2 源文件复制进闭源驱动仓库 | 即使是"参考实现"也属于复制 |
| 绕过 NKM 签名校验或 `NKM_FLAG_ALLOW_UNSIGNED` 进入生产镜像 | 属安全违规，见 [SECURITY.md](SECURITY.md) |
| 使用未公开的内核内部符号、结构布局或私有协议 | 只能使用 `nksym_table` 与冻结 ABI |
| 将闭源驱动冒充为官方内置组件或项目商标 | 商标与署名不受本例外授权 |
| 把本例外用于非驱动的用户态应用 | 普通应用可使用 NUC 格式，但不因此获得闭源例外（NUC 本身不签名、不链接 GPL 代码，通常不构成派生作品；如有疑问按普通 GPL 合规评估） |

> 如果驱动需要新增内核符号或新的 ABI，**先开 Issue / RFC 讨论并把接口公开冻结**，再按 [CHANGELOG.md](CHANGELOG.md) 的 ABI 变更流程升版本。不要通过私有补丁偷偷扩展内核。

---

## 4. 签名与信任模型

闭源驱动与开源驱动在安全上**一视同仁**：

1. **NKM 驱动必须签名**：SHA-256（清零 `[0x50,0xB0)`）+ Ed25519，见 [SECURITY.md §4.1](SECURITY.md)。
2. **签名密钥由谁管理**：
   - 开发/测试：使用项目提供的开发种子（仅 QEMU/自测，不进生产镜像）。
   - 生产：厂商自行生成密钥，并通过项目规定的渠道提交根公钥纳管（当前实现仅有开发公钥，运行时轮换尚未落地，见 [SECURITY.md §5](SECURITY.md)；此项属**已知限制**，不是本例外提供的能力）。
3. **ABI 版本必须匹配**：驱动头部声明的 ABI major 必须与内核一致（[ABI.md](ABI.md) §3/§4）。
4. **故障隔离**：驱动崩溃不得拖垮内核；用户态驱动按进程隔离，Ring0 驱动按 NKM 生命周期约束编写。

> 闭源驱动因为无法审计源码，**更应**使用最小权限、固定边界和完整日志；项目保留在驱动破坏系统稳定性或安全时拒绝其加载的权利。

---

## 5. 分发与标注要求

每个闭源驱动分发包必须包含一份 `DRIVER_NOTICE`（纯文本或 Markdown），至少写明：

```text
Driver:       <名称>
Version:      <版本>
Vendor:       <厂商/作者>
License:      <专有许可证名称>
ABI Version:  <例如 NUC 0x0100 / NKM major 1>
Kernel:       <支持的 V1.6-FinalSpec 或具体版本>
Signature:    <签名公钥指纹或厂商密钥标识>
Notice:       This is a third-party closed-source driver.
              It is NOT an official Novus OS component.
              Licensed under <license>, not GPLv2.
              See DRIVER-LICENSE.md for the granted exception.
```

分发包还须：

- 保留本文件（[DRIVER-LICENSE.md](DRIVER-LICENSE.md)）与 [LICENSE](LICENSE) 的副本。
- 不修改或删除项目其他组件的版权与许可声明。
- 在驱动加载失败时给出明确错误码，不静默回退到不安全状态。

---

## 6. 兼容性责任

- 闭源驱动使用冻结 ABI，因此**同主版本内应可跨内核小版本运行**；跨主版本必须重新验证（见 [COMPATIBILITY.md](COMPATIBILITY.md)）。
- 如果出现 **[ABI BREAK]**（见 [CHANGELOG.md](CHANGELOG.md)），项目会升主版本并公告；驱动作者需在此后发布兼容版本。
- 驱动不得依赖未冻结的调试接口、sysinfo 私有请求号或 QEMU 特定行为。
- 驱动应声明最低内存模式（LowMem/Full）与所需外设能力。

---

## 7. 例外与 GPLv2 的关系

| 文件 | 许可 | 说明 |
|---|---|---|
| `LICENSE` / `COPYING` | GPLv2 原文 | **不修改**；项目主体代码适用 |
| 项目源码（`kernel/`、`fs/`、`graphics/` 等） | GPLv2 | 派生作品必须继续 GPLv2 |
| 符合本文件条件的第三方驱动 | 专有许可证 | 本文件授予的额外权限 |
| 不符合条件的驱动 | 无例外 | 按 GPLv2 评估 |

本例外是**不可撤销的**：对在本文件生效期间发布、且符合本文件条件的驱动版本，项目不会事后收回授权。但项目保留对**新版本**调整例外条款的权利。

---

## 8. 如何申请把驱动列入官方兼容清单

1. 在 Issue 中提供 `DRIVER_NOTICE`、签名公钥指纹、ABI 版本与测试报告。
2. 提供可复现的加载与功能测试（QEMU 与真机各一份日志）。
3. 通过安全审查：无未公开内核符号、无签名绕过、无越界内存访问。
4. 审核通过后，在 [COMPATIBILITY.md](COMPATIBILITY.md) 增加一行，并链接到厂商文档。

列入清单**不代表**项目背书其安全性或质量；仅表示 ABI 与流程合规。
