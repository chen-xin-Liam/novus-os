# INS 快捷方式文件格式规范
> 文档版本：V1.0（对应 Novus OS V1.6-FinalSpec）
> 扩展名：`.ins`（Novus Shortcut）
> 用途：桌面/开始菜单/任务栏快捷方式，指向 `.nuc` 原生程序
> 格式：INI 风格纯文本，UTF-8 编码，人类可直接编辑
> 状态：**冻结**

---

## 1. 概述

`.ins` 是 Novus OS 的桌面快捷方式文件，作用类似：
- Windows 的 `.lnk`
- Linux 的 `.desktop`

用途：
- 桌面图标
- 开始菜单项
- 任务栏快捷启动
- 文件管理器的"发送到"菜单

特点：
- 纯文本 INI 格式，无需二进制解析
- 可手动编辑
- 支持国际化名称
- 支持命令行参数
- 支持自定义图标

---

## 2. 文件存放位置

| 位置 | 用途 |
|---|---|
| `/system/share/applications/*.ins` | 系统预装应用快捷方式（只读） |
| `/usr/share/applications/*.ins` | 用户安装的应用快捷方式 |
| `/home/user/Desktop/*.ins` | 桌面图标 |
| `/system/assets/icons/` | 图标资源目录 |

---

## 3. 文件格式

### 3.1 基本结构

```ini
[Novus Entry]
Type=Application
Version=1
Name=系统设置
Name[en]=Settings
Comment=Novus OS 系统配置面板
Comment[en]=Novus OS Settings
Exec=/system/bin/novus-settings.nuc
Icon=/system/assets/icons/settings.png
Path=/system/bin/
Categories=System;Settings;
Terminal=false
StartupNotify=true
```

### 3.2 编码与换行
- 编码：UTF-8（无 BOM）
- 换行：LF（`\n`）
- 注释：以 `#` 开头
- 空行忽略

---

## 4. 字段完整定义

### 4.1 必填字段

| 字段 | 类型 | 说明 | 示例 |
|---|---|---|---|
| `[Novus Entry]` | 段头 | 固定段名，不可改 | `[Novus Entry]` |
| `Type` | 字符串 | 条目类型，固定 `Application` | `Type=Application` |
| `Version` | 数字 | 格式版本，当前 `1` | `Version=1` |
| `Name` | 字符串 | 应用显示名称（默认语言） | `Name=终端` |
| `Exec` | 路径 | 要执行的 `.nuc` 文件路径 + 参数 | `Exec=/system/bin/terminal.nuc` |

### 4.2 可选字段

| 字段 | 类型 | 说明 | 示例 |
|---|---|---|---|
| `Name[xx]` | 字符串 | 多语言名称（xx=语言代码） | `Name[en]=Terminal` |
| `Comment` | 字符串 | 鼠标悬停提示文字 | `Comment=命令行终端` |
| `Comment[xx]` | 字符串 | 多语言注释 | `Comment[en]=Command Line` |
| `Icon` | 路径 | 图标文件路径（.png） | `Icon=/system/assets/icons/terminal.png` |
| `Path` | 目录 | 工作目录（启动时 cd 到这里） | `Path=/home/user/` |
| `Categories` | 分号分隔 | 分类（用于开始菜单分组） | `Categories=System;Utility;` |
| `Terminal` | 布尔 | 是否在终端中运行 | `Terminal=false` |
| `StartupNotify` | 布尔 | 是否显示启动通知 | `StartupNotify=true` |
| `MimeType` | 分号分隔 | 支持的文件类型 | `MimeType=text/plain;text/markdown;` |
| `OnlyShowIn` | 分号分隔 | 仅在指定桌面环境显示 | `OnlyShowIn=Novus;` |
| `NoDisplay` | 布尔 | 是否隐藏（不在菜单显示） | `NoDisplay=false` |

---

## 5. Exec 字段详细规则

### 5.1 基本格式
```
Exec=<可执行文件路径> [参数1] [参数2] ...
```

### 5.2 路径
- 必须是绝对路径
- 指向 `.nuc` 文件（或 `.npy` 脚本）

### 5.3 参数占位符（冻结）

| 占位符 | 含义 |
|---|---|
| `%f` | 单个文件路径 |
| `%F` | 多个文件路径 |
| `%u` | 单个 URL |
| `%U` | 多个 URL |
| `%%` | 字面 `%` |

### 5.4 示例
```ini
# 无参数启动
Exec=/system/bin/novus-settings.nuc

# 带参数
Exec=/system/bin/terminal.nuc --working-dir=/home/user

# 打开文件
Exec=/system/bin/filemgr.nuc %F
```

---

## 6. Categories 分类表（冻结）

开始菜单按 Categories 分组：

| 分类值 | 菜单分组 |
|---|---|
| `System` | 系统工具 |
| `Settings` | 设置 |
| `Utility` | 实用工具 |
| `Development` | 开发 |
| `Graphics` | 图形/图像 |
| `Network` | 网络 |
| `Office` | 办公 |
| `Game` | 游戏 |
| `Multimedia` | 媒体 |

---

## 7. 完整示例

### 7.1 系统设置
文件路径：`/system/share/applications/novus-settings.ins`

```ini
[Novus Entry]
Type=Application
Version=1
Name=系统设置
Name[en]=Settings
Comment=Novus OS 系统配置
Comment[en]=Novus OS Settings
Exec=/system/bin/novus-settings.nuc
Icon=/system/assets/icons/settings.png
Path=/system/bin/
Categories=System;Settings;
Terminal=false
StartupNotify=true
```

### 7.2 终端
文件路径：`/system/share/applications/terminal.ins`

```ini
[Novus Entry]
Type=Application
Version=1
Name=终端
Name[en]=Terminal
Comment=命令行终端
Comment[en]=Command Line Terminal
Exec=/system/bin/terminal.nuc
Icon=/system/assets/icons/terminal.png
Path=/home/user/
Categories=System;Utility;
Terminal=true
StartupNotify=true
```

### 7.3 文件管理器
```ini
[Novus Entry]
Type=Application
Version=1
Name=文件管理器
Name[en]=File Manager
Exec=/system/bin/filemgr.nuc %F
Icon=/system/assets/icons/filemgr.png
Path=/home/user/
Categories=Utility;
MimeType=text/plain;text/markdown;inode/directory;
Terminal=false
```

### 7.4 任务管理器
```ini
[Novus Entry]
Type=Application
Version=1
Name=任务管理器
Name[en]=Task Manager
Exec=/system/bin/taskmgr.nuc
Icon=/system/assets/icons/taskmgr.png
Categories=System;
NoDisplay=false
```

### 7.5 磁盘管理器
```ini
[Novus Entry]
Type=Application
Version=1
Name=磁盘管理器
Name[en]=Disk Manager
Exec=/system/bin/diskmgr.nuc
Icon=/system/assets/icons/diskmgr.png
Categories=System;Settings;
Terminal=false
```

---

## 8. 解析规则（桌面/WM 端）

### 8.1 加载顺序
1. 扫描 `/system/share/applications/*.ins`
2. 扫描 `/usr/share/applications/*.ins`
3. 扫描 `/home/user/Desktop/*.ins`

### 8.2 解析步骤
```
1. 打开 .ins 文件
2. 找到 [Novus Entry] 段
3. 逐行读取 key=value
4. 提取 Name、Exec、Icon
5. 按 Categories 分组
6. 渲染到开始菜单/桌面
```

### 8.3 点击行为
```
用户点击图标
  ↓
读取 Exec 字段
  ↓
解析参数（%F 等替换为实际文件路径）
  ↓
调用 sys_nuc_exec() 启动程序
  ↓
窗口管理器接管窗口
```

### 8.4 错误处理
| 情况 | 行为 |
|---|---|
| .ins 文件不存在 | 不显示图标 |
| Exec 指向的 .nuc 不存在 | 灰色图标，点击报错 |
| Icon 路径无效 | 使用默认图标 |
| [Novus Entry] 段缺失 | 跳过此文件 |

---

## 9. 与 Windows .lnk 的对比

| 维度 | .ins | Windows .lnk |
|---|---|---|
| 格式 | 纯文本 INI | 二进制 |
| 可编辑 | 直接文本编辑器改 | 需专用工具 |
| 大小 | ~几百字节 | ~几KB |
| 解析难度 | 极低 | 高（二进制格式） |
| 适合 | 自研 OS、极简 | Windows 生态 |

---

## 10. 文档冻结声明

本规范为 `.ins` 快捷方式文件格式唯一基准。约束：

- 段头固定 `[Novus Entry]`
- 必填字段：Type、Version、Name、Exec
- Exec 占位符仅 `%f %F %u %U %%`
- 编码 UTF-8，换行 LF
- Categories 分类值固定

任何字段扩展必须走格式版本升级（Version 字段）。

文档状态：✅ **冻结，可作为桌面/开始菜单实现的基准。**
