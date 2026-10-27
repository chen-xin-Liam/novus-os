# gui-app：最小桌面应用

`src/main.rs` 是一个完整的 Novus OS 桌面应用：创建窗口、用 Fluent 控件画一页、
按钮点击计数、Esc / 标题栏关闭、跟随主题与语言切换。

## 单独编译（不改主仓）

```bash
bash verify.sh
```

预期最后三行：`Type: EXEC`、`Entry point address: 0x400000`、blob 约 30 KB。
本目录的 `Cargo.toml`、`.cargo/config.toml`、`link.ld` 只为这一步服务；接进系统
时**不需要**它们。

## 接进系统

只复制 `src/main.rs`，按根目录 `../README.md` §1.2 改 4 处（`apps/src/bin/`、
`apps/Cargo.toml`、`Makefile`、`kernel/apps.c`），然后 `make`，在 nsh 里敲应用名。

## 改哪儿

| 位置 | 作用 |
|------|------|
| `CW` / `CH` | 客户区尺寸（标题栏另算 `wm::TITLE_H`） |
| `Win::create(..., 320, 200, ...)` | 窗口在桌面上的初始位置 |
| `BTN` | 按钮矩形；`draw()` 与点击判断共用，避免画的和点的不一致 |
| `draw()` | 界面内容，用 `ui::page/card/button/row/section/progress/empty` |
| `_start()` 的 `match ev` | 交互分支 |

## 注意

- 每帧最多 40 个图形、48 段文字；超出会被丢弃。
- 无分配器：不要 `Vec` / `String` / `Box`，用固定数组。
- 颜色只用 `ui::bg()/card_bg()/text()/dim()/accent()/line()` 等主题令牌。
- 空闲时 `sys::yield_now()`；任务运行期间不会被抢占。
