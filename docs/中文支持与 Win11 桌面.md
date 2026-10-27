# 中文支持与 Windows 11 桌面

M9 把桌面从深色演示界面改成 **Windows 11 Fluent 风格**，并让整条文本链路
支持中文：从字模、排版、输入法到三个 Rust 应用。

## 1. 字形：完整 Unicode

```
unifont.hex + unifont_upper.hex   GNU Unifont 16x16 / 8x16（GPLv2+ 字体嵌入例外）
        ▼  tools/fontgen/fontgen.py
graphics/nd_font_data.bin         3.5 MB：每字形 32 字节（16 行 × 2 字节，左对齐）
graphics/nd_font_uni.c            稀疏页索引 + 每码点步进表（生成物，提交）
apps/src/unicode_data.rs          同一份步进表，Rust 侧使用
```

* 字形集不再由"界面语料"决定：BMP 与补充平面共 114,397 个字形，
  任意已分配码点都有形状，终端输出、文件名、用户输入不再出现豆腐块。
* 存储是稀疏页：`g_uni_page[cp >> 8]` 给出槽号，槽内 256 位的
  `valid`/`wide` 位图和一块密集字形。查找 = 页 → 位 → popcount rank，
  不随字形数变慢。
* ASCII 仍是 5x7（6px 步进），其余码点用 Unifont 字宽：8px 半角、
  16px 全角。组合记号（Mn/Me/Cf）步进 0，画在前一个字形上。
* 3.5 MB 点阵由 `graphics/nd_font_blob.S` 的 `incbin` 嵌入，不经过 C
  编译器；EFI 镜像从 2.5 MB 增至约 6.4 MB，仍在 El Torito 32 MB 以内。
* 未分配码点与私用区没有字形，画空心方框；宽度按 East Asian Width
  （全角 16，其余 8）保留排版。混排时整行取较高单元格、底边对齐。

重新生成（需要 Unifont 的 hex 构建，见脚本头部 URL）：

```bash
python3 tools/fontgen/fontgen.py --bmp unifont.hex --upper unifont_upper.hex
```

## 2. 文本管道

| 接口 | 作用 |
|------|------|
| `nd_utf8_next()` | 严格解一个码点；超长编码、代理区、>U+10FFFF 均为 U+FFFD |
| `nd_utf8_encode()` | 1–4 字节编码，拒绝代理区与超范围 |
| `nd_char_adv(cp, scale)` | 控制/组合 0、ASCII `6*scale`、其余 8/16 × scale |
| `nd_glyph_find(cp, &wide)` | 码点 → 32 字节点阵，附带 8/16 字宽 |
| `nd_text_width()` / `nd_text_width_n()` | 整串 / 前 N 字节的绘制宽度 |
| `nd_text_height()` | 该行需要的单元格高度（有正宽非 ASCII 时为 16） |
| `nd_text_limit(..., max_px, ...)` | 按**像素**裁剪，只在码点边界断开 |

所有"字节数 = 字符数 = 6px"的假设都改成了查度量：文本框的滚动、光标与
点击落点（`nvgui.c` 的 `edit_skip`/`edit_prev`/`edit_place`），窗口管理器
的文本损伤矩形（`nwm.c`），终端的换行与光标（`nsh.rs`），Rust 侧的
`ui::width()`、定宽列 `Buf` 与右对齐列。协议里的标题/文本缓冲在两端都
退到码点边界，不再从半个汉字中间截断。

## 3. 拼音输入法（`graphics/nd_ime.c`）

```
tools/imegen/pinyin.txt        402 音节 → 最多 6 个候选（按字频降序）
        ▼  tools/imegen/imegen.py --gen
graphics/nd_ime_dict.c         音节池 + 候选码点表（提交进仓库）
```

* 提取自 ibus-table 的拼音表（`--extract`），只保留常用字，共 2348 个候选；
  这些码点在完整字形表里当然也有形状。
* 状态机：`a-z` 累积 → 候选窗列出 → `空格/回车` 上屏第一个、`1-6` 直接选、
  `←/→` 换选、`Esc` 取消、`退格` 改拼音。
* **前缀校验**：缓冲区始终是某个音节的前缀（`n-i-h` 会被拒绝），因此候选
  列表不会为空，也就不需要"无候选"状态。
* 候选窗是独立的 **overlay 图层**：`ng_render_overlay()` 在窗口之后绘制，
  并按 `ng_overlay_hit()` 优先接收点击——开始菜单同理。
* 上屏走 `ng_edit_insert_cp()`：UTF-8 多字节插入（含 4 字节），光标/退格
  按码点移动。

### 客户端窗口的输入（终端等）

`nd_ime_key_ex()` 把同一个状态机从"文本框"里解耦出来：窗口管理器把按键
先交给 IME，被消费就不转发；组合完成时把码点作为普通按键事件投递给焦点
窗口。因此 `nsh` 这类 Rust 客户端不需要链接输入法代码。

* `Ctrl+Space` 在客户端窗口切换中/英（默认英文，终端里输入命令不受影响）；
  候选窗画在焦点窗口客户区内（`nd_ime_draw_at`）。
* `Ctrl+Shift+U` → 1–6 位十六进制 → `Enter/空格` 提交任意 Unicode 码点
  （`nd_input.c`），这是键盘布局与拼音词库都覆盖不到的字符的入口。
* 会话窗口的用户名现在接受任意非控制 UTF-8（`kernel/session.c`）。

## 4. Windows 11 桌面

| 区域 | 实现 |
|------|------|
| 桌面 | 云母蓝渐变（`theme.desktop_top/bottom`），左上 `Novus OS` 水印 + 2x2 桌面磁贴 |
| 任务栏 | 底部居中 560x48 悬浮胶囊（圆角 12、1px 边框），开始 / 文件 / 任务 / 设置 + 时钟 |
| 开始菜单 | 360x402 悬浮卡片，`NG_WF_OVERLAY` 图层，"已固定" 三磁贴 + "推荐" 三行 + 用户页脚 |
| 窗口 | 32px 标题栏、10px 圆角、14px 柔影、右侧 `— □ ✕`（46x32，关闭悬停 `#C42B1C`） |
| 时钟 | `kernel/rtc.c` 读 CMOS（处理 BCD / 12 小时制 / UIP），每分钟只在变化时重绘 |
| 演示窗口 | 不再开机自动显示，从开始菜单的"控件演示 / 控件与列表"打开 |

配色（`nvgui.c: theme_init` 与 `apps/src/ui.rs` 各一份，保持一致）：

```
桌面 #EEF2F8 → #E3EAF3   窗口 #FFFFFF   标题栏 #F3F3F3
正文 #1B1B1B   次要 #5F6368   强调 #0067C0   边框 #E1E5EA
关闭悬停 #C42B1C   成功 #107C10   警告 #B45309
```

## 5. 重新生成与验证

```bash
python3 tools/fontgen/fontgen.py --bmp unifont.hex --upper unifont_upper.hex
python3 tools/imegen/imegen.py --gen # 字典改动后重生成
make                                  # 生成物已提交，普通构建不需要字体
scripts/wm-churn.sh 4G 20             # 应用启停压力（含窗口装饰点击）
```

验收点：桌面与三个应用全中文；开始菜单浮于窗口之上且点击优先；文本框键入
`nihao` 出现候选窗、空格上屏"鸟"；`[IME] pinyin up: 402 syllables`；
`wm-churn` 20 轮无缺页、tick 持续；ISO 从 CD-ROM 启动同样可用。
完整 Unicode 的验收：终端里 `Ctrl+Shift+U 0410 Enter` 出现西里尔 `А`，
`Ctrl+Space` 后输入 `nihao` 候选窗出现在窗口内、空格上屏"鸟"，两者与
ASCII 混排时光标与换行按像素对齐。

## 6. M10：Novus Fluent 与深色主题

M10 把"Win11 风格"收敛为可复用的设计语言，并补上深色：

- 令牌单一来源：`tools/themegen/theme.toml` → `graphics/nd_theme.[ch]` +
  `apps/src/theme.rs`。`nvgui` 不再自带调色板，应用不再重复写颜色常量。
- 三种模式：浅色、深色、跟随时间（19:00–07:00 深色）；设置页与快捷设置同时可改，
  经 `SYSINFO_REQ_THEME` 落到 `nwm_theme_set_mode()`，版本号变化后壳层重建、
  客户端重绘。
- 新增圆角显示列表图元（op 6/7），卡片与胶囊按钮不再用四条线拼角；
  `apps/src/ui.rs` 成为统一组件库。
- 中文覆盖仍由 `fontgen` 保证：新增文案（安装向导、主题页）已重新生成
  `graphics/nd_font_cjk.c`（2445 个字形）。

完整的颜色、间距、组件与页面规范见
[`docs/Novus UI 设计语言规范 V1.0.md`](Novus%20UI%20设计语言规范%20V1.0.md)；
安装向导页面与主题联动见
[`docs/图形安装向导设计与实现.md`](图形安装向导设计与实现.md)。
