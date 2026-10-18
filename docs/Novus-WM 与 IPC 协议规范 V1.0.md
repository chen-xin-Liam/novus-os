# Novus-WM 与 IPC 协议规范 V1.0

对应里程碑 M6（README 第 10 部分、第 19.1 节）。本文档描述已经实现并运行的
那一套协议：内核 IPC 子系统、系统调用 ABI，以及 Novus-WM 的窗口协议。
实现位于 `kernel/ipc.c`、`wm/novus-wm/`、`graphics/nd_demo.c` 与
`sched.c` 的 tick 钩子。

---

## 1. 分层

```
ring 3 客户端（例如 wmclient.nuc）
        │  syscall: ipc_send / ipc_recv / ipc_lookup
        ▼
内核 IPC（kernel/ipc.c）：端口表、有界队列、校验和、阻塞/唤醒
        ▲
        │  同一套 API，内核内调用
Novus-WM（wm/novus-wm/nwm.c）：窗口表、Z 序、焦点、输入路由、合成器
        ▼
NovusDisplay（graphics/）：帧缓冲 HAL、2D 渲染器、鼠标指针
```

WM 作为“合成器线程”运行在内核里，与驱动同地址空间；对客户端而言，
唯一的边界就是 IPC。这样 M6 只需要定义一套协议，客户端不需要任何
共享内存或内核头文件。

---

## 2. 系统调用 ABI

入口：x86_64 `syscall` 指令（MSR_LSTAR → `syscall_entry`）。
参数：`rax`=调用号，`rdi`、`rsi`、`rdx`、`r10`；返回值在 `rax`，
负值表示错误码（第 3 节）。调用号集中在 `kernel/trap.c` 的
`g_syscalls[]` 表里，新增调用只是新增一行。

| 号 | 名称 | 参数 | 返回 |
|----|------|------|------|
| 1 | yield | — | 0 |
| 2 | write | rdi=fd, rsi=buf, rdx=len | 写入字节数 |
| 3 | getpid | — | 本进程 pid（同时是自己的收件端口号） |
| 4 | ipc_send | rdi=端口, rsi=类型\|标志<<16, rdx=载荷, r10=长度 | 0 或负错误码 |
| 5 | ipc_recv | rdi=端口（0=自己的收件箱）, rsi=缓冲, rdx=容量, r10=标志 | 载荷长度或负错误码 |
| 6 | ipc_lookup | rdi=名字字符串 | 端口号或 `IPC_ENOENT` |
| 7 | ipc_stats | rdi 保留, rsi=缓冲, rdx=容量 | 写入的 `ipc_stats_t` 字节数 |
| 60 | exit | rdi=退出码 | 不返回 |

### 2.1 阻塞接收

`ipc_recv` 的标志含 `IPC_F_BLOCK` 且队列为空时，当前任务被挂起：

1. 任务状态置 `TASK_BLOCKED`，记录 `wait_port` 与超时（500 tick ≈ 5 s）；
2. **返回帧的 RIP 回退 2 字节**（`syscall` 指令长度），`rax` 保持不变；
3. 调度器在环出中断时切换到别的任务；
4. 消息到达时 `ipc_send` 唤醒该任务，它重新执行同一条 `syscall`，
   这次拿到消息；超时则以 `IPC_ETIMEDOUT` 返回。

因此阻塞语义不需要“续延状态”，只有一次重放。参数寄存器在帧里，
重放时无需重新装载。

---

## 3. IPC 消息

### 3.1 头部（32 字节，`ipc_hdr_t`）

| 偏移 | 字段 | 说明 |
|------|------|------|
| 0 | magic | `0x4E495043`（"NIPC"） |
| 4 | type | 16 位消息类型（见第 5 节） |
| 6 | flags | `IPC_F_SYNC`、`IPC_F_REPLY` |
| 8 | sender | 发送者 pid，**由内核填写**，客户端无法伪造 |
| 16 | target | 目的端口号，由内核填写 |
| 24 | len | 载荷长度，由内核填写 |
| 28 | checksum | FNV-1a over 头部（不含本字段）+ 载荷 |

载荷上限 `IPC_PAYLOAD_MAX` = 128 字节；每个端口队列
`IPC_QUEUE_LEN` = 8 条，满时 `ipc_send` 返回 `IPC_EBUSY`（消息洪水
防护，README 10.3）。`ipc_recv` 交付前重算校验和，坏条目被丢弃并计数，
不会投递给应用。

### 3.2 端口

* 端口表静态：`IPC_MAX_PORTS` = 16，创建/销毁不分配内存；
* 每个任务创建时自动获得一个**收件箱**，端口号等于 pid，
  所以服务端用 `hdr.sender` 直接回复即可；
* 命名端口（如 `"nwm"`）由 `ipc_port_create` 注册，号段从
  `IPC_PORT_BASE`(0x1000) 起，不会与 pid 冲突；
* 进程退出时它拥有的端口全部回收，挂在其上的等待者由超时唤醒。

### 3.3 错误码

| 值 | 名称 | 含义 |
|----|------|------|
| 0 | `IPC_OK` | 成功 |
| -1 | `IPC_EBADF` | 端口/缓冲区非法 |
| -2 | `IPC_EBADMSG` | 魔数、长度或校验和错误 |
| -3 | `IPC_EMSGSIZE` | 载荷过长（>128） |
| -4 | `IPC_EBUSY` | 队列满 |
| -5 | `IPC_ENOENT` | 端口不存在 |
| -6 | `IPC_EAGAIN` | 非阻塞且无消息 |
| -8 | `IPC_ETIMEDOUT` | 阻塞超时 |

---

## 4. Novus-WM 窗口协议

端口名 `"nwm"`，协议版本 `0x00010000`（1.0）。请求由客户端发往该端口，
应答发往 `hdr.sender` 的收件箱，事件发往窗口属主。

### 4.1 请求

| 类型 | 名称 | 载荷 | 说明 |
|------|------|------|------|
| 0x0101 | CREATE | `nwm_create_t` | 建窗，`win=0` 表示由 WM 分配；**总是回执**窗口号 |
| 0x0102 | DESTROY | `nwm_win_t` | 销毁窗口 |
| 0x0103 | MOVE | `nwm_move_t` | 移动，超出屏幕自动收敛回可见区 |
| 0x0104 | RESIZE | `nwm_resize_t` | 改尺寸，受最小尺寸约束，回发 `EV_RESIZE` |
| 0x0105 | RAISE | `nwm_win_t` | 置顶 |
| 0x0106 | MINIMIZE | `nwm_win_t` | 最小化切换（不再参与命中与绘制） |
| 0x0107 | MAXIMIZE | `nwm_win_t` | 最大化切换（记住还原矩形） |
| 0x0108 | RESTORE | `nwm_win_t` | 还原 |
| 0x0109 | FOCUS | `nwm_win_t` | 置顶并取得焦点 |
| 0x010A | DRAW | `nwm_draw_t` | 提交绘制命令列表（≤7 条） |
| 0x010B | TITLE | `nwm_title_t` | 改标题 |
| 0x010C | EFFECT | `nwm_effect_t` | 内容透明度（窗口淡出特效） |
| 0x010D | CLEAR | `nwm_win_t` | 清空该窗口的形状/文本列表（v1.1，M8） |
| 0x010E | SHAPE | `nwm_shape_t` | 追加一个图元：填充/边框/横线/竖线/圆 |
| 0x010F | TEXT | `nwm_text_t` | 追加一段文字（≤96 字节，scale 1/2） |
| 0x0110 | INVALIDATE | `nwm_win_t` | 用完整显示列表重绘整个客户区（帧尾调用） |

`nwm_cmd_t`（16 字节）：`op`（1 填充 / 2 边框 / 3 横线 / 4 竖线 /
5 圆）、`x,y,w,h`、`color`；坐标相对客户区左上角。未知 `op` 被忽略，
便于协议向前兼容。

v1.1 起窗口内容是一份**显示列表**：CLEAR 开一帧，SHAPE/TEXT 逐条追加，
WM 在每次重绘时按顺序回放并裁剪到客户区。列表上限每窗 28 个图元、
40 段文字；满了返回 `NWM_EFULL`。这样一屏列表只需 20 多条消息，且客户端
不需要任何共享内存 —— 它仍然只能画在自己的窗口里。

### 4.2 应答

| 类型 | 名称 | 载荷 |
|------|------|------|
| 0x0001 | RSP_OK | `nwm_rsp_t{win, 0}` |
| 0x0002 | RSP_ERR | `nwm_rsp_t{win, 负错误码}` |

**回执策略**：异步请求成功时不回执——否则一个只读事件的应用会在自己的
收件箱里堆满对自己 MOVE 的应答，把真正的事件挤掉（队列只有 8 条）。
CREATE 必须回执（窗口号是结果）；任何错误也总是回执。请求可置
`IPC_F_SYNC` 强制要回执，这就是 README 10.3 的同步/异步两种模式。

### 4.3 事件

| 类型 | 名称 | 载荷 | 触发 |
|------|------|------|------|
| 0x0201 | EV_CREATED | `nwm_rsp_t` | 窗口建立 |
| 0x0202 | EV_MOUSE | `nwm_ev_mouse_t` | 指针在客户区内移动/按下/抬起（客户区坐标） |
| 0x0203 | EV_KEY | `nwm_ev_key_t` | 窗口持有焦点时的按键 |
| 0x0204 | EV_FOCUS | `nwm_ev_focus_t` | 焦点进入/离开 |
| 0x0205 | EV_RESIZE | `nwm_ev_resize_t` | 尺寸变化（客户区大小） |
| 0x0206 | EV_CLOSE | `nwm_win_t` | 用户点了标题栏关闭按钮 |

### 4.4 错误码

`NWM_OK`=0、`NWM_EBADWIN`=-1、`NWM_EBADREQ`=-2、`NWM_EFULL`=-3、
`NWM_EDENIED`=-4、`NWM_EBADPAY`=-5。

---

## 5. 窗口模型与合成

* **Z 序**：`g_z[]` 自底向上；点击/置顶把窗口移到末尾；
* **装饰**：24 px 标题栏（标题、最小化/最大化/关闭三个 12×12 按钮），
  1 px 边框，6 px 阴影；按下标题栏拖动，抓取点偏移保持不变；
* **裁剪**：装饰与内容分别先 `nd_surface_set_clip` 再绘制，
  客户端命令只能落在自己的客户区内（README 10.4 的窗口裁剪）；
* **双缓冲**：合成目标是屏幕大小的后备缓冲，一次 blit 的只有损伤矩形。
  直接画进帧缓冲会出现“先擦掉窗口、几毫秒后再画回来”的中间帧，
  双缓冲后屏幕只出现完整帧；
* **损伤重绘**：窗口/焦点/内容变化只把受影响矩形（含阴影外扩）
  并进损伤区，每个 tick 最多合成一次；
* **属主存活**：每 40 tick 检查一次窗口属主是否还在，进程没了窗口即关闭。

---

## 6. 一个 ring-3 客户端的时序

M6 开发时尚无用户态编译器，因此示例客户端 `wmclient` 使用手写机器码；M7 cppcc 后续交付，但该示例仍保留手写实现，
在 `wm/novus-wm/nwm_demo.c` 里，走完整套协议：

```
getpid()                        → 自己的收件箱端口
ipc_lookup("nwm")               → WM 端口
ipc_send(CREATE, 280x150@560,140)  → 请求窗口
ipc_recv(block)                 → 阻塞等回执，取回窗口号
ipc_send(DRAW, 3 条命令)          → 标题条 + 彩色条 + 边框
loop:
  ipc_send(MOVE, x += 24)       → 窗口向右走，到头回到 560
  ipc_recv(不阻塞)               → 有事件？改一下彩色条并重发 DRAW
  yield                         → 按调度片节奏推进
```

启动日志对应：

```
[NWM] compositor: double buffered
[NWM] Novus-WM up: protocol 0x0000000000010000, port 0x1002, max 8 windows
[NWM] selftest: PASS
[NWM] ring-3 client started, it will ask for a window over IPC
[NWM] create win=0x3 from pid=0x4
[NWM] event 0x202 -> pid 0x4        (指针进入窗口)
```

---

## 7. 验证点

1. `[IPC] selftest`：端口创建/查找、往返、头部字段、长度上限、队列满、
   校验和损坏丢弃、未知端口；
2. `[NWM] selftest`：两窗口重叠的 Z 序与命中、移动、绘制列表、特效、
   最小尺寸收敛、越权修改被拒（`NWM_EDENIED`）、销毁、未知请求；
3. 交互：标题栏拖动位移与指针位移一致；关闭按钮 → `EV_CLOSE` + 销毁；
   客户区内点击 → `EV_MOUSE` → 客户端重绘；客户区外点击 → 交由 shell
   （M5 控件仍可正常点击）；
4. 暂停虚拟机连续截图，窗口始终完整（双缓冲，无中间帧）；
5. ISO 从 CD-ROM 启动直达桌面并同样可交互。
