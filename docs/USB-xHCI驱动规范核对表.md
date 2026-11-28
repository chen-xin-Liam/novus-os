# USB / xHCI 驱动规范核对表

> 历史说明：全屏启动诊断页及其 `diag_strip()` / `diag_report()` / `usb_banner()` / `usb_port_dump()` 等辅助代码已移除；文中 U-50/U-61 相关段落仅作历史记录，不再代表现行代码。

本文是 `kernel/usb.c` 的**唯一改动依据**。目的不是记录已经修过什么，而是把 xHCI 1.1 规范里每一条会决定"设备到底能不能被认出来"的要求，落到具体代码位置和具体验证手段上。

## 使用纪律

1. 任何 USB 相关改动，先在本表里找到对应条款，改完回填"现状"和"验证"。找不到条款的改动，先补条款再改代码。
2. 一条改动只对应一条根因。同一轮里不允许出现"顺手调一下"式的第二处修改；确实需要多处时，逐个条款分开写。
3. **QEMU 通过不等于真机通过**。QEMU 的 xHCI 是宽松实现，缺失必填字段、事件环溢满、控制器停机都不会报错。凡是在真机上失败而在 QEMU 上正常的现象，一律按"本驱动偏离规范"处理，而不是"硬件特殊"。
4. 表内"未验证"必须写明缺什么证据，不能改写成"已支持"。

## 规范基线

- xHCI 规范 1.1：第 3 章（寄存器）、4.6（命令）、4.7（事件）、4.8（端口状态机）、6.2（TRB）、6.4（设备上下文）。
- 语义参考 Linux `drivers/usb/host/xhci*.c` 与 Intel 13 代处理器数据手册的 PORTSC 位域表；**只取语义，代码自行改写**，不整段搬运。
- 本项目当前实现面向**轮询**（不使用 xHCI 中断），这一点在条款 U-16 里单独说明。

## 条款

### U-1 物理地址映射（上游依赖）

- 要求：控制器 MMIO 与 DMA 缓冲的物理地址必须是完整 64 位；BAR 在 4 GiB 以上时不得截断。
- 位置：`kernel/mm.c` `mm_map_phys()`。
- 现状：已修。原实现用 32 位常量 `~0x1FFFFFu` 做页对齐掩码，把 >4 GiB 的地址位全部清零，映射到物理 0；两台控制器的寄存器窗口因此都读成了低内存页表项。
- 验证：QEMU 日志里同一窗口修复前读到 `hcs1=0 / hcc=0x40400083`（周期 2 MiB 的页表值），修复后读到 `hcs1=08001040 / hcc=00087001`；真机两台控制器的 BAR 由"完全相同"变为 `1d170000 / 1d160000`。

### U-2 BAR 处理

- 要求：固件已经分配的 BAR 直接注册，不做重定位；只有固件完全没分配（`bar64 == 0`）时才由操作系统分配，且必须落在总线实际解码的窗口内。
- 位置：`usb_init()` BAR 选择段、`relocate_bar0()`、`xhci_cap_sane()`。
- 现状：已改。旧实现把 >4 GiB 的 BAR 搬到自选的 32 位地址，真机上那不在 PCIe aperture 内，每次寄存器读都是 bus abort，叠加 4 个 8,000,000 次的等待循环，表现为"卡在初始化堆"。现在能力寄存器不合规时直接跳过该控制器并打印窗口地址，不再改写 BAR。
- 验证：QEMU 两台控制器窗口 `0x800004000`，无重定位，端口 5/6 正常枚举。

### U-3 命令环与命令完成事件

- 要求：命令完成事件按 TRB Pointer[63:4] 回指被完成的命令；不得只按事件类型匹配。
- 位置：`cmd_submit()` / `wait_cmd()`。
- 现状：已改。旧实现只比较事件类型，上一轮超时的命令的迟到完成会被当作下一条命令的完成，拿到的 slot id 写进 DCBAA，后续设备的 setup 落到不存在的 slot 上。现在不匹配的事件按 stale 计数丢弃并继续等待。
- 验证：QEMU 三种场景无 stale 记录；真机待验（报告页 `ec=` 若有 `T` 需结合日志判断）。

### U-4 事件环与端口变更事件

- 要求：Event Ring Segment 按 ERST 描述；ERDP 在消费后前移并写 EHB 清除；Port Status Change Event 必须应答端口变更位，否则控制器持续产生事件直到事件环满，随后丢弃真正需要的完成事件。
- 位置：`controller_start()`、`event_pop()`、`xhci_port_changes()`。
- 现状：已改。新增 `xhci_port_changes()`，`wait_cmd` / `wait_transfer` 在循环里遇到 PSCEG 就应答。
- 验证：QEMU 三种场景通过；真机待验。

### U-5 控制传输 TD

- 要求：setup / data / status 三段属于同一 TD，setup 与 data 置 Chain（bit 4）；setup TRB 的 TRT（bit 17:16）必须与实际数据阶段一致（0 无数据、2 OUT、3 IN），DIR（data/status bit 16）随之确定。
- 位置：`control_xfer()`。
- 现状：已改。旧实现三个字段全缺，QEMU 接受而 Intel 控制器判定 TD 非法，直接停机。
- 验证：QEMU 枚举鼠标/键盘/存储/空载四场景通过；真机待验。

### U-6 命令与传输的超时预算

- 要求：等待预算必须覆盖真实设备的响应时间（控制传输毫秒级、中断端点按 bInterval）。
- 位置：`usb_pace()`，被 `wait_cmd` / `wait_transfer` 每 64 次迭代调用一次。
- 现状：已改。原来 8,000,000 次纯内存轮询只有几十毫秒，短于一次真实的控制传输。
- 验证：QEMU 三场景无回归。成功路径每次只多一次 port-80 读，不影响启动时间；失败路径的等待上限从几十毫秒提高到约一秒，这是**故意**的。

### U-7 控制器停机自愈

- 要求：USBSTS.HCH 置位表示控制器停止。**恢复必须走完整流程**：停 RS → 清 HCH → HCRST → 重新编程 DCBAA / CRCR / ERST / ERDP / CONFIG → 置 RS。只清 HCH 再置 RS 不够，因为命令环仍停在出错的位置，下一条命令不会完成。
- 位置：`controller_program()`（从 `controller_start()` 拆出）、`xhci_hc_reset()`，`wait_cmd` / `wait_transfer` 超时路径。
- 现状：已按上条重做。旧版 `xhci_halt_clear()` 只做"清 HCH + 置 RS"，真机上 `ht=1` 之后其余 4 口 ENABLE_SLOT 仍然全部 `T`，证明局部自愈无效。现在整控制器复位，并把该控制器上已 attach 的设备标记为 gone（slot 已不存在）。报告页 `ht=` 记录次数。
- 验证：QEMU `ht=0`（不触发）；**新的完整复位路径在真机上未验证**——这正是本轮要拍的数字。

### U-14 标准请求的字段放置

- 要求：setup 包八字节布局固定为 `bmRequestType, bRequest, wValue(lo,hi), wIndex(lo,hi), wLength(lo,hi)`。GET_DESCRIPTOR 的**描述符类型在 wValue 高字节**（`setup[3]`），低字节是索引；把类型写进 `setup[2]` 等于请求"类型 0"，设备一律 STALL。
- 位置：`attach_base()` 读设备描述符、读配置描述符；`hid_read_descriptor()`；`hid_set_protocol` / `hid_set_leds` / `hid_get_led_report`。
- 现状：已修。设备描述符 8 字节读取原本写成 `setup[2] = 0x01`，QEMU 串口直接报 `usb_desc_get_descriptor: unknown type 0 (len 8)`，完成码 6（STALL）。改为 `setup[3] = 0x01` 后同一镜像 QEMU 枚举鼠标、键盘、存储全部成功。
- 验证：QEMU 三场景 `at=` 到 9、`ec=` 无失败；真机待验。


### U-8 Slot 建立与 EP0 数据包长度

- 要求：Address Device 先以 8 字节 EP0 建立（全速/低速），读设备描述符第 8 字节得到 `bMaxPacketSize0`，不一致时 DISABLE_SLOT 后重新以真实值 Address；高速固定 64、超速固定 512。
- 位置：`enable_slot()`、`disable_slot()`、`address_device()`、`attach_base()`。
- 现状：已改。旧实现按速度硬推 8/64/512，全速设备若声明 16/32/64，后续所有描述符读取都用错的包长。
- 验证：QEMU 全速设备（`bMaxPacketSize0 = 8`）走不重试分支；非 8 分支**未在真实设备上验证**，需一台 EP0 为 16/32/64 的全速设备。

### U-9 端口状态机与复位

- 要求：PORTSC 写入只带"写 1 生效"的位（PR、PP、变更位、LWS+PLS）；PED 是 RW1CS（写 1 = 禁用），PP 是电源开关，整字回写会禁用并断电端口；请求复位时应同一次写入预清 PRC，否则上一次的 PRC 会被读成本次复位完成。
- 位置：`port_power()`、`port_reset()`。
- 现状：已修。两处旧写法（整字回写、只写 PR 不清变更位）分别造成"自己发现设备又自己断电"和"读到陈旧 PRC 就往下走"。
- 验证：真机 `cn=5`（5 个口全部 link）、`rs=0303300330333333`，端口状态从 `0x0080`（PLS=4 Disabled + PP=0）恢复为正常值。

### U-10 设备上下文与端点上下文

- 要求：Input Context 首 32 字节为控制上下文，Slot Context 紧随其后，端点上下文按 CSZ 步进；端点数组下标 = DCI-1，A-flag 与 Context Entries 用 DCI；EP Type 位于 DW1 bit 5:3、MaxPacket 位于 bit 31:16。
- 位置：`address_device()`、`configure_endpoints()`、`hid_configure()`。
- 现状：已符合。此前"下标用 DCI 直接写"和"EP Type 与 CErr 写反"两处已按本条款改正。
- 验证：QEMU 鼠标、键盘、存储全部到位（`kind=mouse` / `keyboard` / `usb00`）。

### U-11 HID 接口识别

- 要求：按接口描述符的 class/subclass/protocol 判定 HID 与键盘/鼠标，端点归属必须落在所属接口内；协议为 0 的接口仍需读报告描述符判定用途。
- 位置：`parse_config()`、`parse_config_iface()`、`hid_read_descriptor()`、`attach_hid_interface()`。
- 现状：符合，但 `parse_config()` 仍是"首个匹配"式扫描，复合设备靠 `cfg_hid_iface` 二次按接口号遍历补偿。**这是结构性技术债**，不影响单接口设备。第十一轮补上了一个真正的违背：`xhci_attach_port()` 的 step 7 判据曾要求 `hid_kind != 0`，于是协议字节为 0 的接口在读描述符之前就被丢掉（见 U-28）。
- 验证：QEMU 复合设备（键盘+鼠标各一接口）两半分别就绪。

### U-12 报告页取证契约

`usb_port_dump()` 每台控制器输出：

| 字段 | 含义 |
| --- | --- |
| `xh` / `b=` / `id` / `cl` / `h1` / `mp` / `pt` / `hcc` / `bar` | 控制器身份与能力，`bar` 必须两台不同 |
| `sp=` | 支持协议表：`族@首口+数量` |
| `spa=` / `n=` | Scratchpad Buffer Array 的物理地址（`DCBAA[0]` 指向它）与条目数。`spa=00000000` = 控制器取表会打到物理 0，对应 `sts` 的 bit2 |
| `psc=` | 报告时刻实时 PORTSC（低 16 位） |
| `pre=` / `pr2=` | 复位前的 PORTSC 快照，判定"原本就坏"还是"我们改坏" |
| `rs=` | 每口复位结果：0 成功、1 被拒、3 无设备 |
| `at=` | 枚举阶段：1 ENABLE_SLOT、2 ADDRESS_DEVICE、3 GET_DESCRIPTOR(DEVICE, 8)（含全速/低速的 EP0 包长复查与重寻址）、4 配置描述符头 9 字节、5 完整配置、6 parse_config + SET_CONFIGURATION、7 HID 接口未解析、8 中断端点未挂上、9 就绪 |
| `ec=` | 失败原因：十六进制完成码，`T` 未返回，`o` 本驱动拒绝描述符，`.` 无失败 |
| `chk=` | 控制器自检四位：bit0 运行中、bit1 CRCR 落在本驱动的命令环区间内、bit2 ERDP 落在事件环段内、bit3 CONFIG.MaxSlotsEn 等于写入值。`f` = 四条全部成立 |
| `ev=` / `sts=` | 上一次枚举失败瞬间的事件环位置（索引/周期）与 USBSTS。事件索引没有前移即控制器压根没写事件，可以排除"事件到了但匹配错了"；`sts` 的 bit0 = 停机、bit12 = HSE |
| `ev0=` / `sts0=` | 同两项在**本次启动第一次失败**瞬间的快照。第一个放弃的设备才是拖垮总线的那个，最后一个只反映残局；`ev0=ff/1` 表示本次启动从未失败过 |

banner 字段：`cn=` link 数、`msd=` U 盘数、`mi=` 鼠标数、`ht=` 停机自愈次数。

### U-15 Scratchpad 缓冲

- 要求：HCSPARAMS2 的 Scratchpad 计数（bits 25:21 高位 + bits 7:4 低位）若不为 0，软件必须在启动控制器**之前**准备好这些页，且**只能通过一级间接发布**——`DCBAA[0]` 指向一个"Scratchpad Buffer Array"（连续的一页 64 位表），表内第 i 项指向第 i 个 scratchpad 页（xHCI 4.6.4 / 6.1；Linux `xhci-mem.c: scratchpad_alloc()` 就是 `ctx_array[0] = sp_dma`，再逐项写 `sp_array[i] = page dma`）。DCBAA 的槽项是 `1 .. MaxSlotsEn`，`DCBAA[0]` 从不属于任何 slot，专门留给这张表。
- 位置：`usb_init()` 分配段、`xhci_t` 的 `scratch_arr / scratch_pages` 字段、报告页 `spa=` / `n=`。
- 现状：**两轮**。第一轮只把页地址直接写进 `DCBAA[MaxSlotsEn .. ]`，属于把二级表当成槽项写——`DCBAA[0]` 仍是 0，控制器去物理 0 取 scratchpad 表，这正是 USBSTS.HSE（bit2）的来源，而 HSE 一旦发生控制器就停机、此后任何传输都不会产生事件。本轮改为 `DCBAA[0] = scratch_arr_phys`，表内逐项填页地址，并在置 RS 之前完成。
- 验证：QEMU `h2=0000000f` → 计数 0，不分配（该路径在 QEMU 覆盖不到），鼠标键盘无回归；真机需看 `spa=` 非零、`n=25`（37 十进制 = 0x25）与 `sts` 的 bit2 是否消失。**真机尚未复验。**
- 未做：Linux 在 SPR（HCS2 bit26，本机为 1）置位时还会为"scratchpad restore"保留同一张表的内容语义；本驱动只保证控制器首次取表可用，不做内容恢复/copyback。

### U-17 TD 首 TRB 的所有权发布顺序

- 要求：TRB 的 cycle 位就是该 TRB 对控制器的"所有权"位，而控制器可以**预取** dequeue 之后的 TRB。多 TRB 的 TD（setup + data + status，或 bulk 多段）必须先把除首 TRB 以外的所有 TRB 写进内存，**最后**才翻转首 TRB 的 cycle 位，然后 fence、敲门铃。否则控制器可能先取到"已归属、且 Chain=1"的 setup TRB，再去找同一 TD 的后续 TRB——那些还没归属，于是它等一个永远不完整的 TD，既不报错也不停机。Linux 为此把首 TRB 的 cycle 放在 `giveback_first_trb()` 里，在所有 TRB 之后、敲门铃之前写（`xhci-ring.c`，`xhci_queue_bulk_tx` 里也留有 "Don't change the cycle bit of the first TRB until later" 的注释）。
- 位置：`control_xfer()`（setup TRB 先以 `start_cycle ^ 1` 写入，status 写完后 `mfence` 再翻转 bit0，再 `mfence` 才 `db[slot] = 1`）。
- 现状：本轮新增。旧实现 setup TRB 一写入就带正确 cycle 且带 Chain，data/status 随后才写；单 TRB 传输（bulk、HID 中断、命令环）不受影响，只有控制传输这一个多 TRB TD 有该暴露窗口。
- 验证：QEMU `td0=00030851/00010c11/00001021` 与改动前逐字相同（发布顺序改变不影响最终字面），`mi=1 kb=1`、`at=00009900`、`evs=02/06/0e` 无回归；真机待拍。

### U-16 报告字段补充（本轮取证）

- `evs=` 事件构成（端口变更/命令完成/传输事件三个计数）、`hch=` 在任何恢复动作**之前**采样的 USBSTS.Halt 位、`td0=` 首次控制 TD 的三个控制字。
- `dc0=` 控制器自己写回的设备上下文（slot DW0 / EP0 DW0 / EP0 DW1 / EP0 出队低 32 位 / slot DW3），配 `ep0lo=`（我方交给它的 EP0 环物理低 32 位）。
- `dbo=` DBOFF 原始值、`sl=` 控制器分配的 slot 号、`h2=` HCSPARAMS2。
- `dq0=` / `es0=`：第一次失败瞬间从控制器输出上下文读出的 EP0 dequeue 低 32 位与 EP0 状态（1 = Running）。`dq0 == ep0lo|1` 表示一个 TRB 都没被消费；`es0 != 1` 表示门铃本就会被忽略（端点非 Running）。注意规范只说"TD 完成后才更新 dequeue"，所以 `dq0` 不前移既可能是没取指、也可能是取了但事务一直没结束（设备不回应）——这也是为什么本轮改用 `rz=` 从设备一侧做二分，而不是继续在控制器侧猜。
- `rec=` / `rcf=`：走 U-18 恢复（按状态选端点命令 + Set TR Deq）后重试成功的次数 / 恢复本身失败的次数。两者之和 = 一次事件都没回来的控制传输数；`rec=00` 而 `ec=T` 时先看 `rcf`，否则会被"恢复没做"和"恢复做了但被拒"混为一谈。
- `rty=`：走 U-20 整端口重试（重新复位 + 重新寻址）的端口数。非 0 说明首轮 bring-up 被判无效。
- `rz=`：走 U-22 地址 0 探针的结果。`ff` = 地址 0 也没有事件（传输通道不通）；`ee` = 探针前的命令失败；`00` = 设备在地址 0 应答（问题在 SET_ADDRESS 之后）；其它 = 该完成码。
- `rce=` / `rsv=` / `rde=`：恢复用的端点命令 / 该码属于哪条命令（`0` Reset Endpoint、`1` Stop Endpoint、`2` 没发）/ Set TR Deq 的完成码。`00` = 已发出并被接受（或未发出），`ff` = 命令超时，其它 = 控制器给的码——例如 `13` = 19 = Context State Error，即这条命令对 EP0 当前状态不合法。
- `wms=`：最后一次失败等待实际耗时（毫秒）。等于 `USB_WAIT_MS` 预算 = 设备真的没应答；远小于预算 = 等待提前退出；`fffe` = 撞上迭代上限（机器没有可用时钟），不是一个时长。
- 判读纪律：`dc0` 后四项必须与 `ep0lo` 相符（出队 = `ep0lo | 1`）；不相符就是上下文层被拒，相符而 `evs` 传输计数仍为 0，则问题在 doorbell/环取指，不再往描述符方向猜。`sts` 的 bit2（HSE）单独看：HSE 必然意味着控制器对某个我们给的地址做过 DMA，先查 `spa=`。

### U-18 端点错误恢复

- 要求：一次传输"没有任何完成事件"时，端点内部可能留有半途事务，**再次敲门铃不是规范定义的恢复手段**。定义的做法是一道端点命令（按端点状态选，见 U-24）加 **Set TR Dequeue Pointer**（类型 16，参数 = 环首物理地址 | DCS，EP ID = DCI 放在 bits 20:16）。Linux 在 `xhci_urb_dequeue` 里发 Stop Endpoint，`xhci_handle_halted_endpoint` 里才发 Reset Endpoint。另外 USB 2.0 9.2.6.3 要求 SET_ADDRESS 之后留恢复间隔再发下一个请求。
- 位置：`control_xfer()`（首次超时 → `ep0_recover()` → 重试一次）、`cmd_submit_ep()`（带 EP ID 的命令提交）、`ep0_recover()`。
- 现状：第六轮新增；SET_ADDRESS 之后的恢复间隔由 U-19 的 `usb_delay_ms(10)` 显式给出。**第十轮更正**：原文把"Reset Endpoint + Set TR Dequeue"写成无条件的两步，那是错的——Reset 只对 Halted 端点合法，本机因此十四次全被拒（`rce=13`，完成码 19 Context State Error）。命令选择已按端点状态重写，见 U-24。
- 验证：QEMU 成功路径不触发（`rec=00`），鼠标+键盘无回归；真机待拍 `rec=` 与 `at=` 是否改变。
- 未做：bulk / 中断端点在超时后的同类恢复（当前存储与 HID 在读失败时直接放弃该设备）。

### U-19 时间基与两段恢复间隔

- 要求：USB 2.0 7.1.7.3 规定端口复位结束后必须留 **TRSTRCY（10 ms）** 才允许发第一个请求；9.2.6.3 规定 SET_ADDRESS 之后还要一段恢复间隔。Linux 的对应实现是：`hub_port_wait_reset()` 复位后按 `HUB_ROOT_RESET_TIME`（根口 60 ms）/`HUB_SHORT_RESET_TIME`（10 ms）步长轮询 PORTSC，即复位后至少有 10 ms 量级的间隔才去读设备；`hub_port_init()` 在请求 SET_ADDRESS 之前还有一句 `msleep(10)`，注释 "let SET_ADDRESS settle, some device hardware wants it"。本驱动在此之前**一处都没等**：读到 PED 置位就在同一微秒里 Address Device，等于让设备在复位恢复期内收包。
- 时间基：此刻没有时钟驱动，唯一可用的是 ACPI FADT 的 PM Timer（3.579545 MHz、24 位，端口 0x608）。`usb_delay_ms()` 先探活（读两次间隔 20000 次 paced 读，不变即判死），活着就按 `(p1 - p0) & 0xFFFFFF >= ms * 3579` 等够；死了就用 port-80 读（每次约 1 µs，每毫秒 2000 次）**超睡而不是少睡**——晚到的包只是慢，早到的包是协议违规。
- 位置：`usb_delay_ms()` / `usb_pm_timer_running()`（`usb_pace()` 之后）、`port_reset()` 成功返回前 10 ms、`attach_base()` 每次 `address_device()` 之后 10 ms。
- 现状：本轮新增。判死只探一次并缓存，串口会打印 `[USB] PM timer silent, delays use port-80 fallback`。
- 验证：QEMU 走到该分支（`PM timer silent`），`at=00009900`、`ec=........`、`mi=1`、`chk=f`、`evs=02/06/0e`、`td0`/`dc0`/`ep0lo` 与上一版逐字相同；真机待拍（真机若 PM Timer 活着，报告页不再打印该行）。
- 未做：复位后 10 ms 之外，规范对 LS/FS 设备的 TPORSCL 等更细的窗口未细分；hub 下游端口的复位间隔（本驱动不展开 hub）。

### U-20 整端口重试（不是重试单个请求）

- 要求：设备对一次 bring-up 完全不回应时，规范给的补救不是把失败的请求重发，而是**重新复位整条端口再寻址**——因为只有复位能把链路两端的收发状态一起重置。Linux `hub_port_init` 的外层就是 `GET_DESCRIPTOR_TRIES`（默认 2，`CONFIG_USB_FEW_INIT_RETRIES` 时 1）轮循环，每轮之间 `msleep(100)` 并重新 `hub_port_reset`；SET_ADDRESS 内层另有 `SET_ADDRESS_TRIES`（默认 2）次。只重发请求的前提是"复位已经生效"，而这正是本机证据怀疑的地方（EP0 dequeue 零前移、传输事件计数为 0）。
- 位置：`xhci_attach_port()`（`attach_base` 两轮：失败且 `g_att_step >= 3` 时 `disable_slot` → `usb_delay_ms(100)` → `port_reset` → 重跑 `attach_base`）、`g_bn_retry`、报告页 `rty=`。
- 现状：本轮新增，先做 **2 轮**，与 Linux `GET_DESCRIPTOR_TRIES` 的默认值 2 一致；若真机 `rty=01` 仍失败再考虑加轮，避免把一次启动拉长到分钟级。
- 验证：QEMU `rty=00`、`rec=00` 不触发；成功路径数字与上一版一致。真机待拍 `rty=`：非 0 且 `at=` 前进 = 首轮 bring-up 确实无效、复位重来有效；`rty=01` 后仍 `ec=T` = 端口层面已无计可施，问题在设备侧应答或上游 hub。

### U-21 环重置必须收回所有权

- 要求：TRB 的 cycle 位就是所有权位，**cycle 与环一致 = 控制器拥有、会执行**。软件把 TD 交还（"give back"）的规范手段是把该 TRB 的 cycle 写成相反值（xHCI 4.9.3），清零的 TRB（type 0）也永远不被拥有。只把软件的 `idx` 归零、重写 Link TRB，**并不等于**收回了环内旧 TRB：它们仍然带正确 cycle，仍然会被执行。
- 后果（本机形状）：一次尝试失败后重试，同一 `u->ep0` 环被复用；`Address Device` 把控制器 dequeue 指回环首（`dc0` 里 `dq0 == ep0lo|1`），而环首仍是一次"带 Chain 的 Setup TRB"，于是控制器可以**重放上一轮的旧 TD**（旧的数据缓冲、旧的请求），既不产生本轮的事件，也不报错。
- 位置：`ep_ring_reset()`（先逐项清零 0..TRBS-2，再写 Link TRB 并把 idx/cycle 归 1）。
- 现状：本轮新增。此前只在 `attach_base` 的 step 2 与 FS/LS 重寻址路径各调一次，两处都留有旧 TRB。
- 验证：QEMU 成功路径 `at=00009900`、`ec=........`、`mi=1`、`td0`/`dc0`/`ep0lo` 逐字不变；真机待拍（`at=` 是否比 `3030043003000000` 前进）。

### U-22 地址 0 探针（BSR）

- 要求：USB 2.0 9.4 规定设备在收到 SET_ADDRESS 之前一直停留在地址 0，且**必须**能在地址 0 回答 `GET_DESCRIPTOR(DEVICE, 8)`。Linux 正是把这条请求当成"设备在新地址不应答"时的兜底（`hub_port_init` 的 old scheme：先读描述符、再设地址、再重读）。xHCI 4.6.5 给 Address Device 一个 BSR 位（命令 TRB 控制字 bit 9）：建好 slot 与 EP0 上下文但**不**发 SET_ADDRESS，设备留在 Default 状态。
- 用途（本机唯一可用的二分）：一个"什么都没回"的控制传输，可能是（A）控制器根本没跑这个 TD，也可能是（B）设备被寻址后不再应答。**同一条请求在地址 0 上重发一次就能区分**：地址 0 也没事件 = A（传输通道本身不通）；地址 0 成功 = B（链条断在 SET_ADDRESS 之后）。
- 位置：`addr0_probe()`（`disable_slot` → 100 ms → `port_reset` → `enable_slot` → 环重置 → `address_device(..., bsr=1)` → 10 ms → `GET_DESCRIPTOR`），由 `xhci_attach_port()` 在"第一轮就死在 step 3"时调用一次；`address_device()` 新增 `bsr` 参数，`cmd_submit_ep()` 新增 `flags` 参数承载 BSR。
- 报告字段 `rz=`：`ff` = 没跑或地址 0 也无事件；`ee` = 探针前的步骤失败；`00` = 设备在地址 0 应答了；其它 = 该完成码。
- 现状：本轮新增，探针只做诊断，不改变枚举路径（探针结束后 slot 一律丢弃，第二轮 bring-up 重新开始）。
- 验证：QEMU 成功路径不触发（`rz=ff`），无回归；真机待拍 `rz=`。

### U-23 等待预算必须是时间，不是循环次数

- 要求：xHCI 4.3.4 与 USB 2.0 9.2.6.3 都把"超时"写成时间；Linux 给一次控制传输 5000 ms（`USB_CTRL_GET_TIMEOUT`），命令走 `XHCI_CMD_DEFAULT_TIMEOUT`。本驱动用的是**循环次数**（`USB_TIMEOUT = 8,000,000`，每 64 次迭代读一次 port-80，注释按"约 1 秒"设计）。
- 实测：port-80 读在真机上一次约 1 µs，于是 125,000 次读 ≈ **0.1 秒**，不是 1 秒——比 Linux 的预算小一到两个数量级，也比一个设备刚被复位后合法需要的应答时间短。超时太短产生的现象与"控制器一个事件都没产生"**完全无法区分**，而这正是本机连续几轮报出来的形状（10 次 bring-up 只收到 2 个传输事件）。
- 修法：`wait_cmd()` / `wait_transfer()` 改用 `usb_wait_over()` 的时间预算（`USB_WAIT_MS = 1000`），时钟来源见 U-25；无时钟的机器仍走原迭代上限，但报告页把这种情况单独标出来（`wms=fffe`，不是时长）。
- 报告字段 `wms=`：最后一次失败等待实际用了多少毫秒。等于预算 = 设备真的没应答；远小于预算 = 等待自己提前退出；**`fffe` = 撞上迭代上限（机器没有可用时钟），不是时长**。
- 现状：第九轮新增，第十轮重写时钟来源。第九轮版本在真机上 `wms=ffff`，原因见 U-25。
- 验证：QEMU 成功路径 `wms=0000` 无回归；真机待拍 `wms=`（应为一个接近 1000 的四位十六进制数，例如 `03e8`）。

### U-24 端点恢复命令必须按端点状态选择

- 要求：xHCI 4.8.3 的 Reset Endpoint 只对 **Halted** 状态的端点合法，4.8.4 的 Stop Endpoint 用于把 Running/Stopped 端点停下来；对不在允许状态的端点发命令，控制器回 **完成码 19（Context State Error）**。FreeBSD 在 `sys/dev/usb/controller/xhci.c` 的 `xhci_setup_endpoint()` 里把这条状态机写成 `switch (xhci_get_endpoint_state())`：`DISABLED` 什么都不发、`HALTED` 发 Reset Endpoint、其余状态发 Stop Endpoint，之后一律 `Set TR Dequeue Pointer` 回环首；Linux 从另一头得到同一结论——`xhci_urb_dequeue()` 发 Stop Endpoint，Reset Endpoint 只出现在 `xhci_handle_halted_endpoint()`。
- 本机证据：第九轮照片 `rcf=0e`、`rce=13`、`rde=00`。十四次恢复全部被拒，完成码 0x13 = 19 = Context State Error，而 `rde=00` 说明第一步失败后 Set TR Dequeue 根本没发出。配合 `es0=1`（EP0 状态 Running），结论是：**我们对一个 Running 的 EP0 发 Reset Endpoint，这个命令本身不合法**，所以"恢复后重试"从来没有拿到一个干净的端点，重试等于在控制器不知情的情况下重写环。
- 修法：`ep0_recover()` 先读控制器自己维护的 EP0 上下文（`ep0_out_snap()`，DW0 bits 2:0），再按状态选命令：`Disabled(0)` → 不发命令直接失败（只有重新 Address Device 能救）；`Halted(4)` → Reset Endpoint；`Stopped(3)` → 只做 dequeue 更新；其余（含 `Running(1)`）→ Stop Endpoint。之后仍然 `Set TR Dequeue Pointer`（`ep0_phys | 1`）。新增 `TRB_STOP_EP = 15`。
- 位置：`ep0_recover()`、`ep0_out_snap()` 的前置声明、`TRB_STOP_EP` 常量。
- 报告字段：`rce=` 现在是"状态命令"的完成码（以前固定是 Reset Endpoint 的），`rsv=` 说明这条码属于哪条命令：`0` = Reset Endpoint、`1` = Stop Endpoint、`2` = 没发命令（Stopped/Disabled）。`rde=` 仍是 Set TR Dequeue 的完成码。
- 现状：第十轮新增。
- 验证：QEMU 成功路径不触发（`rec=00 rcf=00 rsv=0 rce=00 rde=00`），`at=00009900`、`mi=1`、`chk=f`、`evs=02/06/0e`、`td0`/`dc0` 逐字不变。真机待拍：期望 `rce=00`（Stop Endpoint 被接受）+ `rsv=1` + `rec` 开始增长，或 `rce` 变成另一个码，说明状态判断读错了上下文。
- 未做：若 `rsv=2`（判成 Stopped/Disabled）而实际在跑，需再核对 `ep0_out_snap()` 的 `ctx_size` 步长（CSZ=1 时 64 字节，否则 32）。

### U-25 时钟来源：内核已校准的 TSC，驱动不再自己探端口

- 要求：驱动需要的是"经过了多少时间"，不是"读了多少次端口"。xHCI 的超时是毫秒量级（U-23），要拿到毫秒就必须有一个已校准的时基；**端口号不是规范保证的东西**，ACPI FADT 的 `PM_TMR_BLK` 才是权威来源，而它在不同 PCH 上并不都是 0x608。
- 本机证据：第九轮照片 `wms=ffff`。`ffff` 是报告页的截断值，它同时可能是"耗时长到 65535 ms"和"撞上 8,000,000 次迭代上限"；结合 U-23 的预算逻辑，只能是后者——`usb_pm_timer_running()` 在真机上判定 0x608 静默，整个毫秒预算退化成迭代计数。也就是说第九轮以为修好的"真实时间预算"在真机上一行都没生效。
- 修法：内核本来就有校准好的时基（`kmain.c` 的 `timeline_probe_hz()`：先 CPUID leaf 0x15 的 crystal/ratio，再 leaf 0x16 的基频，最后用 FADT 指定的 PM timer 端口做 TSC 校准）。新增 `cpu_uptime_us()`（`kernel.h`）把它导出为单调微秒数，`usb_ms_now()` = `cpu_uptime_us()/1000`，`usb_delay_ms()` 用它忙等到目标微秒数；时基不可用时才退回 port-80 读，并在串口打印 `[USB] no calibrated clock, waits fall back to the iteration cap`。
- 关键点：这次不再有任何硬编码端口，也不再重复标定——一个内核里只保留一处时基来源，驱动只是使用它。`cpu_uptime_us()` 内部先除整秒再乘 1e6，避免 `cycles * 1e6` 在 3 GHz 下约一小时就溢出 64 位。
- 位置：`kernel.h`（声明）、`kmain.c`（`cpu_uptime_us()` 实现，紧邻 `cpu_delay_us()` 之前）、`usb.c`（`usb_clock_running()` / `usb_delay_ms()` / `usb_ms_now()`）。
- 现状：第十轮新增；U-19 里"PM timer 端口 0x608"的假设随之作废（`USB_PM_TIMER_PORT` / `USB_PM_TICKS_PER_MS` / `g_pm_timer_alive` 一并删除）。
- 验证：QEMU 启动串口无 `no calibrated clock` 行，说明时基在模拟器里也走 TSC 路径；`at=00009900`、`ec=........`、`mi=1`、`chk=f`、`evs=02/06/0e`、`wms=0000` 无回归（QEMU 截图 `/tmp/r113.png`，镜像 `5861862f20d4`）。真机待拍 `wms=`：`fffe` 说明时基仍不可用（此时要看 ACPI/CPUID 路径），`03e8` 附近说明预算真的按毫秒跑了。
- 未做：`cpu_uptime_us()` 依赖 `novus_timeline()` 已经跑过并完成标定；若某台机器连 CPUID 0x15/0x16 和 FADT PM timer 都没有，仍然只能退回迭代上限（报告页会显示 `fffe`）。

### U-26 （已撤回）速度编码本来就是同一套，不需要换算

- 原条款的前提是错的，此处留档以免再犯：曾把 Slot Context 的 Speed 与 PORTSC 的 PSIV 当成两套编号（"1↔2 互换"），并据此在 `xhci_slot_speed()` 里把全速改写成低速。
- 事实：Linux `drivers/usb/host/xhci.h` 在 slot context 的 dev_info 字段上写得很直白——`Device speed - values defined by PORTSC Device Speed field - 20:23`；`xhci-mem.c` 也是把端口的速度直接写进 slot（`SLOT_SPEED_FS/LS/HS/SS` 即 1/2/3/4）。也就是说**只有一套编码**：`1 = 全速, 2 = 低速, 3 = 高速, 4 = 超速`。
- 后果：那个互换会把一台全速鼠标声明成低速——控制器按 1.5 Mbit/s 驱动一条 12 Mbit/s 的链路，设备不该答，事务丢失，连传输事件都不产生。QEMU 覆盖不到，因为它的三台设备都跑高速（码 3，两种编号相同）。
- 现状：互换已删除，`xhci_slot_speed()` 现在原样返回；函数保留是为了让 `address_device()` 与 `configure_endpoints()` 两处调用点都指向同一段解释，删函数会把这段依据一起删掉。
- 验证：改动前后 QEMU 都是 `at=00009900`（无 tablet）/`at=00009990`（含 tablet），`speed=3` 不变，无回归。真机判读：插全速设备时 `dc0` 的 slot DW0 速度位应为 1，`at=` 也应继续前进（在 U-29 生效的前提下）。

### U-29 确认端口变化位时不许把 PP 写成 0

- 要求：`PORTSC` 的 bit 9（PP，端口供电开关）不是写 1 清零位，而是"写什么就是什么"的开关。只为了确认 17–23 的变化位而写整个寄存器，就会把 PP 按读到的 0 写回去，等于**在枚举中途把端口断电**。Linux 从不这样写：`xhci_port_state_to_neutral()`（`xhci-hub.c`）保留只读字段与 PP，只清掉 RW1C 位，再由调用方把要确认的变化位置 1。
- 本机证据：旧 `xhci_port_changes()` 写的是 `sc & 0x00FE0000u`，PP 位是 0，等于在每次确认 Port Status Change Event 时把 PP 写回 0。需要如实说明：真机 `hcc=00087001` 的 PPC（bit 3）**是 0**，这台机器的端口电源不由软件控制、PP 写入被忽略，所以这一条**不是**本机主因；它是真错，但只在 PPC=1 的控制器上才会真的断电（QEMU 同样 PPC=0，两边都看不到）。保留的必要性在于：换到 PPC=1 的平台时，它会原样重现"每个端口的第一次控制传输零事件"。
- 位置：`xhci_port_changes()`（`kernel/usb.c`），改为 `(sc & 0x00FE0000u) | (sc & (1u << 9))`。
- 现状：本轮新增。`port_power()` / `port_wake()` / `port_reset()` 本来就是显式写 PP=1，只有这一处漏了。
- 验证：QEMU `at=00009900`、`dc0=08300000/00000001/00400026/00389001/10000001`、`dw1=00050000`（根端口 5 落在 23:16）、`evs=02/06/0c`、`ep0lo=00389000`、鼠标与键盘 HID 接口均 ready，基线无回归。真机判读：若这条是主因，`at=` 应该不再停在 3，`evs` 里的传输事件计数应从 2 明显上升。

### U-30 传输超时不许把整台控制器 HCRST

- 要求：一个 TD 没完成是端点级故障，不是控制器级故障。Linux 的 `xhci_urb_dequeue()`/`xhci_stop_endpoint()` 只做 Stop Endpoint + Set TR Dequeue Pointer；整机复位（HCRST）会销毁所有 slot，把"一个设备的超时"升级成"全部设备掉线"。
- 本机证据：旧 `wait_transfer()` 超时后直接 `xhci_hc_reset()`；紧接着调用方 `ep0_recover()` 发的端点命令必然拿到完成码 19（Context State Error）——真机 `rcf=0e rce=13 rde=00` 正是它（`0e` = 命令超时、`13` = 19）。
- 位置：`wait_transfer()` 超时分支，只在 `USBSTS.HCH` 置位（控制器自己停了）时才重启控制器。
- 现状：本轮修改。真机上这轮把"一个口的超时"传染成"所有口都超时"的放大器就此拿掉。
- 验证：QEMU 无超时路径可走，仅确认无回归（`at=00009900`）。真机判读：`rcf/rce` 不再出现 `0e/13` 这一对，`at=` 里各口不再成片停在同一个 step。

### U-31 毫秒预算必须用不依赖 TSC 标定的时基

- 要求：`usb_delay_ms()` 与 `wait_cmd()` / `wait_transfer()` 的预算都是毫秒语义，必须有一个在 `usb_init()` 阶段就已可用的时钟。真机 `wms=ffff` 是"撞上 8,000,000 次迭代上限"的哨兵，说明 TSC 标定在这些早期阶段拿不到，所有 10 ms / 1000 ms 都退化成一串 port-80 读（约 0.1 s 量级）。
- 修法：`usb_pace()` 每 64 次轮询调一次 `cpu_delay_us(10)`——它走 ACPI FADT 指定的 PM timer（没有就退 port-80），与 TSC 标定无关——并把时间累加进 `g_pace_us`；`usb_ms_now()` 在无 TSC 时返回 `g_pace_us / 1000`；`wait_cmd()` / `wait_transfer()` 的预算改成恒为 `USB_WAIT_MS`（1000）；`usb_delay_ms()` 的无钟分支改成 `cpu_delay_us(ms * 1000)`。
- 位置：`usb_pace()`、`usb_ms_now()`、`usb_delay_ms()`、`wait_cmd()`、`wait_transfer()`（`kernel/usb.c`），新增 `static UINT64 g_pace_us;`。
- 现状：本轮修改（U-25 之后时基仍可能在早期不可用，本条是它失效时的兜底，不是替代）。
- 验证：QEMU `wms=0000`（TSC 路径可用，不触发兜底），`at=00009900` 无回归。真机判读：`wms=` 应停在 `03e8`（1000）附近，而不是 `ffff`。

### U-32 I2C-HID 的寄存器必须走非缓存映射

- 要求：DesignWare/LPSS 的控制器寄存器是设备内存，只能用非缓存（UC）映射访问。写 `IC_ENABLE` 后立刻读 `IC_ENABLE_STATUS` 是这条控制器的使能握手；若写停在 cache line 里、读拿到过期的 0，控制器就"拒绝使能"，后面所有传输都不成立。
- 本机证据：`kernel/i2c_hid.c` 原先直接用物理地址经 identity map 访问，而这张 identity map 是 write-back（USB 驱动里对同一件事已有注释）。QEMU 无 cache，所以永远看不出问题；真机 `i2c=0 bus=3` —— 三个 0x0C/0x80 串行总线控制器都在，没有一个能应答。这与"触控板完全无反应"是同一件事。
- 位置：`kernel/i2c_hid.c`：`dw_rd` / `dw_wr` / `lpss_wake` / `dw_wait` / `dw_init` / `dw_xfer` / `ih_probe_one` 与 `g_ih.base` 全部从 UINT32 加宽到 UINT64；`i2c_hid_init()` 改用 `bar64`（回退 `bar[0]`）+ `mm_map_phys(bar, 0x1000, MM_MAP_UC)`，基址 = 映射 VA + `(bar & 0x1FFFFF)`；不再因 BAR 在 4 GiB 以上而跳过。
- 现状：本轮修改，待真机验证。
- 验证：QEMU 依然是 `[I2C] HID-over-I2C buses seen=0`（模拟器没有这些控制器，属预期）。真机判读：`i2c=` 应从 0 变成 ≥1，`bus=` 也跟着变。

### U-33 报告页新增 slot DW1（端口号）字段

- 要求：照片是唯一证据通道，读不出"slot 属于哪个根端口"就必须靠猜。`g_dc0` 由 5 项扩到 6 项，新增 `dw1=`（slot context DW1，bits 23:16 是 `ROOT_HUB_PORT`），同时把 `g_dc0[4]` 的注释订正为"slot DW3：slot state 31:27、address 7:0"（原注释把 state 写成 31:24）。
- 位置：`g_dc0[6]`、`usb_port_dump()`（`kernel/usb.c`）。
- 验证：QEMU 报告页 `dw1=00050000`，与串口 `port 5 connected` 一致；`dc0` 其余五项与基线逐字相同。
- 未做：没有任何一条真机读数能把 `dw1` 与 `dc0[4]` 的 slot state 一起证明"这个 slot 确实是这个口"，只能靠同一屏两行自洽。

### U-27 请求的报告长度必须与设备当前所处的协议一致

- 要求：HID 接口有两套报告格式，驱动必须知道设备此刻在用哪一套。boot 协议（SET_PROTOCOL(0) 被接受）下键盘固定 8 字节、鼠标固定 3 字节（HID 1.11 附录 B）；报告协议下长度只能来自报告描述符——它是唯一知道报告 ID、滚轮、额外按键的东西。
- 本机证据：旧实现是"先读描述符、再发 SET_PROTOCOL"，长度却仍按描述符算。QEMU 串口那行 `protocol=boot report=4` 就是自相矛盾：一个 3 字节的 boot 鼠标被要求交 4 字节。更糟的是描述符带报告 ID 的设备：boot 报告第一个字节是按键掩码，却被拿去和报告 ID 比较，`hid_decode_pointer()` 返回 0，**鼠标移动被静默丢弃**。
- 位置：`usb_t.hid_boot`（新增）、`attach_hid_interface()`（只在没有描述符版式时才发 SET_PROTOCOL，成功才置 `hid_boot`）、`hid_report_len()`（`hid_boot` 优先，其次描述符）、`hid_decode_mouse()`（`hid_generic` 与 `hid_boot` 互斥）。
- 现状：本轮新增。描述符已把接口判成指针的（`hid_generic=1`）不再切换协议，留在报告协议，滚轮与额外按键因此可用；描述符读不懂、只能按协议字节认定的，才走 boot 固定格式。
- 验证：同一镜像，改前 `protocol=boot report=4`，改后 `protocol=report report=4`（鼠标）与 `protocol=boot report=8`（键盘），两者自洽；鼠标实际推动指针（1024×768 截图前后对比，箭头从 (1015,265) 移到 (940,355)），`[USB] xhci HID report addr=… bytes=4` 证明 4 字节报告真的到了。
- 未做：boot 键盘的指示灯仍只在 SET_REPORT 成功时更新，没有周期性回读。

### U-28 协议字节为 0 的 HID 接口必须交给描述符去判定

- 要求：USB HID 的 `bInterfaceProtocol` 为 0 表示"没有 boot 协议"，不是"不是指针"。平板、触摸屏、多数无线鼠标接收器、复合设备的第二接口都这么声明，判定的唯一依据是报告描述符。这条与 U-11 是同一要求的两半：U-11 说"要读描述符"，本条说"读之前不许先把接口丢掉"。
- 本机证据：把一台 usb-tablet（协议字节 0、绝对坐标）接到 QEMU 的 xHCI 上，旧实现报告页 `at=00009790` 里该口停在 **7**，串口**一行日志都没有**——它在"接口是 HID 且有中断 IN 端点"之后、读描述符之前，仅凭 `hid_kind == 0` 就把接口扔了，连 GET_DESCRIPTOR(REPORT) 都没发出。
- 位置：`xhci_attach_port()` 的 step 7 判据（去掉 `v->hid_kind == 0u`，只保留 `parse_config_iface()` 的"是 HID 且有中断 IN 端点"）。
- 现状：本轮新增。判定改由 `attach_hid_interface()` → `hid_read_descriptor()` 完成；描述符也说不出所以然时打印 `HID interface ignored` 并按原路径回收 DMA。
- 验证：同一台 tablet，改后 `at=00009990`、`mi=2`，串口 `HID pointer: 0x30 bits, absolute, buttons 3` + `protocol=report report=6 kind=mouse`；注入绝对位置后 `[NDC] cursor set#2 to 0,0`，光标跳到左上角，桌面随后正常重绘。相对鼠标与键盘无回归（`at=00009900`、`mi=1`）。
- 未做：绝对指针的坐标映射以"逻辑范围覆盖整屏"为前提，多显示器尚未考虑。

### U-34 USB2 端口上不许写 RxDetect（PLS=5）

- 要求：`PORTSC` 的 PLS 字段在不同协议下的含义不同。RxDetect（5）是 **SuperSpeed 专属的链路状态**；在 USB2 端口的 PLS 字段里 5 是保留值，写它属于未定义行为。Linux 从不对 USB2 端口做这件事——`xhci-hub.c` 对 USB2 只在 resume 时写 U3→U0，其余一律用 `PORT_RESET`；USB3 端口才用 RxDetect 重新侦听。
- 本机证据：旧代码在 `port_wake()` 和 `port_reset()` 里**无条件**写 `(1<<16)|(5<<5)|(1<<9)`，也就是说每次复位一个活着的 USB2 端口之前，先把这条链路按 USB3 语义推到 RxDetect。QEMU 的 PORTSC 写处理忽略 PLS，所以模拟器从头到尾看不出问题；真机（Intel PCH，USB2 口）上这一步是"用一条端口不该进入的状态去唤醒它"，与"设备从不回应、TD 一个事件都不产生"是同一形状。
- 位置：`port_is_usb3()`（新增，按 Supported Protocol 能力块的 bcdUSB 主版本号建 `hc->port3mask`，Linux 在 `xhci-hub.c` 里正是这样填 `usb3_ports[]`）、`port_wake()`、`port_reset()`。
- 现状：本轮修改。`port_wake()` 对 USB2 端口直接返回（上电与 CCS 判定交给扫描），`port_reset()` 只对 USB3 端口做 RxDetect 前置。
- 验证：QEMU `at=00009900`、`mi=1`、`evs=02/06/0c`、`dc0`/`dw1` 与基线逐字一致，鼠标 4 字节报告到达、光标画出并停在注入点；模拟器的 qemu-xhci 根本不报 Supported Protocol 能力块（串口 `sp=none`、`sn=0`），所以 `port3mask=0`、所有端口按 USB2 处理，改后行为与改前在模拟器上等价——差别只在真机。
- 未做：USB3 端口的 warm reset / U3 恢复路径仍未按 USB3 规范细分。

### U-35 复位后要校验链路回到 U0，而不是只看 PED

- 要求：`PED=1` 只说明端口被使能，不说明链路可用。端口可以"已使能但停在 Polling"——此时控制器把 TD 挂上去、设备永远答不上来，驱动等满整个预算后只能报"一个事件都没有"。USB2 的复位完成判据是 `PED=1` **且** PLS=U0。
- 位置：`port_reset()` 末尾（非 USB3 端口）：PLS 不为 0 时打印 `[USB] port N reset left PLS=… PORTSC=…`。
- 现状：本轮新增（判定 + 如实上报，不改流程）。
- 验证：QEMU 无该行输出（端口复位后确实到 U0），基线无回归。真机判读：这一行若出现，就直接解释了"停在 step 3"；若不出现，说明链路是通的、问题在设备侧。
- 未做：尚未按 PLS 结果改变流程（例如 U3 自动 resume），只上报。

### U-36 传输完成事件不许在等命令完成时被丢掉

- 要求：事件环是共享的。等命令完成时若收到传输完成事件，它属于某个仍在等待的端点，必须停放而不是丢弃；对应的 `wait_transfer()` 也必须先查停放区，再看事件环。
- 本机证据：旧 `wait_cmd()` 对 `TRB_TRANSFER` 只做计数然后 `continue`。于是"命令前的那个 EP0 TD 在命令等待期间完成"这种时序下，完成被吃掉，随后的 `wait_transfer()` 报"零事件"，看起来像控制器没干活。
- 位置：`usb_defer_drop()`（新增）、`wait_cmd()`（改为 `usb_defer_push`）、`wait_transfer()`（每轮先 `usb_defer_take`）、`control_xfer()`（发新 TD 前 `usb_defer_drop(1,0)`，防止上一次的残留被当成这次的答复）。
- 现状：本轮新增。
- 验证：QEMU 鼠标/键盘枚举与报告均正常（`protocol=report report=4 kind=mouse`、`protocol=boot report=8 kind=keyboard`，注入后 `[USB] xhci HID report addr=1 bytes=4` 与光标落点一致），`at=00009900` 无回归。
- 未做：停放区按 `(dci, ep)` 单键匹配，没有请求级序号，理论上仍可能把两次同端点请求的完成认错；已用 `usb_defer_drop` 在每次 EP0 请求前清空该端点缓解。

### U-37 启动路径不承担端口枚举（对齐 Linux 的 probe / 枚举分工）

- 要求：控制器初始化和端口枚举是两件事，不许挤在同一个同步调用里。Linux 的 `xhci_pci_probe()` 只做 `xhci_reset` / `xhci_start`、注册根 hub 就返回，设备枚举发生在 `hub_event`（工作队列）里，而且只针对有变化位的端口；Windows 同样把这段交给 hub 驱动的 worker。启动关键路径上不许出现"每端口复位 + 描述符走查 + 两次重试"。
- 旧状：`usb_init()` 在首帧之前同步跑完所有控制器、所有端口的 `port_power()` / `port_reset()` / `xhci_attach_port()`（每口两次 bring-up 重试 + 100 ms 等待），一个不答应的端口就能把首帧推到它整段预算之后。
- 位置：`usb_init()`（端口循环删除，只保留一次 `g_rep_pre` 快照与 `hc->rep` 记录）、`hub_port_probe()`（新增，原循环体单口化）、`usb_hub_probe_all()`（新增，`usb.h` 导出）、`kmain.c` 的 `boot_deferred()` 开头与无桌面分支。
- 验证：QEMU（qemu-xhci 8 口、鼠标 + 键盘），`usb_init` 从 `nvme probe done → usb probe done` 的 **68 ms**（193→261）降到 **21–24 ms**（192→213、229→253）；首帧之后的端口段 **46–51 ms**（`usb hub done`）。`at=00009900`、`mi=1`、`evs=02/06/0c`、`td0=00030851/00010c11/00001021`、`dw1=00050000` 与基线逐字一致；注入鼠标后 `[USB] xhci HID report addr=1 bytes=4` 到达，光标落在注入点并保持。
- 未做：仍是轮询而非中断驱动；真正并行枚举需要第二个上下文（内核线程 / workqueue），本内核目前没有。

### U-38 热插拔只在连接变化位（CSC）上动作

- 要求：`PORTSC` 的 CSC（bit 17）是 RW1C 的连接变化位，"有新东西插进来"只有它说了算。周期重扫可以读状态，但**不许**对"CCS=1 却没有我们的设备"的端口复位——那等于每轮都把一个已经枚举失败的端口再复位一次（每次都可能撞满 8,000,000 次迭代预算），并把同一控制器上的 HID 轮询拖在后面。
- 旧状：`xhci_hotplug_scan()` 每 32 个输入 tick 遍历所有端口：`port_wake()` + 读 PORTSC，只要 `connected && !live` 就 `port_reset()` → `xhci_attach_port()`，没有停止条件。
- 位置：`xhci_hotplug_scan()`（新增 `changed` 判定；CSC 以 `(1<<17) | (PP 读回值)` 清除，PED/PR 不写；只有 `changed` 才复位；`g_hub_done == 0` 时直接返回，避免与启动端口遍历把同一端口枚举两次）。
- 验证：QEMU 连续运行 36 s，日志无 `hotplug:` 行、无第二次 `HID interface ready`，`cn=2` 与实际设备数一致；鼠标注入与光标仍正常。
- 未做：真机拔插的完整回归没做（QEMU 无物理插拔），本轮只证明了"不误触发、不重复枚举"。

### U-39 日志介质在端口枚举之后再选一次

- 要求：`usblog_init()` 只能从**已经注册**的块设备里挑介质。U 盘本身走 USB，端口枚举挪到首帧之后后，启动阶段的选盘必然看不到它；因此枚举结束必须再选一次，否则诊断日志永远落不到 U 盘，45 s 报告页每次都会出现。
- 位置：`boot_deferred()`（`usb_hub_probe_all()` 之后调用 `usblog_retry()`）、无桌面分支同一处。
- 验证：QEMU 无 USB 块设备，`log=0` 与报告页行为与改前一致（也是本轮唯一的对照）；真机要看 `[LOG]` 行与状态条 `log=` 是否变为 1。
- 未做：`usblog_retry()` 只在启动阶段调用，运行期插上的 U 盘不会重新选盘。

### U-40 hub 类请求的 wValue / wIndex 不许写反，端口供电只在有 PPC 时请求

- 要求：USB 2.0 第 11 章里 `SetPortFeature` / `ClearPortFeature` 是 `bmRequestType=0x23`、**wValue = 特性选择子、wIndex = 端口号**。把特性编号塞进 wValue 高字节、端口号塞进低字节（或把端口号丢进 wIndex=0）在真 hub 上是非法组合：hub 把 `wIndex` 当端口序号检查，0 直接越界，一律 STALL。第二个要求是端口供电：`wHubCharacteristics` 的 D1:D0 = `00` 成组供电、`01` 逐端口供电、`1x` **不做端口供电**；只有前两种才允许发 `SetPortFeature(PORT_POWER)`，没有供电控制的 hub 对这条请求回 STALL 而不是忽略。
- 本机／QEMU 证据：`kernel/usb.c` 旧代码四条 hub 类请求全部写成 `(UINT16)((HUB_FEAT_x << 8) | i)` 且 index 传 0；QEMU `qemu-xhci + usb-hub` 起机日志里依次出现 `SET_FEATURE` 与 `CLEAR_FEATURE` 都 `code=6`（Stall）、`hub port 2 reset did not complete`、`hub walk stopped at port 3`，hub 描述符能读（`ports=8 char=0x000A poweron=0x0001`）但任何端口操作都失败。`char=0x000A` 的 D1:D0 = `10`，正是"不做端口供电"，旧代码仍无条件发 PORT_POWER。
- 位置：`hub_walk()`（五处 `hub_req(hub, 0x23u, 3u/1u, (UINT16)HUB_FEAT_x, (UINT16)i, …)`；供电循环改为仅当 `(wHubCharacteristics & 0x0003) <= 1` 时执行，否则打印 `no port power switching, ports left as they are` 并跳过 `bPwrOn2PwrGood` 等待）。
- 参照：Linux `drivers/usb/core/hub.c` 的 `hub_power_on()` / `hub_hub_status()` 分支与 `include/linux/usb/ch11.h` 的 `HUB_CHAR_LPSM` 判定；QEMU `hw/usb/dev-hub.c` 的 `SetPortFeature` 只接受挂起/复位/供电三种特性、其余 `goto fail`（即 STALL）。
- 验证：同一个 QEMU 场景（`qemu-xhci` + `usb-hub` 挂根端口 1、鼠标在 hub 口 2、键盘在 hub 口 5）修正后日志为 `hub: no port power switching` → `hub port 2 device speed=1` → `HID pointer … relative` → `xhci HID addr=2 … kind=mouse` → `HID interface ready` → `hub port 2 device attached`（键盘同理 addr=3/口 5）；注入 `mouse_move` + `sendkey` 后 `[USB] xhci HID report addr=2 bytes=4` 与 `addr=3 bytes=8` 都到达，光标从 (512,256) 移到 (639,383) 再到 (766,510)，桌面正常。同一镜像根端口场景（鼠标口 5、键盘口 6）无回归。
- 未做：hub 挂 **高速 hub + 低速/全速子设备** 的 TT 路径无法在 QEMU 6.2 构造（`usb-hub` 只有全速实现），`slot[2]` 的 TT 字段只按 `xhci-mem.c` 语义写好、未经硬件验证；多层 hub（>1 级）同样只做了三层上限保护。

### U-41 端口遍历必须是"有预算、可续跑"的状态机

- 要求：端口遍历是后台工作，不是启动步骤。它的成本上限必须由**调用方给的时间预算**决定，而不是由"端口数 × 每个请求的最坏等待"决定；预算用完就停在当前端口，下次从那一口继续，不许从头再来。
- 旧状：`usb_hub_probe_all()` 一口气跑完所有控制器所有端口，每个请求用 1000 ms 预算。QEMU 上 8 个 hub 端口就把 `usb hub done` 推到首帧之后 **+8130 ms**；真机上 4 个停在 step 3 的端口若各撞几次 1000 ms 超时，桌面、报告页、日志写入会一起被推后几十秒——现象就是"没有日志"。
- 位置：`g_xfer_budget_ms`（新增全局，`wait_transfer()` 的预算取它）、`usb_hub_step(UINT32 budget_ms)`（新增，静态游标 `g_hub_ci` / `g_hub_port`，一次推进一个端口，进入时预算降到 150 ms、退出恢复）、`usb_hub_probe_all()`（改为 `usb_hub_step(700)` 的薄包装并打印"剩余端口交给输入 tick"）、`usb_hid_poll()`（`g_hub_done==0` 时每 tick `usb_hub_step(40)`）、`hub_walk()`（自带 500 ms 截止，超时打印 `hub walk stopped at port N after M ms`）、`usb_hub_step()` 完成时 `usblog_retry()`（U-39 并入此处，插在遍历期间才枚举到的 U 盘也能落日志）。
- 验证：QEMU hub 场景 `first frame +2229 ms` → `usb hub done +2291 ms`，端口段 **62 ms**（改前同场景 8 秒级）；三个设备（hub、鼠标、键盘）全部枚举完成；根端口场景 `+2164 → +2217`，**53 ms**，`ccs=2 cn=2`。报告页之后的桌面与输入正常。
- 未做：预算对"响应慢但活着"的设备是硬上限——**已由 U-48 改掉**（预算改由调用方决定：启动 1000 ms / 输入 tick 250 ms）；内核没有 workqueue，续跑只能挂在输入 tick 上，桌面未启动时（45 s 报告页）这段不推进。

### U-42 `CONFIG.MaxSlotsEn` 写最高槽位号，不是槽位数

- 要求：`CONFIG`（operational offset 0x38）低 8 位 `MaxSlotsEn` 是"控制器可分配的最高槽位号"，写 `MaxSlots - 1`。Linux `xhci_gen_setup()` 即 `xhci_write_32(&xhci->op_regs->config_reg, num_slots - 1)`。
- 旧状：写 `hc->max_slots`，多要一个控制器没有的槽位。QEMU 不校验，真机可能被丢弃或回读不符。
- 位置：`controller_program()`；报告页 `chk` 的 bit3 校验同步改为 `(CONFIG & 0xFF) == max_slots - 1`，否则自检会假报 bit3 丢失。
- 验证：QEMU hub 场景报告页 `chk=f`（四个 bit 全亮，bit3 与写入值一致）。

### U-43 EP0 默认包长：全速按 64 猜，失败再按 8 退

- 要求：Linux `xhci_setup_addressable_virt_dev()` 的表是 超速 512 / 高速 64 / **全速 64** / 低速 8，注释写明"USB core guesses at a 64-byte max packet first for FS devices"。包长决定控制器怎么切 SETUP 与数据包，用错会在真机上丢事务（QEMU 不校验包长，两种都能过）。
- 旧状：全速也按 8，只有高速拿 64。真实 MPS=64 的全速设备被 8 字节框架驱动。
- 位置：`ep0_default_mps()`（全速 64）；`attach_base()` 的"按 `bMaxPacketSize0` 重新寻址"条件改为 `mps != want`（不再只认 16/32/64），并补 Linux `hub_port_init` 的"assume 8"退路：首次 8 字节头读失败时，以 8 重新寻址再读一次。
- 验证：QEMU 的两个全速设备（`bMaxPacketSize0=8`）第一条日志变为 `ep0 mps=8 (device disagreed with 0x40)` 后重寻址成功——两条分支（猜 64 与实际值不符、需要按真实值重建 slot）都被走到并恢复。

### U-44 控制传输的 IN 数据 TRB 必须带 ISP

- 要求：Linux `xhci_queue_ctrl_tx()` 对 IN 数据阶段写 `TRB_ISP | TRB_TYPE(TRB_DATA)`。ISP 让该 TRB 的中断只报一次，缺它会让多阶段 TD 的完成上报与驱动的等待配对错位。
- 旧状：只有 DIR_IN 与 CH。
- 位置：`control_xfer()` 的数据 TRB 条件里加 `(1u << 2)`。
- 验证：QEMU 两个场景（根端口 / hub 子设备）全部枚举 + 注入事件正常，无回归。

### U-45 USB2 端口复位后链路不是 U0：resume 或按"复位失败"重试

- 要求：PED=1 不等于链路可用——端口可以"已使能但停在 Polling"，此时 TD 挂上去、设备永远答不上来，驱动只会等到超时后报"零事件"。Linux 对 USB2 口只在 resume 时写 `LWS | PLS=U0`，对 Disabled 的端口重新复位。
- 旧状：U-35 只打印 `reset left PLS=…`，然后照旧拿去寻址。
- 位置：`port_reset()` 末尾——USB2 端口 PLS=U3 写 `LWS|PLS=U0` 并等回 U0；PLS 仍非 0 时返回 `-2`（"复位本身没成功"），由 `xhci_attach_port()` 的阶梯重新复位；报告页 `rs` 扩成 0=已连接 / 1=复位被拒 / 2=复位完成但链路非 U0 / 3=口上没设备。
- 验证：QEMU 上空口现在报 `3`、挂设备的报告 `0`（`rs=33330333`），不再把"没插设备"和"失败"混成一个值；USB2 的 PLS 行为 QEMU 不实现，真机才显形。

### U-46 bring-up 必须是三遍阶梯，热插拔与启动走同一条路径

- 要求：Linux `hub_port_init()` 给 `GET_DESCRIPTOR_TRIES`（3）遍，每遍之间 `msleep` 递增并**重新复位端口**——不回应往往只是设备还没从总线复位里缓过来。运行期插入的设备与启动时已在的设备必须走同一套阶梯，不许有"更弱的第二份实现"。
- 旧状：`xhci_attach_port()` 只有两遍、间隔 100 ms，且失败在 step<3 时直接放弃；`xhci_hotplug_scan()` 另有一份"复位一次 + 寻址一次"的拷贝。
- 位置：`xhci_attach_port()` 的 tries 循环（3 遍、间隔 100/200 ms、每遍前 `Disable Slot` + `port_reset`，失败打印 `port N bring-up failed at step S, retrying`，用掉的遍数记进报告页 `try`）；`xhci_hotplug_scan()` 改为调用 `hub_port_probe()`。
- 验证：故障注入镜像 `d64537c667a5` 强制第一遍失败，QEMU hub 场景仍然 `HID interface ready` + `hub port 2 device attached` + `usb mice=1`（说明阶梯真的能救回来）；最终镜像已去掉注入。

### U-47 清 `USBSTS.FATAL`，并等 `xHCNR` 清零再编程

- 要求：`USBSTS.FATAL`（bit 1，HSE）是粘滞位，Linux 在 fatal 分支里 `xhci_clear_cmd_ring`/写 1 清除；HCRST 之后 `xHCNR`（bit 11）为 1 时控制器不接受寄存器写（Linux `xhci_reset` 用 `xhci_handshake(STS_CNR, 0, 1s)` 等）。不清 HSE、不等 CNR，就是"命令都能完成、EP0 一个事件也没有"和"CONFIG 写了但没生效"的来源。
- 旧状：HSE 从未检查/清除；`controller_start()` 只打印 CNR 不等待。
- 位置：`hub_port_probe()`（bring-up 前检查并写 1 清 HSE，计数进报告页 `hse`）；`controller_start()`（CNR 清零等待，超时才打印）。
- 验证：QEMU 报告页 `hse=0`（模拟器从不报 HSE，此路径只有真机能触发）；CNR 等待在 QEMU 上不改变时序（`chk=f`、枚举时间与改前同级）。

### U-48 控制请求预算由调用方给，不在端口遍历里一律压到 150 ms

- 要求：预算的作用是"不卡首帧"，不是"判定设备死亡"。Linux 给已枚举端口上的控制请求 5 s（设备一直 NAK 就继续等）。把预算压到 150 ms，会让"200 ms 才答的设备"和"从不回应"在页面上长得一模一样（都是 `ec=T`）。
- 旧状：`usb_hub_step()` 进入时无条件 `g_xfer_budget_ms = 150`。
- 位置：新增 `g_port_xfer_ms`（调用方设定）——`boot_deferred()` 路径用 `USB_WAIT_MS`（1000 ms），`usb_hid_poll()` 续跑时设 250 ms 并在之后恢复。
- 验证：QEMU 两个场景的端口段耗时与改前同级（`usb hub done` 前后相差几十毫秒），说明正常设备不受影响。

### U-49 热插拔成功后必须重选日志介质并立刻落文字

- 要求：日志介质的搜索只在"启动端口遍历之后"跑一次（U-39），而在一台"唯一可写介质就是 U 盘"的机器上，U 盘几乎总是后插的；`usblog_header()` 又只写头那一扇区，文字段要等下一次周期性 flush。两件事叠起来的结果是"介质被选中了，但日志正文永远是空的"，与"没有任何介质"在诊断价值上一样。
- 旧状：`xhci_hotplug_scan()` 的 attach 成功分支不重选介质；选中之后没有任何人强制写一次。
- 位置：`xhci_hotplug_scan()` attach 成功分支——`usblog_retry()`，随后 `if (usblog_state() == 1) usblog_flush(1);`（强制 flush 把 64 KiB 缓冲里从 `[T] +55ms kmain start` 起的整段日志一次写进去）。
- 验证：镜像 `3a9ddcb584b6`，空控制器起机 → 报告页 → 桌面 → 运行中插 64 MiB 原始介质：日志 `hotplug: device on port 0x4 attached` → `[LOG] boot log -> usb00 at raw LBA 0xC801`；停机后读镜像，LBA 0xC800 是 `NOVUS boot log (local test build) / device: usb00`，文字段 47 扇区全非零，首行 `[T] +55ms kmain start`。未加 flush 的同场景版本文字段为 0，只有头部。

### U-50 报告页退出后必须整屏重绘（历史记录：报告页已移除，见 B-5）

- 要求：`nd_demo_report()` 直接写 framebuffer 并 `nd_present(全屏)`，窗口管理器对这块像素一无所知，damage 集合里也没有它。桌面起来后只重绘自己认为脏的区域，页面上留下的像素就成了"内容区纯白、边框和标题栏还在"的真机症状。
- 旧状：报告页返回后无人通知 WM。
- 位置：`wm/novus-wm/nwm.c` 的 `g_repaint_all` 与 `nwm_repaint_all()`，`nwm_tick()` 开头把它变成一次全屏 damage 并打印 `[NWM] full repaint requested by an outside writer`；`kernel/kmain.c` 在启动桌面前保留一次调用。原报告页已移除（B-5），但任何绕过 WM 直写 framebuffer 的路径仍须遵守该规则。
- 验证：QEMU 两个场景日志里 `[NWM] full repaint requested by an outside writer` 都出现在报告页 dump 之后、桌面首帧之前；截图里文件管理器/设置窗口内容完整。

### U-51 介质搜索要能重试到"枚举完成之后"

- 要求：U-37 把枚举搬到首帧之后，而介质搜索在 `usb_init()` 阶段跑——开机就在插槽里的 U 盘，在搜索那一刻还没被枚举出来，之后也没有人回头看，日志就永久停在内存里。Linux 同样不靠一次同步搜索，而是在工作队列里反复看到设备出现为止。
- 旧状：搜索一次，失败即 `no writable medium, log stays on screen only`，永不重试。
- 位置：`usb_hid_poll()`——`usblog_state() != 1` 时每 64 个输入 tick 重试一次、最多 16 次（覆盖桌面头一分钟），成功即强制 flush。
- 验证：镜像 `ed1477e4d79e`，U 盘在开机时就在：日志先出现两次 `no writable medium`（正是竞态），随后 `[LOG] boot log -> usb00 at raw LBA 0xC801`；停机后读镜像得到 36 个非零文字扇区，首行 `[T] +55ms kmain start`。

### U-52 报告页要能证明"毫秒预算有没有意义"

- 要求：这份驱动的每个超时都是 `cpu_uptime_us()` 的毫秒预算；标定没跑起来的机器上，所有等待要么立刻到期、要么永不到期，而页面看起来与"控制器从不回应"完全一样。诊断页必须能把这一层单独证伪。
- 旧状：页面只有 `wms`（最后失败的等待实际花多久），无法区分"时钟死了"与"时钟在但被误用"。
- 位置：`usb_port_dump()` 的 `hse=`/`try=` 之后——两次采样夹住 1000 次 `cpu_delay_us(10)`（本应 10 ms），打印 `tmb=`（`usb_clock_running()` 的结论）与 `tm=`（实测毫秒）。
- 验证：镜像 `0c23a8b17c2d` 的 40 s 页照片 `tmb=1 tm=000a`——忙等实测 10 ms，时基活且刻度正确。

### U-53 hub 下游端口的运行期热插拔

- 要求：根端口有 `PORTSC.CSC`，hub 下游端口没有任何控制器寄存器能报告连接变化，Linux 用 hub 的状态变化中断端点 + `hub_event` 工作队列处理。驱动里没有这条通道，就必须自己轮询；而且"重扫整只 hub"必须是幂等的，否则每次扫都会把已枚举的设备重新复位、重新登记。
- 旧状：只在 hub 刚配置好时走一遍 `hub_walk()`，之后插在 hub/扩展坞上的鼠标永远认不出来；`hub_walk()` 也没有"这条 route 上已经有活设备"的判断，不能安全地重复调用。
- 位置：`hub_walk()` 的单口逻辑抽成 `hub_port_attach()`（复位、等 `C_PORT_RESET`、查 `PORT_ENABLE`、组 route 与 TT、交 `xhci_attach_port()`）；新增 `route_in_use()` 与 `hub_child_step()`（每 96 个输入 tick 探一个端口，一次 `GET_PORT_STATUS`、250 ms 预算，游标跨 tick 保留）；`usb_hid_poll()` 在 `g_hub_done` 之后调用它。
- 验证：镜像 `49e1c078befe`——空 hub 起机 60 s 后运行期插鼠标：`hotplug: hub port 0x2 connected speed=0x1` → `HID pointer 0x20 bits, relative` → `xhci HID addr=2 port=5 kind=mouse` → `hub port 0x2 device attached`，注入后光标 (512,256)→(698,373)；hub 上开机即挂鼠标+键盘跑满 60 s，`device attached` 只有 2 次（幂等，无重复复位）。

### U-54 完成事件必须按 slot + endpoint 认领

- 要求：Transfer Event 的归属是两个维度——`TRB[63:56]` 的 Slot ID 与 `TRB[55:48]` 的 Endpoint ID。Linux `handle_tx_event()` 先按 slot 找 `xhci->devs[slot_id]`，再在它的 endpoint 数组里按 EP ID 取 ring。只看 EP ID 时，"键盘和鼠标都用 interrupt IN EP1"的两台设备 dci/epid 完全相同，先被轮询的那台就会把另一台的报告吃掉——真机上表现为"设备枚举成功、光标不动"，且日志里是一份被错误解码的报告。
- 旧状：`wait_transfer()` / `usb_hid_poll()` / `ep0_wait_stopped()` 三处 `event_pop()` 消费点只比对 epid；等待队列 `usb_defer_push/take/drop` 只用 `(dci, epid)` 作键，被误领的报告连寄存后归还的退路都没有。
- 位置：`usb_defer_t` 增加 `slot`；三个队列函数改成 `(slot, dci, epid, …)`；三处消费点与 `wait_cmd()` 的寄存点都从 `(ev.p[3] >> 24) & 0xFF` 取 slot，与 `u->slot` 不等就按事件自带 slot 寄存。
- 验证：镜像 `ecaaba4c7477` 四个场景——① hub 键盘开机 + 鼠标运行期插口 2：`HID report addr=3 bytes=4`（不再是键盘的 8 字节），光标 (512,256)→(698,373)；② 鼠标开机 + 键盘运行期插口 2：`mouse_move 80 60` 得 `addr=2 bytes=4`、光标 592,316，`sendkey a` 得 `addr=3 bytes=8`、`[NDQ] pop type=4 key=97`；③ 根口鼠标 + hub 键盘 + 根口 U 盘：两设备各自 4/8 字节报告；④ 两只鼠标 + 键盘，12 次 `mouse_move 20 10` 后光标 512,256→752,376（240/120 位移一次不丢）。

### U-55 Stop Endpoint 后要等 Endpoint Stopped 再改 dequeue

- 要求：Linux `xhci_urb_dequeue()` 先 `xhci_stop_endpoint_sync()` 等 `COMP_STOP`/`COMP_STOP_INVAL` 的 transfer event，确认端点停在环上某一点，再 `xhci_set_dequeue()`。端点还在跑时改 dequeue 指针，控制器可以整条 TD 一个事件都不报——正是"命令通道可用、传输通道静默"的形态。
- 旧状：Stop 命令丢出去就立刻 Set TR Deq，中间没有等待，也没有"等不到"的痕迹。
- 位置：`ep0_recover()` 调用新增的 `ep0_wait_stopped(u, 50u)`（按 slot + EP0 匹配 `code==26`）；等不到则 `g_ep0_rst=1`，报告页 `rsv=` 后写 `/rst`，然后照旧 Set TR Deq。
- 验证：故障注入镜像 `7f1fbe6b85a1`（`control_xfer` 首笔强制 timeout）——日志 `ep0: no endpoint-stopped event within 50 ms, dequeuing anyway`，随后 `HID pointer`、`HID report addr=1 bytes=4`、桌面正常；注入已移除。QEMU 6.2 的确不在 50 ms 内产生 Stopped 事件，因此这条分支在模拟器上永远是"等不到"的那一支。

### U-56 报告页把 CRCR 自检从比特变成两个数

- 要求：`chk` 的 bit1 只说明"CRCR 落在命令环范围内"。CRCR 读回来的是命令环 dequeue 指针而非写入的 base，真机上一旦该位为 0，"寄存器读成 0"与"指针跑到别处"在照片上无法区分。
- 旧状：只有 `chk=` 一个十六进制位。
- 位置：`usb_port_dump()` 的 `chk=` 之后新增 `crq=<CRCR 低 32 位>/<cmd_phys 低 32 位>`。
- 验证：镜像 `9d3bd6f317ba` 的诊断页 `chk=f crq=00372000/00372000`——两值相等。

### U-57 空环探针：用控制器自己的回答判定"环有没有被读过"

- 要求：规范 4.9.4 规定，控制器被要求从传输环取 TRB 而该 TRB 不被它拥有（Cycle 不符）时，必须报 Ring Underrun（code 14）；不认识的 TRB 类型报 TRB Error（5）。因此"环里除 link TRB 外没有任何 owned TRB + 写门铃"这一设置下，"有事件"与"无事件"分别唯一对应"控制器读过这条环"与"从未读过"，与 TRB 内容、Cycle 编码、设备是否回话完全无关。
- 旧状：所有既往证据（`evs=…/02`、`dq0` 零前移、端点仍 Running）都只能证明"没有完成事件"，无法区分"门铃没送到"与"设备不回话"。
- 位置：`ep0_underrun_probe()`（`control_xfer` 之前定义），只在 `att_fail()` 末尾、`g_ur == 0` 时调用一次；`ep0_recover()` 先把环清成空环再探；结果写 `g_ur`/`g_ur_code`，报告页 `ur=<状态>/<code>`。
- 验证：探针本身在 QEMU 上**不能被验证**——测试镜像 `3a9e57b30431` 把探针挂在每次 `address_device()` 之后，QEMU 照常枚举（`hub slot=1 rootport=5`、`HID keyboard application`、`HID interface ready`），而探针回 `empty ring produced no event at all`：QEMU 的 xHCI 在环内无 owned TRB 时直接停止取环、不产生 underrun（与它对畸形 TD 不报错是同一个宽松点）。故 `ur=2` 在模拟器上是常态、在真机上是结论；注入已移除。

### U-58 门铃前后读 EP0 dequeue，并记录前 8 个事件与故障现场

- 要求：控制器是否取走 TD，唯一的直接观测量是设备上下文里 EP0 的 dequeue 指针是否前移（`dq` 的低 4 位为 DCS）。门铃写前与写后各读一次，2 ms 间隔足以覆盖正常控制器的一次取环；两次相等即"这条 TD 从未被取走"。
- 旧状：`dq0` 只在失败后被读一次，没有"写门铃前"的对照，也没有任何"门铃实际写到哪个地址"的记录；事件只计数不入日志，故障现场只有一张照片。
- 位置：`control_xfer()` 门铃写入段（`g_db_a/g_dq_pre/g_dq_post/g_es_post`）、`event_pop()` 的前 8 条事件日志（`g_ev_log`）、首个失败传输的 `at failure: portsc=… ep0deq=… usbsts=…`（`g_psc_at_fail`）；报告页新增 `da=`（写后 dequeue）、`do=`（门铃在 BAR 内的字节偏移）、`e8=`（已入日志的事件数）。第一次写无效时第二次写门铃并复查（"门铃写丢"的昂贵假设，顺手证伪）。
- 验证：镜像 `6e34ff6f27e8`（`bar_off=0x2004 slot=0x1 deq=0x01c1d001/0x1 -> 2ms 0x01c1d031/0x1 (ring fetched)`，dequeue 前移 3 个 TRB）；同版诊断页 `da=01c1d031 do=2004 e8=8 … ur=00`，枚举与输入不受影响（`at=00000990`，鼠标与键盘均识别）。首版 `bee1b394baeb` 把"写前"快照错放在门铃之后，读数恒等，已修正。

### U-59 日志写完必须让介质把缓存落盘

- 要求：写进介质不等于写进盘。带写缓存的内盘 NVMe 与多数 U 盘在收到 WRITE 后先留在易失缓存，断电即丢；`blkdev_t` 已提供 `blk_flush_fn flush` 钩子（`kernel/blkdev.h`），写完就必须调用它。
- 旧状：`usblog_flush()` 写完扇区后从不调用 `g_dev->flush`。这台机器的内盘日志（照片里 `log=1 dev=nvme0 sec=45`）在上一轮被直接读回时为全零，正是这条缺口。
- 位置：`kernel/usblog.c` `usblog_flush()` 末尾，`written > 0 && g_dev->flush != NULL` 时调用 `g_dev->flush(g_dev->ctx)`。
- 验证：QEMU 的 virtio 盘不提供 flush 钩子，因此该路径只能在真机上体现；实现按"确实写出过字节"才调用，无写出时不产生额外等待。

### U-60 逐口探针行 + EP0 停止事件 code：区分"没走到"与"走到了卡死"

- 要求：后台端口遍历下，"接收器所在口根本没被走到"与"走到了停在第几步"在报告页上无法区分——页内数组只给被走到的口填值，没走到的口读 `0`，和空口同形。每个口一行 `probe port` 把游标位置与终态落到串口/磁盘日志；EP0 收到 Stop Endpoint 命令后的每一条完成事件也要记录 code，因为"端点挂着一条未完成 TD"与"环根本没被取走"在 `ur=2`、`da==do` 时外观相同，而该 slot EP0 的一条 Transfer Event 本身就是控制器拿过 TD 的证据，code 26 = 带未完成 TD 停止（设备没应答 SETUP），其余 = 传输完成后迟到上报。
- 旧状：事件只计数不入日志；失败现场只有一张照片，端口覆盖范围不可考。
- 位置：`hub_port_probe()` 末尾 `[USB] probe port <p> speed=<s> step=<k> ok|failed`（`g_att_step`）；`ep0_wait_stopped()` 内本 slot/EP0 事件 code 与 resid 日志（`g_stop_ev_n` 上限 8 条）。
- 验证：QEMU 下探针行格式正常（镜像 `938E7430318E`：`probe port 5 speed=3 step=9 ok`、`probe port 7 speed=3 step=9 ok`）。真机镜像 `10C37B18AE92` 的首启照片 `at=3000000000000000`（仅口 1 走到 step 3，口 2…16 未走到）；当次探针行落在 RAM、因内盘写拒绝（`log=2 pend=0x4e0e`）未回读到磁盘日志，真机探针读数待下一次可写介质或报告页照片复核。

### U-61 报告页停留期间切片喂端口遍历续跑（历史记录：报告页已移除，见 B-5）

- 要求：端口遍历的续跑挂在输入 tick 的 `usb_hid_poll()` 上（`g_hub_done==0` 时 `usb_hub_step(40)`，口预算 250 ms）；而诊断报告页在 `usblog_state()!=1` 时阻塞停留 45 s，期间不跑任何 tick，其余端口永远走不到——真机照片里 `at=3000000000000000`（游标停在口 2）正是这个缺口：用户拍完照即关机，续跑拿不到机会。
- 旧状：报告页整段 `nd_demo_report(rep, 45000)` 一次性停留，遍历续跑要等报告页结束、调度器桌面 tick 起来之后才发生。
- 位置：`kernel/kmain.c` 报告块重构——`diag_strip()`（logo/log 状态/why/GPU/gap/PS2 与 USB 两条输入横幅）、`diag_report()`（状态条 + usblog_report + HID mice/reports 行 + usb_port_dump + hda_banner + 提示语）；live 分支先首绘报告，`usblog_state()!=1` 时进入 45 s 循环切片：`nd_demo_report_draw(rep)` → `nd_wait_ms(250)` → `usb_hid_poll()`（喂遍历 + 轮询 HID + 热插拔 + 子口 + 介质重试）→ `usblog_retry()` → `usblog_flush(1)` → 重建报告；循环结束 `nwm_repaint_all()`。`graphics/nd_demo.c`：`nd_demo_report` 拆为 `nd_demo_report_draw(text)`（填充/绘制/present）+ 带等待包装；`graphics/nd.h` 补原型。
- 验证：QEMU（q35 + pflash `OVMF_CODE_4M.fd`/`OVMF_VARS_4M.fd`，virtio 盘，qemu-xhci + usb-mouse/usb-kbd）镜像 `938E7430318E`：鼠标口 5、键盘口 7 均 `HID interface ready`、`probe port … step=9 ok`，`hub scan done: ccs=2 cn=2`，桌面设置/文件管理器窗口与指针截图正常；旧对照镜像 `10C37B18AE92` 同 harness 同样枚举正常。镜像已写入笔记本 SanDisk 盘并回读 `match=True`。**真机未复验**：尚未从固件选 U 盘启动；报告页 45 s 切片循环在真机介质拒绝写（`state=2`）条件下的时序与端口覆盖待拍照复核。
- harness 教训（非源码回归）：pc/i440fx 机器 + 单块 `-bios OVMF.fd` 会在 `mounts applied` 之后访问 Local APIC SVR（`cr2=0xFEE000F0`）#PF 停机，新旧镜像同址复现；q35 + pflash 4M 成对固件启动正常。复验必须用旧模板：`-machine q35` + CODE/VARS 两个 pflash。

### U-62 控制传输入出口逐笔入日志 + EP0 每条完成事件带 code/resid（本轮诊断版）

- 要求：真机 Intel 控制器（808651ed）上五个有设备的端口全部停在枚举中途，端口 6 走到 step 4（第一笔 8 字节 GET_DESCRIPTOR 成功、第二笔 9 字节 config header 超时），端口 1/3/7/10 在 step 3 就超时；同时 14 次 Stop Endpoint / Set TR Dequeue 全部返回 `rcf=00 rce=00 rde=00` 且无完成事件。这组读数只能证明"控制器知道有 pending TD"，无法证明第二笔传输是否曾被控制器取走、以及它回来的是什么码——因为事件日志只保留开机后的前 8 条，EP0 的事故现场永远排在窗口外。
- 旧状：`event_pop()` 只把前 8 条事件入日志；`ep0_wait_stopped()` 只记录停止等待窗口内的 EP0 事件；控制传输本身除失败那条外不留痕，成功一笔后环指针推进到哪、下一笔从哪个 idx/cycle 追加，事后不可复原。另需纠正一条判据：门铃后 `dq0` 不前移在 Intel 控制器上是正常行为（不写回 context dequeue），U-58 的"不前移 = TD 未被取"在这台机器上不成立，`dq0 == ep0lo|1` 不能当作未取证据。
- 位置：`kernel/usb.c` —— `event_pop()` 事件日志上限 `8u` → `48u`；新增静态 `g_ep0_ev_n`（上限 24 条，独立计数，避免 HID 报告流量挤占）；`wait_transfer()` 内 transfer event 的 EP ID == 1 时逐条打印 `[USB] ep0 ev code=<code> resid=<resid> slot=<slot>`；`control_xfer()` 在 `for (pass…)` 入口打印 `[USB] xfer port/step/slot/pass/idx/cy/len/req`，成功返回前打印 `[USB] xfer done port/step/idx/cy`。纯加日志，不改变任何既有行为。
- 验证：`build_main.cmd` exit=0，镜像 67,108,864 B、SHA256 前 12 位 `44AD959E4FB0`；QEMU（q35 + pflash 4M，qemu-xhci + usb-mouse/usb-kbd）逐笔读数正常：鼠标口 5 `step=3…8` 每笔 `xfer done`、`idx` 由 0→3→6→9→B→E 连续推进，`code=0x01` 成功；8 字节后 9 字节读 config 时 `ep0 ev code=0x0D resid=0xCB`（Short Packet，QEMU 返回 44 字节配置），随后 `probe port 5 … step=9 ok`；键盘口 7 同理，`probe port 7 … step=9 ok`，`hub scan done: ccs=2 cn=2`。镜像已写入笔记本 SanDisk 盘，回读 `match=True`。**真机待复验**：下一轮要回答的是"第二笔 9 字节传输回来的是哪个 code（0x01 迟到完成 / 0x0D 短包 / 0x14 Ring Underrun / 0x05 TRB Error / 什么都没有）"，以及失败后 `xfer` 行里的 `idx/cy` 是否与控制器 `dq0` 指向的 TRB 对齐。

### U-63 No-Op TRB 探针 + 首笔传输 3 秒预算：把"环没被读"与"设备不应答"分开

- 要求：真机 U-62 日志（镜像 `44AD959E4FB0`）把故障收窄到"TD 已在环上、控制器却不产生任何传输完成事件"，但还差一个能自证的判别实验。两类原因在现有读数下外观完全相同：(a) 控制器根本没读这条 EP0 环（环基址/门铃/slot 上下文问题）；(b) 控制器读了 TD、发出 SETUP，设备不应答。前者改端口、改 reset 永远无效，后者是设备/链路问题，方向完全不同。
- 判据：xHCI 传输环上的 No Op TRB（type 23）按规范必须在不产生任何 USB 总线流量时完成，并回报 Transfer Event（completion code 1）。因此 —— 有完成事件 = 门铃、环基址、slot 的 EP0 上下文都被控制器正常读取，真实 TD 的沉默只能是设备没有应答；无完成事件 = 控制器没有在读这条环。
- 旧状：只有"失败后 Stop Endpoint 报 code 0x1A、residual=8"这一条间接证据（说明控制器侧确实挂过一条 8 字节 TD），无法区分它是"被取走但设备不应答"还是"取了却卡在控制器内部"；`ur=2`（空环探针无事件）本身也不能证明环未被读，因为空环本来就不产生事件。另一处时间维度缺口：所有传输只有 1 秒预算，而 1 秒是本驱动的上限、不是控制器的上限，控制器本该自己回报的 Transaction Error / Stall 可能排在预算耗尽之后。
- 位置：`kernel/usb.c` —— 新增 `TRB_NOOP 23u`；新增 `ep0_noop_probe()`（静态 `g_noop_done` 保证每 boot 只跑一次，在首个拿到 slot 的设备 Address Device 成功后立刻追加一条 No-Op TRB、敲门铃、等完成，打印 `[USB] noop probe slot/port/rc/code` 并附结论文字；无论结果如何随后 `ep0_recover()` + `ep_ring_reset()` 把端点与环恢复到确定状态，避免污染其后的枚举）；`attach_base()` step 3 的首个 `GET_DESCRIPTOR` 用 3 秒预算跑一次（静态 `g_slow_once`，打印 `[USB] first xfer budget 3000 ms`），之后恢复默认预算。
- 验证：`build_main.cmd` exit=0，镜像 67,108,864 B、SHA256 前 12 位 `BB7AC83323A3`；QEMU（q35 + pflash 4M，qemu-xhci + usb-mouse/usb-kbd）：`noop probe slot=1 port=5 rc=-1 code=0xFFFFFFFF (no completion…)` —— **QEMU 的 xHCI 模型不实现 type 23，直接忽略**，该探针在 QEMU 上永远报"无完成"，不能作为反向证据；同一次启动的后续枚举不受影响，鼠标口 5、键盘口 7 均 `step=9 ok`、`hub scan done: ccs=2 cn=2`，证明探针的超时与恢复路径不会卡住枚举。真机读数待下一轮。

### U-64 零完成事件的端口不再重试 + 退役空环探针 + defer 表扩容

- 要求：真机每一轮都能看到同一种浪费——某口的第一笔控制传输**完全没有完成事件**（不是错误码，是连 event 都没有），驱动仍然把同一笔请求跑第二遍、再把整个 bring-up 做三轮（每轮带一次端口复位、一个全新 slot、两笔传输，各 1 秒预算）。五个有设备的端口 × 3 轮 × 2 遍 ≈ 30 秒，全部落在同一个已知失败上，`deferred phase` 因此涨到 73.5 s。同时 U-57 的空环探针在每轮失败处还会再花 60 ms，而它的判据本身是错的。
- 判据订正：空环（只有一条 Link TRB、没有 owned TRB）无论控制器是否取走环都不会产生事件，所以"空环无事件"不能证明"环未被读"。`ur=2` 曾被当作证据使用了数轮，属伪判据；替代它的是 U-63 的 No-Op TRB（type 23，按规范必须完成）。
- 位置：`kernel/usb.c` ——
  1. `control_xfer()` 第一遍失败且 `code == 0`（无任何完成事件）时置 `g_att_noev` 并直接返回，不再跑第二遍（第二遍只是恢复端点后重复同一请求）；
  2. 端口重试循环读到 `g_att_noev` 即打印 `[USB] port <p>: no completion at all, one bring-up is enough` 并结束该出口，每个口只 bring-up 一次；`attach_base()` 入口清零该标志；
  3. 删除 `ep0_underrun_probe()`（U-57），`att_fail()` 不再调用，报告页 `ur` 字段固定打印 `0xFF` 表示"未运行"；
  4. `USB_DEFER_MAX` 8 → 32：复合设备（键盘+鼠标+多媒体端点）在命令、HID、EP0 合并轮询时可能溢出 8 槽，溢出即丢完成事件；
  5. `ep0_noop_probe()` 的无完成结论措辞改为"ring not read, or No Op not implemented"，两种可能并列，避免把单一读数当结论。
- 验证：`build_main.cmd` exit=0，镜像 67,108,864 B、SHA256 前 12 位 `E3EE0552C8F2`；QEMU（q35 + pflash 4M）：`noop probe slot=1 port=5 … (no completion: ring not read, or No Op not implemented)`，`[INP] usb mice=1`，`probe port 5/7 … step=9 ok`，`hub scan done: ccs=2 cn=2` —— 说明零事件短路逻辑在"每笔都有完成事件"的正常路径上不会被误触发。镜像已写入笔记本 SanDisk 盘，回读 `match=True`。**真机未复验**：`deferred phase` 时长是否回落、五个失败端口的日志是否变成每口一行，待下次启动读数。

### U-13 未实现 / 未验证（如实列出）

- 已实现（旧条目更新）：hub 下游端口枚举与运行期热插拔——`USB_KIND_HUB` 不再"只记录不展开"，hub 保持在线并按 U-40 的语义走完端口、把子设备带 route / TT 上下文交给普通 attach 路径；运行期由 U-53 的子端口轮询接管。QEMU（全速 hub）已验证运行期插入；高速 hub 下的低速/全速子设备（TT 路径）未验证。
- 未实现：USB3 warm reset、SuperSpeed burst、UAS、等时端点、USB4 主机路由。
- 未使用：xHCI 中断，全部轮询。
- 未验证：非 8 字节 EP0 的全速设备（U-8 重试分支）——**U-43 已把"全速按 64 猜、失败退 8"的两条分支都实现在 QEMU 上跑通**，但仍没有真机上的非 8 字节全速设备读数；真机 USB3 端口（`0e63`）上设备的 SuperSpeed 上下文；U-3/U-4/U-5/U-7/U-14 的真机行为（镜像 `29b554249bf7` 尚未在真机上复验）。
- 已知未闭环：真机最近一次全量读数（第九轮镜像 `0221EC6D4108`）：`cn=5`、`rs=0303300330333333`、`at=3030043003000000`、`ec=T.T..TT..T.....`、`ht=0`、`chk=d`、`ev=10/0 sts=00000018`、`evs=17/38/02`、`td0=00030851/00010c11/00001021`、`dc0=08300000/00000001/00400026/0091a001/11000001`、`dq0=0091a001 es0=1`、`rec=00 rcf=0e rz=ff rce=13 rde=00 wms=ffff`。命令通道可用（56 条命令完成）、EP0 传输通道仍不可用（2 个传输事件、dequeue 零前移、地址 0 也无事件）。U-25（时基）、U-24（恢复命令）、U-42…U-48 都已改，**这张读数之后的所有改动都没有真机证据**。下一个真机判据：报告页的 `tmb`/`tm`（毫秒预算是否可信，U-52）、`chk` bit3（CONFIG 是否生效）、`crq` 两个数是否相等（CRCR 是否就是我们写的那条命令环，U-56）、`rsv` 后面有没有 `/rst`（Stop Endpoint 是否等到了 Stopped 事件，U-55）、`da` 是否前移于 `dq0`（门铃是否真的被控制器执行，U-58）、`do`（门铃写在 BAR 内的字节偏移，用来核对我们对 DBOFF 的理解）、`ur`（空环探针：1 = 控制器读过这条环、2 = 从未读过，U-57）、`e8`（前 8 个事件是否已进日志）、`hse`、`try`、`rs` 里有没有 `2`。从 `6e34ff6f27e8` 起另有一条不靠照片的路：内盘 NVMe 的日志段（头 LBA 64、文字 LBA 65 起）在 U-59 之后能活过断电，开机一次后用 `tools/read-usblog` 同族的方式直接读回文字日志，里面会有 `[USB] ep0 doorbell: …`、`[USB] ev#n …`、`[USB] at failure: portsc=… ep0deq=… usbsts=…` 三类行。

