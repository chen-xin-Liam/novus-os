# 中文支持与 Windows 11 桌面

M9 把桌面从深色演示界面改成 **Windows 11 Fluent 风格**，并让整条文本链路
支持中文：从字模、排版、输入法到三个 Rust 应用。

## 1. 字形：生成、提交、二分查找

```
tools/fontgen/charset.txt     界面语料（额外补充用）
        + 扫描 graphics/ wm/ apps/src/ kernel/ 里的非 ASCII 字符
        ▼  tools/fontgen/fontgen.py（Noto Sans CJK SC）
graphics/nd_font_cjk.c        16x16 1bpp 点阵 + 升序码点表（提交进仓库）
```

* 生成物是 **C 源码**，构建不依赖宿主字体；换文案后跑一次
  `python3 tools/fontgen/fontgen.py` 即可补齐新字。
* `nd_cjk_find()` 码点二分，最多 12 次比较；缺字画空心方框，缺口一眼可见。
* ASCII 仍是 5x7 点阵（放大 2 倍 = 12px 步进、14px 行高），汉字 16x16
  （16px 步进）。混排时整行取较高单元格、底边对齐：

```c
cell = nd_text_height(text, scale);   /* 16 if the line has CJK */
draw_ascii(..., y + cell - 7 * scale, ...);
draw_cjk(...,   y + cell - 16, ...);
```

## 2. 文本管道

| 接口 | 作用 |
|------|------|
| `nd_utf8_next()` | 解一个码点并前进（坏字节 → U+FFFD，绝不越界） |
| `nd_char_adv(cp, scale)` | ASCII `6*scale`，CJK `16` |
| `nd_text_width()` / `nd_text_width_n()` | 整串 / 前 N 字节的绘制宽度 |
| `nd_text_height()` | 该行需要的单元格高度 |
| `nd_text_limit(..., max_px, ...)` | 按**像素**裁剪，只在码点边界断开 |

所有"字节数 = 字符数 = 6px"的假设都改成了查度量：文本框的滚动、光标与
点击落点（`nvgui.c` 的 `edit_skip`/`edit_prev`/`edit_place`），窗口管理器
的文本损伤矩形（`nwm.c`），Rust 侧的 `ui::width()` 与右对齐列。

## 3. 拼音输入法（`graphics/nd_ime.c`）

```
tools/imegen/pinyin.txt        402 音节 → 最多 6 个候选（按字频降序）
        ▼  tools/imegen/imegen.py --gen
graphics/nd_ime_dict.c         音节池 + 候选码点表（提交进仓库）
```

* 提取自 ibus-table 的拼音表（`--extract`），只保留常用字，共 2348 个候选，
  与字形表同一批码点，所以输入法产不出"没有形状"的字。
* 状态机：`a-z` 累积 → 候选窗列出 → `空格/回车` 上屏第一个、`1-6` 直接选、
  `←/→` 换选、`Esc` 取消、`退格` 改拼音。
* **前缀校验**：缓冲区始终是某个音节的前缀（`n-i-h` 会被拒绝），因此候选
  列表不会为空，也就不需要"无候选"状态。
* 候选窗是独立的 **overlay 图层**：`ng_render_overlay()` 在窗口之后绘制，
  并按 `ng_overlay_hit()` 优先接收点击——开始菜单同理。
* 上屏走 `ng_edit_insert_cp()`：UTF-8 多字节插入，光标/退格按码点移动。

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
python3 tools/fontgen/fontgen.py     # 文案改动后补字形
python3 tools/imegen/imegen.py --gen # 字典改动后重生成
make                                  # 生成物已提交，普通构建不需要字体
scripts/wm-churn.sh 4G 20             # 应用启停压力（含窗口装饰点击）
```

验收点：桌面与三个应用全中文；开始菜单浮于窗口之上且点击优先；文本框键入
`nihao` 出现候选窗、空格上屏"鸟"；`[IME] pinyin up: 402 syllables`；
`wm-churn` 20 轮无缺页、tick 持续；ISO 从 CD-ROM 启动同样可用。
