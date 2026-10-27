# Changes

## QEMU 网卡全线打通（e1000 / e1000e / virtio-net / rtl8139）

- 新增 `kernel/net_rtl8139.c`：I/O BAR 驱动，8 KiB 收环 + CBR/CAPR 握手、
  4 个发送描述符（用 TOK 位判复用，不依赖厂商的 OWN 语义）。**CAPR 必须写
  “读指针 − 16”**，写成读指针本身会让 QEMU 把缓冲判成满而收不到任何包——
  这是上一轮没接通的原因。实测 DHCP/DNS/NCSI 全通。
- `kernel/net_igb.c` 修掉三处真缺陷，此前该驱动在**任何**平台上都收不到包：
  - 高级接收描述符结构错位（`status_error` 在 0x18、`length` 在 0x1C，
    原结构多出一个 dword 且 status 落在 0x20，RDLEN 40×64 也不是 128 的整数倍）；
  - `SRRCTL0` 偏移写成 0x02100，82575+ 实际在 **0x0C00C**，导致队列没有缓冲；
  - 缺 `RXDCTL0`/`TXDCTL0` 队列使能（8254x 不需要，82575+ 必需），
    初始化顺序改为与 Linux 一致（先环基址，再 SRRCTL，最后使能队列）。
  仍保留“STATUS.LU 置位才激活”的判定：QEMU 的 igb 模型从不置 LU，其收包
  路径也随之丢弃所有帧（QEMU trace `e1000x_rx_link_down` 可证），所以 QEMU
  下 igb 如实显示“未连接”；真机 82576/82580/I350 不受影响。
- `kernel/net_e1000.c`：MAC 改为优先读 RAL/RAH（固件已写入的权威副本），
  EERD 只作兜底。e1000e 的 NVM 是 flash，旧顺序会读到 `52:54:00:00:00:00`；
  现在 `-device e1000e` 得到完整的 `52:54:00:12:34:56`。网卡名按注册序号
  取 `ethN`，多网卡不再重名。
- 修复 DHCP 超时后的重发循环：超时分支没有把 `g_phase` 清零，导致每个
  tick 都重新 DISCOVER、重复打印 fallback，并反复触发 NCSI 探针。
- `scripts/run.sh`：新增 `NOVUS_NIC` 变量选择 QEMU 网卡模型，
  默认仍是兼容性最好的 `e1000`。
- QEMU 10.0.13 实测矩阵（`-netdev user` + DHCP/DNS/NCSI）：

  | 模型 | 结果 |
  |---|---|
  | `e1000` / `e1000e` | 通（Intel 82540EM / 82574L） |
  | `virtio-net-pci` | 通（legacy virtio） |
  | `rtl8139` | 通（Realtek RTL8139） |
  | `igb` | 驱动加载，QEMU 模型不置 STATUS.LU，收包被 QEMU 丢弃 |
  | `vmxnet3` / `ne2k_pci` / `pcnet` | 未实现 |

- 验证：`make` 通过；四种可用模型均跑出 `[DNS] answer` → `[NCSI] connected`。
  未创建 git commit。

## 更多网卡：统一身份表、virtio-net 与无线识别

- 新增 `kernel/net_ids.c`：PCI 身份表把 `vendor:device` 映射到驱动家族和型号
  （Intel 8254x/e1000e/I21x、82575/82576/82580/I350、virtio、Intel
  AX200/AX210/**AX211**、MediaTek MT7921/MT7922、Realtek、Qualcomm、
  Broadcom）。驱动不再各自维护 if-chain，而是扫一趟总线后用家族匹配。
- `net_e1000.c`：设备表从 3 个 ID 扩到全部 8254x/82571/82572/82573/82574/
  82583 与 I210/I211/I217/I218/I219；改为一次 `pci_scan`；MAC 读回做
  全 0 / 全 FF / 组播位校验，失败才回退 RAL/RAH（此前 I21x 会把
  `FF:FF:FF:...` 当成本机地址）。
- `net_igb.c`：同样改为一次扫描 + 家族匹配，补上 82575/82576/82580/
  I350 的其余 ID。
- 新增 `kernel/net_virtio.c`：virtio-net（legacy PCI，I/O BAR + PFN 队列），
  RX/TX 各一条队列、10 字节 virtio_net_hdr、只协商 `VIRTIO_NET_F_MAC`，
  收包在 `net_tick()` 轮询；virtio 1.0-only（0x1041）如实报“无 legacy BAR”。
  QEMU `-device virtio-net-pci` 实测 DHCP/DNS/NCSI 全通。
- `net_wifi.c`：枚举所有无线控制器并命名，注册为 `wlanN` + `NET_STATE_NODRV`，
  日志说明缺的是固件下载与 802.11/WPA2 关联。Intel AX211（`8086:51f0` /
  `54f0` / `7af0`）等现在会以型号出现在设备列表里，而不是一个笼统的
  “wireless controller present”。
- `netdev_t` 增加 `model[32]`（`net_strcpy()` 做 UTF-8 安全截断），
  `sysinfo` 快照带型号，网络面板新增“型号”一行；`NET_MAX_DEVS` 4 → 8。
- 未做：Intel I225/I226（igc）与 Realtek RTL8168/8125（r8169）驱动、USB/WWAN，
  以及无线真正上线（需要固件、802.11 与 WPA 握手，远超当前内核范围）。
- 验证：`make` 通过；QEMU 分别以 `-device e1000` 和
  `-device virtio-net-pci` 启动，均得到 DHCP 租约、
  `[DNS] answer 154.36.184.64`、`[NCSI] connected`；日志含型号
  （`Intel 82540EM` / `Virtio net (legacy)`）。未创建 git commit。

## NCSI 联网检测

- 新增 `kernel/net_dns.c`：单事务 A 记录解析，查询固定从 UDP 5353 发出，
  服务器地址来自 DHCP 选项 6（`net_dhcp.c` 现在请求/解析该选项），DHCP
  超时回退时使用 QEMU 用户态网络的 `10.0.2.3`。
- 新增 `kernel/net_ncsi.c`：按 NCSI 惯例抓 `http://ncsi.novus-os.ryzn.pro/
  connecttest.txt`，三阶段推进（解析域名 → 建连 → 读响应），失败时区分
  无链路 / DNS / TCP / HTTP / 超时；租约生效后自动跑一次，结果写入启动日志。
- `kernel/net_tcp.c` 从"回显探测"改成通用单会话客户端：`net_tcp_open()`
  推一段请求、`net_tcp_rx_data()` 取回响应，修复了校验和伪首部按字节序
  计算错误的问题；`net_tcp_test()` 保留为兼容包装。
- `kernel/net.c`：目的地址不在本网段时经网关转发（此前只对目的 IP 做 ARP，
  互联网主机永远不可达）；UDP 入站把 5353 端口交给 DNS；tick 里驱动 DNS
  与 NCSI 状态机。
- `sysinfo` 增加 `SYSINFO_REQ_NCSI`（0x031A）与快照字段 `ncsi` /
  `http_code` / `ncsi_ms`；网络面板（`apps/src/bin/netman.rs`）加"联网检测"
  卡片与按钮，实时显示阶段、HTTP 状态码与耗时。
- `scripts/run.sh` 从 `-net none` 改为 `-netdev user -device e1000`，默认
  `make run` 即有 DHCP/DNS/NAT；没有网卡时（如 `qtest.sh`）检测如实报
  "无网络设备"。
- 验证：`make` 通过；QEMU 用户态网络实测启动日志为
  `[DNS] answer 154.36.184.64` → `[NET] tcp established` →
  `[NCSI] connected: {"online":true,...}`。未创建 git commit。

## 完整 UTF-8 / Unicode：字形、度量、输入、终端与路径

- **字形从"界面语料"改为整个 Unicode**：`tools/fontgen/fontgen.py` 改为读取
  GNU Unifont 的 `unifont.hex` + `unifont_upper.hex`，生成
  `graphics/nd_font_data.bin`（3.5 MB，114,397 个字形，每字 32 字节）、
  `graphics/nd_font_uni.c`（稀疏页索引 + 步进区间表）和
  `apps/src/unicode_data.rs`（同一份步进表）。旧的
  `graphics/nd_font_cjk.c` / `nd_font_cjk.h`（2451 个语料字）删除。
- 存储是稀疏页：`g_uni_page[cp >> 8]` → 槽号，槽内 `valid`/`wide` 位图 +
  密集字形块，查找为页 → 位 → popcount rank。点阵用
  `graphics/nd_font_blob.S` 的 `.incbin` 嵌入，C 编译器不处理 3.5 MB 数组；
  `NOVUS.EFI` 由 2.5 MB 增至 6.4 MB，仍在 El Torito 32 MB 上限内。
  Unifont 源文件不入库，脚本头部给出下载地址。
- 度量改为查字形宽度：ASCII 仍 5x7/6px，其余 8px 半角、16px 全角；
  Mn/Me/Cf 组合记号步进 0，画在前一个字形上；行高按"有正宽的非 ASCII"
  取 16。`nd_utf8_next()` 现在拒绝超长编码、代理区与 >U+10FFFF；新增
  `nd_utf8_encode()`（1–4 字节）。绘制统一走 `nd_glyph_find()`。
- 事件 ABI：`ND_KEY_*` 移到 `0x110000` 之上（`ND_KEY_BASE`），字符键直接
  携带 Unicode 码点，控制键不再与 `U+0100..U+010A` 冲突；Rust 侧改为
  `wm::KEY_*` 常量。`nd_input.c` 增加 Alt 修饰与 `Ctrl+Shift+U` 码点直输
  （1–6 位十六进制 + 回车/空格，Esc 取消），提交前校验代理区与上限。
- 输入法服务化：`nd_ime_key_ex()` 把拼音状态机从文本框解耦，
  `nwm` 在转发按键前先交给 IME，组合完成时把码点作为按键事件投递给焦点
  窗口；`Ctrl+Space` 切换客户端中/英，候选窗用 `nd_ime_draw_at()` 画在
  焦点窗口内。`ng_edit_insert_cp()` 改用 `nd_utf8_encode()`，文本框可以
  直接键入并编辑 4 字节码点（原先 `>= U+10000` 被拒绝）。
- 终端 `nsh`：窗口改用 1x 文字（ASCII 6px、中文 16px，144 列 × 26 行），
  换行/滚动/光标全部按**像素宽度**；退格、Delete、左右键按码点移动；
  接受任意可打印码点并编码插入；滚动缓冲在码点边界折行，历史载入不再带
  尾随零字节。顺手修掉 `Drawer::flush` 用段尾 `x` 作起点导致整行右移的
  缺陷。
- Rust 侧同步度量：`ui::char_adv()` 查 `unicode_data`，`width()` 去掉
  "最后一格无间隔"的 `-1`，`width_big()` 对所有字符乘 scale（CJK 也放大），
  定宽列 `Buf` 按显示宽度补齐并用安全 `from_utf8`；`wm::copy_title`/
  `text_at` 按码点截断（新增 `fit_bytes`），`nwm` 服务端对标题与文本再做
  一次边界回退。
- 终端/文件：`shell::glob_match` 的 `?` 匹配一个码点、`*` 只在码点边界
  对齐；`push_history`、别名/变量、`ls` 名称、`parse` 的单词缓冲、`fileman`
  行名、`sinfo::spawn_name`、`session` 输入都退到码点边界；`fs.rs`/VFS
  路径复制改用 `VFS_PATH_MAX`（256）而不是组件上限 56，超长组件与超长
  路径返回失败而不再静默截断；`kernel/trap.c` 的 `user_path()` 拒绝非法
  UTF-8 路径；`kernel/sysinfo.c` 的 `copy_str` 截到码点边界；用户名接受
  任意非控制 UTF-8。
- 验证：`make` 全量通过（无新增告警）。QEMU/OVMF 无头启动截图确认桌面、
  文件管理器、设置、任务管理器与中文水印正常，临时水印测试串
  `Ελληνικά Русский é ñ ← ⚙ 😀` 全部有字形且 8/16 宽混排正确。临时让
  会话自动启动 `nsh` 做交互验证：`Ctrl+Shift+U 0410 Enter` 在提示符后
  出现 `А`；`Ctrl+Space` 后输入 `nihao`，候选窗出现在终端窗口内，空格
  上屏"鸟"，显示为 `user@novus:/$ А鸟`。验证用的临时改动（水印文案、
  自动启动、调试日志、截图脚本）均已还原或删除。

## 开发者模板目录 `templates/`

- 新增 `templates/`：三条扩展路径的最小骨架，复制即可开工，不必先读内核。
  `templates/README.md` 是索引，含完整接入步骤、每帧绘制预算（40 图形 / 48 段
  文字）、`no_std` 无分配器约束和调试手段（COM1）。
- `templates/gui-app/`：一个完整的桌面窗口应用（页面/卡片/按钮/点击计数/Esc
  关闭/主题跟随），自带 `Cargo.toml`、`.cargo/config.toml`、`link.ld` 和
  `verify.sh`，可以脱离主仓单独 `cargo build` 并检查入口地址；接进系统只需复制
  `src/main.rs` 并按文档改 4 处（`apps/src/bin/`、`apps/Cargo.toml`、`Makefile`
  的 `APPS_NAMES`、`kernel/apps.c` 的 `g_apps[]`）。
- `templates/shell-command/`：给 `nsh` 加内建命令的片段，含参数/退出码、读
  `Shell` 状态、读管道 stdin 三个例子；接入只需粘贴函数并在 `BUILTINS` 加一行，
  `help`/`which`/管道/重定向/命令替换自动生效。
- `templates/nuomi/`：两个脚本示例 —— 语言特性（递归、`map/reduce`、向量、
  闭包、`while`、`draw` 裸输出）与文件操作（建目录、读写追加、复制改名、删除），
  直接用 `nuomi <脚本>` 运行。
- 验证：模板 app 单独编译通过（入口 0x400000，blob 30,700 字节），并临时接入
  `apps/` + `Makefile` + `kernel/apps.c` 完整 `make` 通过（NOVUS.EFI 链接成功，
  验证后已撤销）；命令模板临时粘进 `cmds.rs` 编译通过；两个 `.nuomi` 脚本用
  同一份 `apps/src/nuomi.rs`（mock fs/shell）跑通并核对输出。

## nsh 集成 Nuomi（糯米）语言：`nuomi` 指令

- 新增 `apps/src/nuomi.rs`：把 `user/Nuomi_pl` 的 C 解释器移植成 `no_std`、
  无堆分配的 Rust 版本。语法（`❝…❞`/`"…"` 字符串、`'` 引用、`[...]` 向量、
  `;` 注释）、特殊形式（`quote/if/while/define/set!/lambda/let/let*/letrec/
  begin/and/or`）和内置函数名与 C 版一致；`print` 对字符串加引号、`str`/`draw`
  按原样输出，也与 C 版一致。
- `apps/src/shell/cmds.rs` 增加 `nuomi` 内建命令：`nuomi 脚本.nuomi` 执行脚本，
  `nuomi -e 表达式` 求值单条表达式（`nuomi` 不带参数打印用法）。`help`/`which`
  会自动列出该命令。
- 存储用一块随命令栈帧分配、按对象切分的 bump arena，没有垃圾回收：脚本能用到
  的容量就是 arena 的容量，用完报 `out of memory`。环境单独放在可回收区，
  函数调用/`let` 返回时回卷；闭包捕获了环境时保守地不回卷，保证闭包仍有效。
- 相对 C 版的**有意差异**：不提供 JSON/HTTP/进程/窗口原语；脚本在终端里不阻塞
  取键，`key` 恒为 `nil`、没有 `input`；`sqrt`/`pow` 由本移植实现（core 无 libm），
  `pow` 只接受整数指数；递归深度按 eval 层数限制（约 500 层函数调用），超出报
  `maximum recursion depth exceeded` 而不是撞穿栈；`reverse` 真正反转、`assoc`
  保持表序（C 版 `reverse` 实际不反转、`assoc` 会把表颠倒）。
- 资源上限（都在文件头的常量里）：对象 256 KiB（其中环境 128 KiB 可回收）、
  源码 6 KiB、单次调用 24 个实参、列表/向量字面量 64 个元素。
- 内核配合两处：`nuc_spawn_blob()` 的镜像缓冲 64 KiB → 128 KiB（nsh 带解释器后
  约 69 KiB，超了原来的上限会静默拒绝启动）；内建应用的栈由 64 KiB 提到
  `NUC_STACK_MAX` = 4 MiB——nsh 的滚动缓冲、Nuomi arena 和解释器递归本来就远超
  64 KiB，文件加载的 `.nuc` 仍按自身头部申请栈。
- 验证：在 host 上把 `nuomi.rs` 与 mock 的 `fs`/`shell`/`sys` 一起编译成测试程序，
  与 C 版（`user/Nuomi_pl` 源码编出的 `nuomi_ref`）对拍 `samples/` 的
  closures/factorial/fib/lists/logic/scope/stdlib/strings/strings2/vectors/while，
  除上面列的差异（深递归上限、`reverse`、`assoc` 表序）外输出逐字节相同；
  另覆盖了文件原语、`-e`、语法错误/未绑定变量/参数个数错误路径。`make` 全量
  构建通过。尚未做 QEMU 里点开终端的交互验证。

## 显示呈现重构：消除闪烁与撕裂，启动进度条

- **真实硬件花屏/卡顿的根因修复**：帧缓冲被当作可缓存(WB)内存访问。`nd_init()`
  此前对低 4 GiB 帧缓冲直接用了恒等虚拟地址，而恒等映射对低内存是 WB 写回；
  只有高 4 GiB 帧缓冲才走 `mm_map_phys(...,MM_MAP_UC)`（置 PCD=不可缓存）。
  QEMU(TCG) 不区分缓存属性所以模拟器里看不出问题，但真机上 CPU 把呈现写入
  留在缓存里、显示器做 UC 读取，看到的是陈旧/乱序的像素 = 花屏；大块 memcpy
  进缓存又让显示更新滞后 = 不流畅。现改为**主屏与镜像帧缓冲一律经
  `mm_map_phys(...,MM_MAP_UC)` 映射**（新增 `nd_map_framebuffer`，并补偿 2 MiB
  大页对齐的页内偏移），`nd_init()` 在映射后 `wbinvd` 冲刷掉启动阶段经 WB 恒等
  映射留下的陈旧缓存行，消除 WB/UC 别名。内核阶段的进度条也相应提前到 `nd_init()`
  之后绘制，使其落在不可缓存的帧缓冲上。
- **所见花屏/色偏的真正来源：壁纸 JPEG 解码器**。用宿主桩把 `graphics/nd_jpeg.c`
  与 `wp_cover()` 单独编译解码同一张包内 JPEG：宿主侧输出正常（平均 RGB≈71,129,174），
  但**内核侧解码同一条流水线得到平均≈0x2C 的偏暗/错误图像**（同一代码、同一数据，
  差异只在编译环境，属于工具链相关的未定义行为），于是桌面背景是错色/发黑即花屏。
  另定位到帧缓冲虽已按 UC 映射，但背景黑块并不是它造成的——强制走纯色回退即整屏
  正常。鉴于 920 行的自研解码器盲调风险高，桌面背景改为**程序化竖向渐变**
  （`nd_wallpaper_paint()` 用主题 `desktop_top→desktop_bottom` 逐行插值，
  `wp_lerp()`），保证恒为正确、干净的背景；照片壁纸的编解码保留，待该解码器修好
  再启用。已在 QEMU 截图验证：背景为平滑渐变、无黑块、通道未交换。
- **回扫等待收紧**：`ND_BLANK_SPINS` 定为 800（约 0.4 ms），`ND_BLANK_MIN_AREA`
  降到 0x20000（约 362x362）。阈值仍高于光标/图标尺寸，所以对延迟敏感的指针移动
  不等待；但常见窗口拖动重绘（数百平方像素）会落进消隐区，减少撕裂；该等待运行
  在关中断的定时器 tick 里，上限必须小，否则拖垮输入（"不流畅"）。
- 显示层由"直接画显存"改为 Windows 式合成再呈现：`nd_init()` 分配与主屏等大
  的离屏缓冲，`nd_canvas()` 是唯一绘制目标，`nd_present()` 把一次完成的矩形
  拷贝到屏幕。半成品帧不再出现在玻璃上——拖动窗口、切换主题、重建壳层都不会
  再闪。
- `nd_present()` 在拷贝前做有界垂直回扫等待：探测 VGA 状态口 `0x3DA` 的
  retrace 位（可用则 `[ND] compositor: ..., vsync available`），大块呈现尽量
  落在消隐区；等待上限约 1.2 ms，因为它运行在关中断的时钟 tick 里，等满一帧
  会拖垮输入。彻底免撕裂仍需硬件 flip 或 vblank 中断，代码与日志均如实说明。
- 呈现与镜像改为整行 `memcpy`（字节序一致时），`nd_blit()` 同样走该快路径：
  壁纸贴图与全屏呈现从逐像素循环变成逐行拷贝。
- 镜像输出改为从合成缓冲拷贝，不再从主屏显存回读——此前每次呈现会对第二块
  屏幕做数百万次 uncached MMIO 读，是双屏卡顿的主因。
- 鼠标指针从 save-under 改为合成覆盖层：`nd_cursor_set()` 只记录新旧位置
  damage，箭头在每次 present 末尾画到帧上。绘制前 hide、绘制后 draw 的"闪一下"
  没有了，保存的像素也不会过期（旧实现会在下方内容重绘后把它盖回去）。
  `nd_cursor_take_damage()` 把旧位置交给合成器重绘。
- `nd_demo_init()` 先把桌面画进合成缓冲再一次呈现，启动时不再出现半张桌面；
  `nd_demo_theme_sync()` 之前会把控件上下文重新绑到显存（主题切换后逐控件直画，
  即最明显的那次闪烁），已改为绑回合成缓冲。
- 新增 `graphics/nd_boot.c`：启动画面（深色渐变、N 标记、Novus OS 字样、圆角
  进度条与阶段文案）。状态放在 `.data`，因为 `kernel_entry` 会清 `.bss`；绘制
  只用渲染原语，不依赖堆，因此在 UEFI 阶段即可画。进度条只重绘自己的矩形。
- 启动阶段重排：`stage3_prepare()` 只做块设备探测与 ESP 打开，菜单之后由新的
  `stage3_load()` 读取负载，并在 `nd_capture_boot()` 之后切到图形启动画面
  （`boot_quiet()` 关掉文字控制台与固件光标，日志改走 COM1，不再与画面互相
  覆盖）。内核阶段继续同一条进度条：内存 → 页表 → 堆 → 设备 → 显示 → 文件
  系统 → 多核 → 会话 → 桌面，到 `nd_demo_init()` 呈现首帧时结束。
- 顺带修复 `kernel/shared_mem.h`：`wpkg_img_base/size` 的静态断言早已存在、字段
  却缺失，导致整个镜像无法编译；按既有布局补上（0x308/0x310，`reserved` 相应
  缩短 16 字节，块仍为 8 KiB）。
- 验证：`make` 通过（无新增告警）；`CONFIG_NOVUS_DISPLAY/WM/GUI=n` 时相关文件
  单独编译通过；QEMU q35 + OVMF 启动，串口可见
  `[ND] compositor: off-screen buffer, vsync available` 与
  `[ND] boot splash kept until the desktop presents`；截图确认进度条（86% 进入
  内核、96% 挂载文件系统）、桌面首帧、光标移动无残影。

## M9 Win32 兼容层原型

- 新增 `kernel/pe.h` / `kernel/pe.c`：PE32+（x86-64）加载器。按文件偏移读
  头部与节表，校验 MZ/PE 签名、Machine、Magic、节边界与重叠、入口所在节，
  拒绝 DLL、TLS、延迟导入与序号导入；在 `0x50000000` 映射头部、各节
  （`.bss` 清零）、thunk 页、命令行页、64 KiB 栈与返回跳板，应用
  `IMAGE_REL_BASED_DIR64`，按名称填 IAT，最后交给
  `sched_create_user_ex(..., TASK_ABI_WIN32)`。
- 新增 `kernel/win32.h` / `kernel/win32.c`：API 门与模拟注册表。15 个 API
  （控制台输出、tick、`Sleep`、错误码、命令行、模块基址、`Reg*`），每条
  thunk 保存并恢复 RDI 后以 `syscall 40` 进入；`Sleep` 走
  `sched_park` + `f->rip -= 2` 重放。注册表是 16 项扁平表，`REG_SZ`/
  `REG_DWORD`，持久化到 `/registry.cfg`，live 只读根下仅内存态。
- `task_t` 增加 `abi`、`win32_last_error`、`win32_image_base`；调度器新增
  `sched_create_user_ex`，`syscall_dispatch` 对 `TASK_ABI_WIN32` 只放行
  `SYS_WIN32` 与 `SYS_EXIT`。
- 新增样例 `user/win32compat/samples/hello.c`（不用 `<windows.h>` 与 CRT），
  `Makefile` 用 `x86_64-w64-mingw32-gcc` 生成 `out/win32/hello.exe` 并
  `xxd -i` 成 `build/win32_blobs.h`；新增 `CONFIG_NOVUS_WIN32`（默认 y）与
  `CONFIG_NOVUS_WIN32=y` 的 defconfig 项。
- sysinfo 新增 `/exe` 目录（`SYSINFO_DIR_EXE`、`SYSINFO_KIND_EXE`），文件
  管理器增加 `/exe` 标签，`KIND_EXE` 可选中运行。
- 验证：`.reloc` 存在且为 DIR64；导入表为 KERNEL32/ADVAPI32 的 9 个函数；
  QEMU 中从 `/exe` 运行 `hello.exe`，串口依次输出加载行、三条 Win32 输出
  与 `task hello.exe exited with code 0`；`CONFIG_NOVUS_WIN32=n` 仍可构建。
- 文档：新增 `docs/M9 Win32 兼容层原型.md`；README 里程碑 M9 标记完成，并把
  原有的 Win11 桌面/中文说明限定为「M9 的 Win11 桌面/中文部分」。

## M11 阶段 1：会话、双语与安装器

- live 介质启动直接进入 `live` 会话，不再要求用户名/密码；只有安装到可写
  NovusFS 后才进行首次账户创建。安装模式只启动 `setup`。
- 新增中英双语：`tools/langgen/strings*.toml` 生成
  `graphics/nd_lang_dict.c` 与 `apps/src/lang_dict.rs`；`nd_text` /
  `nd_text_width` 在渲染层翻译，`ui::width` 用同一张表测量。
- 新增 `SYSINFO_REQ_LANG (0x030F)` 与 `kernel/lang.c`：切换立即生效，
  已安装系统持久化到 `/language.cfg`，安装器把选择写入新系统盘。
- 安装器改为七页：语言、协议、磁盘、方案、确认、进度、完成/失败；新增 EULA、
  文件系统选择（NovusFS 可用，NTFS/ext4 标注"规划中"并由内核拒绝）。
- 新增 `kernel/usb.c`：xHCI 控制器（命令/事件环、端口复位、Enable Slot、
  Address Device、描述符、SET_CONFIGURATION、Configure Endpoint）与 BOT
  （CBW/CSW、READ CAPACITY、READ10/WRITE10），磁盘注册为 `usb00`。
- `kernel/pci.c` 支持 64 位 BAR：按 BAR 对读取基址与长度，新增
  `pci_bar_assign()`；固件把 xHCI 的 BAR0 放在 4 GiB 以上时，驱动把它搬进
  32 位 PCI 空洞并读回能力寄存器确认。
- 引导阶段从引导卷的设备路径解析 USB 端口并发布到共享内存；内核据此给
  正在启动的盘打 `BLKDEV_FLAG_BOOT_ONLY`，向导拒绝它作为目标。
- 新增 `docs/多语言、会话与 M11 输入输出.md`，记录已完成与未完成部分。
- 多显示器 / EDID / 镜像尚未实现，仍为单输出 GOP 路径。

## M11 阶段 1 续：多输出显示与窗口重绘

- `graphics/nd_hal.c` 枚举固件提供的全部 GOP 输出（上限 4 个），读取 EDID
  首选时序与数字/模拟链路，控制器支持时切到首选模式；第一个输出作主屏，
  其余输出镜像同一画面（`nd_mirror_rect()`，字节序不同时逐像素交换红蓝）。
- `nd_outputs()` 按"每个帧缓冲一个条目"报告，固件里指向同一显存的第二个
  头衔不再被当成一块显示器；设置 → 系统新增"显示输出"卡片。
- `kernel/sysinfo.c` 新增 `SYSINFO_REQ_DISPLAY (0x0318)` /
  `SYSINFO_RSP_DISPLAY (0x0319)`，客户端 `sinfo::display()`。
- 修复窗口重绘缺陷：CLEAR/SHAPE/TEXT 不再逐条触发重绘，改由帧尾 INVALIDATE
  一次性绘制——此前一帧 50 余条消息会让半帧状态闪现（关窗后下方窗口内容
  只剩一半）。
- `nd_selftest` 增加 3 项镜像字节序往返检查（37 项全过）。

## 验证

- 标准安装（AHCI 目标）端到端通过；英文安装通过；安装后磁盘启动读取
  `[LANG] loaded: en` 并进入首次账户设置；live 启动出现
  `[SESSION] live session: no account required`。
- USB：`[USB] BAR0 moved to 0x00000000F0000000`、`disk usb00 capacity
  0x0000000000100000 sectors`，安装到 U 盘 `begin on usb00` →
  `partition table written` → `ESP written` → `system image written` →
  `verify root ok` → `install complete`；从该 U 盘启动得到
  `[BOOT] live medium is on USB` 与 `is the boot medium`，Merkle 校验 PASS。
- 未创建 git commit。

## M10 UI 统一与图形安装向导

- 新增 Novus Fluent 令牌单一来源 `tools/themegen/theme.toml`，生成
  `graphics/nd_theme.[ch]` 与 `apps/src/theme.rs`；`make theme` 重新生成，
  支持浅色、深色、跟随时间（19:00–07:00 深色）三种模式。
- `nvgui` 的主题改由 `nd_tokens()` 填充；壳层在版本变化时重建控件树，窗口管理器
  标题栏按当前主题绘制；`sysinfo` 主题回复尾部追加 `mode/resolved/version`。
- Novus-WM 显示列表新增圆角填充/圆角描边（op 6/7，半径在 `pad`），
  `apps/src/ui.rs` 升级为组件库（page/card/nav/row/button/ghost/progress/empty）。
- 设置、文件管理器、任务管理器、网络与登录界面改用角色色并轮询主题版本；
  设置页增加主题模式选择、强调色与"打开安装程序"入口。
- 引导阶段把 `ESP.IMG` 暂存到 reserved pages，并在共享内存保留尾部发布
  `esp_img_base/esp_img_size/install_flags`；菜单 3 与 `INSTALL.YES` 改为设置
  `boot_mode=INSTALL` 后进入图形安装，文本安装保留在 `CONFIG_NOVUS_INSTALLER_TEXT`。
- 新增 `kernel/install.[ch]` 与 `SYS_SETUP=19`：候选盘判定、GPT、ESP/系统镜像
  分步复制、NovusFS 挂载与 Merkle 校验、setup pid 授权；`blkdev_release()` 归还
  分区视图槽位。
- 新增 `apps/src/install.rs` 与六页向导 `apps/src/bin/setup.rs`；应用表新增 `setup`，
  启动改为按名字解析（`apps_launch_name`、`SYSINFO_REQ_SPAWN_NAME`），
  新窗口创建即取得焦点。
- 新增 `scripts/run-install-test.sh`、`docs/Novus UI 设计语言规范 V1.0.md`、
  `docs/图形安装向导设计与实现.md`；重新生成中文字模与主题文件。
- 更正 README 中"SMP AB双核"为"SMP AP双核"。

## 验证

- 构建：`make` + `make iso` 通过（mingw 交叉 gcc，Rust 应用以 dev 用户与
  `~/.cargo` 构建）。
- 安装：QEMU q35 + OVMF，ISO 菜单选择 3 后 `setup` 启动，键盘走完六页；
  串口输出 `partition table written` → `ESP written` → `system image written` →
  `verify root ok` → `install complete`。
- 启动：从安装后的目标盘单独启动，`[MRKL] ... PASS`、`NovusFS mounted`、
  `launching session`。
- 主题：`-rtc base=localtime` 下 AUTO 解析为深色，桌面、任务栏、登录窗口同步切换。
- 修复：`nd_tokens()` 首次调用时懒构建调色板。此前浅色模式启动时 `g_current`
  全零，桌面被画成黑色（深色模式恰好因自动解析触发构建而正常）。
- 修复：瓦片标题统一用 1x 字号。2x 汉字每字 32 px，五个字的名称在 96 px 瓦片里
  会被裁掉。
- 修复：候选盘判定允许 Novus 自身布局（恰好 ESP + NovusFS 两分区）覆盖重装；
  失败页的"重试"因此可用，其他分区表仍拒绝。
- 未创建 git commit。

## cppcc M7 文档与构建集成

- 新增 `user/cppcc/README.md`，说明实际支持的 C++ 子集、构建/CLI 用法、syscall builtin 和已知限制。
- 新增 `docs/M7 cppcc 实现说明.md`，记录直接 AST 驱动后端、x86_64 codegen、NUC V1.0 字段映射与 boot-stage 加载路径。
- 修订 `docs/M7 cppcc 实施方案.md` 的状态和与实际 codegen 不同的 IR、栈、syscall 与 `_start` 描述；保留其历史规划用途。
- README 第 7 部分及顶层摘要改为 x86_64 原生机器码加载执行，并注明当前 fixed-path staging 与尚未实现的通用 shell exec。
- 将 M8、WM 文档中的 M7 暂缓语境标注为历史；NKM 规范明确 cppcc 输出 NUC、`nkmgen` 负责测试 NKM 镜像。
- 新增 `user/cppcc/examples/hello.cc`，调用 `novus_write` 输出 `Hello, Novus!`。
- Make/Kconfig 增加 `CONFIG_USER_CPPCC`；`make`/`make cppcc` 构建 host 工具，`make cppcc-demo` 生成 `out/cppcc/hello.nuc`。
- `make run` 在启动 `scripts/run.sh` 前将 smoke NUC staging 为 `esp/APPS/DEMO.NUC`；ESP 镜像规则打包同一路径。cppcc 启用时优先占用 boot-stage 固定 demo 槽。
- 更新 README 第 7 部分的执行模型，并将 M8/WM/NKM/M7 历史说明改为与当前实现一致；NUC V1.0 格式规范未修改。

## 验证

- cppcc Rust 文件静态诊断：无错误。
- 已添加 NUC 字节布局、Hello 生成、控制流/函数调用以及语法错误行列测试。
- 本机缺少 `cargo`、`rustc`、`make`、MinGW、`qemu-system-x86_64` 和 `xxd`；`cargo test`、实产物字节检查和 QEMU 启动未执行。
- 仓库未发现 `nucdump`。
- 未创建 git commit。