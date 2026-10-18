# NUP 应用安装包格式规范
> 文档版本：V1.0（对应 Novus OS V1.6-FinalSpec）
> 扩展名：`.nup`（Novus Package）
> 用途：应用商店/安装器专用安装包，打包 .nuc 程序、.ins 快捷方式、图标、资源
> 状态：**冻结**

---

## 1. 概述

`.nup` 是 Novus OS 的应用安装包格式，作用类似：
- Windows 的 `.msi`
- Debian 的 `.deb`
- Android 的 `.apk`

用途：
- 应用商店下载安装
- 命令行 `novus-install xxx.nup`
- 双击安装

特点：
- 自包含（一个文件包含所有内容）
- 纯文本清单 + 文件归档
- 支持依赖声明
- 支持签名（可选）
- 安装/卸载清晰可逆

---

## 2. 文件整体结构

```
偏移 0x0000 ┌──────────────────────────┐
            │  NUP Header (nup_hdr_t)  │   固定 64 字节
偏移 0x0040 ├──────────────────────────┤
            │  Manifest (清单文本)       │   描述包信息、文件列表
偏移 ...    ├──────────────────────────┤
            │  File Entry Table        │   每个文件的元信息
偏移 ...    ├──────────────────────────┤
            │  文件数据区              │   各文件原始字节顺序拼接
偏移 ...    ├──────────────────────────┤
            │  签名区（可选）           │   Ed25519 签名
            └──────────────────────────┘
```

---

## 3. NUP Header（文件头，64 字节）

```c
#define NUP_MAGIC 0x4E555021   // "NUP!"

typedef struct __attribute__((packed)) {
    uint8_t  magic[4];           // 0x00  "NUP!"
    uint16_t version;             // 0x04  格式版本，当前 = 1
    uint16_t flags;               // 0x06  标志位
    uint32_t manifest_offset;     // 0x08  Manifest 在文件中的偏移
    uint32_t manifest_len;        // 0x0C  Manifest 长度
    uint32_t file_table_offset;   // 0x10  文件表偏移
    uint16_t file_count;           // 0x14  包内文件数量
    uint16_t reserved1;           // 0x16
    uint32_t total_uncompressed;  // 0x18  解压后总大小（字节）
    uint32_t signature_offset;     // 0x1C  签名区偏移（0=无签名）
    uint32_t signature_len;        // 0x20  签名长度（0=无签名）
    uint8_t  package_name[32];     // 0x24  包名（'\0'结尾）
    uint8_t  reserved[20];         // 0x44  填充到64字节
} nup_hdr_t;                       // 总大小 64 字节
```

### flags 位定义

| 位 | 名称 | 含义 |
|---|---|---|
| bit0 | NUP_FLAG_SIGNED | 1=有签名 |
| bit1 | NUP_FLAG_SYSTEM | 1=系统包（安装到 /system） |
| bit2 | NUP_FLAG_REBOOT | 1=安装后需重启 |
| bit3~15 | 保留 | 必须为 0 |

---

## 4. Manifest（清单，文本格式）

Manifest 是一段纯文本（UTF-8），描述包的元信息和文件列表。

### 4.1 清单格式（INI 风格）

```ini
[Package]
Name=novus-settings
Version=1.2.0
Release=1
Arch=x86_64
Summary=系统设置面板
License=GPLv2
Author=Novus Team
Category=System

[Dependencies]
Dep1=novus-wm >= 1.0.0
Dep2=novus-sdk >= 1.0.0

[Files]
File1=/system/bin/novus-settings.nuc
File2=/system/share/applications/novus-settings.ins
File3=/system/assets/icons/settings.png

[Icons]
MainIcon=/system/assets/icons/settings.png

[Desktop]
EntryFile=novus-settings.ins
```

### 4.2 字段定义

#### [Package] 段

| 字段 | 必填 | 说明 | 示例 |
|---|---|---|---|
| Name | ✅ | 包名（唯一） | `Name=novus-settings` |
| Version | ✅ | 版本号 | `Version=1.2.0` |
| Release | ✅ | 发布号 | `Release=1` |
| Arch | ✅ | 架构 | `Arch=x86_64` |
| Summary | ✅ | 一句话描述 | `Summary=系统设置面板` |
| License | ✅ | 许可证 | `License=GPLv2` |
| Author | ❌ | 作者 | `Author=Novus Team` |
| Category | ✅ | 分类 | `Category=System` |
| Description | ❌ | 详细描述 | 多行 |

#### [Dependencies] 段
- `DepN=包名 >= 版本`
- 安装时检查依赖是否已安装
- 依赖缺失 → 报错提示用户先装依赖

#### [Files] 段
- `FileN=安装后的目标路径`
- 列出包内所有文件安装到系统的哪里

#### [Icons] 段
- `MainIcon=图标路径`（用于应用商店展示）

#### [Desktop] 段
- `EntryFile=.ins 文件名`（开始菜单/桌面快捷方式）

---

## 5. File Entry Table（文件表）

每个文件在包内的元信息，16 字节/条：

```c
typedef struct __attribute__((packed)) {
    uint32_t file_offset;     // 0x00  文件数据在 .nup 中的偏移
    uint32_t file_size;       // 0x04  文件大小（压缩后）
    uint32_t original_size;   // 0x08  文件原始大小
    uint16_t path_len;        // 0x0C  路径字符串长度
    uint8_t  compress_type;    // 0x0E  0=不压缩 1=lz4
    uint8_t  reserved;         // 0x0F
} nup_file_entry_t;            // 16 字节
```

路径字符串紧跟在文件表之后（每个文件的安装路径）。

---

## 6. 文件数据区

- 各文件原始字节（可选 lz4 压缩）顺序拼接
- 每个文件按 4 字节对齐填充
- 安装器按文件表逐个解压、写入目标路径

---

## 7. 签名（可选）

### 7.1 签名位置
- Header 的 `signature_offset` / `signature_len` 指定
- `flags bit0 = 1` 表示有签名

### 7.2 签名算法
- 与 NKM 一致：Ed25519
- 签名原文：NUP 文件除签名区外全部字节的 SHA256
- 公钥：与内核同一公钥池

### 7.3 验证流程
1. 计算文件前 `signature_offset` 字节的 SHA256
2. 用公钥验证签名
3. 失败 → 安装器拒绝安装

---

## 8. 安装流程

```
1. 用户双击 .nup 文件（或 novus-install xxx.nup）
2. 安装器打开文件，读 Header
3. 校验 magic == "NUP!"
4. 校验 version == 1
5. 如果有签名：验证 Ed25519
   - 验证失败 → 弹窗"包签名无效"，终止
6. 解析 Manifest
7. 检查依赖：遍历 [Dependencies] 段
   - 依赖未安装 → 弹窗提示"需要先安装 XXX"，终止
8. 检查磁盘空间：total_uncompressed < 剩余空间？
9. 逐文件解压并写入目标路径
   - /system/ 下的文件需要 fs.full CAP（管理员确认弹窗）
10. 注册快捷方式（.ins 文件自动生效）
11. 写入安装记录到 /var/db/packages/novus-settings.nupdb
12. 完成弹窗"安装成功"
```

---

## 9. 卸载流程

```
1. 用户在设置→应用管理中点击"卸载"
2. 安装器读取 /var/db/packages/<包名>.nupdb
3. 遍历已安装文件列表
4. 逐文件删除（/system/ 下文件需管理员确认）
5. 删除安装记录文件
6. 删除开始菜单条目（.ins 文件）
7. 完成弹窗"卸载成功"
```

---

## 10. 完整示例：novus-settings.nup

### 10.1 包内容

```
novus-settings.nup 包含：
  ├── novus-settings.nuc     (可执行文件)
  ├── novus-settings.ins    (快捷方式)
  └── settings.png           (图标)
```

### 10.2 Manifest 内容

```ini
[Package]
Name=novus-settings
Version=1.2.0
Release=1
Arch=x86_64
Summary=Novus OS 系统设置面板
License=GPLv2
Author=Novus Team
Category=System

[Dependencies]
Dep1=novus-wm >= 1.0.0

[Files]
File1=/system/bin/novus-settings.nuc
File2=/system/share/applications/novus-settings.ins
File3=/system/assets/icons/settings.png

[Icons]
MainIcon=/system/assets/icons/settings.png

[Desktop]
EntryFile=novus-settings.ins
```

### 10.3 安装后系统状态
```
/system/bin/novus-settings.nuc              ← 程序
/system/share/applications/novus-settings.ins ← 快捷方式
/system/assets/icons/settings.png            ← 图标
/var/db/packages/novus-settings.nupdb        ← 安装记录
```

---

## 11. 安装器命令行

```bash
# 安装
novus-install novus-settings.nup

# 卸载
novus-remove novus-settings

# 列出已安装包
novus-pkg list

# 查询包信息
novus-pkg info novus-settings
```

---

## 12. 安装记录文件（.nupdb）

每个已安装包在 `/var/db/packages/` 下有一个 `.nupdb` 文件，记录：

```ini
[Installed]
Name=novus-settings
Version=1.2.0
InstallDate=2026-10-18
InstalledBy=root

[Files]
File1=/system/bin/novus-settings.nuc
File2=/system/share/applications/novus-settings.ins
File3=/system/assets/icons/settings.png
```

卸载时读取此文件，逐个删除记录的文件。

---

## 13. 与其他格式对比

| 维度 | .nup | .deb | .apk | .msi |
|---|---|---|---|---|
| 格式 | 自描述归档 | ar+tar.gz | zip+DEX | MSI数据库 |
| 元数据 | INI文本 | control归档 | AndroidManifest.xml | 二进制数据库 |
| 签名 | Ed25519 | debsig | v1/v2签名 | Authenticode |
| 依赖 | Manifest 声明 | Depends 字段 | uses-library | 表 |
| 大小 | ~几百KB | ~几MB | ~几MB | ~几MB |

---

## 14. 文档冻结声明

本规范为 `.nup` 安装包格式唯一基准。约束：

- Header 64 字节，魔数 `NUP!`
- Manifest 为 INI 文本，UTF-8
- 文件表 16 字节/条
- 签名（可选）使用 Ed25519
- 安装记录存 `/var/db/packages/*.nupdb`
- 安装目标路径在 Manifest [Files] 段声明

任何格式变更必须升级 Version 字段。

文档状态：✅ **冻结，可作为应用商店/安装器实现的基准。**
