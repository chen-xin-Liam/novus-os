# Changes

## USB：把"端口没走到"和"走到了第几步"分开，把停止事件的 code 写出来（U-60）

上一版 `6e34ff6f27e8` 刷进 U 盘、开机两次之后，真机文字日志已经能直接从内盘 NVMe 读回来（不再靠拍照）。逐字读完那 706 行之后，发现两处**先前被页面掩盖的事实**，都不是设备的问题：

- **旧一版（`9d3bd6f317ba`，日志里的第二次开机）其实读通了接收器的设备描述符。** 那次 `at=3030043003000000` 里端口 1/3/7/10 是 `3`（停在设备描述符），而**端口 6（Logitech 接收器）是 `4`**——即 step 3（读 8 字节设备描述符）**成功了**，失败出现在 step 4（读 9 字节配置描述符头）。这与"这台机器上 EP0 完全不工作"是两件事：EP0 至少在这颗接收器上跑通过一次。"四个端口全停在 step 3"这个持续了十几轮的结论，本身就掺了一个走得更远的端口。
- **当前这一版（`6e34ff6f27e8`）那次开机里，接收器所在的端口根本没有被走过。** 报告页 `at=0000000000000000`（端口 2…16 全是 0），而 `at=3` 只出现在端口 1（摄像头）。原因是端口遍历的续跑挂在输入 tick 上，而 deferred 阶段结束到报告页画出来之间（+12.6 s → +50 s）没有任何输入 tick——于是"端口 1 用掉 700 ms 预算 → 剩下的移交输入 tick"变成了"移交之后没人接"，接收器那口一次都没试。

这一轮只做两件让下一次开机自己说话的事，不加任何猜测性补丁：

- **U-60a：端口遍历每走完一口写一行。** `hub_port_probe()` 在 `xhci_attach_port()` 返回后打印 `[USB] probe port <p> speed=<s> step=<k> ok|failed`。理由是报告页的 `at`/`rs` 数组只能表达"这一口停在第几步"，而**没被走过的端口读出来也是 `0`**，两者在页面上同形；文字日志里一行一口，才能分清"走过了、停在哪"和"压根没走"。`step=9` 是既有约定：9 = 整条 bring-up 成功。
- **U-60b：Stop Endpoint 之后，这条 EP0 产生的每个完成事件连同 code 都写出来（上限 8 条）。** `ep0_wait_stopped()` 原先只认 `code==26`（Endpoint Stopped），其它 code 静默丢弃，而"端点当初有没有握着我们的 TD"恰恰只能由这些事件证明：控制器**只会**在把一条 TD 交还软件时才写 Transfer Event，所以只要出现一条属于本 slot EP0 的事件，就说明门铃送到的、环被取走了、TD 被控制器执行过；`26` = TD 未完成就被停（设备没回 SETUP），别的 code = 其实完成了、只是报晚了。这正好补上 `ur=2`（空环探针无事件）与 `es0=1`/`da==do`（dequeue 零前移）之间那个自相矛盾的缺口——单看页面，这两种读数长得一样。
- 验证：构建 `exit=0`（增量编译，只重编 `usb.c`，无新增警告），镜像恒 67,108,864 B，**`10c37b18ae92`**。QEMU（qemu-xhci，鼠标根口 5、键盘根口 3）：`probe port 5 speed=3 step=9 ok`、`probe port 7 speed=3 step=9 ok`、`HID pointer 0x20 bits, relative`、`HID interface ready` ×2、`hub scan done ccs=2 cn=2`、`ep0 doorbell … (ring fetched)`；全程无 `transfer timeout`、无 `ep0 stop event`（正常路径不该有失败，故该行未触发，与新代码一致）。
- 未做 / 未验证（如实）：① **真机仍未验证**——这两条都只是把下一次开机的日志变成可判读的证据，本身不修任何东西。② 端口遍历在 deferred 阶段与报告页之间的空档仍未解决（本机没有第二个上下文，续跑只能挂输入 tick）；桌面起来之后它确实会继续走，但 45 s 报告页期间不推进，这一点仍写在 B-12 补充条款里。③ `probe port` 行数 = 控制器端口数（本机 16+4），一次开机约 20 行，日志段够用但比上一版长约 1 KB。④ 停止事件的 code 上限 8 条：四个端口 × 两遍重试可能超过，超过后的样本会丢。⑤ 源码仍未提交。

## USB：门铃到底有没有把控制器叫醒——一次开机就能答的实验，以及让日志活过断电（U-57…U-59）

上一版 `9d3bd6f317ba`（U-54…U-56）刷进 U 盘之后用户回"鼠标依旧不可用"。这一轮先做取证复核，结论很不客气：**这台机器上一次开机没有留下任何一份文字证据**。U 盘 LBA 51200 处是空的（`no Novus boot log at LBA 51200`，说明那次运行内核从未写到 U 盘，与 U 盘枚举不了同源），内盘 NVMe 的日志段（头 LBA 64、文字 LBA 65 起）同样全零——而更早版本的照片里明明有过 `log=1 dev=nvme0 sec=45`。也就是说：日志**写过了，但活不过断电**。

这一轮不猜电路，也不再加"顺手调一下"式补丁：只做一件能定性的事，把"控制器根本没读过我们的传输环"和"环读了、设备不回话"这两件事分开；同时把日志通道修到能从 Windows 侧直接读回来，从此不必再靠拍照。

- **U-57：EP0 空环探针——唯一一个不需要设备配合就能判 doorbell 是否生效的实验。** 做法：把 EP0 传输环清成"除 link TRB 外没有任何 owned TRB"，然后照常写门铃。真正读到这条环的控制器**必然**跑出环尾并回一个 Ring Underrun（completion code 14）或 TRB Error（5）；从未读过这条环的控制器什么都不回。TRB 内容、Cycle、设备回应在这一设置下都不参与，所以"没有事件"只有一个含义：门铃/环基址没送到控制器。探针只在 `att_fail()` 末尾跑一次（一次开机一个样本），页面新增 `ur=<状态>/<code>`：`1` = 控制器答了空环（门铃、取环、事件通道全活，问题在线上），`2` = 什么都没回（这正是"门铃没送到"的读数），`3` = EP0 连可探的端点状态都没能建立。
- **U-58：门铃写下去之后 2 ms，读控制器自己的 EP0 dequeue。** 这是第三个独立判据，而且比探针更直接：写门铃**之前**读一次 `dequeue`，2 ms 之后再读一次。指针动了 = 控制器确实取走了 TD（因此静默发生在设备/链路一侧）；指针没动 = 这条 TD 从未被取走。若第一次写没有效果，再写一次门铃并复查（"门铃写丢"是个便宜的假设，顺手证伪）。三个数（`bar_off`、写前 `dequeue`、写后 `dequeue`）连同 `slot` 一起进日志，页面同时写 `da=`（写后 dequeue）与 `do=`（门铃写在 BAR 内的字节偏移）。QEMU 上这条测量是先做的，避免自己骗自己：`bar_off=0x2004`、`deq=0x01c1d001 → 2ms 0x01c1d031`（前进 3 个 TRB，`(ring fetched)`）。
- **U-58 附带：事件与故障现场也进日志。** 一次开机的前 8 个事件按 `type/slot/ep/code/p2` 逐条写下（`[USB] ev#n …`），第一个失败的 EP0 传输额外写 `portsc`、EP0 `dequeue/state`、`usbsts`。理由是这台机器"两个传输事件、dequeue 零前移、端点仍 Running"的组合既可能是门铃没到、也可能是设备的 SETUP 被无限 NAK（控制传输对 NAK 不设超时，控制器会一直重试且**不产生任何事件**）——这两种解释要靠 `portsc` 的链路状态（PED/PLS）和 dequeue 是否前移才能分开，而它们现在都会出现在文字日志里。
- **U-59：日志写完必须 flush 设备缓存。** `usblog_flush()` 把扇区写进介质就返回，从不调用 `blkdev_t` 的 `blk_flush_fn`（`kernel/blkdev.h` 有该钩子）。内盘 NVMe 与多数 U 盘都有写缓存，这台机器的日志正是"写过、缓存没落盘、用户按电源键"这条路丢掉的——上一轮在 NVMe 上读到全零就是这个原因。现在只要真的写出过字节，就调用 `g_dev->flush(g_dev->ctx)`（介质没有该钩子时跳过）。
- **撤回一条上一轮的怀疑（重要，防止继续误改）**：`address_device()` 里 `ep0[4] = 8u` **不是**偏差。Linux 的 `EP_AVG_TRB_LENGTH(p)` 定义是 `(p) & 0xffff`（低 16 位），`EP_MAX_ESIT_PAYLOAD_LO(p)` 才是 `((p) & 0xffff) << 16`；`xhci-mem.c` 对 EP0 写的就是 `EP_AVG_TRB_LENGTH(8)`，即 `tx_info = 8`。这一处保持原样，本轮没有改动它。
- 验证：构建 `exit=0`，无新增警告（只有既有的 `net_http.h` 注释警告），镜像恒 67,108,864 B，最终镜像 **`6e34ff6f27e8`**。
  - **U-57 的探针本体在 QEMU 上无法被验证，这一点先说清楚**：把探针临时挂在每次 `address_device()` 成功之后（测试镜像 `3a9e57b30431`），QEMU 照常枚举出 hub 与键盘（`hub slot=1 rootport=5`、`HID keyboard application`、`HID interface ready`），而探针回报 `empty ring produced no event at all`——QEMU 的 xHCI 在"环里没有 owned TRB"时直接停止取环、不产生 underrun 事件（同一原因让它在早期版本里对畸形 TD 一声不吭）。因此 `ur=2` 在模拟器上是常态、在真机上才是结论；这个探针只对真机有意义。测试注入已移除。
  - **U-58 的顺序正确性**：首版把"写前"快照放在了门铃写之后（`bee1b394baeb`），读数因此恒等、等于没测。修正后同一场景（`6e34ff6f27e8`）日志为 `[USB] ep0 doorbell: bar_off=0x2004 slot=0x1 deq=0x01c1d001/0x1 -> 2ms 0x01c1d031/0x1 (ring fetched)`——dequeue 前移 0x30（3 个 TRB），测量方向正确。
  - **U-58 页面**：`6e34ff6f27e8` 根口鼠标+键盘、30 s 诊断页照片：`dq0=ffffffff es0=f da=01c1d031 do=2004 e8=8 rec=00 rcf=00 rty=00 rz=ff rce=00 rsv=0/0 ur=00 rde=00 wms=0000`（`dq0=ffffffff` 是因为本轮没有失败、快照没被填；`e8=8` 说明 8 个事件已进日志；`ur=00` 说明探针未触发）。同一版枚举与输入不受影响：`at=00000990`、`mouse/kbd` 均识别。
  - **U-59**：QEMU 的 virtio 盘不提供 flush 钩子，因此这条只能在真机上体现；代码路径是 `written > 0` 才调用，无写出时不会白等。
- 未做 / 未验证（如实）：① **真机仍未验证**——这一轮的全部读数（`ur`、`da`、`do`、`ev#n`、`at failure: portsc=…`）都还没有真机样本，它们存在的意义就是把下一次开机的文字日志变成判决书。② `ur` 的三个取值里 `2` 在 QEMU 上必然出现，所以它只能与 `da`（是否前移）联读，不能单独当证据。③ 探针会让 EP0 的环在一次失败后被"跑过一遍"，之后该 slot 立刻被 Disable，因此不进入正常枚举路径；但若某台控制器在 underrun 后把端点停住并拒绝后续 Set TR Deq，探针可能污染同一 slot 的第三次尝试——QEMU 无法验证这一点。④ `usblog_flush` 的 flush 在输入 tick 上下文里同步执行，慢介质上可能多一次可见卡顿，未在硬件上量过。⑤ 内盘日志段的写入位置仍是 LBA 64/65，需要固件留下 ≥384 扇区的 GPT 空隙才可用；这台机器的照片曾经显示可用，但是否每台机器都成立未验证。⑥ 源码仍未提交。

## USB：事件必须先分清是谁的——按 slot 认领完成事件，补上 Endpoint Stopped 的等待（U-54…U-56）

上一版（`091ad6bd4f65`）的回归里出现了本轮最干净的一个新 bug：hub 上键盘在口 5、鼠标运行期插进口 2，鼠标枚举完全成功（`xhci HID addr=3 port=5 kind=mouse`、`hub port 2 device attached`），但注入 `mouse_move` 之后串口只出现 `xhci HID report addr=0x2 bytes=0x8`——**键盘拿到了鼠标的报告**，被当成 8 字节键盘包解码，`[INP] usb mice=0`，光标一动不动。这不是"鼠标没枚举"，是"报告投递错了人"。

- **U-54：完成事件的归属必须带 slot，`(dci, epid)` 这个键本身是错的。** 规范里 Transfer Event 的 Slot ID 在 `TRB[63:56]`、Endpoint ID 在 `TRB[55:48]`；Linux 的 `handle_tx_event()` 先按 slot 找到 `xhci->devs[slot_id]`，再在它的 endpoint 数组里按 EP ID 取 ring，**两个维度缺一不可**。这份驱动的 `event_pop()` 只是把共享事件环里的下一个事件弹出来，`wait_transfer()` 与 `usb_hid_poll()` 都只比对 epid，于是"键盘和鼠标都用 interrupt IN EP1"时 dci/epid 完全相同，谁先被轮询谁就把对方的报告吃掉。等待队列 `usb_defer_push/take/drop` 同样只用 `(dci, epid)` 当键，被误领的报告连"寄存后归还"这条退路都没有。现在：
  - `usb_defer_t` 增加 `slot` 字段，三个队列函数全部改成 `(slot, dci, epid, …)` 三键匹配；
  - `wait_transfer()`、`ep0_wait_stopped()`、`usb_hid_poll()` 三处 `event_pop()` 消费点都从事件里取 `slot = (ev.p[3] >> 24) & 0xFF`，与 `u->slot` 不等就按事件自带的 slot 存进等待队列，不再"认领别人的完成"；
  - `wait_cmd()` 里那处纯寄存也改成用事件自带的 slot，保证寄存方与取用方的键一致。
- **U-55：Stop Endpoint 之后不等 Endpoint Stopped 事件就发 Set TR Deq。** Linux `xhci_urb_dequeue()` 的顺序是：先 `xhci_stop_endpoint_sync()` 等 "Endpoint Stopped" 的 transfer event（`COMP_STOP`/`COMP_STOP_INVAL`），确认端点真的停在环上某一点，再 `xhci_set_dequeue()` 把 dequeue 指针挪到当前 TD 之后。旧代码把 Stop 命令丢出去、立刻换 dequeue 指针：真机上如果端点还在跑，两边同时改 dequeue 语义，控制器可以整条 TD 都不报事件——正是"命令通道可用、传输通道静默"的形态。现在 `ep0_recover()` 用新增的 `ep0_wait_stopped(u, 50u)` 等 `code==26` 的事件（同样按 slot + EP0 匹配），等不到就记 `g_ep0_rst=1` 并在报告页 `rsv=` 后面写出 `/rst`，然后再照旧 Set TR Deq——**先保证顺序对，再保证"等不到"这件事自己是可见的**。
- **U-56：报告页加 `crq=`，把 CRCR 自检从"一个比特"变成两个数。** `chk` 的 bit1 检查"CRCR 落在我们的命令环里"，而 CRCR 读回来的是**命令环 dequeue 指针**（不是我们写进去的 base），真机上一旦 bit1 为 0，照片无法区分"寄存器读成 0（op 寄存器窗口不可达）"和"指针跑到别处"。现在 `chk=` 后面紧跟 `crq=<CRCR 低 32 位>/<cmd_phys 低 32 位>`，两个数相等即证明 CRCR 就是我们在写的那条环。
- 验证：构建 `exit=0`，无新增警告（只有既有的 `net_http.h` 注释警告），镜像恒 67,108,864 B。
  - **U-54 正向**（镜像 `ecaaba4c7477`；hub 根口 1、键盘开机在口 5、60 s 后 `device_add usb-mouse,port=1.2`）：`hotplug: hub port 0x2 connected speed=0x1` → `HID pointer 0x20 bits, relative` → `xhci HID addr=3 port=5 kind=mouse` → `HID report addr=0x3 bytes=0x4`（**鼠标拿到自己的 4 字节报告，不再是键盘的 8 字节**），注入 `mouse_move 120 90` + `mouse_move 60 40` 后光标 (512,256)→(698,373)，桌面截图光标可见。
  - **U-54 反向**（镜像同上；鼠标开机在口 5、60 s 后 `device_add usb-kbd,port=1.2`）：`mouse_move 80 60` → `HID report addr=0x2 bytes=0x4` → `[NDQ] pop#1 type=1 key=0 at 592,316` → `cursor set#2 to 592,316`（512+80、256+60，逐位精确）；`sendkey a` → `HID report addr=0x3 bytes=0x8` → `[NDQ] pop#2 type=4 key=97` → `NWM key -> win`——**键盘的 8 字节报告没有被鼠标抢走，按键真的被解出来了**。
  - **U-54 混拓扑**（镜像 `ecaaba4c7477`；鼠标在根口 2、键盘在 hub 口 5、U 盘在根口 4）：`xhci HID addr=3 port=5 kind=keyboard`、`addr=4 port=6 kind=mouse`；`mouse_move 70 -50` 后光标 512,256→582,206、`sendkey b` 后 `[NDQ] pop#2 type=4 key=98`。
  - **U-54 多鼠标蓄积**（镜像 `ecaaba4c7477`；根口鼠标 + 运行期 hub 口 1.2 第二只鼠标 + 键盘）：第二只鼠标 `addr=5`；连续 12 次 `mouse_move 20 10` 之后光标 512,256→**752,376**（240/120，位移一次不丢），`[NDC] cursor set` 因限流只打印了 3 行，但最终坐标证明 12 份报告全部按 slot 归到同一只鼠标。
  - **U-54 + U-49/U-51 同场**（镜像 `ecaaba4c7477`；hub + 鼠标口 2 + 键盘口 5 + U 盘根口 4 开机即在）：先 `no writable medium` 两次（正是启动竞态）→ `[LOG] boot log -> usb00 at raw LBA 0xC801` → `[BOOT] report page skipped: the log is on the medium`；停机读那块 64 MiB raw：头部 `NOVUS boot log (local test build) / device: usb00`，文字段 **38 个扇区非零**，首行 `[T] +55ms kmain start`；同时 `HID report addr=3 bytes=4`、光标 512,256→602,326→562,296（+90/+70、-40/-30 精确）。
  - **U-55**（故障注入镜像 `7f1fbe6b85a1`，`control_xfer` 首笔强制 timeout）：日志 `ep0: no endpoint-stopped event within 50 ms, dequeuing anyway`，随后 `HID pointer`、`HID report addr=1 bytes=4` 到达，桌面正常——**恢复路径走通，且"等不到 Stopped"这条分支自己会说话**（QEMU 6.2 在 50 ms 内不产生 Stopped 事件，真机上则应等到）。注入已移除。
  - **U-56**（镜像 `9d3bd6f317ba`，空控制器 + hub + 键盘、无日志介质）：诊断页 `chk=f crq=00372000/00372000`——CRCR 与 `cmd_phys` 逐位相等；同页 `tmb=1 tm=000a`、`hse=0 try=0`、`q0=` 字段正常。该镜像上再做一次运行期 hub 插鼠标：`HID report addr=3 bytes=4`、光标 (512,256)→(572,296)。
- 未做 / 未验证（如实）：① **真机仍未验证**——本轮全部结论来自 QEMU 6.2（`qemu-xhci` + `usb-hub`），真机的 slot 分配、slot 复用（设备拔出后槽位回收顺序）与这份驱动的假设是否一致，只有刷盘开机才能回答。② `ep0_wait_stopped()` 的 50 ms 预算在真机上是否够（Intel PCH 的 Stop Endpoint 完成时间未测），不够时页面会显示 `rsv=…/rst`，届时按该字段调整。③ `usb_defer` 表只有 8 个槽，两台以上 HID 设备同时被别的消费者打断时有被覆盖的可能（`usb_defer_push` 满则丢弃），未做压力测试。④ hub 上同时插两只鼠标时"两只都在动同一个光标"没有做去重/合并，符合规范但没有在硬件上验证手感。⑤ U-56 只是把证据变多，不改变 CRCR 行为。⑥ 源码仍未提交。
## USB：让日志一定落得到介质上，并给"时基是否可信"一个可拍照的答案（U-49…U-53）

这一轮的出发点是一条此后反复出现的事实：真机拿不到日志，唯一证据是一张 45 秒诊断页的照片。而"日志为什么落不下去"与"鼠标为什么不能用"是同一件事的两面——日志要写到 U 盘，U 盘要和鼠标走同一条 EP0 路径。所以先把日志通道堵死的几处缺口补完，再给页面加一个能远程判断"超时预算到底有没有意义"的字段。

- **U-49：热插拔成功的分支不重选介质，也不落文字。** U-39 只在启动端口遍历之后选一次介质；在一台"唯一可写介质就是 U 盘"的机器上，U 盘几乎总是后插的，于是那一次必然落空，此后不再重选。现在 `xhci_hotplug_scan()` 的 attach 成功分支里补 `usblog_retry()`，并且**紧接着 `usblog_flush(1)`**：`usblog_header()` 只写头部那一扇区，文字段要等下一次周期性 flush 才出去，而刚开机的机器没有"下一次"。强制 flush 把从 `+55ms kmain start` 起的整段启动日志一次写进去。
- **U-50：报告页退出后没有整屏重绘。** `nd_demo_report()` 直接写 framebuffer 并 `nd_present(全屏)`，窗口管理器的 damage 集合对此一无所知，桌面起来以后只重绘自己认为脏的区域，于是"窗口边框和标题栏在、内容区纯白"。现在 `wm/novus-wm/nwm.c` 增加 `nwm_repaint_all()`（一个待消费标志），`nwm_tick()` 开头把它变成一次全屏 damage 并打印 `[NWM] full repaint requested by an outside writer`；`kernel/kmain.c` 在报告页返回之后（仅"没有日志介质"分支）调用它。
- **U-51：启动竞态导致的"永远找不到介质"。** 介质搜索在 `usb_init()` 阶段跑，而枚举已经搬到首帧之后（U-37）：U 盘明明是开机就在的，搜索时它还没被枚举出来，之后再没有人回头看，日志就永久停在这台机器的内存里。现在 `usb_hid_poll()` 每 64 个输入 tick 重试一次（最多 16 次，覆盖桌面头一分钟），成功即强制 flush。
- **U-52：报告页新增时基自检 `tmb=` / `tm=`。** 这份驱动里每个超时都是 `cpu_uptime_us()` 的毫秒预算；一台标定没跑起来的机器，所有等待要么立刻到期、要么永远不到期，而页面看起来和"控制器从不回应"一模一样。新字段用两次采样夹住一段"本应耗时 10 ms"的 `cpu_delay_us` 忙等：`tmb=1 tm=000a` 是活钟且刻度正确，`tmb=0` 是死钟。这样下一张照片就能把"超时预算"这一层从嫌疑名单里划掉或者钉死。
- **U-53：hub 下游端口的运行期热插拔。** 根端口有控制器的 `PORTSC.CSC`，hub 的下游端口没有任何控制器寄存器能报告变化——Linux 用 hub 的状态变化中断端点加工作队列做，这份驱动没有那条通道。旧状是只在 hub 刚配置好时走一遍 `hub_walk()`，之后插在扩展坞/hub 上的鼠标永远不会被认出来。现在把单口的那一段（复位、取速度、组织 route/TT、交给 `xhci_attach_port()`）从 `hub_walk()` 里抽成 `hub_port_attach()`，两条路径共用；新增 `hub_child_step()`：每 96 个输入 tick 探一个端口（一次 `GET_PORT_STATUS`，250 ms 预算），游标跨 tick 保留，整轮扫完再从头来。为了让"重扫"是幂等的，`route_in_use()` 先查这条 route 上是否已经有活设备，已枚举过的端口既不复位也不重复登记。
- 验证：构建 `exit=0`，无新增警告（只有既有的 `net_http.h` 注释警告），镜像恒 67,108,864 B。
  - **U-49**：镜像 `3a9ddcb584b6`，`qemu-xhci` 空控制器起机 → 报告页 → 桌面 → 运行中 `device_add usb-storage,drive=stick,bus=usb-bus.0,port=4`：日志 `hotplug: device on port 0x4 attached` → `[LOG] boot log -> usb00 at raw LBA 0xC801`；停机后直接读那 64 MiB 原始镜像：LBA 0xC800 = `NOVUS boot log (local test build) / device: usb00`，文字段 47 个扇区非零，首行 `[T] +55ms kmain start`——**整段启动日志真的写进了热插拔上来的 U 盘**（未加 flush 时同样场景文字段为 0，只有头部）。
  - **U-51**：镜像 `ed1477e4d79e`，U 盘在开机时就在（`-device usb-storage` 直接在命令行）：日志先出现两次 `no writable medium, log stays on screen only`（正是竞态），随后 `[LOG] boot log -> usb00 at raw LBA 0xC801`，停机后读镜像得到 36 个非零文字扇区，首行同上。
  - **U-52**：镜像 `0c23a8b17c2d` 的 40 s 诊断页照片：`tmb=1 tm=000a`（10 ms 忙等测出 10 ms，时基活且刻度正确），同页 `chk=f`、`hse=0 try=0`。
  - **回归**（镜像 `0c23a8b17c2d`，`usb-hub` 在根端口 1 + 鼠标 hub 口 2 + 键盘 hub 口 5）：`hub ports=8 char=0x000A` → `hub port 2 device speed=1` → `HID pointer 0x20 bits, relative` → `xhci HID addr=2 port=5 kind=mouse` → `addr=3 port=5 kind=keyboard` → `hub scan done ccs=1 cn=1` → `+1260ms usb hub done`；`rs=33330333`、`at=00009000`、`chk=f`，与上一版逐字一致。
  - **终版回归**（镜像 `ed1477e4d79e`，U-49…U-52 全量）：① 双层 hub（`usb-hub` 根口 1、第二层在 1.2、鼠标在 1.2.3）：`hub slot=1 rootport=5 level=1` → `hub port 2 device speed=1` → `hub slot=2 level=2` → `hub port 3 device speed=1` → `HID pointer 0x20 bits, relative` → `xhci HID addr=3 port=5 kind=mouse` → `hub port 3 attached` / `hub port 2 attached` → `+1278ms usb hub done`；连续两次 `mouse_move` 后光标落在 (912,552)、`xhci HID report addr=3 bytes=4`，文件管理器与设置窗口内容完整。② 根端口（鼠标口 2、键盘口 3）：`xhci HID addr=1 port=6 kind=mouse`、`addr=2 port=7 kind=keyboard`、`hub scan done ccs=2 cn=2`；只注入鼠标移动时 `NWM totals w=3 s=67 t=105`、光标移动、窗口正常。
  - 观察到但未归因（不在本轮 USB 范围内）：注入 `sendkey a` / `sendkey b` 之后桌面变成右侧图标列表、三个窗口不再显示，且 `NWM totals` 不再出现；同配置只注入 `mouse_move` 时窗口正常。暂按"键盘按键触发了外壳的应用列表视图"记录，未继续追。
  - **U-53**：镜像 `49e1c078befe`。① 开机只有 hub（根口 1，口上空）：60 s 后 `device_add usb-mouse,bus=usb-bus.0,port=1.2` → `hotplug: hub port 0x2 connected speed=0x1` → `hub port 0x2 device speed=0x1` → `HID pointer 0x20 bits, relative` → `xhci HID addr=2 port=5 kind=mouse` → `hub port 0x2 device attached`；注入 `mouse_move` 后 `xhci HID report addr=2 bytes=4`，光标从 (512,256) 移到 (698,373)——**运行期插在 hub 上的鼠标真的能用**。② 幂等性：hub + 鼠标(口 2) + 键盘(口 5) 开机即在，跑满 60 s：`device attached` 全程只有 2 次（没有重扫导致的重复复位/重复登记），注入后光标照常移动、窗口内容正常。
  - 新增 `tools/read-usblog/read-usblog.ps1`：把一个镜像文件或物理盘里的日志段读成文本，省掉抄照片（对未写过日志的镜像会明确报 `no Novus boot log`）。
- 未做 / 未验证（如实）：① **真机仍未验证**，四条改动只有 QEMU 与日志正文作为依据。② `usblog_flush(1)` 在输入 tick 上下文里同步写扇区（一次最多 255 扇区、按 8 扇区一批），真机上一个慢 U 盘可能让这一帧明显卡顿一次；未在硬件上量过。③ U-51 的重试上限是 16 次，超过一分钟才插上的 U 盘只能靠 U-49 的 CSC 路径，若那块盘是"枚举了但一直没报 CSC"（例如开机就在、只是慢），一分钟后再也拿不到日志。④ U-52 只能证明 TSC/时基"动了并且刻度对"，不能证明它在长跑中不漂移。⑤ 运行期 hub 子端口扫描是一口一 tick 的轮询（U-53），一个 8 口 hub 约每 96 tick 才把全口看完，插上到认出有一段延迟；高速 hub 下的 LS/FS 子设备 TT 路径仍只能在真机上验。⑥ `ep0_recover()` 的恢复顺序（Stop Endpoint 后不等 Stopped 事件就发 Set TR Deq、未 Reset Endpoint）仍未改。⑦ 源码仍未提交。

## USB：把控制传输按 Linux 逐条对齐，并把"慢设备"和"卡死的控制器"都当故障处理（U-42…U-48）

用户这轮的原话是"服务器 USB 鼠标还是用不了，给我死磕这玩意直到你给 USB 的 bug 都修好确认能用再找我"。所以这轮不再加特性，也不再改 hub 那一侧：把 EP0 控制传输路径与 Linux `xhci-ring.c` / `xhci-mem.c` / `xhci.c` / `hub.c` 一行一行对完，把对不上的地方全部改掉，再用 QEMU 的两个场景 + 一次故障注入把每条新路径跑通。

- **U-42：`CONFIG.MaxSlotsEn` 写成了槽位数，不是最高槽位号。** 规范与 Linux（`xhci_write_32(&xhci->op_regs->config_reg, num_slots - 1)`）都是 `MaxSlots - 1`；旧代码把 `hc->max_slots` 原样写进去，等于向控制器索要一个它没有的槽位。QEMU 不作校验，只有真机才会表现为 CONFIG 被丢弃/回读不符。报告页 `chk` 的 bit3 校验同步改成 `max_slots - 1`，否则这项自检会假报。
- **U-43：全速设备的 EP0 默认包长按 8 算，Linux 按 64 算。** `xhci_setup_addressable_virt_dev()` 写明"USB core guesses at a 64-byte max packet first for FS devices"，即超速 512、高速 64、**全速 64**、低速 8。包长决定控制器怎么把 SETUP/数据切成包，8 字节的框架套在一个 64 字节端点上，在真机上会产生丢事务（QEMU 根本不校验包长，所以两种都对）。同时补上 Linux `hub_port_init` 的"assume 8"退路：第一次 8 字节头读失败时，把 slot 以 8 重新寻址再读一次——否则"猜 64"对真实 MPS=8 的设备就是硬失败。
- **U-44：数据 TRB 缺 ISP。** Linux `xhci_queue_ctrl_tx()` 对 IN 数据阶段设 `TRB_ISP | TRB_TYPE(TRB_DATA)`；旧代码只设了 DIR_IN 与 CH。ISP 是"中断在这一 TRB 上只报一次"的语义标记，缺它不影响 QEMU，但真机在多阶段 TD 上的完成上报会与预期不一致。
- **U-45：USB2 端口复位后链路不在 U0 时，只打印不处置。** U-35 只把这件事实写上串口。现在按 Linux `xhci-hub.c` 的 USB2 语义处置：PLS=U3 时用 `LWS | PLS=U0` 把链路 resume 回来；仍是别的值（含 Disabled）就以 `-2` 返回给调用方——"复位本身没成功"和"设备不回应"是两件事，前者应该再复位一次，而不是拿去寻址。报告页 `rs` 因此从 0/1/3 扩成 0=已连接、1=复位被拒、2=复位完成但链路不是 U0、3=口上没设备，空口不再和失败混在一起。
- **U-46：bring-up 只有一次机会，且热插拔路径是另一份更弱的拷贝。** Linux 给三遍（`GET_DESCRIPTOR_TRIES`），每遍之间 `msleep(100)` 递增并且**重新复位端口**，因为不回应往往只是设备还没从总线复位里缓过来。旧的两次尝试之间只等 100 ms 且失败在 step<3 时直接放弃；热插拔路径更只有一次复位+寻址。现在统一成 `xhci_attach_port()` 里的三遍阶梯（100 / 200 ms 间隔，每遍前 `Disable Slot` + 重新 `port_reset`），失败时打印 `port N bring-up failed at step S, retrying`；`xhci_hotplug_scan()` 改为直接调用同一个 `hub_port_probe()`，运行期插上的设备走完全一样的阶梯。
- **U-47：两处"控制器状态没等/没清"。** ① `USBSTS.FATAL`（bit 1，HSE，RW1C）是粘滞位，Linux 在 `xhci_handle_event` 的 fatal 分支里清它；这份驱动从来没清过，一次早期坏 DMA 就能让此后所有控制传输静默失败——正是"命令都能完成、EP0 一个事件都没有"的形状。现在每次端口 bring-up 前检查并清除，计数进报告页 `hse`。② HCRST 之后只打印 `xHCNR` 不等待；CNR=1 期间控制器不接受寄存器写，CONFIG 那次写就可能被静默丢弃。现在按 Linux 的 `xhci_handshake(STS_CNR, 0)` 等到清零再编程。
- **U-48：控制请求的预算是 150 ms，对"活着但慢"的设备是硬判死。** 端口遍历期把每个请求压到 150 ms，只为了不卡首帧；但 Linux 给一个已枚举端口上的控制请求 5 s（设备一直 NAK 时继续等）。真机上 `ec=T`（从未完成）与"设备 200 ms 才答"在页面上长得一模一样。现在预算由调用方决定：`boot_deferred()` 用 1000 ms（`USB_WAIT_MS`），输入 tick 续跑时用 250 ms，遍历本身的时间预算不变。
- 报告页新增 `hse=`（清除过多少次 HSE）与 `try=`（最后一个失败端口用掉了第几遍 bring-up）；`rs` 的编码见 U-45；`chk` 的 bit3 语义见 U-42。这样下一张照片就能同时回答三个问题：控制器有没有记录过致命错误、设备是"没答"还是"答慢了"、复位到底成没成功。
- 验证：构建 `exit=0`，无新增警告（只有既有的 `net_http.h` 注释警告），镜像恒 67,108,864 B。
  - 逐项构建验证：`825b139c9c67`（U-42…U-48 首版）→ `d64537c667a5`（**故障注入版**：强制第一次 bring-up 失败）→ 最终 **`29b554249bf7`**（去掉注入并补 `rs` 编码）。
  - QEMU 根端口场景（`qemu-xhci` + `usb-mouse` 口 2 + `usb-kbd` 口 3）：`port 7 connected speed=3` / `port 6 connected speed=3`、`HID pointer 0x20 bits, relative`、`protocol=report report=4 kind=mouse`、`protocol=boot report=8 kind=keyboard`、`hub scan done ccs=2 cn=2`；注入 `mouse_move` + `sendkey` 后 `HID report addr=1 bytes=4` 与 `addr=2 bytes=8` 均到达，光标 (512,256)→(645,378)，桌面正常。
  - QEMU hub 场景（`usb-hub` 在根端口 1、鼠标 hub 口 2）：`ep0 mps=8 (device disagreed with 0x40)` → 重寻址 → `xhci HID addr=2 port=5 kind=mouse`、`hub port 2 device attached`；注入后光标从 (512,256) 移到 (639,383) 并留在窗口上，`usb mice=1`。
  - 报告页（hub 场景 45 s 屏）：`rs=33330333`（只有挂 hub 的那一口是 0）、`at=00009000`、`ec=........`、`chk=f`（**CONFIG 改成 `max_slots-1` 后 bit3 仍然亮，说明自检与写入一致**）、`hse=0 try=0`、`evs=01/0b/1a`、`dw1=00050000`、`dq0=ffffffff es0=f`。
  - 故障注入版（`d64537c667a5`）把第一遍 bring-up 强制判失败：日志出现第二次 `ep0 mps=…`（说明真的重走了描述符）、hub 口鼠标仍然 `HID interface ready`、`hub port 2 device attached`、`usb mice=1`——阶梯能从"第一遍必失败"恢复。这条只在测试镜像里验证，最终镜像已移除注入。
  - 刷盘：`diskpart clean`（disk 4，SanDisk Cruzer Blade）→ 写 `\\.\PhysicalDrive4` 前 67,108,864 B → 回读 SHA256 与文件一致（`29b554249bf7`，`match=True`）。
- 未做 / 未验证（如实）：① **真机仍未验证**。这一轮九处改动全部只有 QEMU 与代码对齐作为依据，金属上没有一条新证据；U-42/U-43/U-44 在 QEMU 里本就不显形（它不校验 MaxSlotsEn、不校验包长、ISP 只影响事件合并）。② 高速 hub 下的低速/全速子设备（TT 路径）仍无法在 QEMU 6.2 构造，`slot[2]` 未上过硬件。③ 三遍阶梯让一个"彻底不回应"的端口最坏要花约 6 s（三次 × 两遍 × 1000 ms + 190 ms），启动期只跑一遍、其余摊到输入 tick（250 ms 预算），真机上若 12 个口全死会明显拖慢后台遍历，但不影响桌面。④ 45 s 报告页规则未动：日志仍写不进介质时，页面期间输入不推进。⑤ 端口复位后的 U3 resume 只在真机 USB2 口上有意义，QEMU 不实现 PLS。⑥ `nd_demo_report()` 结束后不整屏重绘的问题仍未处理。⑦ 源码仍未提交。

## USB：hub 类请求写反、端口供电无条件请求、端口遍历不设预算（U-40 / U-41）

用户这轮说的是"我修好了但是好像没有日志，然后你参考其他开源系统去做 USB 的控制和 USB 鼠标"。前半句先照上一轮自己引入的回归查：把端口遍历塞进 `boot_deferred()` 之后，一个不肯回应的端口就能把桌面、报告页、日志写入一起推到几十秒之后，看起来就是"没有日志"。后半句按 Linux / QEMU 的 hub 实现逐条对，查出两条真错。

- **U-40：hub 类请求的 wValue / wIndex 写反了，端口操作一律 STALL。** `SetPortFeature` / `ClearPortFeature` 的报文是 `wValue = 特性选择子、wIndex = 端口号`（USB 2.0 11.24），旧代码却写成 `(HUB_FEAT_x << 8) | port` 并把 `wIndex` 传 0；hub 拿 `wIndex` 当端口序号做越界检查，0 直接判非法并回 STALL。同一个函数还无条件给每个端口发 `SetPortFeature(PORT_POWER)`，而 `wHubCharacteristics` 的 D1:D0 = `1x` 表示这个 hub 根本不做端口供电——这类 hub 对供电请求也是 STALL 而不是忽略。两处合起来，hub 能读描述符、能做任何端口操作之外的类请求，却无法让任何一个下游设备上电复位。现在五处请求全部改成 `(feature, port)`，供电循环只在 D1:D0 ≤ 1 时执行，否则打印 `no port power switching, ports left as they are` 并跳过 `bPwrOn2PwrGood` 等待。
- **U-41：端口遍历改成有预算、可续跑的状态机。** 新增 `g_xfer_budget_ms`（`wait_transfer()` 的预算取它）与 `usb_hub_step(budget_ms)`：静态游标一次推进一个端口，进入时把每个请求的预算降到 150 ms、退出恢复；`usb_hub_probe_all()` 变成 `usb_hub_step(700)` 的薄包装，剩下的端口由 `usb_hid_poll()` 每 tick `usb_hub_step(40)` 接着走；`hub_walk()` 自带 500 ms 截止并打印"停在哪一口、花了多少"。遍历完成时再调 `usblog_retry()`（U-39 并入这里），这样"遍历期间才枚举到的 U 盘"也能落日志。
- 验证：构建 `exit=0`，无新增警告（只有既有的 `net_http.h` 注释警告）；镜像 **`ddd2172321f4`**（67,108,864 B）。QEMU 6.2、`qemu-xhci` + `usb-hub`（挂根端口 1）、鼠标在 hub 口 2、键盘在 hub 口 5：日志 `hub ports=8 char=0x000A poweron=0x0001` → `no port power switching` → `hub port 2 device speed=1` → `HID pointer 0x20 bits, relative` → `xhci HID addr=2 port=5 kind=mouse` → `HID interface ready` → `hub port 2 device attached`（键盘 addr=3 口 5 同理）；端口段 `first frame +2229 ms` → `usb hub done +2291 ms`，**62 ms**（改前同场景 8 秒级）；注入 `mouse_move` + `sendkey` 后 `[USB] xhci HID report addr=2 bytes=4`、`addr=3 bytes=8` 均到达，光标 (512,256)→(639,383)→(766,510)→底栏，桌面正常。根端口场景（鼠标口 5、键盘口 6）`+2164 → +2217`，**53 ms**，`ccs=2 cn=2`，无回归。
- 未做 / 未验证（如实）：① hub 子设备只在 QEMU 的全速 hub 上验过；**高速 hub 下的低速/全速子设备（TT 路径）无法在 QEMU 6.2 构造**（`usb-hub` 只有全速实现），`slot[2]` 的 TT 字段按 `xhci-mem.c` 语义写好但未经硬件。② 多层 hub 只做了三层上限保护，未实测。③ 150 ms 的端口预算对"响应慢但活着"的设备是硬上限，真机首次 GET_DESCRIPTOR 若超过它会被判失败、留到下一次热插拔重试。④ 续跑挂在输入 tick 上，45 s 报告页期间不推进。⑤ **真机仍未验证**：这台笔记本上四个停在 step-3、EP0 零事件的端口与 hub 语义无关，U-40/U-41 只修掉了 hub 与阻塞两条独立问题。⑥ 源码仍未提交。

## 启动路径：把 USB 端口枚举搬出关键路径，热插拔改成只看变化位（U-37…U-39）

用户问的是"为什么启动的时候 USB 要全部探测一遍，看看其他系统怎么做的"。回答写在条款里，也改在代码里：Linux 与 Windows 都不在启动路径上做端口枚举。

- **U-37：`usb_init()` 不再枚举设备。** 旧实现在首帧之前同步跑完所有控制器、所有端口的 `port_power()` / `port_reset()` / `xhci_attach_port()`（每口还有两次 bring-up 重试和 100 ms 等待），一个不肯回应的端口就能把首帧推到它整段预算之后。现在它只做控制器初始化（HCRST、ring、DCBAA、ERST、interrupter）并读一遍每个端口的 PORTSC 作为快照，然后返回；端口遍历、复位、描述符走查全部落在新增的 `usb_hub_probe_all()`，由 `boot_deferred()`（首帧之后）调用，无桌面分支另有一处调用点。这正是 Linux 的分工：`xhci_pci_probe()` 复位并启动控制器就返回，枚举发生在 `hub_event` 这个 workqueue 里；Windows 也把这段交给 hub 驱动 worker。
- **U-38：热插拔只在 CSC 上动作。** 旧 `xhci_hotplug_scan()` 每 32 个输入 tick 把所有端口读一遍，只要 `connected && !live` 就复位并重试——真机上四个枚举失败的端口会被无限重复位，每次都可能撞满 8,000,000 次迭代预算，就发生在 HID 轮询旁边。现在只有 `PORTSC.CSC`（bit 17，RW1C）置位才复位，清位写 `(1<<17)|PP`（PED 是 RW1CS、PR 是"写 1 动作"，都不碰）；`g_hub_done == 0` 时直接返回，避免与启动遍历把同一端口枚举两次。
- **U-39：枚举之后再选一次日志介质。** U 盘走 USB，端口枚举挪到首帧之后后，启动阶段的 `usblog_init()` 必然看不到它；`boot_deferred()` 里在端口遍历之后补一次 `usblog_retry()`，否则诊断日志永远落不到 U 盘、45 s 报告页每次都会出现。
- 验证：构建 `exit=0`，无新增警告（只有既有的 `net_http.h` 注释警告）；镜像 **`a13c68f42204`**（67,108,864 B）。QEMU 鼠标+键盘：`usb_init` 段 **68 ms → 21–24 ms**（`nvme probe done → usb probe done`，193→261 对比 192→213 / 229→253），首帧之后的端口段 46–51 ms（`usb hub done`）；`port 5/6 connected speed=3`、`HID pointer 0x20 bits, relative`、`protocol=report report=4 kind=mouse`、`protocol=boot report=8 kind=keyboard`、`HID interface ready` ×2、`hub scan done ccs=2 cn=2`、`at=00009900`、`chk=f`、`evs=02/06/0c`、`td0=00030851/00010c11/00001021`、`dw1=00050000` 与基线逐字一致；注入 `mouse_move 700 350` 后 `[USB] xhci HID report addr=1 bytes=4` 到达、光标落在注入点。36 s 内无 `hotplug:` 行、无重复枚举。
- 未做 / 未验证（如实）：① 三处改动仍未在真机上验过（本轮没有新照片）。② 枚举仍是轮询，没接 xHCI 中断；真正并行需要内核线程/workqueue，本内核没有第二个上下文。③ 真机拔插回归没做，QEMU 只能证明"不误触发"。④ 运行期插上的 U 盘不会重新选盘（`usblog_retry()` 只在启动阶段调用一次）。⑤ 首帧整体时间在 QEMU 上有 ±300 ms 波动（`demo init start` 从 842 到 1033 ms），所以只报 USB 段的前后对比，不报整机首帧。⑥ 源码仍未提交。
## USB：把端口链路按 USB2 语义重做，并把丢掉的完成事件接回来（U-34…U-36）

用户第二次反馈"还是不行"。这轮回到代码本身，找的是三处**在模拟器上根本不会显形、只有真机才会踩**的偏差。

- **U-34（本轮最可能的真机主因）：USB2 端口被按 USB3 的语义"唤醒"。** `port_wake()` 与 `port_reset()` 无条件写 `PORTSC = (1<<16) | (5<<5) | (1<<9)`，即把 PLS 推到 5。RxDetect（5）是 SuperSpeed 专属的链路状态，在 USB2 端口的 PLS 字段里是保留值；Linux 对 USB2 端口从不这么写（`xhci-hub.c` 只在 resume 时写 U3→U0，其余用 `PORT_RESET`）。QEMU 的 PORTSC 写处理忽略 PLS，所以模拟器一路正常；真机 Intel PCH 的 USB2 口被推入不该进入的状态后，链路就停在"设备从不回应、TD 零事件"——正是这十一轮反复拍照看到的形状。现在按 Supported Protocol 能力块的 bcdUSB 主版本号建 `hc->port3mask`（Linux 在 `xhci-hub.c` 里也是这样区分 `usb3_ports[]`/`usb2_ports[]`），`port_wake()` 对 USB2 端口直接返回，`port_reset()` 只对 USB3 端口做 RxDetect 前置。
- **U-35：复位完成判据补上链路状态。** 只看 `PED=1` 不够——端口可以"已使能但停在 Polling"，此时 TD 挂上去、设备永远答不上来，驱动只能等到超时后报"一个事件都没有"。非 USB3 端口复位后 PLS 不为 0 时打印 `[USB] port N reset left PLS=… PORTSC=…`，把这件事实写上串口而不是留在猜测里。
- **U-36：等命令完成时把传输完成事件丢了。** 事件环是共享的：`wait_cmd()` 旧实现对 `TRB_TRANSFER` 只计数然后丢弃。于是"EP0 的 TD 在随后的命令等待期间完成"这种时序下，完成被吃掉，`wait_transfer()` 得到"零事件"，看起来像控制器没干活。改为停放进 defer 缓冲，`wait_transfer()` 每轮先查停放区再查事件环；`control_xfer()` 发新 TD 前用 `usb_defer_drop(1,0)` 清掉该端点的残留，避免把上一次的完成当成这一次的答复。
- **同时如实修正上一轮的一个判断**：`hcc=00087001` 的 PPC（bit 3）是 **0**，不是 1——这台机器的端口电源不由软件控制，U-29 的 PP 写零在它身上不会真的断电。U-29 仍然是对的（PPC=1 的控制器会真的断电），但它是"别的平台上的真错"，不是本机主因；核对表里已改正。
- 验证：构建 `exit=0`，无新增警告；镜像 **`49c5f14b43cc`**（67,108,864 B）。QEMU 鼠标+键盘：`port 5/6 connected speed=3`、`HID pointer 0x20 bits, relative`、`protocol=report report=4 kind=mouse`、`protocol=boot report=8 kind=keyboard`、`HID interface ready` ×2、`at=00009900`、`chk=f`、`evs=02/06/0c`、`dc0=08300000/00000001/00400026/00389001/10000001`、`dw1=00050000`、`ep0lo=00389000`、`dq0=ffffffff es0=f wms=0000`——与基线逐字一致，无回归。注入 `mouse_move 600 300` 后 `[USB] xhci HID report addr=1 bytes=4` 到达、光标画在 (645,383) 并留住在窗口上、桌面正常（`/tmp/r130a.png`）。
  另外记下一件事实：qemu-xhci **不报 Supported Protocol 能力块**（串口 `sp=none`、`sn=0`），因此 `port3mask=0`、模拟器里所有端口都按 USB2 处理——改前改后在 QEMU 上等价，U-34 的差别只有真机能显形。
- 未做 / 未验证（如实）：① 这三处仍然**没有一条在真机上验过**，本轮依旧没有新照片。② U-34 无法在 QEMU 里证明（模拟器既不报协议能力块，也不实现 PLS 写入语义）。③ U-35 只是上报，还不改变流程（没做 U3 自动 resume）。④ U-36 的停放区按 `(dci, ep)` 匹配、没有请求序号，理论上仍可能认错两次同端点请求的完成，只能用"发请求前清空"缓解。⑤ 真机若是 USB3 口上的设备，本轮改动不覆盖 warm reset。⑥ 源码仍未提交。

## USB / I2C-HID：对着 Linux 逐条复读，找到端口断电这一条（U-29…U-33，U-26 撤回）

用户反馈"刷了上一版依旧不能用"，所以这轮不再补特性，而是把 `usb.c`、`i2c_hid.c` 按 Linux 的 `xhci.h` / `xhci-mem.c` / `xhci-hub.c` 逐条复读。查出来一条真错、一条自造错、两处真缺陷，外加一处诊断字段。

- **U-29（本轮主因候选）：确认端口变化位时把 PP 写成了 0，等于枚举中途给端口断电。** `PORTSC` 的 bit 9 是端口供电开关，写什么就是什么；旧 `xhci_port_changes()` 只写 `sc & 0x00FE0000`（变化位），PP 位是 0，于是在每次确认 Port Status Change Event 时把每个端口都关了电。Linux 从不这样写：`xhci_port_state_to_neutral()` 保留只读字段与 PP，只清 RW1C 位。真机 `hcc=00087001` 里 PPC=1（供电真的由软件控制），QEMU 报 PPC=0（PP 写被忽略）——这正好解释那个最反常的形状：命令通道完全可用（56 条命令完成、ADDRESS_DEVICE 成功、slot state=2），而**每个端口的第一次控制传输都零事件**。刚做完 Port Reset 的端口，事件环里第一个几乎总是它自己产生的 PSC 事件，`wait_transfer()` 一确认就把电断了，TD 根本没上过线，于是没有传输事件、dequeue 不前进。改为 `(sc & 0x00FE0000u) | (sc & (1u << 9))`。
- **U-26 撤回：速度编码本来就是同一套。** 上一轮的"1↔2 互换"前提是错的。Linux `xhci.h` 在 slot context 的 dev_info 上写着 `Device speed - values defined by PORTSC Device Speed field - 20:23`，`xhci-mem.c` 也是把端口速度直接写进 slot。所以 `1 = 全速, 2 = 低速` 只有一套，互换反而会把全速鼠标声明成低速（控制器按 1.5 Mbit/s 驱动 12 Mbit/s 链路，设备不回应、零事件）。`xhci_slot_speed()` 现在原样返回，函数留着是为了让两处调用点都指向同一段依据。中间还顺手被证伪过一次：把 `slot[1]` 的 `ROOT_HUB_PORT` 从 bits 23:16 挪到 15:8，QEMU 立刻 `cmd failed code 5`（TRB Error）、两个口都停在 step 2（`at=00002200`），已回退并把这次实验记进代码注释。
- **U-30：传输超时不许 HCRST 整台控制器。** 旧 `wait_transfer()` 超时后复位控制器，等于把一个 TD 的故障升级为全部 slot 消失；随后 `ep0_recover()` 的端点命令必然拿到完成码 19（Context State Error），正是真机 `rcf=0e rce=13` 那一对。改为只在 `USBSTS.HCH` 置位时才重启，其余交给端点级恢复。
- **U-31：毫秒预算不再依赖 TSC 标定。** `wms=ffff` 说明早期阶段拿不到时基，所有 10 ms / 1000 ms 都退化成迭代计数（约 0.1 s）。`usb_pace()` 每 64 次轮询用一次 `cpu_delay_us(10)`（走 ACPI FADT 的 PM timer，与 TSC 无关）累加到 `g_pace_us`，`usb_ms_now()` 在无 TSC 时用它，`usb_delay_ms()` 的无钟分支走 `cpu_delay_us(ms*1000)`。
- **U-32：触控板的寄存器必须走非缓存映射。** `i2c_hid.c` 原先直接用物理地址经 write-back 的 identity map 访问 DesignWare/LPSS 寄存器：写 `IC_ENABLE` 留在 cache line、读 `IC_ENABLE_STATUS` 拿到过期 0，控制器"拒绝使能"，后面所有传输都不成立。QEMU 无 cache 所以永远正常，真机 `i2c=0 bus=3` 正是这个形状。改为 `bar64`（回退 `bar[0]`）+ `mm_map_phys(bar, 0x1000, MM_MAP_UC)`，基址 = 映射 VA + `(bar & 0x1FFFFF)`，寄存器访问类型全链路加宽到 UINT64，不再因 BAR 在 4 GiB 以上而跳过。
- **U-33：报告页加 `dw1=`（slot DW1，bits 23:16 是根端口号）。** 照片是唯一证据通道，"这个 slot 属于哪个口"必须能读出来；顺手把 `g_dc0[4]` 的注释改成"slot state 31:27、address 7:0"（原写 31:24 是错的）。
- 验证：构建 `exit=0`，无新增警告；镜像 `b8cf7b01a632`（67,108,864 B）。QEMU（鼠标+键盘）：`port 5/6 connected speed=3`、`HID pointer 0x20 bits, relative`、`protocol=report report=4 kind=mouse`、`protocol=boot report=8 kind=keyboard`、`HID interface ready` 两次、`at=00009900`、`chk=f`、`evs=02/06/0c`、`td0=00030851/00010c11/00001021`、`dc0=08300000/00000001/00400026/00389001/10000001`、`dw1=00050000`、`ep0lo=00389000`、`dq0=ffffffff es0=f rec=00 rcf=00 rz=ff wms=0000`——与基线逐字一致，说明这五处改动在模拟器上零回归。加 `usb-tablet` 的第二组：`at=00009990`、`mi=2`、绝对指针 `HID pointer 0x30 bits, absolute`，注入绝对位置后 `[NDC] cursor set#2 to 639,383`、`[NDC] take#1 damage 512,256 139x148`、桌面正常。
- 未做 / 未验证（如实）：① 五处改动**没有一条在真机上验过**：本轮没有任何新照片，`at`/`evs`/`wms`/`i2c` 的真机读数仍是第九轮那一屏。② U-29 是否真是主因无法在 QEMU 里证明——模拟器 PPC=0，这条路径只有真机的 PPC=1 才走得到。③ U-26 的撤回同样无法在模拟器里证明：QEMU 三台设备都是高速（码 3），全速/低速只在真机出现。④ 真机插拔时 `dw1` 与 `dc0[4]` 能否互相印证，要看下一张照片。⑤ 桌面起来之后指针事件的节流仍观察不到结论（QEMU 里注入两次只有第一次生效），本轮未动输入层。⑥ 源码仍未提交。

## USB / HID：查代码查出来的三处偏差（U-26…U-28）

这一轮没有拍新照片，也没有新的真机读数：把 `usb.c` 与 HID 路径按 xHCI 1.1 / HID 1.11 逐条对着读，找出三处与规范不符的地方，补条款、改代码、在 QEMU 上把能覆盖到的两处做实。

- **U-26：Slot Context 的速度写错了一半设备。** PORTSC 的 PSIV 是 `1=全速, 2=低速`，Slot Context 的 Speed 是 `1=低速, 2=全速`，代码此前把 PSIV 原样抄进上下文，等于告诉控制器"这台全速设备是低速的"。控制器据此用 1.5 Mbit/s 的信号率驱动 12 Mbit/s 的链路，事务在线上就没了，设备不会回应，所以连传输事件都不产生——和本机拍了四轮的那种"一个事件都不回"是同一种形状。新增 `xhci_slot_speed()` 在唯一入口转换，`u->speed` 从此是 Slot Context 编码；判断"要不要复查 EP0 包长"的那处仍用原始 PSIV，两者不混。QEMU 的三台设备都是高速（两种编码在 3/4 上相同），**模拟器覆盖不到**，只能在真机上验。
- **U-27：报告长度和协议对不上。** 旧顺序是"读描述符 → 发 SET_PROTOCOL(boot)"，长度却仍按描述符算，QEMU 串口里 `protocol=boot report=4` 就是这个矛盾——3 字节的 boot 鼠标被要求交 4 字节。对描述符带报告 ID 的设备更严重：boot 报告首字节是按键掩码，被拿去和报告 ID 比，解码直接返回 0，移动被静默丢掉。改为记录 `hid_boot`：只有描述符读不懂时才切 boot，切成功才用固定 3/8 字节；描述符已经判成指针的留在报告协议，滚轮和额外按键因此可用。改后同一镜像串口变成 `protocol=report report=4`（鼠标）与 `protocol=boot report=8`（键盘）。
- **U-28：协议字节为 0 的 HID 接口被整条丢掉。** 这是 U-11 条款里"协议为 0 仍需读报告描述符"在代码里被违背的地方：`xhci_attach_port()` 的 step 7 判据带了 `hid_kind != 0`，而 `hid_kind` 只能由协议字节得出，于是平板、触摸屏、无线接收器、复合设备的第二接口在**读描述符之前**就被扔掉。把一台 usb-tablet 接到 QEMU xHCI 上，旧镜像报告页 `at=00009790` 里该口停在 7 且串口一行日志都没有；改后 `at=00009990`、`mi=2`、`HID pointer: 0x30 bits, absolute, buttons 3`、`protocol=report report=6`，注入绝对位置后 `[NDC] cursor set#2 to 0,0`，光标跳到左上角。
- 验证：构建 `exit=0`，`usb.c` 无新增警告；镜像 `3dcc5584d6df`（67,108,864 B）。基线（鼠标+键盘）`at=00009900`、`mi=1`、`chk=f`、`evs=02/06/0e`、`td0`/`dc0`/`ep0lo` 与第十轮逐字相同，`live root integrity PASS`；用 QEMU monitor 注入移动后指针真的动了（`(1015,265) → (940,355)`），键盘报告 8 字节。加平板的第二组：`mi=2`、绝对指针就绪、指针跟到 (0,0)。
- 未做 / 未验证（如实）：① U-26 只有规范对齐，QEMU 造不出全速设备，真机读数仍是空白。② 这一轮仍然解决不了真机的 EP0 主线——`at` 停在 3、`dq0=ep0lo|1`、`rz=ff` 这些证据没有新的一条，本轮三处改动都不在传输通道上。③ 板载 I2C-HID 触摸板路径（`i2c_hid.c`）本轮没碰。④ 诊断页那 45 s 在真机上照旧，因为日志仍写不进介质。

## 启动速度：按 systemd + kernel/async 的拆分原则重排启动链

### 第二轮：装载藏进动画窗口；Merkle 后置被实测否决

- **B-8，本轮最大的一笔：UEFI 侧装载与启动动画共用一个窗口**。改前是"动画 720 ms 跑完，再花 407 ms 读盘"，两段相加；现在动画的每帧间隙都在读盘。
  - `arch/x86_64/boot/stage3.c`：`stage3_load()` 拆成状态机 `stage3_load_pump(budget_ms)`（START / MERKLE / MODULES / APP / WALLPAPER / LIVE_OPEN / LIVE_SLICES / TPL_OPEN / TPL_SLICES / DONE），每次至少推进一步，预算用完即返回；`stage3_load()` 变成"无预算跑完"，动画路径与非动画路径共用同一台机器（`stage3.h` 新增声明）。
  - 新增切片读 `slice_open()` / `slice_step()` / `slice_result()`：沿 FAT 簇链的连续段一次 Block I/O，**直读进保留页**（去掉 `esp_read_reserved()` 的 pool 缓冲与整镜像拷贝），读满才发布 `g_live_base`，半截镜像绝不交给内核当根。
  - `arch/x86_64/boot/main.c`：`boot_anim_play()` 从第 2 帧起，每帧先 `stage3_load_pump(100u)` 再等该帧截止时间（帧间隔 120 ms，预算留 20 ms 余量）；进度条按字节推进（`stage_at(64 + wrote*14/size, "挂载系统镜像")`）。
  - **顺手修掉一个真 bug**：切片读第一版把 FAT16 簇指针按 12 位掩码截断（`0x0FFF`），`0x1E7D` 读成 `0x0E7D`，读到错误的簇——内核当场报 `integrity MISMATCH`，fnv 由 `0xD5B5…` 变 `0x99F8…`。`fat32_get()` 早把 FAT16 的 EOC 归一成 `0x0FFFFFFF`，所以掩码必须留 16 位（`0xFFFF`）、EOC 统一比 `0x0FFFFFF8`。修正后 fnv 与旧路径**逐字相同**、`integrity PASS` 复现——这条也正是"生成方式换了、必须用另一条路径回读校验"的实例。
  - 结果：`[SLICE] read 8388608 bytes in 8 runs, 19ms`（旧路径同为 8 MiB，但走 8192 次单扇区调用、约 325 ms）；`stage3 load done` 1144 → **817 ms**，UEFI 侧 −327 ms；7 帧落点 0/121/241/361/481/601/721 与截止 0/120/…/720 逐帧对齐，读盘没有造成掉帧。
- **B-9，把 Merkle 校验挪进后置阶段的假设被实测否决**：先按"结论只是告警、可以后置"改了（`kmain.c` 里把 `novusfs_merkle_verify()` 搬进 `boot_deferred()` 第一步），第一轮 QEMU 就给出 `[M3] live root integrity NOT BUILT`——`novusfs_merkle_invalidate()` 在任意写入时清 `sb.merkle_valid`，而挂载与首帧之间正好有 `sysapps_seed()` / `mountcfg_apply()` 的写入；根被写过之后已保存的 root 就失去意义。**已回退**：校验留在 `novusfs_mount()` 之后、所有写入之前。收益仍是记账：`live root mounted` +368（挂载 1 ms）→ `live root verified` +640（**全树重建 272 ms**）→ `live root` +704（字体/图标 64 ms）。
- **B-10，deferred 阶段放进调度器：本轮明确做不到**。`kernel/sched.c` 只有用户态任务入口（`sched_create_user` / `sched_create_user_ex`，需要 `vm_space_t`），没有内核线程原语；加一条内核任务路径要动调度器的栈与切换，还要处理 `novusfs_selftest()`（会格式化并挂回退根）、`nuc_create_spin()`（会起用户任务）对全局状态的假设。这是调度器的新能力，不是启动顺序的改动，未做，如实记在规范 B-10。
- **B-6 在真机的收益仍无数据**；本轮 `usb probe done` 277 ms（占关键路径约 90 ms）。
- 验证：构建 `exit=0`，`stage3.c` / `main.c` / `kmain.c` 无新增警告（仅剩既有 `net_http.h`）。镜像 `95eaf99c32d0`（67,108,864 B）。QEMU 读数：`exit boot services` 1144 → 817 ms；`demo init start` 2077 → 909；`first frame` **1274**；端到端约 2420 → **2094 ms**；后置阶段 1058 ms 全在首帧之后。字节级回归：`fnv 0xD5B5548AA629063E`、`integrity PASS`、`at=00009900`、`ec=........`、`rs=33330033`、`evs=02/06/0e`、`td0`/`dc0`/`ep0lo` 与第十轮镜像逐字一致；首帧后桌面正常（`/tmp/r122.png`）。
- 未做 / 未验证（如实）：① 全部读数仍是 QEMU；真机无串口、`usblog.txt` 仍 0 字节，真机 U 盘 FAT16 + 单扇区固件的读速未知，切片读的**绝对**收益要真机时间标才能定（比例上只会更大，因为省掉的是调用次数）。② 动画仍固定 723 ms（现在它本身就是 UEFI 侧关键路径，缩短属产品决定）。③ 桌面绘制 365 ms 未拆。④ Merkle 272 ms 属 `novusfs` 哈希性能（≈30 MB/s），不是启动顺序问题。⑤ deferred 1058 ms 仍延迟 `sched_start()`，见 B-10。⑥ 镜像 `95eaf99c32d0` 尚未刷盘、尚未回真机。

### 第一轮（2026-11-24 早些时候）

- **先看 Ubuntu 怎么启动（本轮用户要求）**：systemd `bootup(7)` 给出三条可照抄的行为——启动"高度并行"、只用有限序约束；目标链 `sysinit → basic → multi-user → graphical` 每级只等自己依赖的东西；`timers.target` 被 `basic.target` **异步**拉起。内核侧 `kernel/async.c` 的定位是"把独立的硬件延迟与发现操作解耦、不严格串行"，同时要求对外可见部分仍按序。两条合起来落到本篇三条移植原则：关键路径最小化、关键路径只依赖"发布"不依赖"验证"、诊断不是启动特性。规范见新文档 `docs/启动顺序与启动速度规范.md`（条款 B-1…B-8）。
- **先量，再改**：QEMU 串口 `[T] +Nms` 给出改前读数——`kmain start` 55 ms、`ipc ready` 401 ms、`filesystem selftests` **1461 ms**、`lapic init` 1898 ms、`demo init start` **2077 ms**。即内核侧一半以上的时间花在一个叫"selftest"的块上。
- **B-2（本轮最重要的发现，B-2 不是测试是根）**：`novusfs_selftest()` 名为自测，实际是**回退根**——它格式化并挂载全局 `g_novusfs`，而 `novusfs_root()` 在没有 live root 时正是回退到它。所以它不能删，只能"有 live root 时让路"。`kmain.c` 新增 `g_boot_have_live_root`，有 live root 时打印 `novusfs selftest deferred: live root published` 并移入后置阶段，无 live root 时保持原样（这条路径是给"boot stage 没发布镜像"的机器兜底的，不能一起挪）。
- **B-3 后置阶段**：新增 `boot_deferred()`，把 `vfs_selftest`（往真实根写测试文件）、`ext4_selftest`、`nksym`+`nkm`、`ipc_selftest`、`nuc_selftest`+`nuc_create_spin`+`nuc_nx_probe`、`pe_selftest`、`vidmem`+`crtc` 全部移到首帧之后执行，每步一条 `deferred …` 时间标；`ipc_ready` 之后的原位只留注释，防止后人再加一遍。三处调用点（display-only、install、正常桌面）都在 `nd_demo_init()` 之后。
- **B-4**：后置阶段的 `kputs` 不会画到桌面上，因为 `nd_demo.c:1865` 在桌面就绪时已 `fbcon_end()`。未加新机制。
- **B-5 诊断页降级为证据通道**：45 s 的 USB 诊断页改为**仅当 `usblog_state() != 1`（日志没写进介质）才出现**，日志已落盘时打印 `report page skipped: the log is on the medium` 并直接进桌面。真机当前 USB 面板还写不出日志，所以这台机器的页暂时照旧存在——**这道墙要等 USB 修好才真正消失**，不靠调参数。
- **B-6 端口扫描只在状态变化时等待**：`port_power()` / `port_wake()` 改为返回"本次是否真的上电/唤醒"，`usb_init()` 只对**刚变化**的端口做 `port_wait_ccs(…, 40000u)`，其余单次读 PORTSC。依据是 Linux `hub_power_on()` 只为自己上电的端口等 `2*tg`，而 CCS 不会因为读的次数多而变成 1。改前每个空端口白跑 4 万次 MMIO 读（本机 12 个 USB2 口只有 5 个活口）。热插拔重扫保持忽略返回值（32 tick 一轮，下一轮再看）。
- **B-7 时间标补全**：新增 `novusfs fallback root`、`fonts and icons`、`live root`、`mounts and sysapps`、`first frame`、`deferred *` 共 15 条，让 340 ms 的 live-root 块与 36 ms 的 mount/sysapps 块不再是一个合成数字。
- 验证：构建 `exit=0`，`kmain.c` / `usb.c` 无新增警告（仅剩既有 `net_http.h`）。镜像 `f29eda281d3f`（67,108,864 B）。QEMU 读数：`demo init start` **2077 → 908 ms**（同一时间标，−1169 ms，−56%），首帧 `+1276 ms`；`usb probe done` 311 → 283 ms；后置阶段 1276 → 2351 ms（1074 ms，全部在首帧之后）。端口/设备判定与第十轮镜像**逐字一致**（`at=00009900`、`ec=........`、`rs=33330033`、`evs=02/06/0e`、`td0`/`dc0`/`ep0lo` 不变），B-6 只改等待节奏不改判定。首帧后画面正常（`/tmp/r115.png`）。
- 未做 / 未验证（如实）：① 本轮所有读数来自 QEMU，真机无串口、`usblog.txt` 仍是 0 字节，真机三段耗时**没有一个数字**；比例可信、绝对值不可外推。② `usblog_state()==1` 在真机从未成立过，B-5 的"落盘即跳过"只在代码里成立。③ B-6 在真机的收益未测（真机 PCIe 后端的 MMIO 代价比 QEMU 高，空口越多收益越大，但需要真机时间标）。④ deferred 阶段仍有 1074 ms 延迟 `sched_start()`，首帧后 1 秒内没有调度；彻底解法是把 deferred 变成调度器里的低优先级任务（对齐 Linux 的 async probe），需要任务框架支持，本轮未做。⑤ 启动动画固定 723 ms、stage3 装载 407 ms（其中 live root 读取 320 ms）、live root 的 Merkle 校验 340 ms 均**未动**，只登记为 B-8 候选。⑥ 镜像 `f29eda281d3f` 尚未刷盘、尚未回真机。

## USB/xHCI：按规范重做控制路径，并删除"一坏全死"的连锁

### 第十轮（本轮）

- **真机验证第九轮（`0221EC6D4108`）**：`at=3030043003000000`、`ec=T.T..TT..T.....`、`dq0=0091a001 es0=1`、`evs=17/38/02` 与上一版基本一致，但新增的两个字段直接判了两个前轮结论的死刑：
  - `wms=ffff`——**毫秒预算在真机上根本没生效**。`ffff` 是报告页宽度的截断值，而当时 `budget` 取自 `usb_pm_timer_running()`，真机在 0x608 读不到变化即判"无时钟"，于是整个 U-23 退化成 8,000,000 次迭代上限（`wms` 记录的是那个迭代数）。九轮以来所有"超时"都还是约 0.1 秒。
  - `rcf=0e`、`rce=13`、`rde=00`——14 次恢复全部被拒，完成码 0x13 = 19 = **Context State Error**，`rde=00` 说明第一步失败后 Set TR Dequeue 从未发出。
- **U-24 新条款，恢复命令选错了（真 bug）**：Reset Endpoint 只对 **Halted** 端点合法；对 `es0=1`（Running）的 EP0 发它是非法命令，控制器必然回 19。这与 FreeBSD `sys/dev/usb/controller/xhci.c: xhci_setup_endpoint()` 的 `switch (xhci_get_endpoint_state())` 完全吻合：`DISABLED` 不发、`HALTED` 发 Reset Endpoint、**其余状态发 Stop Endpoint**，之后才 Set TR Dequeue；Linux 也从另一头得到同一结论（`xhci_urb_dequeue()` 发 Stop Endpoint，Reset Endpoint 只出现在 `xhci_handle_halted_endpoint()`）。所以"恢复后重试"从来没拿到过干净端点，重试等于在控制器不知情时重写环。`ep0_recover()` 改为先 `ep0_out_snap()` 读控制器自己的 EP0 状态再选命令，新增 `TRB_STOP_EP = 15`，报告新增 `rsv=`（`0` Reset / `1` Stop / `2` 没发）。
- **U-25 新条款，时钟来源错了**：驱动不该自己"探某个端口"，端口号不是规范保证的东西，权威来源是 ACPI FADT 的 `PM_TMR_BLK`，而它在不同 PCH 上并不都是 0x608。内核本来就有校准好的时基（`kmain.c: timeline_probe_hz()`，CPUID 0x15 → 0x16 → FADT 端口标定 TSC），本轮把它导出成 `cpu_uptime_us()`（`kernel.h`），`usb_ms_now()` = `cpu_uptime_us()/1000`，`usb_delay_ms()` 忙等到目标微秒。删掉 `USB_PM_TIMER_PORT` / `USB_PM_TICKS_PER_MS` / `g_pm_timer_alive`，全内核只保留一处时基来源。`wms=fffe` 现在专门表示"撞上迭代上限"，不再和时长混用。
- **其他操作系统对照（本轮用户要求）**：NetBSD `sys/dev/usb/xhci.c` 的 `XHCI_DCI_EP_CONTROL 1`、`XHCI_TRB_3_EP_SET(dci)`、`xhci_db_write_4(sc, XHCI_DOORBELL(xs->xs_idx), dci)` 三处与本驱动**一致**——EP ID 填 DCI（EP0 = 1）、门铃值也是 DCI，所以"EP ID 编码错"和"门铃写错"这两条假设可以划掉；FreeBSD 给出的是上面那条状态机；Linux 是同一件事的另一半。三家的差异只剩"用什么命令恢复"，本轮按 FreeBSD 的显式状态机落地。
- 验证：构建 `exit=0`，`usb.c` / `kmain.c` 无新增警告（仅剩 `net_http.h`、`net_tcp.c`、`nvme.c` 的既有警告）。镜像 `5861862f20d4`（67,108,864 B）。QEMU 回归（`/tmp/r113.png`）：`at=00009900`、`ec=........`、`mi=1`、`chk=f`、`evs=02/06/0e`、`td0=00030851/00010c11/00001021`、`dc0=08300000/00000001/00400026/00389001/10000001`、`ep0lo=00389000` 与上一版逐字相同；新字段为成功路径的哨兵值 `rec=00 rcf=00 rsv=0 rce=00 rde=00 wms=0000`；串口**没有** `[USB] no calibrated clock` 行，说明时基在模拟器里走的也是 TSC 路径（`cpu_uptime_us()` 非 0）。报告页仍一屏放得下，未被截断。
- 未做 / 未验证（如实）：本轮镜像尚未刷盘、尚未回真机拍照，`wms` 是否变成接近 `03e8`、`rce` 是否变成 `00` 且 `rsv=1`、`rec` 是否增长，全部待验；若真机 `rsv=2`（被误判成 Stopped/Disabled），要先核对 `ep0_out_snap()` 的 `ctx_size` 步长（CSZ=1 时 64 字节）；控制传输预算仍是 1000 ms 而非 Linux 的 5000 ms；`chk` 的 bit1（CRCR 落在命令环区间内）在真机长期为 0 而命令明明在完成，判据可疑，本轮仍没动。

### 第九轮

- **再对 Linux（本轮真正对上的一处）**：`xhci-ring.c` 的 `xhci_queue_ctrl_tx` 与 `xhci_queue_address_device` 逐字段核对——setup TRB 的 IDT/TRT、data TRB 的 DIR_IN/CH、status TRB 在"无数据请求"时置 DIR_IN、首 TRB 的 cycle 最后写、`Address Device` 的 `SETUP_CONTEXT_ONLY ? TRB_BSR : 0`——这些与本驱动**一致**。真正的差异不在 TD 编码，而在**等待预算**。
- **U-23 新条款，超时单位错了**：`USB_TIMEOUT = 8,000,000` 是循环次数，注释按"约 1 秒"设计；实测每次 paced port-80 读约 1 µs，125,000 次读只有约 **0.1 秒**，比 Linux 给控制传输的 5000 ms 小一到两个数量级，也短于设备复位后合法需要的应答时间。**超时太短与"控制器不产生事件"在页面上完全同形**，正好对应本机 10 次 bring-up 只出 2 个传输事件。
- 修法：`usb_ms_now()` 用 PM timer 累计 **tick** 再取毫秒（逐次采样只加"毫秒商"会把不足 1 ms 的增量丢掉，秒表会任意变慢、等于永不超时）；`usb_wait_over()` 给两条等待路径 1000 ms 真实预算，无 PM timer 时回退原迭代上限。报告新增 `wms=`（最后一次失败等待的实际毫秒；等于预算 = 设备真没应答，远小于预算 = 等待自己提前退出）。
- 保留：U-21（环所有权）与 U-22（地址 0 探针）仍在本轮镜像里，未单独验证。
- 验证：构建 `exit=0`、无新增警告；镜像 `0221EC6D4108`。QEMU 无 PM timer，走回退路径：`at=00009900`、`ec=........`、`mi=1`、`chk=f`、`wms=0000`、`td0`/`dc0`/`ep0lo` 与上一版逐字相同，无回归。真机待拍 `wms=` / `at=`。
- 未做（如实）：控制传输预算仍取 1000 ms 而不是 Linux 的 5000 ms（先控制真机启动时间，`wms` 等于 1000 说明还不够时再提）；命令超时后的 `xhci_hc_reset()` 整机复位策略未改。

### 第八轮

- 真机验证第七轮（`AAD6D23E8FEC`）：`rty=05`、`at=3030043003000000` 与上一版**逐字相同**，`rec=00`。即整端口重试与 10 ms 恢复间隔都没有改变任何东西——`rty` 非 0 说明 5 个口都确实重跑过第二遍，两遍死在同一个位置，失败是确定性的、与控制器/端口瞬时状态无关；`rec=00` 配上 `ec=T` 说明 U-18 的恢复**一次都没成功**（否则 `rec` 会加），此前页面只写"没事件"，把"恢复没做"和"恢复被拒"混在了一起。
- **U-21 真 bug，环重置没有收回所有权**：`ep_ring_reset()` 原来只把软件 `idx` 归零并重写 Link TRB，环内旧 TRB 仍然带正确 cycle（= 仍属于控制器）。重试复用同一个 `ep0` 环，`Address Device` 把 dequeue 指回环首（`dq0 == ep0lo|1` 就是这件事），于是控制器完全可以重放上一轮的旧 TD：旧数据缓冲、旧请求，既不产生本轮事件也不报错。现按 xHCI 4.9.3 把整环 TRB 逐项清零再写 Link TRB。
- **U-22 地址 0 探针**：一个"什么都没回"的控制传输有两种完全不同的解释——控制器没跑这个 TD，或设备被寻址后不应答。本机没有串口，唯一能二分它们的实验是 USB 2.0 9.4 规定的那条：设备在被 SET_ADDRESS 之前必须能在地址 0 回答 `GET_DESCRIPTOR(DEVICE, 8)`（Linux 也把这条当成 old scheme 的兜底）。`addr0_probe()` 用 xHCI 4.6.5 的 BSR 位让 Address Device 只建上下文、不发 SET_ADDRESS，然后在地址 0 重发同一条请求；结果进 `rz=`。`address_device()` 因此新增 `bsr` 参数，`cmd_submit_ep()` 新增 `flags` 参数承载该位。
- **取证字段调整**：删掉 `prb=` / `dbr=`（连续两轮没有产出一致可解释的信息，且 `dbr` 是只写寄存器的回读，没有语义）；新增 `rcf=`（恢复失败的次数）、`rz=`、`rce=` / `rde=`（两条恢复命令各自的完成码）。
- 验证：构建 `exit=0`、`usb.c` 无新增警告；镜像 `48EB5E303865`。QEMU 鼠标+键盘 `at=00009900`、`ec=........`、`mi=1`、`chk=f`、`evs=02/06/0e`、`td0`/`dc0`/`ep0lo` 与上一版逐字相同，新字段为哨兵值 `rec=00 rcf=00 rty=00 rz=ff rce=00 rde=00`（成功路径不触发），无回归。真机待拍。
- 未做（如实）：`rz=00`（设备在地址 0 应答）时还没接着把 old scheme 走完（BSR 读描述符 → SET_ADDRESS → 重读），本轮只做二分不做路径切换；`chk` 的 bit1（CRCR 落在命令环内）在真机上一直为 0 而命令明明在完成，判据本身可疑，本轮未动。

### 第七轮

- **U-19 新条款，缺的是时间而不是编码**：对照 Linux `drivers/usb/core/hub.c` 的 `hub_port_init` / `hub_port_wait_reset` 与 `drivers/usb/host/xhci-hub.c` 的 `SetPortFeature(RESET)`（Debian/Ubuntu 用的就是这份代码），发现两处结构性差异。其一是**时间**：USB 2.0 7.1.7.3 要求复位结束后留 TRSTRCY（10 ms）才允许发请求，9.2.6.3 要求 SET_ADDRESS 之后留恢复间隔；Linux 在 `xhci_hub_control` 里只写 PORT_RESET 就返回，真正的间隔由 `hub_port_wait_reset` 的步长轮询（根口 `HUB_ROOT_RESET_TIME` 60 ms）与 `hub_port_init` 里那句 `msleep(10)`（"let SET_ADDRESS settle"）给出。本驱动此前一处都没等，读到 PED 就在同一微秒里 Address Device。新增 `usb_delay_ms()`：以 ACPI PM Timer（0x608，3.579545 MHz）为时间基，探不到就退化为 port-80 读**超睡**（晚到的包只是慢，早到的包是协议违规）。调用点：`port_reset()` 成功返回前 10 ms、`attach_base()` 每次 `address_device()` 之后 10 ms。
- **U-20 新条款，重试的粒度错了**：Linux 失败时不是重发那个请求，而是整条端口重新复位再寻址（外层 `GET_DESCRIPTOR_TRIES` 默认 2 轮，每轮 `msleep(100)` 并重新 `hub_port_reset`）。只重发请求的前提是"复位已生效"，而本机证据恰好怀疑这一点（EP0 dequeue 零前移、传输事件 0）。`xhci_attach_port()` 现在对失败端口做两轮 bring-up：`disable_slot` → 100 ms → `port_reset` → `attach_base`；报告页新增 `rty=`。轮数与 Linux 默认值一致。
- **U-18 收尾**：上一次"重试前的等待天然满足 SET_ADDRESS 间隔"是巧合，现在由 U-19 显式给出，条款文字已改。
- 验证：构建 `exit=0`、`usb.c` 无新增警告；镜像 `AAD6D23E8FEC`，已刷盘、回读 sha256 一致。QEMU 走到 PM Timer 判死分支（串口 `[USB] PM timer silent, delays use port-80 fallback`），鼠标+键盘 `at=00009900`、`ec=........`、`mi=1`、`chk=f`、`evs=02/06/0e`、`rty=00 rec=00`、`td0`/`dc0`/`ep0lo` 与上一版逐字相同，无回归。真机待拍：`rty=` 是否为 1 且 `at=` 是否前进。
- 未做（如实）：Linux 另有 `PORT_RESET_TRIES`(5) / `SET_ADDRESS_TRIES`(2) / `PORT_INIT_TRIES`(4) 三层各自的重试，本驱动只做整端口两轮；`hub_port_disable` 之后的端口状态复位（本驱动用 U-9 的 `port_wake` 循环代替）；复位后的 TPORSCL 等更细窗口。

### 第六轮

- 真机验证第五轮取证：`dq0=0091a001 es0=1`、`ep0lo=0091a000` → **控制器侧 EP0 dequeue 一个 TRB 都没前移**，而 EP0 状态是 1（Running，门铃本应被接受）。HSE 依旧没有、`ht=0`、`evs=0e/0f/01` 与上一轮一致。`prb=ee` 是上一版探针自己的缺陷：把 DCS（dequeue 指针的 bit0）当地址位参与对齐判断，任何环都会被判成"不可探"。已改成先掩掉 bit0 再判断。
- **新条款 U-18 端点错误恢复**：控制传输返回"一个事件都没有"时，规范没有"再敲一次门铃"这条路。定义的恢复是两条命令——Reset Endpoint（EP ID = DCI），再 Set TR Dequeue Pointer 指回环首并置 DCS。Linux 每次传输失败都走这一对（`xhci_handle_halted_endpoint` → `TRB_RESET_EP` + `TRB_SET_DEQ`）。`control_xfer()` 现在在第一次超时后执行 `ep0_recover()`（复位环 → Reset Endpoint → Set TR Deq）并**重试一次**；`cmd_submit_ep()` 因此补上了 TRB 的 EP ID 字段（bits 20:16）。重试天然落在 SET_ADDRESS 恢复间隔（USB 2.0 9.2.6.3）之后，因为第一次尝试已耗尽约 1 s 等待预算。
- 报告页新增 `rec=`（恢复+重试次数）。
- 验证：构建 `exit=0`、无警告；镜像 `6A9ADF75C9A9`，已刷盘、回读一致。QEMU 鼠标+键盘 `at=00009900`、`mi=1 kb=1`、`rec=00`（成功路径不触发恢复）、`evs=02/06/0e`，无回归。真机待拍：`rec` 是否非 0，以及 `at=`/`evs` 是否随之前移。

### 第五轮

- 真机验证第四轮：`spa=008b6000 n=25` / `spa=008f2000 n=25`（`DCBAA[0]` 指向 scratchpad 表、37 页全部发布），`sts=sts0=00000018`——**HSE 消失**，`ht=0`（控制器整轮再没停过机），`evs` 从 `15/09/00` 变成 `0e/0f/01`：出现了第一个传输事件，`at=3030043003000000`（口 6 的设备走过了 step 3、到 step 4 才失败）。即 U-15 修正确实消掉了那类故障，剩下的只是"多数控制传输依旧一个事件都不产生"。
- **探针修正（U-16）**：上一版取指探针固定往 EP0 环头（index 0）写 type-0 TRB。控制器消费过 TRB 之后其 dequeue 已经前移，index 0 落在它身后，探针本身就会拿到"无响应"——`prb=ff/ff` 因此不能证明控制器不取指。现在先读控制器自己维护的 EP0 dequeue，只在它指向的槽位写探针；落不到本环内则记 `prb=ee`（不可探，勿误读）。
- **新取证字段**：`dq0=`（第一次失败瞬间，控制器输出上下文里的 EP0 dequeue 低 32 位）、`es0=`（同一处 EP0 状态，1=Running）。判读：`dq0 == ep0lo|1` = 一个 TRB 都没取（取指/门铃问题）；`dq0` 已前移则取指正常、问题在事务层；`es0 != 1` 则门铃本来就会被告知"端点非 Running"而忽略。
- 验证：构建 `exit=0`、`usb.c` 无警告；镜像 `C0214EFC81CD`，已刷盘、回读 sha256 一致。QEMU 鼠标+键盘 `at=00009900`、`mi=1 kb=1`、`evs=02/06/0e`、`dq0=ffffffff es0=f prb=ff/ff`（无失败路径即哨兵值），无回归。真机待拍。

### 第四轮

- **对照开源实现**：逐段取了 Linux `drivers/usb/host/xhci-ring.c`、`xhci.h`、`xhci.c`、`xhci-mem.c`（Debian/Ubuntu 的内核就是这份代码），只看语义、代码自行改写。核对结论：小编码器的 setup/data/status 控制字与 Linux **逐字一致**（TRT 由 bmRequestType 方向决定、setup 与 data 置 CH、IN 数据阶段的 status 阶段 DIR=0、门铃值 = `DB_VALUE(ep_index) = (ep_index+1)` 即 EP0 为 1、读回门铃以 flush），`dc0` 的 slot/EP0 上下文也与 Linux 的 `xhci_setup_addressable_virt_dev()` 等价。因此差异不在 TD，在**发布顺序**与**scratchpad 的放置方式**两处。
- **U-15 修正（真 bug）**：上一轮把 scratchpad **页地址**直接写进 `DCBAA[MaxSlotsEn..]`，等于把二级表当槽项用。Linux `scratchpad_alloc()` 是 `ctx_array[0] = sp_dma`（一张连续表），表内 `sp_array[i]` 才是页地址；xHCI 4.6.4 的 `DCBAA[0]` 从不属于 slot。按旧写法 `DCBAA[0]` 恒为 0，控制器启动后去物理 0 取 scratchpad 表 → DMA 故障 → USBSTS.HSE（bit2）→ 停机，此后任何 EP0 传输都不会有事件。真机 `sts=0000001d` / `sts0=00000018` 都带 HSE，与该后果一致。现改为分配一页放表、`DCBAA[0]` 指向它、逐项填页地址，并在置 RS 之前完成；报告页新增 `spa=`（表物理地址）与 `n=`（条目数）。
- **U-17 新条款，TD 所有权发布顺序**：TRB 的 cycle 位是所有权位，控制器可预取。旧实现先写 setup TRB（带正确 cycle 且 Chain=1）、再写 data/status，于是控制器有机会先取到"已归属但同 TD 后续 TRB 尚未归属"的 setup，随后一直等一个永远不完整的 TD——既不报错也不停机，正是本机 `evs` 传输计数恒 0 的形状。Linux 把首 TRB 的 cycle 放到 `giveback_first_trb()` 里、在所有 TRB 之后、敲门铃之前写。`control_xfer()` 现按此顺序：setup 以 `start_cycle ^ 1` 写入，status 写完后 `mfence` → 翻转 setup 的 bit0 → `mfence` → 才敲门铃。bulk / HID 中断 / 命令环都是单 TRB TD，无此窗口，不动。
- 判读依据：`spa=00000000` 与 `sts` 的 bit2 是同一件事的两面；`td0` 在改动前后字面相同，说明改的是发布时序而非编码。
- 取证（承接上一版 `E672647800CC` 未记账的探针）：`prb=` / `dbr=`——在已失败的设备上往 EP0 环头写一个保留类型 TRB 并敲门铃，看控制器取不取。真机 `prb=ff/ff`（一个事件都没有）说明控制器对 EP 环**从未取指**，与"TD 没写错、上下文也被接受"并列时，只剩控制器自身故障这一种解释，也就是 HSE。
- 验证：构建 `exit=0`、无新增警告；镜像 `6613EC485725`。QEMU（鼠标+键盘）`at=00009900`、`ec=........`、`mi=1`、`kb=1`、`chk=f`、`evs=02/06/0e`、`td0=00030851/00010c11/00001021`、`dc0` 与 `ep0lo=00389000` 同前，`spa=00000000 n=00`（QEMU 无 scratchpad，该路径覆盖不到），无回归。真机待拍：`spa` 非 0 / `n=25` / `sts` 的 bit2 是否消失。
- 未做（如实）：Linux 在 SPR=1 时对 scratchpad restore 语义的处理（只保证首次取表可用）；U-5 的 `TRB_ISP`（Linux 在 data TRB 上置 ISP，本驱动不置，规范允许）；SuperSpeed 控制传输前置 `TR_NOOP`。

### 第三轮

- **U-15 新条款，能力缺口**：`HCSPARAMS2` 的 Scratchpad 计数此前完全被忽略。规范要求计数非 0 时，必须在启动控制器前把相应数量的页地址发布到 DCBAA 的 `MaxSlotsEn..` 项；没有这些页，控制器有权"命令照常完成、设备传输全部不响应"。`usb_init()` 现在读 `hcs2`、按 5+5 位拼出计数、连续分配页并写入 DCBAA，分配失败则清零计数并在串口说明；报告页新增 `h2=`。
- **取证字段再补**（U-16）：`dc0=`（控制器写回的 slot DW0 / EP0 DW0 / EP0 DW1 / EP0 出队低字 / slot DW3）、`ep0lo=`、`dbo=`、`sl=`、`h2=`。
- 判读依据：真机 `dc0` 前四项与 `ep0lo` 完全相符（EP0 状态 1、类型 4、MPS 64、出队 = `ep0lo|1`），`td0` 与 QEMU 一字不差，`evs=15/09/00` 说明控制器从未写过一次传输事件，`hch=0` 说明当时没有停机——所以这一轮不再往描述符或 TD 方向改，只补规范漏项与取证。
- 验证：构建 `exit=0`、无新增警告；镜像 `097AC8D79059`。QEMU `h2=0000000f`（计数 0、不分配）、`dc0=08300000/00000001/00400026/00389001/10000001 ep0lo=00389000 dbo=2000 sl=1`、鼠标键盘正常。真机待复验。

### 第二轮

- **U-14 新条款，真 bug**：`attach_base()` 读设备描述符时把描述符类型写进了
  `setup[2]`（wValue 低字节）而不是 `setup[3]`（高字节），等于请求"类型 0"。
  QEMU 串口直接报 `usb_desc_get_descriptor: unknown type 0 (len 8)`，完成码 6
  （STALL）；真机上任何设备都会这样被 STALL 掉。改正后同一镜像 QEMU 立即
  枚举成功（`at=` 到 9、`ec=` 无失败、`mi=1`、`msd=1`）。这条是"描述符读不
  回来"的直接原因之一，此前被 `at=` 阶段码掩盖了。
- **U-7 重做**：旧 `xhci_halt_clear()` 只清 HCH + 置 RS，真机上 `ht=1` 之后
  其余 4 口 ENABLE_SLOT 仍全超时，证明局部自愈无效。现拆出
  `controller_program()`，新增 `xhci_hc_reset()`：停 RS → 清 HCH → HCRST →
  重新编程 DCBAA/CRCR/ERST/ERDP/CONFIG → 置 RS，并把该控制器上已 attach 的
  设备标记 gone（slot 已作废）。`controller_start()` 复用同一套编程。
- **报告页扩展**：`ev0=` / `sts0=`——本次启动**第一次**失败瞬间的事件环位置与
  USBSTS。第一个放弃的设备才是拖垮总线的那个，避免只看到最后的残局；
  `ev0=ff/1` 表示从未失败。`at=` 阶段码重排为：1 ENABLE_SLOT、2 ADDRESS、
  3 GET_DESCRIPTOR(DEVICE,8)、4 配置头、5 完整配置、6 SET_CONFIGURATION、
  7 HID 未解析、8 中断端点未挂、9 就绪。
- 文档：`docs/USB-xHCI驱动规范核对表.md` 同步 U-7 改写、新增 U-14、
  U-12 字段表补 `ev0=`/`sts0=` 与新 `at=` 语义、U-13 记录真机未闭环现状。
- 验证：构建 `exit=0`，无新增 `-Wall -Wextra` 警告。镜像
  `89A6EC7F686D`。QEMU：①鼠标+键盘 `cn=2`、`at=00009900`、`ec=........`、
  `mi=1`、`chk=f`、`ev0=ff/1`；②再加 `usb-storage` `ccs=3`、`msd=1`、
  `at=000909900`、无失败；③构建前同镜像（含 DEVICE 描述符 bug）作对照，
  串口 8 次 `transfer failed code=0x6`。真机待复验。

### 第一轮

- 新增 `docs/USB-xHCI驱动规范核对表.md`：把规范条款编号化，之后任何
  USB 改动必须先落到条款，禁止逐现象打补丁。本轮改动逐条对应 U-3、U-4、
  U-5、U-6、U-7、U-8、U-9。
- 根因（U-5）`kernel/usb.c` `control_xfer()`：setup TRB 缺 TRT（bit 17:16）
  与 Chain（bit 4），data TRB 也缺 Chain。QEMU 接受，Intel 控制器判定 TD
  非法并停机；停机后后续端口连 ENABLE_SLOT 都拿不到完成事件——真机
  `at=3010011001000000`（端口 1 走到读配置头失败、其余 4 口卡在 step 1）
  正是这条链。现在三段 TD 按规范拼装。
- U-7 `xhci_halt_clear()`：超时后检测 USBSTS.HCH，写 1 清除并重新置 RS，
  报告页 `ht=` 记次数。单台设备的非法 TD 不再拖死整条总线。
- U-3 `cmd_submit()` / `wait_cmd()`：命令完成事件按 TRB Pointer[63:4] 匹配，
  不再只按事件类型，避免上一条超时命令的迟到完成被下一台设备误领。
- U-4 新增 `xhci_port_changes()`：应答 PSCEG 的端口变更位，`wait_cmd` /
  `wait_transfer` 循环内处理，防止事件环被挤满后丢弃真正的完成事件。
- U-8 `enable_slot()` / `disable_slot()` / `attach_base()`：全速/低速设备
  先以 8 字节 EP0 建立、读设备描述符第 8 字节，若 `bMaxPacketSize0` 不是 8
  则 DROP slot 后以真实值重新 Address Device。
- U-9 `port_reset()`：请求复位与预清 PRC 放在同一次 PORTSC 写入，避免
  陈旧 PRC 被读成本次复位完成。
- U-6 新增 `usb_pace()`：等待循环每 64 次迭代读一次 port-80，把
  `USB_TIMEOUT` 的实际时长从几十毫秒拉到约一秒。
- 诊断字段扩展：`ec=`（每口失败完成码，`T` 超时 / `o` 本驱动拒绝 / `.`
  无失败）、banner `msd=` `mi=` `ht=`。
- 验证：`make` 与镜像构建通过，无新增 `-Wall -Wextra` 警告。
  QEMU 三场景回归：①`qemu-xhci` + `usb-mouse` + `usb-kbd`——鼠标/键盘均
  `kind=mouse/keyboard`，诊断页 `at=00009900`、`ec=........`、`mi=1`；
  ②`qemu-xhci` + `usb-storage`——`usb00` 注册，容量 0x20000 扇区；
  ③`qemu-xhci` 无设备——不崩溃，`cn=0`、`ht=0`，桌面正常。
- 未验证：真机复验（本轮镜像已构建时 sha 前 12 为 `F93C93721038`，后续轮次
  继续演进，见第二轮条目）；非 8 字节 EP0 的全速设备；hub 下游端口（U-13，
  未实现）。未创建 git commit。

## 启动过渡画面直接显示日志

- 新增 `kernel/klog.{h,c}`：一条贯穿两个阶段的日志环。UEFI 启动阶段把
  `boot_puts()` / `boot_timeline()` 的每一行写进共享内存保留区里的 4 KiB
  环（`shared_mem.h` 的尾部 carve-out，冻结字段一个没动）；内核 `kmain()`
  在第一行输出前用 `klog_init()` 继承它，之后 `kputs()` 追加到 32 KiB 环。
  `klog_snapshot()` 在继承前直接读共享内存环，所以 splash 第一阶段就能
  显示固件日志，不需要等内核起来。
- `graphics/nd_boot.c`：启动 splash 在 logo 与状态行之间多了一块半透明
  日志面板，显示日志环里最新的若干行。面板是纯叠加层：logo 和状态行的
  矩形、字体、位置都没变，不参与它们的布局；刷新只重画面板自己的矩形
  （先重铺渐变背景再画面板和文字），屏幕其余像素不动，因此没有整屏重绘
  带来的闪烁。日志有新增时由 `nd_boot_stage()` 触发重画，进度文字更新
  仍只重画状态带。
- `shared_mem_bootstage_init()` 提前到 `efi_main()` 里第一条日志之前，
  否则第一阶段的日志会被它的 `memset()` 清掉。
- 验证：`make` 通过；QEMU 1280x800 截图确认 splash 上日志面板已出现，
  状态行和 logo 位置不变；60 秒 headless 启动无新增内核异常（唯一的
  `#PF` 是既有的 nxprobe 用户态自测）。未创建 git commit。

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
## U-60/U-61 报告页停留期间切片喂 USB 端口遍历

- U-60（此前一轮已入码，本轮补登记）：`hub_port_probe()` 末尾逐口 `probe port <p> speed=<s> step=<k> ok|failed`；`ep0_wait_stopped()` 记录本 slot EP0 停止后前 8 条事件的 code/resid。
- U-61（本轮 Fix A）：诊断报告页在 `usblog_state()!=1` 时不再一次性阻塞 45 s；改为 250 ms 切片循环——绘制报告、`usb_hid_poll()` 续跑端口遍历并轮询 HID/热插拔/子口、重试日志介质并强制 flush、重建报告。`nd_demo_report` 拆成 `nd_demo_report_draw` + 等待包装；kmain.c 新增 `diag_strip`/`diag_report` 两个装配 helper。

### 验证

- 构建：`make CROSS= disk-img -j8` exit=0；`kernel/kmain.o`、`graphics/nd_demo.o` 无新警告（仅剩既有 `kernel/net_http.h:4:33: warning: '/*' within comment`）。镜像恒 67,108,864 B，SHA256 前 12 位 `938E7430318E`。
- QEMU：q35 + pflash OVMF_CODE_4M/VARS_4M + virtio 盘 + qemu-xhci（mouse port 1 / kbd port 3），鼠标枚举到口 5、键盘到口 7，均 `probe port … step=9 ok`，`hub scan done: ccs=2 cn=2`；桌面设置/文件管理器窗口与指针截图正常。
- 对照：旧镜像 `10C37B18AE92` 同一 pflash harness 同样启动并枚举鼠标/键盘，确认 harness 修复未掩盖回归。
- 刷机：镜像已写入笔记本 SanDisk Cruzer Blade（Disk 1），裸写 67,108,864 B，回读 SHA256 `938E7430318E`，`match=True`；脚本补 `Clear-Disk` 解卷锁、`FileShare.ReadWrite`（可移动介质不能 offline，裸设备句柄 Flush/Dispose 会抛拒绝，已 best-effort 吞掉）。

### 未做 / 未验证

- 真机未复验：U 盘已就绪，但尚未从固件选择 U 盘启动；口 2…16 在报告页切片循环下能否被走到、接收器口是否枚举为 HID，待真机启动拍照复核。
- 内盘 NVMe 日志写拒绝（`log=2`、18 扇区后缓冲滞留 RAM）未解；U-60 探针行在真机未落盘。
- 报告页切片循环里 `diag_strip` 的输入横幅每条不重建（仅 `diag_report` 的端口数组刷新）；USB/PS2 横幅静态值不随枚举更新。
- 源码全部未提交；是否 commit / 合并 PR #19 未拍板。

## U-62 控制传输入出口逐笔入日志 + EP0 完成事件带 code/resid

### 为什么要再加一层日志

本轮拿到首次成功的真机完整文字日志（`log=1`，内盘 NVMe 段可读回），读数把故障范围收得很窄：

- 五个端口有设备（`ccs=5 cn=5`），端口 6 枚举走到 **step 4**——即第一笔 8 字节 `GET_DESCRIPTOR` 成功，第二笔 9 字节 config header 超时；端口 1/3/7/10 在 step 3 就超时。
- 14 次 Stop Endpoint / Set TR Dequeue 全部 `rcf=00 rce=00 rde=00` 成功返回，但没有完成事件；同一时段命令通道 70 条完成、事件环 19 PSC + 70 cmd + 14 xfer 正常。
- 端口 reset 全部成功（`rs=0303300330333333`，五个口 = 0 linked）。

由此两条旧判据被推翻：门铃后 `dq0` 不前移在 Intel 控制器上是正常行为（不写回 context dequeue），`dq0 == ep0lo|1` **不能**当作"TD 未被取"；环 wrap 也不成立（端口 6 只用到 idx 3…5，64 TRB 环要 21 笔才 wrap）。真正的缺口是：**第二笔传输回没回来、回来的是什么码，日志里没有任何一行**——事件日志只保留前 8 条，EP0 事故现场永远在窗口外。

### 改动（纯诊断，不改行为）

`kernel/usb.c`：

1. `event_pop()` 事件日志上限 `8u` → `48u`。
2. 新增静态 `g_ep0_ev_n`（上限 24，独立计数，避免 HID 报告流量挤占 EP0 的份额）；`wait_transfer()` 中 transfer event 的 EP ID == 1 时逐条打印 `[USB] ep0 ev code=… resid=… slot=…`（成功、Short Packet 13、Ring Underrun 14、TRB Error 5 一律入日志）。
3. `control_xfer()` 每笔传输入口打印 `[USB] xfer port=… step=… slot=… pass=… idx=… cy=… len=… req=…`，成功返回前打印 `[USB] xfer done port=… step=… idx=… cy=…`。

全部用精确替换完成，未动既有逻辑、未改时序。

### 验证

- `build_main.cmd` exit=0，镜像 67,108,864 B，SHA256 前 12 位 `44AD959E4FB0`。
- QEMU（q35 + pflash 4M 成对固件，qemu-xhci + usb-mouse/usb-kbd）读数正常：鼠标口 5 每笔 `xfer done`，`idx` 0→3→6→9→B→E 连续推进，`code=0x01`；读 config 时 `ep0 ev code=0x0D resid=0xCB`（QEMU 返回 44 字节，属 Short Packet 正常），随后 `probe port 5 … step=9 ok`；键盘口 7 同理 `step=9 ok`；`hub scan done: ccs=2 cn=2`。
- 镜像已写入笔记本 SanDisk Cruzer Blade，回读 sha256 `44AD959E4FB0`、`match=True`。

### 未做 / 未验证

- 真机未复验：尚未从固件选择 U 盘启动。待回答的是"第二笔 9 字节传输回来的是哪个 code"（0x01 迟到完成 / 0x0D 短包 / 0x14 Ring Underrun / 0x05 TRB Error / 什么都没有），以及失败行里的 `idx/cy` 是否与控制器 `dq0` 指向的 TRB 对齐。
- 真机 `deferred phase` 耗时涨到 73.5 s（`[T] +73953ms deferred vidmem crtc`）未归因：与端口 1 每 try 的传输预算 × 多次重试 + 五个失败端口叠加有关，也可能与 1000 ms 级重试预算有关，未量化。
- 内核日志容量（255 扇区文本段）在新上限（48 条事件 + 每笔两行控制传输）下是否需要扩容未评估。
- 源码全部未提交；是否 commit / 合并 PR #19 未拍板。


## U-63 No-Op TRB 探针 + 首笔传输 3 秒预算

### 这一轮真机读数新增了什么

镜像 `44AD959E4FB0` 在真机上跑完，内盘日志完整读回（42,623 B）。新增的事实：

- 端口 1（USB2 口、`speed=3` 高速设备）的**第一笔** 8 字节 `GET_DESCRIPTOR` 就没有任何完成事件：`[USB] ep0 ev` 一行都没出现（U-62 新加的逐条打印生效了，是真的一条都没来），`[USB] xfer done` 也没有，`idx` 始终是 0。
- 门铃写入地址 `0x3004`、slot 1 正确；2 ms 后 `deq` 仍是 `0x14DD001/1`，重敲一次仍不变。
- 失败后 `Stop Endpoint` 仍返回 Transfer Event `code=0x1A`、`residual=8`，即控制器侧确实挂过一条 8 字节 TD。
- 空环探针 `ur=2`，报告页 `at=3000000000000000`、`ec=T...............`、`rs=0000000000000000`（端口 1 reset 成功、链路 U0）、`portsc=0x0E03`（CCS+PED+PP、PLS=U0、speed=3）。
- `dc0=08300000/00000001/00400026/014dd001/11000001`：slot DW0 speed=3（HS）、Context Entries=1；EP0 上下文 `0x11000001`（EP State=Running）、`0x00400026`（Control Bi、CErr=2、MPS=64）、TR Deq=`0x014DD001`（DCS=1）。上下文与环指针在控制器侧都是对的。
- 四条 TD 全部合法：`0x00030851`（Setup，CH=1、IDT=1、TRT=3）、`0x0010C15`（Data IN、ISP、CH=1、length=8）、`0x00001021`（Status OUT、IOC）。
- 这一轮 4 个 slot（1…4）反复重试，全部在第一笔就超时，`enumeration failed`。

结论：TD 合法、上下文合法、链路 U0、控制器挂过 TD，但**没有任何传输完成事件**。剩下的两个可能方向（环没被控制器读 / 设备不应答）用现有读数无法区分——`ur=2` 不构成证据，空环本来就不产生事件。

### 改动

`kernel/usb.c`：

1. 新增 `TRB_NOOP 23u`。传输环上的 No-Op TRB 按规范必须在不产生任何总线流量时完成并回报 Transfer Event（code 1），因此它是一个自证的判别器：**有完成 = 门铃/环基址/slot EP0 上下文都被控制器读取，沉默只能是设备不应答；无完成 = 控制器没在读这条环**。
2. 新增 `ep0_noop_probe()`，`g_noop_done` 保证每 boot 只跑一次，挂在首个拿到 slot 的设备 Address Device 成功之后（`attach_base` step 3 之前）。探针后无条件 `ep0_recover()` + `ep_ring_reset()`，把端点与环恢复到确定状态，不污染其后的枚举。
3. 首个 `GET_DESCRIPTOR` 用 3 秒预算跑一次（`g_slow_once`，打印 `[USB] first xfer budget 3000 ms`），之后恢复默认 1 秒。1 秒是本驱动的上限、不是控制器的上限，控制器本该自己回报的 Transaction Error / Stall 可能排在预算耗尽之后。

### 验证

- `build_main.cmd` exit=0，镜像 67,108,864 B，SHA256 前 12 位 `BB7AC83323A3`。
- QEMU（q35 + pflash 4M，qemu-xhci + usb-mouse/usb-kbd）：`noop probe slot=1 port=5 rc=-1 code=0xFFFFFFFF (no completion…)`。**QEMU 的 xHCI 模型不实现 type 23，直接忽略**，所以该探针在 QEMU 上永远报"无完成"，不能当反向证据；同一次启动后续枚举正常（鼠标口 5、键盘口 7 `step=9 ok`，`hub scan done: ccs=2 cn=2`），说明探针超时与恢复路径不会卡住流程。

### 未做 / 未验证

- 真机未复验：镜像已写入笔记本 U 盘（回读 `match=True`），尚未从固件启动。
- 待判定：`noop probe` 在有设备的端口上有没有完成事件；首个 `GET_DESCRIPTOR` 在 3 秒预算内是否回报 code 4（Transaction Error）/5（TRB Error）/6（Stall）/7（Device Not Available）。
- 已发现的次生缺口（未修）：端口重试时没有重新做 `port_reset`，而设备在第一次 Address Device 后已经拿到地址 1，后续 slot 再发 SET_ADDRESS（目标是地址 0）设备不会应答——重试失败可能有一部分是这个原因，需要下一次单独验证；端口 6 在上一版曾走到 step 4，说明"能应答一次"的现象仍未解释。
- 真机 `deferred phase` 耗时 73.5 s 未归因。
- 源码全部未提交；是否 commit / 合并 PR #19 未拍板。


## U-64 零完成事件的端口不再重试 + 退役空环探针 + defer 表扩容

### 这一版解决的是已经能用日志证实的问题，不是猜测

真机日志（`44AD959E4FB0`）里同一笔账反复出现：端口 1 第一笔控制传输**没有拿到任何完成事件**（U-62 逐条 EP0 事件打印生效，确实一条都没来），驱动仍然把同一笔请求跑第二遍，再把整个 bring-up 做三轮——每轮一次端口复位、一个全新 slot、两笔各 1 秒预算的传输。五个有设备的端口做同一件已知无效的事，光这一段就吃掉约 30 秒（`[T] +47193ms usb hub done`，`+73953ms deferred vidma crtc`）。

### 改动

`kernel/usb.c`：

1. **零事件即止（传输层）**：`control_xfer()` 第一遍失败且 `code == 0`（完全没有完成事件，区别于带回错误码）时置 `g_att_noev` 并直接返回。第二遍只是"恢复端点后重复同一请求"，在端点从未上报过完成的条件下就是同一个实验。
2. **零事件即止（端口层）**：端口重试循环看到 `g_att_noev` 就打印 `[USB] port <p>: no completion at all, one bring-up is enough` 并结束该口，每个口只 bring-up 一次，把预算留给还没试过的端口。
3. **退役 U-57 空环探针**：删除 `ep0_underrun_probe()`，`att_fail()` 不再调用，报告页 `ur` 固定打印 `0xFF`（未运行）。判据本身是错的——空环（只有一条 Link TRB）取不取都不会产生事件，`ur=2` 被当证据用了好几轮。替代它的是 U-63 的 No-Op TRB。
4. **`USB_DEFER_MAX` 8 → 32**：复合设备把 EP0、HID、命令的完成集中在同一张表里，8 槽在多点同时有完成时会溢出，溢出即丢事件。
5. **No-Op 探针措辞收紧**：无完成时并列"环未被读 / 控制器不实现 type 23"两种可能（QEMU 就是后一种），不再单方面下结论。

### 验证

- `build_main.cmd` exit=0，镜像 67,108,864 B，SHA256 前 12 位 `E3EE0552C8F2`。
- QEMU（q35 + pflash 4M）：`noop probe … (no completion: ring not read, or No Op not implemented)`；`[INP] usb mice=1`；鼠标口 5、键盘口 7 `probe port … step=9 ok`；`hub scan done: ccs=2 cn=2`。零事件短路逻辑在"每笔都有完成"的正常路径上没有被误触发。
- 镜像已写入笔记本 U 盘，回读 `match=True`。

### 未做 / 未验证

- 真机未复验：`deferred phase` 时长是否回落、五个失败端口的日志是否变成每口一行，待下次启动。
- 仍未定位：控制器读不读 EP0 环、设备是否应答。这次启动会同时回答这两个问题（No-Op 探针 + 3 秒预算下控制器自报的完成码）。
- PS/2 与 I2C HID 两条内建输入通路没动：`[INP] ps2 … aux_bytes=0 kbd_bytes=0` 只统计到 deferred 阶段，桌面 tick 之后的字节数没有落盘，现在没有证据说它们坏了；I2C 触控板被"控制器 BAR0 未分配"挡住（`[I2C] no assigned BAR, skipped`），要动就得写 PCI BAR 分配器，风险是整机挂死，不在本轮范围。
- 源码全部未提交；是否 commit / 合并 PR #19 未拍板。


## B-12 启动：内存例程加宽 + 呈现走非临时存储 + 阶段细分标记

### 现象（真机 `44AD959E4FB0` 日志）

- `[NDF] present 1920x1080 cycles=3344619029 us=1339991`：一次全屏呈现 1.34 s。
- `[T] +29811ms demo init start` → `[T] +32170ms first frame`：2.36 s，其中呈现 1.34 s。
- 桌面阶段更糟：`[HALT]` 之后 23.6 s 没有任何一行输出，然后才出现 `[NKM]` 自检；这 23.6 s 是桌面首帧合成，每合成一次就是一次 1.34 s 的全屏拷贝。
- `[MM] firmware left CR4.PAT clear` + `[MM] framebuffer at 0x4000000000 marked … cache-disabled`：这台机器的扫描缓冲是**强不可缓存（UC）**，每个像素写都是一次独立总线事务。

### 改动

1. `kernel/novuslib.c`：`memcpy` / `memset` 从逐字节循环改成 8 字节宽 + 4 倍展开（头尾按字节对齐）。全屏呈现因此从"每字节一条写指令"变成"每 8 字节一条"，UC 内存上没有合并缓冲，写的**条数**就是耗时。
2. `graphics/nd_hal.c`：`surface_copy()` 在目的地是扫描缓冲（`dst->fb`）时改走 `movntdq` 16 字节非临时存储（`nt_copy16`，需要换色时用 `nt_copy16_swap` 一次组一个 16 字节向量），整块矩形结束时一次 `sfence`；离屏缓冲仍走 `memcpy`。首次拷贝的诊断行追加 `nt` 标记。
3. `kernel/kmain.c`：把两个最大的黑盒窗口拆成可归因的小段——
   - 设备表窗口：`devtab table` / `touchpad probe` / `i2c hid` / `hda probe`；
   - 网络窗口：`net probes` / `ntp init` / `network probes done`；
   - 显示窗口：`splash rebind`（把 `nd_init` 与 splash 重绑、三个启动阶段分开）。

### 验证

- `build_main.cmd` exit=0，镜像 67,108,864 B，SHA256 前 12 位 `4F026CE37654`。
- QEMU（q35 + pflash 4M，`novus-b12.img`）：完整启动到调度器，`[INP] usb mice=1`（U-64 的枚举修复仍在），fileman/taskman/settings/dwm 全部拉起；新增的阶段标记按顺序落盘：`devtab table 790ms`、`touchpad 844`、`i2c hid 883`、`hda 917`、`net probes 2598`、`ntp init 2604`、`network probes done 2605`。
- 与基线镜像 `E3EE0552C8F2` 同参数对照：两边 `[HALT]` 时都是 182 次 present、之后 `[DWM] primary w=400 h=2=0`、`[NDC] take# empty`，说明 QEMU 里停在上电诊断页是既有行为，不是本轮引入。
- 渲染图与基线截图逐项对照：上电诊断页文字、`usb hid: mouse present`、端口读数一致，`movntdq` 路径没有把画面写坏。

### 未做 / 未验证

- 真机未复验：present 的 `us=` 会落到多少、`first frame` 段与 `[HALT]` 之后那段 23.6 s 是否一起缩短，要等下一次从 U 盘启动。
- 扫描缓冲仍是 UC：UC 下 16 字节存储只是把事务数从 8.3 M 降到 520 K，真正的解法是把它变成 write-combining。CR4.PAT 被固件关掉，`mm_mtrr_wc_cover()` 又因为 0x4000000000 落在固件的 WB RAM 范围里而主动放弃（`since the range is inside someone else's WB range` 那条分支不打日志）。要拿到 WC 得二选一：晚开 CR4.PAT + PAT[4]=WC 再重刷这 4 个 2 MiB 页（风险是开机停在那一行），或拆分固件的 WB MTRR 范围（风险更大）。本轮没动。
- 5.9 s 的 `device table done` 与 7.3 s 的 `network probes done` 只加了标记，没有改逻辑：QEMU 里这两段分别是 99 ms 和 7 ms，模拟器的 PCI/IO 端口太快，量不出真机的等待，必须等真机新日志里的四个细分标记。
- `ahci probe done` 1.08 s、`nvme probe done` 1.14 s、`usb probe done` 2.13 s 都是驱动内部的重置等待，没动：先把真机每段拆开再决定，避免瞎改驱动时序。
- 源码全部未提交；是否 commit / 合并 PR #19 未拍板。

## B-13 扫描缓冲改成 write-combining（晚开 CR4.PAT，默认关闭）

### 现象（承接 B-12）

- 真机 `[MM] firmware left CR4.PAT clear` + `[MM] framebuffer at 0x4000000000 marked … cache-disabled`：扫描缓冲是强不可缓存（UC），每个像素写就是一次独立总线事务。
- `mm_mtrr_wc_cover()` 在这台机器上注定失败：0x4000000000 落在固件的 WB RAM 范围里，MTRR 类型按交集解析，WB ∩ WC 还是 WB，所以它按"别人已经描述过这段"直接返回 0。
- B-12 的 16 字节非临时存储把事务数从 8.3 M 降到 520 K，但仍然是 UC；真正的解法只有让页表选择 PAT 的 WC 类型。

### 改动

1. `kernel/mm.c`：新增 `mm_enable_pat_late(fb_phys, fb_size)`——在 CR3 已经切到自家页表之后才动手：
   - 置 CR4.PAT（**关键点**：固件页表在用时置位会改变固件自己映射的位含义，之前那次"开机停在这一行"就是这条）；
   - 写 IA32_PAT entry 4 = WC（entry 0 仍是 WB、entry 2 仍是不可缓存，其余映射行为不变）；
   - 用 `mm_map_window_bits(g_id_pdpt, …)` 把扫描缓冲那 4 个 2 MiB 叶子改成 PAT 位；
   - 4 KiB 粒度 `invlpg` 整段 + `wbinvd`；
   - **自证**：往第一个像素写 0x5A5A5A5A、`mfence`、读回；读不回来就把叶子改回 PCD 并 wbinvd 退出，绝不把画面留在"看起来开了 WC 其实是垃圾"的状态。
2. `kernel/mm.c`：新增静态 `g_id_pdpt`，在 identity PDPT 建好时记下来，供上面这一趟用。
3. `kernel/mm.h`：导出 `mm_enable_pat_late()` 的约定说明。
4. `graphics/nd_hal.c`：走"启动映射直接用"那条路径时，用 `mm_pat_available()` 这个唯一事实源填 `g_fb_wc`，并打印 `[ND] boot scanout is write-combining (PAT)` 或 `stays uncached; full-frame copies stay slow`。
5. 默认**不启用**：调用点包在 `#if defined(NOVUS_PAT_LATE) && NOVUS_PAT_LATE` 里，`-DNOVUS_PAT_LATE=1` 才开。

### 验证

- `build_main.cmd` exit=0，镜像 67,108,864 B，SHA256 前 12 位 `B9271DD38D0F`（关闭态）。
- QEMU（q35 + pflash 4M）：完整启动到调度器，`[MM] paging enabled`，`[ND] primary scanout used as it stands (boot mapping)` + `stays uncached`，`[ND] copy … direct nt`，`[INP] usb mice=1`，`[HALT]` 一次，fileman/taskman/settings 全部拉起。
- 关闭态与 B-12 同参对比（QEMU 内相对量）：`first frame +3533ms → +1094ms`、`usb hub done +4749ms → +2190ms`（B-12 的加宽存储 + 非临时存储确实在起作用的旁证）。

### 未做 / 未验证

- **开启态在任何模拟器上都验证不了**：QEMU 的 qemu64 模型 CPUID 报有 PAT，却在 `mov %rax,%cr4` 上拒绝——RIP 停在 `mov %rax,%cr4` 不动、CR4 读回还是 0x668、没有任何串口输出。所以这一条是"代码就绪、真机未跑"。
- 真机开它意味着：如果停在 `[MM] PAT late: enabling CR4.PAT with our own tables live` 之后没有任何后续行，就是 CR4 写被拒，回退办法是把镜像换回 B-12/B-13 关闭态（不动源码）。
- 打开后仍要测的两件事：`[NDF] present … us=` 是否从 B-12 的几十毫秒级掉到个位数毫秒；`[NDF]` 的桌面首帧那 23.6 s 是否进一步塌掉。
- MTRR 拆分路线（把固件 WB 范围切成 WB 下段 + WC 扫描段 + WB 上段）没做：重叠 MTRR 不同类型在 SDM 里是未定义行为，为了一个已经能用 PAT 解决的问题去动固件 MTRR 不划算。
- 源码全部未提交；是否 commit / 合并 PR #19 未拍板。
## B-14 晚开 CR4.PAT 转默认开启（`-cpu max` 下验证通过）

### 做了什么

- `kernel/mm.c` `mm_enable_pat_late()` 的自证屏障从单条 `mfence` 改为 `sfence + mfence`：WC 缓冲由 SFENCE 刷出，MFENCE 只负责顺序。原来只靠 mfence 时，读回可能落在未合并的写之前，把自证误判成"扫描缓冲不应答"而白白退回 UC。
- `Makefile`：`-DNOVUS_PAT_LATE=$(if $(filter 0,$(PATLATE)),0,1)` —— 默认开，`PATLATE=0` 可关（B-13 的反向开关保留，CFLAGS 里展开值不变时不会触发重编）。
- 构建 `E0E07967251C`，67,108,864 B。

### 为什么这次算验证过了

B-13 结论里"QEMU 死在 `mov %rax,%cr4`"是对的，但只对默认 qemu64 模型成立。换 `-cpu max` 后同一条路径完整跑通（先用仅差 fence 的 `E6DF4F7A6F86`，再用本版 `E0E07967251C` 各跑一次）：

- `[MM] PAT late: enabling CR4.PAT with our own tables live`
- `[MM] PAT late: scanout is write-combining, 0x0000000000400000 bytes` —— 写/读自证通过
- `[MM] paging enabled, CR3=0x…`、`[ND] boot scanout is write-combining (PAT)`
- `[T] +1212ms first frame`、`[INP] usb mice=1`、`[BOOT] report drawn`，随后 `[NWM] first frame win=3/4/5`（fileman/taskman/settings 三个窗口全起），`[NDC] take# empty` 之后长时间稳定运行（日志 5174 行无 panic）

### QEMU 量不出收益

TCG 不模拟 WC/UC 的写合并行为，`-cpu max` 下 `present 1024x768` 的 cycles 与关闭态几乎一致（14.6M 对 14.6M）。QEMU 在这里只能证明"功能正确"，不能证明"变快"；判据只能是真机。

### 未做 / 未验证

- **真机未跑**：只保证 CR4.PAT 写入被接受、自证通过、系统继续启动。EliteBook 上 `[NDF] present … us=` 的实际值、`[HALT]` 之后那段静默桌面合成缩短到什么程度，都还没有读数。
- 真机若自证失败，会打 `[MM] PAT late: scanout did not answer under write-combining, put back to uncached` 然后按 UC 继续——这是设计好的回退，不是新 bug。
- 兜底记录（真机不该发生）：若卡在 `enabling CR4.PAT` 之后再无任何后续行，把 U 盘换回 `B9271DD38D0F` 或 `4F026CE37654` 即可，不需要动源码。
- PAT 开启后 PCD 页走 index 2，而我们写的 IA32_PAT 里 index 2 = 7（UC-，Intel 出厂默认），于是 MMIO 从"强不可缓存"变成"不可缓存、允许弱序"。所有 MMIO 访问都带 volatile 和显式屏障，静态审计未发现依赖强 UC 的点；但真机上 AHCI/NVMe/USB 的 DMA 环长时间跑压力没做过。
- 源码全部未提交。

## B-15 启动日志按字符计费：把两处最贵的转储收进一行

### 依据

真机 `44AD959E4FB0` 日志逐段量的结果（按 `[T] +Nms` 切段、累加字符数）：

| 窗口 | 字符数 | 时长 | 每字符 |
| --- | --- | --- | --- |
| `power and storage probed` → `device table built` | 4,780 | 5.9 s | 1.23 ms |
| `session started` → `session and network ready` | 4,298 | 7.3 s | 1.70 ms |

115200 波特下串口每字符 87 µs，所以这两个窗口的时间有 93% 以上不在"探测"上，而在**打印**上：设备表窗口 85 行打印里有 48 行是每张 SSDT 打两行横幅（本机 24 张 SSDT，全都既没有 I2cSerialBus 也没有 PNP0C50），网络窗口 4,298 字符里约 4,100 是 `pci_log()` 用 16 位零填充打印 22 个设备的七列十六进制。

### 做了什么

- `kernel/pci.c` `pci_log()`：改成一行、不补零。`[PCI] 22 devices:` 后面每个设备按 `bus:dev.fn=vendor:device/classh+subclass[@bar0]` 排列（`pc_hex` 去前导零，`pc_hex2` 保证 class:subclass 读成 `0600` 而不是 `60`），缓冲区 896 字节，溢出补 ` ...`。同样的信息，约 1/8 的长度。
- `kernel/touchpad.c`：`scan_table()` 先统计后打印，**没有命中的表一行都不打**；命中才打横幅 + 命中行 + 十六进制窗口。新增文件级计数器 `g_tp_tables` / `g_tp_tables_hit` / `g_tp_serial_total` / `g_tp_pnp_total`，`touchpad_probe()` 末尾出一行汇总 `[TP] acpi tables visited=… scanned=… with-hits=… serialbus=… hid-over-i2c=…`。本机因此从 50 行降到 2 行左右。
- 构建 `C063DCE2FDBE`，67,108,864 B。

### QEMU 验证（q35 + pflash 4M，`-cpu max`）

完整启动到桌面三个窗口全起，新格式按预期落盘：

- `[PCI] 7 devices: 0:0.0=8086:29c0/0600 0:1.0=1234:1111/0300@c0000000 0:2.0=1b36:d/0c03 0:3.0=1af4:1001/0100 0:1f.0=8086:2918/0601 0:1f.2=8086:2922/0106 0:1f.3=8086:2930/0c05`
- `[TP] acpi tables visited=0x6 scanned=0x1 with-hits=0x0 serialbus=0x0 hid-over-i2c=0x0`

### 未做 / 未验证

- **真机未跑**：省下的秒数要下一次真机日志里 `devtab table` / `net probes` 两个标记的差值才能确认；按每字符成本推算设备表窗口约省 3 s、网络窗口约省 6 s，但这只是推算。
- 十六进制窗口（`dump_window`）本身没动，命中时才打；本机 DSDT 命中两处，仍会打 3 行左右。没去掉它是因为它是"这台机器的触控板挂在哪"的唯一现场。
- `probe_pci()` 里每个 serial-bus 控制器仍用十六进制逐字段打印（本机 3 个约 330 字符），未改。
- 源码全部未提交。

## B-16 文本控制台改画在影子缓冲里，整窗一次贴上去

### 依据

`kernel/fbcon.c` 的控制台窗口最大 1200x648，每换一行 `fbcon_scroll()` 就要把这 2.95 MB 从扫描缓冲搬到扫描缓冲——在扫描缓冲被标成不可缓存的机器上，这是每行一次约 3 MB 的"读 + 写"不可缓存流量；每画一个字符还要逐像素 `fbcon_fill()`。这是"每字符 1 ms 级"开销的来源之一。

### 做了什么

- `kernel/fbcon.c`：控制台改为先画进普通可缓存 RAM（`pmm_alloc_contig`，最大 1200x648x4 = 3.1 MB），整窗经新增的 `fbcon_present()` 一次写进扫描缓冲（只写不读、结束 `sfence`）。取不到这段连续物理页时**自动退回**原来的直写路径。
- 贴窗节奏：每换一行记一次待贴，**满 3 行或满 8 ms** 贴一次（`FBCON_FLUSH_US`）；`fbcon_start()` 首画与 `fbcon_end()` 强制贴。
- 影子块按最大需求分配并跨 `fbcon_begin` / `fbcon_begin_box` 复用（splash 的日志面板更小，直接套用）。
- 构建 `5ADFA6B94214`。

### QEMU 验证

完整启动；`[BOOT] report drawn` 之后的报告页与原构建逐项一致（`log=0 why=ACPI_BGRT nogpt`、`evs=02/08/0e`、`dq0=ffffffff es0=f` 等），三个桌面窗口全起，无 `[WARN]`。

### 未做 / 未验证

- **真机未跑**，而且真机上这条路径目前不会被走到：`kmain.c` 只在 `nd_boot_splash() == 0`（没有 splash）时才 `fbcon_begin()`，而 EliteBook 全程有 splash。所以 B-16 眼下只对"无 splash 的机器 / shell 构建"有意义，对笔记本启动时间没有直接收益。
- 3.1 MB 是 pmm 连续分配，真机内存充裕，但没有在内存碎片化下验证过；退回路径已在代码里。
- 源码全部未提交。

## B-17 日志面板不再一行一重画：五行或三十毫秒合一次

### 依据

splash 的日志面板 `splash_log_paint()` 每收到一行就整块重画：黑底填充 + 半透明混合 + 把所有可见行重新排版。面板是扫描缓冲上的一条带（本机 2880x2160），在不可缓存扫描缓冲上一行一次的代价就是"1 ms/字符"的主体。`kputs()` 每个完整行都调 `nd_boot_log_refresh()`，于是"打印越快、画得越多、启动越慢"。

### 做了什么

- `graphics/nd_boot.c` `nd_boot_log_refresh()`：改成合并刷新——**待画满 5 行或距上次刷新满 30 ms** 才重画一次（`SPLASH_LOG_REFRESH_US` / `SPLASH_LOG_MAX_PENDING`）。既有的 `nd_boot_stage()` 重画会在阶段切换时把剩余行补上。
- 构建 `2A9D945B2D1E`（含 B-14…B-17）。

### QEMU 验证

完整启动；`绘制桌面` 阶段截图正常，`[NWM] first frame` 三个窗口全起，报告页数据正常，无 `[WARN]`。

### 未做 / 未验证

- **真机未跑**：合并后 80 行的突发从 80 次面板重画降到约 16 次，但真机每次重画的实际毫秒数和省下的总秒数都还没有读数。
- 机器若在突发中途挂住，最多有 4 行已完成的行不在屏幕上；U 盘/NVMe 上的日志逐行写入，那 4 行仍在。
- 面板仍是直接画在扫描缓冲上（没做影子面），每次重画成本仍受"不可缓存 blend"限制，只是次数少了。要再省就得把整个 splash 改成"画在 RAM、整帧贴上去"，那是下一步。
- 源码全部未提交。
## B-18 打印与绘制之外：先把"一帧到底多贵"读出来（诊断）

### 起因（真机 `2A9D945B2D1E` 一次启动的实测，2026-11-26）

用提权脚本回读 NVMe 上的日志后，终于拿到 `2A9D945B2D1E` 这一 boot 的读数（日志头被 usblog 的 RAM 环形缓冲溢出丢掉，只能从 `+7553ms` 看起）。关键几条：

- `[NDF] present 1920x1080 cycles=1463638960 us=586393`（第一次整屏 present，586 ms）
- 紧接着 `[NDF] rise ... us=707204`
- 第三次起 `present ... us=1384/1400/1461`，即 **1.4 ms** 同一张整屏

同一次启动里 `present` 从 586 ms 掉到 1.4 ms，差在**途中那 9 行 `[ND] copy row` 诊断打印**：`surface_copy()` 当时每 128 行打一行"读地址/写地址"。也就是说整屏拷贝本身只要 1.4 ms（6 GB/s 级，写合并是成立的），而 9 行日志花了 585 ms —— **约 65 ms/行、0.76 ms/字符**。

同一份日志的窗口账也对得上：`power and storage probed +7553` → `device table done +12141` 这 4.6 s 里约 4.8 KB 文本；`session init +18075` → `network probes done +23767` 的 5.7 s 里约 6 KB 文本。

### 做了什么

- `graphics/nd_hal.c` `surface_copy()`：删掉 `[ND] copy row` 的每 128 行回显（只保留前两次拷贝的一行 `[ND] copy … direct nt` 与它自己的读/写地址）。9 行/整屏 × 65 ms 是直接摊在 present 里的。
- `kernel/mm.c`：新增 `g_pat_late` 记录 B-14 晚开 PAT 的结论（0 未跑 / 1 成功 / 2 CPUID 无 PAT / 3 窗口不在 identity map / 4 回读不应答），并新增 `mm_report_scanout()`：把扫描缓冲那一页**从活页表里读回来**的页属性（PAT/PCD/PWT）、覆盖该物理地址的 **MTRR 类型**、MTRR 槽位与 `MTRRdefType`、以及晚开 PAT 的结论，拼成一行打印。判断依据是"有效类型 = 页属性 ∩ MTRR"，所以只看页属性不算结论。
- `graphics/nd_hal.c`：新增 `nd_report_scanout()` 与 `nd_fb_bench()`。后者用同一套 `movntdq` 非临时存储往**最后一行可见扫描行**写满一整行（1920×4 B），用 TSC 计时并打印 `cycles/byte`，写完再把那一行从画布贴回去（一行拷贝，不是重画）。
- `kernel/kmain.c` 桌面分支：`novus_timeline("first frame")` 之后立刻调这两个。位置刻意放在首帧之后——日志介质的头部会被丢掉，写在 +700 ms 的结论没人能看到。
- 构建 `3FE00397FD5B`（含 B-18 + B-19）。

### QEMU 验证

完整启动，桌面起，`[INP] usb mice=1`，无 panic。新增两行实测输出：

```
[MM] scanout type: page=WC mtrr=UC leaf=0x00000000C00010E3 pat_late=ok mtrr_slots=0x3/0x8 mtrr_def=WB fix=1
[ND] fb bench 0x1000 B in 0x23C76 cycles, 0x23 cycles/byte
```

QEMU 里 `mtrr=UC` 是 QEMU 的 MTRR 布局（不是本机），`pat_late=ok`、页属性 `WC` 说明晚开 PAT 这条路径本身是通的。

### 未做 / 未验证

- **真机未跑**：`page=`/`mtrr=`/`cycles per byte` 三个值在 EliteBook 上还没有读数。B-18 的价值就是把"586 ms 里多少是拷贝、多少是别的东西"变成可读的量。
- 如果真机读出 `mtrr=UC`，说明页属性的 WC 被 MTRR 交集掉了（这时 `mm_mtrr_wc_cover()` 也没救：8 MiB 的 WC 范围与固件那条覆盖 16 GiB 的 WB 范围部分重叠，部分重叠一律放弃）。那 present 就回到"少写字"的路子上，memory type 这条路走不通。
- `nd_fb_bench()` 会覆盖最下面一行可见扫描行几十毫秒，然后靠一次单行 present 复原；画布不可用时那一行会留到下一次重画。
- 源码全部未提交。

## B-19 文本输出不再走每字符一次的老式端口 I/O（启动速度的主项）

### 判断

B-18 从真机读数里得到一个坐实的事实：**本机每打印一个字符约 0.76 ms**，而整条启动日志约 30 KB 文本 —— 25 s 左右的时间是"打印"本身，不是探测，也不是绘制。`kputs()` 有四个落点：VGA 文本缓冲、COM1、RAM 里 `klog` 环、`usblog` 缓冲，以及 splash 面板。其中两个是**逐字符访问老式 I/O 端口**的：

- `kernel/serial.c` `serial_putc_sync()`：每个字符一次 `io_in8(0x3FD)` 等 THRE + 一次 `io_out8(0x3F8)`。
- `kernel/vga.c` `vga_putc()`：**每个字符**都调 `vga_update_cursor()`，而那是一次 4 个 `io_out8`（`0x3D4/0x3D5`）的 CRTC 更新。

这两组端口在笔记本上由平台固件接管（SMI trap），一次访问就是约 0.7 ms —— 0.76 ms/字符正好对上，一个字符两次访问还只是"部分被 trap"。115200 baud 的位时间只有 8.7 µs，完全解释不了 0.76 ms。

### 做了什么

- `kernel/serial.c`：`serial_init()` 末尾用 `rdtsc` **实测一次端口访问的周期数**（两次 `LSR` 读 + 一次 scratch 寄存器写，取最大值）；超过 `SERIAL_SLOW_CYCLES`（100000 cyc，约 40 µs@2.5 GHz，真实 16550 只该是几千 cyc）就把控制台置为静默：`serial_putc()` 直接返回。日志仍由 VGA、`klog` 环、`usblog`/NVMe 和屏幕承载，一个都不丢。
- `kernel/vga.c`：`vga_init()` 用同样的办法实测一次 CRTC 更新（4 次端口写）的成本；判定"病态"就不再碰 CRTC。无论是否病态，`vga_putc()` 都从"每字符更新光标"改成"每行更新一次"（`g_vga_cursor_due`）。文本缓冲的写入不变，真接 VGA 输出的机器看到的内容一模一样。
- 两个驱动各自导出 `serial_io_cost()/serial_is_quiet()`、`vga_io_cost()/vga_is_quiet()`。
- `kernel/kmain.c`：`serial_init()`/`vga_init()` 之后打一行 `[CON] text sinks: uart <cyc>/access (on|quiet), vga crtc <cyc>/write (on|quiet)`。不管结论是哪一个，下次回读日志就能直接看到实测值。
- 构建 `3FE00397FD5B`。

### QEMU 验证

完整启动。QEMU 的端口访问本来就便宜，两个 sink 都保持开启，串口日志逐行完整：

```
[CON] text sinks: uart 0x31E cyc/access (on), vga crtc 0x4B2 cyc/write (on)
```

即"正常的机器不受影响，串口抓取照旧"这条前提在 QEMU 上成立。

### 未做 / 未验证

- **真机未跑**：EliteBook 上这两个 `cyc/access` 到底是多少、`(quiet)` 会不会触发、带不带得回 20 s 量级，都要等一次真机启动的回读。
- 若真机读出**两个都便宜**，那 0.76 ms/字符另有出处（例如 `kbm()` 调试端口、`vga_scroll()` 的 UC 内存搬移，或 splash 面板），B-19 就不省时间；但 `[CON]` 那一行会把两个候选排除掉，下一次直接查剩下的。
- 静默 UART 后，真机上不再有串口输出；这台机器本来就没有可用的串口接线，日志靠 NVMe。
- 光标改成每行一次，真接 VGA 文本输出的机器上，一个人为盯着光标"逐字符走"的观感变了（内容不变）。
- 源码全部未提交。

## B-20 PCI 配置空间改走 MCFG/ECAM

### 依据（真机读数）

`2A9D945B2D1E` 那次真机启动里，`device table done` 一段是 **927 ms / 174 字符**：按本机实测 ≈0.76 ms/字符算，打印只占 130 ms，剩下约 800 ms 全是计算。同一段在 QEMU 是 72 ms，且 QEMU 只找到 22 个功能——一次 256 总线 × 32 设备的机制 #1 遍历是 `256×32 = 8192` 个设备头读，加多功能的 id 读，约 9000 次 `0xCF8/0xCFC` 访问；800 ms / 9000 ≈ **110 µs 一次**。真实 82801 级南桥的一次 CF8 访问只该是几百纳秒，差三个数量级的唯一解释是**固件把这对 legacy 端口 trap 进 SMM**。更要命的是这不只发生一次：`net_init` 里五个驱动各自 `pci_find`（每次都是从总线 0 扫到 255），hda、i2c、touchpad、input 各一次，真机 `session init → net probes` 那 5607 ms 里约 4.9 s 就是这些重复遍历。

### 改动

- `kernel/pci.c`：新增 ECAM（MMCONFIG）支持。`pci_ecam_init(base,first,last)` 校验窗口并把 `g_ecam/g_ecam_first/g_ecam_last` 立起来（`base==0` 是"显式关闭"，`last<first` 拒绝）；`pci_ecam_from_mcfg(mmap,size,stride)` 用 `acpi_find_table(...,"MCFG")` 找到表，从 `table+44` 取第一条配置项——地址按两个 dword 拆开读（表是 packed、可能不是 8 字节对齐），`first=e[10]`、`last=e[11]`；`ecam_at()` 拼 `base + (bus-first)<<20 | (dev&0x1F)<<15 | (func&7)<<12 | (off&0xFC)`；`ecam_covers()` 判断范围。`pci_read32`/`pci_write32` 在覆盖范围内走 `movl (%1),%0` / `movl %0,(%1)` 的 volatile 访问，否则照旧 CF8/CFC。
- **先量再选（不是无条件启用）**：`pci_ecam_bench()` 各打 32 次读，用 `rdtsc` 比出每条路径的周期数；ECAM 不更快就把窗口关掉，回 CF8。理由是"哪条更便宜"是**机器属性**：笔记本的 CF8 被 trap 到 110 µs、ECAM 是普通的 uncached 载入，差两个数量级；而 QEMU 的 MMIO 退出比它的端口快路径贵。`pci_config_cost()` 把两个实测值导出。
- `kernel/pci.h`：加 `pci_ecam_from_mcfg`/`pci_ecam_init`/`pci_ecam_info`/`pci_config_cost` 四个原型。
- `kernel/kmain.c`：`mm_init()` 之后、任何 pci 访问之前（`heap_init` 之后）调用，并且**只在窗口落在 identity map 内时才采用**（`base + 总线数×1 MiB <= mm_ram_top()`），保证一次读也不可能缺页；否则显式关闭。打一行实测：
  - 采用：`[PCI] config space is memory-mapped at 0x…, buses 0x0..0xFF, 8xx cycles/access vs 274xxx on 0xCF8`
  - 不采用：`[PCI] configuration stays on 0xCF8/0xCFC (3443 cycles/access, ECAM 3457)`

### QEMU 验证

- 采用分支真实命中：`[PCI] config space is memory-mapped at 0x00000000B0000000, buses 0x0000000000000000..0x00000000000000FF`，随后 `[PCI] 7 devices: …` 与之前逐字节相同（`8086:29c0/0600`、`1234:1111/0300@c0000000`、`1b36:d/0c03`、`1af4:1001/0100`、`8086:2918/0601`、`8086:2922/0106`、`8086:2930/0c05`），说明经 ECAM 读出的 id/class/BAR 与 CF8 一致。
- 加了 bench 之后，QEMU 实测 `3443 vs 3457`，判定 ECAM 更贵，**自动退回 CF8**，`device table done` 回到 +366 ms（不加 bench 的 ECAM 版本是 +503 ms）——"慢机器不被这个改动拖累"这条前提在 QEMU 上成立。

### 未做 / 未验证

- **真机未跑**：笔记本离线，`[PCI]` 那一行的两个实测值（以及 927 ms / 4.9 s 两段到底塌到多少）都还没拿到。
- ECAM 只取 MCFG 的**第一条**配置项。多段 MCFG（不同总线段落在不同窗口）的机器会只覆盖第一段，其余仍走 CF8——正确但没提速。
- 若固件把窗口放在 `ram_top` 之上（例如 4 GiB 以上的 PCIe 段），按设计退回 CF8，不冒险。
- 依赖 `acpi_find_table` 已提前可用：`pci_ecam_from_mcfg` 在 `mm_init` 之后调用，此时用固件的 identity map 读表；XSDP 找表的路径与 `acpi_pm_timer_port` 完全相同。
- 源码未提交。

## B-21 PCI 表只走一次并缓存

### 依据

B-20 解决了"一次遍历多贵"，没解决"遍历几次"。真机 `net_init` 五个 probe 各一次、hda/i2c/touchpad/input 各一次，即使 ECAM 把单次压到毫秒级，十几次也是几十毫秒白烧；在**没有 MCFG** 的机器（含 QEMU）上这些遍历是实打实的 900 ms 级支出。

### 改动

- `kernel/pci.c`：表放进 `.bss`（`g_pool[PCI_SHARED_MAX]`，160 项，约 20 KiB，本来就不该在引导栈上），`g_pool_valid/g_pool_n` 标记状态。原来的 `pci_scan()` 改名内部 `pci_walk()`；新的 `pci_scan()` 从缓存拷贝（`n>max` 截断）；`pci_scan_shared()` 首次调用建表、之后直接返回；`pci_find()`/`pci_find_class()`/`pci_log()` 全部改读缓存（顺带把 `pci_dev_t list[32]` 这两个栈上数组去掉了）。
- 新增 `pci_scan_invalidate()`（`kernel/pci.c` + `kernel/pci.h`），供开机后的热插拔路径重新遍历。
- 顺带的好处：BAR sizing 的"写全 1 再读回"以前每次遍历都会对每个设备重做一遍，现在只做一次，少了一串对真实设备的写入。

### QEMU 验证

同一次构建 `E068CA3462E6`：`net probes` 952 → **721 ms**，`device table done` 366 → 286 ms，`first frame` 1521 → 1271 ms，桌面起来、`usb mice=1`。设备表 7 项与未缓存版本逐字节相同。

### 未做 / 未验证

- **真机未跑**。热插拔路径改用了 `pci_scan_invalidate()` 之外没有任何调用者，即"开机后重新枚举"这条路本轮没被任何测试覆盖（USB 热插拔不经过 pci 扫描）。
- 缓存意味着运行期插入的 PCIe 设备（雷电/USB4）在调 `pci_scan_invalidate()` 前不会被看到。

## B-22 USB：控制器级"全程静默"后把每笔预算压到 300 ms

### 依据

`44AD959E4FB0` 的时间线里 `first frame +32170 → usb hub done +47193` 是 **15 s**，而 QEMU 同段只有 1.1 s。逐口算：5 个有设备的端口 × 3 轮 bring-up × `USB_WAIT_MS`(1000 ms) ≈ 15 s，正对得上。U-64 让"第一笔就零事件"的端口只做一轮，但那仍是一轮 1 s × 5 口 = 5 s。

### 改动

- `kernel/usb.c`：新增 `g_ctrl_silent[USB_MAX_CONTROLLERS]`（**按控制器**，一张 xHCI 的静默不该诬陷另一张）与 `USB_SILENT_BUDGET_MS 300u`。`control_xfer` 里判定 `pass==0 && code==0`（一笔传输既没有完成事件也没有任何完成码）时置位 `g_att_noev` 与该控制器的 `g_ctrl_silent`——这条证据属于**控制器的完成事件投递**，不是设备：坏设备会回 completion code（babble/transaction error/stall），只有"什么都不回"才是控制器侧的事。此后同一控制器的每次 `wait_transfer` 把预算压到 300 ms（`g_xfer_budget_ms` 本身不动，导航层仍然可以要求更小）。`usb_init()` 里 `memset` 清零。
- 能出事件的控制器一个字节都不改：预算只在"这台控制器已经证明确实不出事件"之后才打折。

### QEMU 验证

`AA25B6CF480B`：鼠标/键盘照常枚举（`usb mice=1`），`usb hub done` 在预算没有被压过的路径上照旧（QEMU 每笔都是立刻完成，`g_ctrl_silent` 不会置位）。

### 未做 / 未验证

- **真机未跑**：真机上到底省多少（预期 15 s → 约 2 s（4 口 × 300 ms + 首口 1 s））要等回读。若某口的设备其实会答但答得慢（>300 ms），这一改动会把它判成失败——但那种设备在同一台机器上必然先有一个"零事件"的端口，说明控制器确实不投递事件。
- 300 ms 是拍的，不是量的：真机没有一次"控制器能出事件"的成功样例可以标定。
- 与 USB 主线（为什么这台机器的完成事件一个都不来）无关，纯粹是止损。

## B-23 两个文本 sink 的成本改按"整字符/整次写"实测

### 依据

B-19 只测了**单次端口访问**的成本，因此漏掉了两种真实的贵：① UART 的 `LSR.THRE` 等待——115200 下一个字符 87 µs，若线速不是分频器说的那样或端口被 trap，等待会变成接近 1 ms，而**这段等待里没有任何端口访问可供计时**；② `0xB8000` 文本平面本身的写——GOP 机器上这块 legacy 区域根本不上屏，但被固件 trap 的机器照样按字符收费。真机实测 ≈0.76 ms/字符、而单次访问只有微秒级，正指向这一类。

### 改动

- `kernel/serial.c`：`serial_init()` 在原有单次访问计时之后，再**用同一个等待路径实打实推 8 个 NUL 字节**并计时（`g_line_cost`），平均超过 `SERIAL_SLOW_CYCLES*3`（约 120 µs）就 `g_serial_quiet=1`。导出 `serial_char_cost()`；`kernel/serial.h` 加原型。
- `kernel/vga.c`：`vga_init()` 在光标计时之后，对 `0xB8000` 的右下角单元连写 16 次并计时（`g_vga_store_cost`），超过 `VGA_SLOW_CYCLES` 就 `g_vga_text_off=1`：`vga_putc()` 仍维护行/列计数（后面接 `fbcon` 时要读），但不再往文本平面写。导出 `vga_store_cost()`；`kernel/vga.h` 加原型。注意右下角那格 `vga_clear()` 刚清过，写完仍是空格。
- `kernel/kmain.c`：`[CON] text sinks`（开头与首帧后各一行）扩成四项：`uart <cyc>/access, <cyc>/char (on|quiet), vga crtc <cyc> + text <cyc> cyc/write (on|quiet)`。

### QEMU 验证

`AA25B6CF480B`：

```
[CON] text sinks: uart 0x0D34 cyc/access, 0x9767 cyc/char (on), vga crtc 0x85F + text 0x189F cyc/write (on)
```

两个 sink 都保持开启（QEMU 端口便宜），串口日志逐行完整、桌面正常。

### 未做 / 未验证

- **真机未跑**：真机的 `<cyc>/char` 与 `text <cyc>` 才是这两个新判据的唯一证据，还没拿到。
- 若真机 `<cyc>/char` 也便宜（例如 40 万 cycle 以上才算超限，而它只有 20 万），串口会照样全速写，那 0.76 ms/字符就还得在别处找（`kbm()` 的调试端口、或 splash/`fbcon` 的绘制路径）。
- `serial_init()` 会往 COM1 推 8 个 NUL：日志文件开头因此多 8 个 NUL 字节，读的时候按二进制忽略即可。
- `g_vga_text_off` 只在实测病态的机器上生效；真 VGA 输出（CSM/legacy）的机器不受影响。

## B-24 桌面交接动画 8 步压到 4 步 + ECAM 采用前的等价性闸门

### 依据

`nd_demo_init()` 的收尾是"码头从下升起"的交接动画：`rise_steps = 8`，每一步都是 **整屏合成（`nd_demo_paint(full)`）+ 整屏上屏（`nd_present(full)`）+ `nd_wait_ms(18)`**。这段动画位于"机器已经启动完成"和"桌面可以接受输入"之间，所以它的每一毫秒都算在人的感受里。QEMU 同一台机器实测：一步 rise 是 45–70M cycle（约 18–28 ms，其中 18 ms 是等待），一次 present 是 36–57M cycle（约 15–23 ms）；八步合计约 0.3 s，而真机 `demo init start +23855 → first frame +27082` 那 3.2 s 里这段也占一份。

同时补一条**正确性闸门**：B-20 的 ECAM 采用逻辑虽然校验了"窗口在 identity map 内"，但没有校验"这个窗口描述的确实是同一台机器"。固件若发布一条内核读不懂的 MCFG 项（base 写错、或第一条不是总线 0 的段），ECAM 会把所有设备读成"不存在"——整台机器丢盘、丢音频、丢 xHCI，比慢几个数量级更糟。

### 改动

- `graphics/nd_demo.c`：`rise_steps` 8 → **4**。缓动仍是 ease_out 均分四帧，动效还在（"升起来"这件事仍然看得见），但整屏合成与上屏各少四次、等待少 4×18 ms。
- `kernel/pci.c`：`pci_ecam_bench()` 在计时之前先做**等价性校验**——用 ECAM 与 CF8 各读一次 `bus=first / dev 0 / fn 0 / off 0`，两者不等就关掉 ECAM（`g_ecam=0`、`first/last` 清零），退回机制 #1。代价是两条读，收益是"固件 MCFG 不可信时不会把整棵树读成空"。

### QEMU 验证

`A1038340AE4E`：

```
[T] +690ms session init
[BA] handover: wallpaper in one frame, dock rise
[NDF] totals frames=0x5 cycles=0xC63BAB4 pixels=0x3C0000 max_cycles=0x35A328C
[T] +1154ms first frame
[T] +2278ms usb hub done
[INP] usb mice=1 reports=0 i2c=0
```

- `frames=5`（首发 1 + rise 4），此前同参数是 9（首发 1 + rise 8）。
- `first frame` 1238 → **1154 ms**（同一台 VM、同一套 QEMU 参数下的相对下降约 84 ms，正好是四步的等待 + 四步的合成/上屏）。
- ECAM 等价性校验通过（QEMU 的 MCFG 窗口读出的 id 与 CF8 一致），随后 bench 判定 CF8 更便宜并退回，`[PCI] configuration stays on 0xCF8/0xCFC (1634 cycles/access, ECAM 1939)`。
- 桌面正常起来，`usb mice=1`。

### 未做 / 未验证

- **真机未跑**：四步动画在 1920×1080 上到底省多少（取决于真机一步 present 是 1.4 ms 还是更贵）没有读数。
- 动效观感变化：原来的八帧缓动变成四帧，起落更"干脆"。若认为太生硬，`rise_steps` 是一个常量，改回 8 即可，其余逻辑不动。
- 等价性校验只看 `dev 0 / fn 0 / off 0` 这一个 dword。它足以识别"窗口错位/指向别的段"，但如果固件把窗口整体平移了整数个设备槽（恰好让 0:0.0 仍读到同一值），这条闸门看不出来——那种情况下 bench 也会因为 ECAM 不更快而退回，只是不一定。
- 源码未提交。
### B-25 串口静音改成累计时间预算（不再用单次采样阈值）

- 起因：给 QEMU 加第二块盘做日志路径测试时，串口在 `exit boot services` 之后**一行都没有**。原因不是回归，是 B-23 的判据太脆：`serial_init()` 里那 8 个 NUL 的单次采样在负载机器上测出 >`SERIAL_SLOW_CYCLES*3`，于是整条控制台被静音，而 UART 本身完全正常（QEMU 2 核上同时挂着 5 个旧 QEMU，load 4.5）。
- 改法（`kernel/serial.c`）：B-23 的单次采样保留，但只作记录（进 `[CON]` 行），**决定权改成累计**。`serial_putc_sync()` 在每次真正等待+写字节时用 rdtsc 包住，累加到 `g_spent`（新静态量，`UINT64`）；一旦 `g_spent > SERIAL_BUDGET_CYCLES`（1500000000 ≈ 0.6 s@2.5GHz / 1.5 s@1GHz）才置 `g_serial_quiet=1`，并把 `g_spent` 夹住不再增长。
- 语义：健康 UART 全程花不到预算 → 永远不静音；只是“忙”的机器也到不了预算 → 控制台保住；只有真正病理（0.76 ms/字符这类）在几百行之后自动退场，且退场前头部已经在外面了。
- 新增导出 `serial_spent()`（`kernel/serial.c` + `serial.h`）。`kernel/kmain.c` 的首部与首帧后两行 `[CON] text sinks` 都扩成 `uart <cyc>/access, <cyc>/char, <cyc> spent (on|quiet)`。
- QEMU 实测（`B01CE198F545`，单盘干净运行）：首部 `uart 0xD4E/0xA30F, 0x167C4A spent (on)`；首帧后 `… 0xEEA6406 spent (on)`；串口全程有输出，`first frame +1118 ms`、`usb hub done +1128 ms`（无 USB 设备配置）、`+1136/+2273`（有 USB 鼠标配置），与 B-24 基线 `1154/2278` 持平。
- 未做/未验证：真机没有读数（`SERIAL_BUDGET_CYCLES` 是否在笔记本上把日志砍得太早，只能等下一次刷盘）；`g_spent` 不计中断驱动的 `serial_tx_drain()` 路径里的等待（那条路径在早期 boot 上不启用，IRQ 关着就走同步路径）。

### B-26 日志介质：开写前清空文本区 + 头部带计数；并修强制刷尾的扇区计数

- 三处改动（全在 `kernel/usblog.c`）：
  1. `usblog_clear_region()`（新）：`usblog_init()` 找到介质后、写头部之前，把 255 个文本扇区按 8 扇区一批全零写一遍。原因：区域里留着**上一次** boot 的文本时，读回脚本“遇全零扇区停”的规则会把新旧两段拼成一条流，头部看起来像“被截断”（真机 `novus-log.txt` 开头那行孤立十六进制就是这么来的嫌疑最大）。清空后每次读回只描述这一次启动。代价 130 KiB 写一次。
  2. `usblog_header_final(sectors, chars)`（新）：把 LBA 64 的头部重写成带计数的版本——`device`、`first text sector`、`so far: N sectors, M chars, dropped K chars`、`state: …`、以及区域说明。**每次真正写出扇区之后都重写一次**（不是只在最后一次），因为 boot 在最后一个 checkpoint 之后还会继续打印（`[HALT] entering the scheduler`、应用启动），写一句“已关闭”再配过时的数字，正是读回时无法验证的那种陈述。
  3. 修反向刷尾计数：原来 `if (written > 0u && written <= g_len)` 才推进 `g_next`，而强制刷（`force=1`）会把最后扇区零填充，`written` 必然大于 `g_len`，于是这批扇区写进去了但 `g_next` 不涨，头部计数比磁盘实际少。改成 `written > 0u` 就推进 `written/512`，`g_len` 归零或左移。副作用是失败批次（`written < g_len`）也会按已写扇区推进，这本来就是磁盘真相。
  4. 新增 `g_total`（`UINT32`，不复位）：`usblog_puts()` 每接受 n 个字符就累加，作为头部 `chars` 的来源；`g_dropped` 仍是 init 时归零的那个。
- 端到端验证（QEMU 里第一次跑通笔记本实际走的那条路径）：给测试盘加一个手工 GPT（首分区 4096 起，`sgdisk/parted` 在 vm 上都没有，用 Python 按规范写 header/entries/backup），挂成 `-device nvme,serial=novlog`，启动后：
  - `[LOG] gpt lowest partition at LBA 0x1000`、`[LOG] no stick, using the disk's GPT gap on nvme0`、`[LOG] boot log -> nvme0 at raw LBA 0x41`、`diag: log=1 … gap=ok`、`[BOOT] report page skipped: the log is on the medium`（45 s 报告页保持被跳过）。
  - 主机侧按真机读回脚本的同一条规则（header LBA 64、正文 LBA 65 起、遇全零停）读盘：头部 `so far: 46 sectors, 23318 chars, dropped 0 chars` / `state: 1 (region has room)`；正文 400 行、从 `[CON] text sinks` + `[T] +55ms kmain start` 一直到 `[HALT] entering the scheduler`，无旧 boot 残段。
  - 桌面截图（无 45 s 保持那条路径）：设置窗口**内容区正常绘制**（系统/设备信息/显示输出/资源），文件管理器列出 `/Novus/bin` 下 fileman/taskman/settings/netman/session/setup/nsh/devmgmt/novusd，dock、时钟 15:12 正常，指针在。即之前认为“窗口内容纯白”的现象在这条路径上不出现。
- 未做/未验证：头部 `chars` 与正文实际字符数仍有约 115 字符的差（最后一次强制刷之后还有 3 行 `[APPS]/[NUC]/[HALT]` 被后续 flush 写出，头部没跟着刷新到）。不影响内容完整性（无丢字符、无旧数据、末行正确），只是计数偏小一点，作为本地测试辅助工具留作已知限制，未再追。
- 另外记住的两件事：QEMU 直连两块 virtio 盘时内核查不出“磁盘 gap”（virtio 盘在内核里只以 `liveimg`/`ramdisk` 形式出现），要么挂 NVMe 控制器、要么就走不到这条路径；两次采样判静音的坑已在 B-25 里收掉。
### B-27 固件阶段日志进介质日志（补上启动前 30 秒的盲区）

- 问题：内核阶段的所有日志（串口/屏幕/磁盘）都从 `kmain` 起算，而真正耗时最长的是它之前的固件阶段（stage3：读 ESP、解 live root、校验、splash 动画）。真机 `novus-log.txt` 里最早一条是 `+7553ms power storage probes`，前面整段没有记录。boot stage 其实一直在往共享内存的 `boot_log` 环形缓冲（4096 字节）里写，`klog_init()` 只把它灌进 RAM 环形缓冲，供 splash 面板显示，**没有进介质**。
- 改法（`kernel/kmain.c`）：`kmain()` 进来第一件事（`shared_mem_kernel_init()` 之后、`klog_init()` 之前）用 `klog_snapshot()` 取那份交接环（此时 `g_len==0`，函数走的就是 `boot_snapshot()` 分支），把 4 KiB 尾巴按顺序 `usblog_puts()` 进介质日志，并加两行分隔：`[BOOT] firmware stage, tail of the handoff ring:` 与 `[BOOT] kernel stage follows`。缓冲区是 `static char boot_tail[4096]`（`kmain` 里，`.bss`），不占内核栈。
- 为什么放在最前面：`usblog_puts()` 只是往 RAM 缓冲追加，介质晚到也照样按调用顺序落盘，所以固件尾巴天然排在 `[CON] text sinks` 之前，读回就是完整时间线。
- QEMU 验证（`B573F00540C6` + NVMe gap 盘）：头部 `so far: 52 sectors, 26184 chars, dropped 0 chars`；正文 472 行，开头即分隔行 + `[T] +56ms efi_main start`、`+62ms shared memory init`、`+70ms stage3 init`、`+115ms stage3 menu done`、`+121ms splash capture done`、`+140ms live root done`、`+847ms splash anim done`、`+848ms exit boot services`，随后 `[CON] text sinks` 与内核时间线，末尾仍是 `[HALT] entering the scheduler`。整段固件尾巴 2906 字节，未截断。
- 稳定性：同一镜像连跑三次标准配置（q35+pflash+qemu-xhci+usb 鼠标）：`first frame +1142/+1127/+1143 ms`、`usb hub done +2277/+2263/+2279 ms`、`usb mice=1` 三次一致，无漂移无异常。
- 未做/未验证：真机没有读数；环形缓冲只有 4096 字节，真机固件阶段打印若超过这个量，读回只能看到最后 4 KiB（时间戳都在后半段，够用）；`boot_tail` 若末尾没有换行会补一行，否则与 `[CON]` 行粘在一起。
### B-28 整屏 present 拆成三段计时 + 报出此刻的 scanout 内存类型

- 目标：真机上第一次整屏 present 是 586 ms、第二次 707 ms、第三次起 1.4 ms（`[NDF] present` 的读数）。同一个动作差三个数量级，只可能是“前两次跑在另一种内存类型/另一种映射上”，但现有日志只在首帧之后打一次 `[MM] scanout type`，看不到 present 当刻的状态。
- 改法（`graphics/nd_hal.c` `nd_present()`）：把 `present_wait_blank()`、`surface_copy()`、以及剩下的部分（镜像刷新 + 指针合成 + sfence）各用一次 rdtsc 括起来；对**前三张整屏 present**打一行：
  `[NDP] full present#N wait=<cyc> copy=<cyc> rest=<cyc> wc=<0|1>`
  `wc` 取 `nd_framebuffer_is_wc()`。整屏以外的小面积 present 不打，避免刷屏。每次 present 多两次 rdtsc（约 40 周期）。
- QEMU 读数（`7C81A4B2AAA8` / `849F3B507455`）：`full present#1 wait=72734 copy=23215554 rest=692806 wc=1`、`#2 wait=6900 copy=20729664 rest=42668 wc=1`、`#3 copy=16182300`。`wait` 只有几十微秒，`copy` 占绝对多数——QEMU 是 TCG 模拟，这个比例在真机上本来就不该这样，正好用来对照。
- 未做/未验证：真机没有读数。这一行是**给下一次真机启动准备的判据**：如果 `wc=0`，586 ms 就是未缓存映射写的代价（下一步该在首帧之前把 scanout 段切成 WC）；如果 `wc=1` 而 `copy` 依然巨大，问题在别名映射或页表，不在内存类型。

### B-29 一个控制器两次“什么都没回来”之后不再走它的其余端口

- 依据：B-22 已经把“静默控制器”的单端口预算压到 300 ms，但真机上 5 个有设备的端口 × 多轮重试仍然是秒级开销，而它们测的是同一件事——控制器根本不投递 transfer event。端口多不代表证据多。
- 改法（`kernel/usb.c`）：新增 `g_ctrl_silent_hits[]`（每控制器累计“首笔零事件”判定次数）与 `g_ctrl_skip_logged[]`；在原来置 `g_ctrl_silent[ci]` 的地方同步自增。`usb_hub_step()` 在探一个端口之前先看：该控制器命中 ≥2 次就把 `g_hub_port` 推过 `scan_ports`（下一轮循环自然跳到下一个控制器），并只打印一次 `[USB] controller N: two ports never got a completion, skipping the rest`。`usb_init()` 的 memset 块一并清这两个数组。
- 为什么是 2 而不是 1：单个端口坏掉（坏线、坏设备）是真实存在的情况，那一个端口静默不该连累同控制器的其它端口；两个端口都静默，才说明问题在控制器的事件投递。
- QEMU 回归（`849F3B507455`，q35+qemu-xhci+usb 鼠标）：`usb mice=1`、`hub scan done: ccs=1 cn=1`、**没有** skip 行，`first frame +1215 ms`、`usb hub done +2349 ms`，与前一版一致（QEMU 的 xHCI 永远会回事件，走不到这条分支）。
- 未做/未验证：真机没有读数；真机上这一条能把 USB 段从“5 口 × 3 轮 × 预算”压到“2 口 + 一轮”，但压到什么量级要等 `[INP] usb mice` 与 `usb hub done` 的时间差。跳过逻辑不区分“两个端口各一个正常设备但都静默”这种极端情况——那本来也说明控制器没在工作。

## B-30 — 串口"每字符代价"自静音（比 B-25 的总额预算更早生效）

**问题** B-25 用总额预算（1.5e9 cycles）给控制台的成本封顶。在真机 0.76 ms/字符的代价下，
这个预算要 790 个字符才触及（约 0.6 s）——封顶了，但第一次启动仍然要先付 0.6 s。
更糟的是 B-25 之前那条"单次 8 字节采样 + 阈值"的路子在忙机器上误判（本次 QEMU 复现：负载下
八字节采样 35512 cyc/char，与真机的 1900000 差 54 倍，单点阈值分不开）。

**改动** `kernel/serial.c`
- 新增 `g_chars`（真正从 sink 出去的字符数），`serial_chars()` / `serial_avg_cost()` 导出。
- `serial_putc_sync()` 每写一个字符累加 `g_spent`、`g_chars`；当 `g_chars >= 512` 且
  `g_spent / g_chars > 150000`（2.5 GHz 下 60 µs/字符，慢于 17 kbaud）时置 `g_serial_quiet`。
  - 为什么是 512：一次突发（QEMU 里见过单点 128k cycles/字符的 flush）会污染短样本。第一版把
    阈值定在 20000 cycles / 96 字符，QEMU 在 `[CON]` 那一行打到一半就被自己静音了——这台模拟器的
    平均是 13k–35k，一个 burst 就顶过 20k。
  - 为什么是 150000：真机实测 0.76 ms/字符 = 1900000 cycles，是这条线的 12.7 倍；模拟器 13k–35k
    在线下 4–11 倍。中间这条线只有"固件把 COM1 陷进 SMM"才够得着。
- `kernel/serial.h`：新增 `serial_chars()`、`serial_avg_cost()` 原型。
- `kernel/kmain.c`：两处 `[CON] text sinks` 扩成
  `uart <cyc>/access, <cyc>/char, <cyc> spent, <chars> chars avg <cyc> cyc (on|quiet)`，
  这样"控制台为什么哑了"和"它到底付了多少"是同一行里的两个数，不用再猜。

**验证（QEMU 6.2，q35 + pflash + `-cpu max`，2 vCPU）**
- 标准配置（virtio 盘 + qemu-xhci + usb-mouse）：控制台全程存活，`[CON] text sinks late` 报
  `0x36BB chars avg 0x450B cyc (on)`（14011 字符 / 17675 cycles 每字符），`usb mice=1`，
  该配置无日志介质，走 45 s 报告页路径（与本次改动无关，是既有行为）。
- NVMe-GPT 配置（`-device nvme,serial=novlog` + 手工 GPT 盘）：`[LOG] no stick, using the disk's GPT gap
  on nvme0` → `boot log -> nvme0 at raw LBA 0x41`，`[BOOT] diag: logo=mk log=1 ... ok`，
  `report page skipped: the log is on the medium`，桌面窗口内容正常（设置/文件管理器/dock/时钟 16:00）。
  时间线：`first frame +1180 ms`、`usb hub done +2317 ms`、`deferred … vidmem crtc +3313 ms`。
- 第一版（20000/96）的失败也留在记录里：`[CON]` 行打到 `…0x71 cha` 就被截断，此后整段串口为空——
  正是它自己把证据掐了。

**未做 / 未验证**
- 真机一次都没跑过：`[CON] text sinks` 的 `<chars> chars avg <cyc>` 两个新字段、以及第一次启动时
  控制台在第几百个字符处自静音，都要等真机日志回来才算数。
- 阈值是拿"真机 0.76 ms/字符"这一个读数定的；如果真机实际更慢/更快，这条线需要重新标。
- 只覆盖 `serial_putc_sync()` 那条同步路；IRQ 队列排空（`serial_tx_drain()`）不经这里计时，
  所以当 IRQ 正常工作时控制台的"账面成本"会偏低（在真机上 IRQ 是否起来未验证）。

## B-31 — 竖线探针也加时间上限（原本只按次数封顶）

**问题** `graphics/nd_hal.c` 的 `present_probe()` 为了判断 CRT 的竖线回扫位是否可用，会去读
`0x3DA` **最多 20000 次**，且只看次数、不看时间。这台笔记本上 `0x3DA` 是"固件把端口陷进 SMM"
的同一类端口（COM1 就是这样，实测 0.76 ms/次）：20000 次读 = 最多 15 s，正好发生在第一帧之前，
而且因为它没有自己的时间线标记，只会在"第一帧为什么晚了"里消失掉。B-19/B-23/B-25/B-30 修的都是
同一个根因（幽灵 I/O 端口），唯独这个探针还留着纯次数的上限。

**改动** `graphics/nd_hal.c`
- 新增 `ND_PROBE_CYCLES 100000000ULL`（2.5 GHz 下 40 ms，1 GHz 下 100 ms）。
- `present_probe()` 循环增加时间闸门：每 256 次读检查一次 `nd_tsc()`，超时就退出；判定仍是
  `seen == 3` → 可用。时间闸门每 256 次才查一次，自己不会变成成本。
- 新增 `g_retrace_probe_reads` / `g_retrace_probe_us`，并把这台机器的探针代价打到启动日志那一行：
  `[ND] compositor: off-screen buffer, vsync available|unavailable (probe <N> reads, <M> us)`。
  "没有竖线位"在一台每次端口读 1 ms 的机器上和在一台读端口免费的机器上是两个不同的事实，
  现在这一行把它分开。
- `hal_dec()` 的前向声明提到文件上部（原来在 1171 行，本行先用到它）。

**验证（QEMU 6.2，q35 + pflash + `-cpu max`，NVMe-GPT 日志盘）**
- `[ND] compositor: off-screen buffer, vsync unavailable (probe 20000 reads, 2751 us)`：
  模拟器上 20000 次读只花 2.75 ms，走的是次数上限，与改动前判定一致（`unavailable`）——没有行为变化。
- 开关行为不变：`first frame +1199 ms`、`usb hub done +2338 ms`、`usb mice=1`、
  `report page skipped`、`[CON] text sinks late … 0x371B chars avg 0x45AC cyc (on)`。
- 镜像 `6D47CB232F53`。

**未做 / 未验证**
- 真机未跑：真机上这个探针到底花多少（`probe N reads, M us`）没有读数；如果 `0x3DA` 在这台机器上
  是陷端口，这一行会直接显示"只读了几十次就放弃"，那也顺带说明 COM1 的 0.76 ms 不是孤例。
- 40 ms 的上限是按"真 CRTC 一帧 16.7 ms"和"每次读 1 µs"的普通机估算的；一台 30 Hz 的面板需要
  33 ms 才扫完一帧，可能仍然判成不可用（那只是退回"不做竖线对齐"，不是错误）。
- 只改了探针，没有改 `present_wait_blank()` 的 800 次自旋；那条路径本身已被面积阈值和
  `g_retrace != 1` 挡住，在真机上大概率根本不进。

## B-32 — 启动不再造一个"spinner"演示任务

**问题** `kernel/kmain.c` 在启动末段创建了一个只做 `sched_yield()` 循环的演示任务。它在整机只有一个可见核心的 QEMU 配置里与 idle 争抢，把本来就该空闲的调度器时间片花在"自己让给自己"上；真机上它同样占一个 slot。

**修法** 不再创建该任务（`kmain.c`）。

**证据** QEMU 镜像 `99C3D1ECD8548E34` 起，`[SCHED] ticks=` 的 `current=` 绝大多数落在 `idle`（偶发 `dwm`），不再出现 spinner 任务名。

**未做/未验证** 真机启动时长未测（笔记本离线）；该改动只影响 CPU 占用，不属于启动关键路径。

## B-33 — 关窗/标题栏按钮的 release 语义硬化（原归因错误已推翻）

**问题** 原记录写的是"点窗口 X 关闭后，点击会穿到桌面并打开背后的应用"。本轮做了带打点的实验镜像（`nwm.c` 的 `route()` 打印 down/up 坐标、`title_button` 返回码、`g_swallow_up`；`nd_demo.c` 的 `nd_demo_dispatch()` 打印到达 shell 的事件），结论是**原归因不成立**：一次点击只产生 1 个 down（命中 `btn=1`）与 1 个 up，up 被吞，没有 `up->desktop`，shell 也没收到 down/up。真相是任务管理器窗口一直存在、被设置窗口完全遮住（set 300..1004×84..710、taskman 326..966×190..650），关闭设置只是把它**露出来**。

**修法（保留的部分）** 即使是"被标题栏按钮消费的 press 拥有自己的 release"这一条，实现位置也有隐患：原来的 swallow 早退写在 `route()` 的 `switch` 里，而 `ng_overlay_hit()` 分支在 switch 之前就 `nd_demo_dispatch()` 并 `return`，release 会绕过语义检查。把 latch 挪到 `route()` 最前面（`g_mouse_x/y = ev->x/y` 之后、overlay 分支之前）：

```c
if (ev->type == ND_EV_MOUSE_UP && g_swallow_up != 0u) { g_swallow_up = 0u; g_capture = -1; return; }
```

switch 内原分支保留作兜底。

**证据** 实验镜像 `9649DC0AD75261B4`（带打点）证明只有 1 down/1 up 且 up 被吞；清点后重建 `9D5D0923439B4D02` 与后续镜像行为不变。`/tmp/nov/p2b.png`（同 `cal4.png`）是"关闭设置后露出任务管理器"的直接截图。

**未做/未验证** 临时打点已全部删除；`nd_demo.c` 里残留的一处换行错位已修回。截图未随交付上传。

## B-34 — `yield` 只在 idle 上下文真正让出 CPU

**问题** ring3 的 `yield_now()` 走 `SYS_YIELD`，任何任务调用都把时间片交回调度器再立刻被重新选中，等价于空转；桌面多个应用主循环都这么写，整机看起来"CPU 100%"。

**修法** `kernel/sched.c` + `sched.h` 增 `sched_yield_is_idle_only()`：只有 idle 上下文（或无可运行任务）才真正触发重调度；`kernel/trap.c` 的 yield 分支据此分流。

**证据** QEMU 复验后任务管理器的 CPU 列从 100% 降到 0%（镜像 `C01BAF82034D4630` 的桌面截图 `n9a.png`：`CPU:0%`）。

**未做/未验证** 真机未测；`yield` 语义变化对第三方应用的影响未评估（本仓库内没有依赖"yield 一定换人"的代码）。

## B-35 — 窗口管理器每 tick 排空上限 32 条

**问题** `wm/novus-wm/nwm.c` 的 `pump()` 原来每 tick 只取一条消息，客户端一次重绘上百条 shape/text 时队列立刻打满，客户端 EBUSY 风暴。

**修法** `while (n < 32u && ipc_try_recv(...) >= 0)`。

**证据** IPC 丢弃从 `133,580 / 12 s` 降到 `417 / 12 s`，`[NWM] stats req` 从 7699 降到 2137（同 12 s）。

**未做/未验证** 32 这个值取自"一帧典型消息量的 1/4"经验，未做参数扫描。

## B-36 — 加 `SYS_SLEEP` 系统调用

**问题** 应用没有"睡一个 tick"的手段，只能用 `yield_now()` 忙等，无法实现 B-39 的阻塞式主循环。

**修法** `kernel/trap.c` 增 `SYS_SLEEP`（号 38）与 `sys_sleep_call` 及中断表项；`apps/src/sys.rs` 增 `sleep_ticks()`。

**证据** QEMU：`sleep_ticks(1)` 生效后 idle 型应用的 CPU 归零，任务管理器 `CPU:0%`。

**未做/未验证** 睡眠精度受 10 ms tick 限制（产品上可接受，未做高精度睡眠）。

## B-37 — dwm 合成主循环降频

**问题** `apps/src/bin/dwm.rs` 主循环无事件时仍全速轮询（172/194 两处 loop、`compose_frame()==None` 分支、主循环尾部）。

**修法** 无事件时阻塞到下一 tick。

**证据** 同 B-39 的 `CPU:0%` 读数。

**未做/未验证** 真机上光追/v2 surface 场景的帧率未测；若真机掉帧，需要把该处改回"有 surface 时不睡"。

## B-38 — IPC 丢弃与坏消息可诊断（并把 `bad=2` 定性）

**问题** 队列打满时每秒上万条拒绝日志把串口淹了，而"坏校验和消息"只加计数不留证据，"谁在灌队列、坏的是谁的什么消息"都查不到。

**修法**
- `ipc_port_t` 增 `UINT32 dropped`；`ipc_send()` 队满处 `p->dropped++`，只在**该端口第一次被拒**且 `g_stats.dropped < 16` 时打一行（target/sender/type）。
- 增 `ipc_dump_ports()`（idx/id/owner/queued/refused/name）与 `ipc.h` 声明；`kernel/sched.c` 的 `sched_tick()` 在 `g_ticks==300||1200` 打端口表与 `[IPC] stats`。
- `ipc_try_recv()` 在校验和失败处打印 target/sender/type/len/magic（前 16 条）。

**证据与定性** 端口表在两个采样点都只有 6 个活端口（`0x0FFF`、`nwm 0x1000`、`dwm 0x1001`、`sys 0x1002` 及各任务 inbox 1/4/5/6）；`selftest.a/b` 在自测结束后已被释放，**不存在编号错位**。队满日志里 `target=0x1003 sender=0 type=2` 是 `ipc_selftest` 故意灌队列验证 EBUSY/dropped 的测试流量，不是泄漏。`bad=2` 同样来自自测：一条是 `ipc_send(..., IPC_PAYLOAD_MAX+1)` 的 `IPC_EMSGSIZE`（`ipc_send` 里也 `g_stats.bad++`），一条是自测主动把 `payload[0] ^= 0xFF` 造出的校验和错误。实测打印为 `[IPC] bad target=0x1004 sender=0 type=3 len=4 magic=0x4E495043`——端口 b、内核发起、magic 正常、长度 4，正是那段自测。

**未做/未验证** 诊断代码保留（16 行上限不会污染串口）；真机上是否还有自测之外的坏消息未知。

## B-39 — 应用主循环全速空转（一整类，最贵的一处）

**问题** 多个应用主循环尾部是 `sys::yield_now()`，而 `idle += 1; if idle >= 40 {...}` 这类"每 N 次循环"的节流在微秒级循环里等于"每秒上万次"。最贵的是任务管理器：每 40 次循环重采任务表并整窗重画（每帧上百条 IPC），把 `nwm` 队列打成持续溢出（实测 13.5k 次/秒拒绝）。

**修法** 主循环无事件时阻塞一个 tick（10 ms），使 `idle>=40` 真正等于 0.4 s：
`apps/src/bin/taskman.rs`、`session.rs`、`settings.rs`、`fileman.rs`、`dwm.rs`、`hello.rs`、`netman.rs`、`nsh.rs`（控制台读空与无 WM 事件两处）、`setup.rs`（仅在 `!app.started` 时睡，安装中不降速）、`devmgr/mod.rs`、`shell/cmds.rs`（同时让 1200 tick 的 `novusd` 超时第一次真的成立）、`nuomi.rs`（`sleep(ms)` 原实现是 `ms/5` 次 yield，等于不睡；改成 `ms/10` 个 tick，上限 200）。

**证据** 镜像 `CCEBC795C2FE12C5`（12 s 采样）：`dropped=0x1A2=418`、`sent=0x9CD=2509`，桌面状态栏 `2514 发出 / 418 丢弃`，5 个任务 3 个窗口正常，设置/文件管理器/任务管理器均可用。

**未做/未验证** `setup` 安装过程的实际耗时未在真机测（仅保证不因睡眠降速）；`nuomi` 的 `sleep` 精度从"瞬时"变成"10 ms 粒度"，脚本行为变化未逐例回归。

## B-40 — 客户端 EBUSY 重试无退避

**问题** `apps/src/wm.rs` 的 `send()` 在 `IPC_EBUSY` 上 `yield_now()` 死转：客户端自己把 WM 队列占满，又成为唯一能重试的线程，结果以 ring3 速度反复撞墙（实测 13k 次/秒）。

**修法** 前 8 次仍 `yield_now()`（队列只差一条时的快速路径），第 9 次起 `sleep_ticks(1)`，把切片让给 WM 一次排空。

**证据** 与 B-39 同一次采样：丢弃 133,580 → 417（12 s）。

**未做/未验证** 8 这个阈值是经验值；未测极端场景（一帧上千条消息）下的首帧延迟。

## B-41 — 更新检查卡在"检查中"不收敛，且失败后按钮失效

**问题** `security/novu/novu_service.c` 的 `request_check()` 是唯一写 CHECKING 的地方，而只有传输层能把它带出去；没有 DHCP 租约时探针根本不启动，面板就永远停在"检查中"。同时 `request_check()` 只在 `IDLE` 生效，一旦跌到 `ERROR`，按钮再也按不动（除重启）。

**修法** `novu_service.h` 增 `NOVU_CHECK_TIMEOUT 1200u`（10 ms tick ⇒ 12 s）；`novu_service.c` 增 `g_check_armed`/`g_check_deadline`，`tick()` 的 CHECKING 分支首个采样 tick 打点、超时置 `g_svc.retry = NOVU_MAX_RETRY; state = NOVU_SVC_ERROR` 并打 `[NOVU] check timed out, no answer from the update transport`（选 ERROR 而不是 IDLE，因为"0 个可用更新"会骗用户说已是最新）；`request_check()` 允许 `IDLE || ERROR` 重入；`begin()/set_state()/fail()` 均清 `g_check_armed`。

**证据** 镜像 `99C3D1ECD8548E34`、tag `v1`：点"检查更新"→ 状态"检查中"（`v1b.png`）→ 12 s 后"错误"、日志出现 timeout 行；再点一次回到"检查中"，日志出现第 2 条 `[NOVU] check requested`（`v2b.png`：状态错误、`0 个可用更新`、按钮仍在、footer"已请求检查更新"）。

**未做/未验证** 有网络时的正常成功路径未在本轮跑（QEMU `-nic none`）；12 s 超时是否足够真机慢速链路未测。

## B-42 — 登录窗口 Enter 不能提交（键盘唯一路径被堵）

**问题** `apps/src/bin/session.rs` 里 `Enter`/`Tab` 都只做 `focus = 1 - focus`：在密码框按 Enter 会把焦点退回用户名框，**永远无法用键盘登录**。这台真机当前既没有可用鼠标也没有可用键盘（USB/PS-2 都不通），但一旦输入设备恢复、或用户处于只有键盘的场合，这条路径就是唯一可用的；而且登录代码被复制粘贴在右键菜单和按钮两处，正是它长出"Enter 没有提交分支"的原因。

**修法** 抽出 `fn attempt(op: u8, ...) -> (i64, &'static str)` 统一执行 `sys::session()` 并把返回码映射成提示行（-2/-3/-4/其它）；三处调用（右键菜单、按钮、Enter）改为调它。`Enter` 在密码框且未登录时等同按"登录"，在用户名框仍是切到密码框；`Tab` 保持循环切换。

**证据** `cargo build` 通过（镜像 `87AF9F4F158DFB23` 起每一版都含本项）。

**未做/未验证** **没有在运行中验证**，原因已查清：live 介质上 `session_init()` 走的是"`g_live=1`、`g_logged_in=1`、直接 `session_start_desktop()`"这条分支（日志 `[SESSION] live session: no account required`），**登录应用根本不会被启动**——日志里没有 `[APP] session manager up`，桌面上也不存在"用户会话"窗口。用一个临时镜像（在 `session.rs` 的 `win.raise()` 处打点，`7369834405C1B0C5`）确认了这一点：即使应用自己要求置顶，那个窗口也不会出现，因为它没跑。所以 Enter 提交这条路径只在**装到持久 NovusFS 根上**的系统里存在，那需要先用 `setup` 装一次盘，QEMU live 镜像做不到。临时打点已回滚（镜像 `2D01A095B6956742`）。等有安装好的根或真机时按"用户名→Tab→密码→Enter"复验。

## B-43 — `#[repr(C)]` 重复声明（构建警告清零）

**问题** `apps/src/wm.rs` 的 `struct Title` 上写了两次 `#[repr(C)]`，`cargo` 每次构建都报 `repeated_reprs`。

**修法** 删掉重复的一行。

**证据** 构建日志不再出现该警告（镜像 `87AF9F4F158DFB23`）。

**未做/未验证** 布局未变（重复的属性与单次等价），无行为影响。

## B-44 — USB：一个哑端口不再终止整个控制器的遍历

**问题** `kernel/usb.c` 的 B-29 规则写的是"**两个端口**都没拿到完成事件就不再走这台控制器的其余端口"，但计数实现是**按失败次数**自增（`ep0_xfer` 里每次"什么都没有回来"都 `g_ctrl_silent_hits[ci]++`）。真机上端口 1 的 EP0 会因为 4 个 slot 依次超时而贡献多次命中，于是**同一个端口两次失败就把整台控制器判死**，其余端口（比如接收器所在的那个）根本没被走到。这与报告页上 `ccs=5 cn=5 msd=0 mi=0`（5 个口有设备、5 个口 link 起来了、一个也没枚举出来）的形状完全吻合。

**修法** 新增 `g_ctrl_silent_mask[]`，按"端口位图"去重：只有 **root port**（`u->route == 0`）的静默才计数，且同一个端口只计一次；`g_ctrl_silent_hits[]` 的含义变成"已经静默的**不同**端口数"，于是原有的 `>= 2u` 判据重新变回它注释里写的语义。`usb_init()` 里一并清零该位图；跳过时的日志文本不变。

**证据** 镜像 `181A5411E28E2B09`，QEMU 用 8 口 xHCI（`p2=6,p3=2`）在端口 1 挂 usb-mouse、端口 6 挂 usb-kbd：`[USB] probe port 3 ... ok`、`[USB] probe port 8 ... ok`、`[USB] hub scan done: ccs=2 cn=2`、`[INP] usb mice=1`——高位端口照样被走到并完成整条描述符链。

**未做/未验证** QEMU 造不出"连接了但 EP0 永不应答"的设备，所以**没有复现真机那条路径**：本条是按真机日志（端口 1 四 slot 全超时、`code=0x1A residual=8`）与代码语义不一致推出来的修复。真机复验必须等笔记本上线刷 `181A5411E28E2B09` 后看 `[USB] probe port …` 是否走满 5 个 `ccs=1` 的端口、`mi=` 是否变 1。

## B-45 — 键盘只有"接口使能"，没有"设备开始扫描"

**问题** `graphics/nd_input.c` 的 PS/2 初始化把 8042 侧该做的都做了（翻译开、两个时钟线放开、`0xAE` 使能键盘接口、注册 IRQ1/IRQ12），但**从来没有给键盘本身发 `0xF4`（Enable Scanning）**。固件把设备留在 `0xF5`（扫描关）时，接口是"使能且永远安静"的：每条 8042 命令都 ACK，`kbd_bytes=0 pkts=0`，看起来和"这台机器没有键盘"一模一样——正是这块 HP 报告的形状。Linux 的 `atkbd` 在自检之后同样要发 `0xF4`，就是这个道理。

**修法** `ps2_cmd(PS2_CMD_ENABLE_K)` 之后追加一次 `ps2_write(PS2_DEV_ENABLE)` 并读回 ACK：ACK → `g_ps2_kbd_enable=1`，超时 → `0`，其它 → `2`，三种结果都写进日志（`[PS2] keyboard enable (0xF4): ACK 0xFA` / `no reply` / `unexpected`）。**报告页的输入横幅**（`in ps2 aux=… en=…` 那一行）新增 `ke=` 字段，用来区分"设备没应答使能"和"设备应答了但一个字节都没来"；串口的 `[INP] ps2` 行保持原样。

**证据** 镜像 `2D3388F214C1AF27`，QEMU：`[PS2] keyboard enable (0xF4): ACK 0xFA` 出现在 `[PS2] enable reporting (0xF4): ACK 0xFA` 之后，随后 `sendkey a/b/c` 在日志里落成 `[NDQ] pop#1 type=4 key=97`、`[NWM] key -> win=5 pid=6`——键盘从 8042 到窗口整条路径可用，改动没有把已有的键路径弄坏。

**未做/未验证** 真机上"键盘原本是不是被固件留在 `0xF5`"没有证据，这条是按"接口使能 ≠ 设备扫描"的协议事实补的；真机复验要看 `[PS2] keyboard enable (0xF4)` 是 ACK 还是 `no reply`，以及 `ke=`/`kb=` 是否从 0 开始增长。

## 本轮"查过但不是缺陷"的几项（避免后续重复挖）

- **`target=0x1003 type=2` 队满日志**：`ipc_selftest` 故意的溢出测试，sender=0 由内核发起，端口在自测后被释放。见 B-38。
- **`[IPC] bad=2`**：一条 EMSGSIZE（自测发 `IPC_PAYLOAD_MAX+1`）、一条自测主动破坏 payload 造的校验和错误。见 B-38。
- **"关窗后点穿到桌面"**：不是点穿，是下层窗口被露出。见 B-33。
- **开机动画固定 710 ms（`splash anim done` +816 ms）**：不是"白白等 710 ms"。`boot_anim_play()` 每帧只用 100 ms 预算跑 `stage3_load_pump()`，日志显示 `[T] +816ms stage3 load done` 与动画结束**同一时刻**——装载本身就要吃掉这段窗口。单纯缩短动画只会把装载推到动画之后，总时不变；要缩短得同时压缩 stage3 的读盘总量。本轮**未动**（属产品/观感取舍）。
- **设置页导航高亮"切到最后一个字"**：字形贴边，非裁剪缺陷。
- **应用页"设置 6 等待"**：任务是阻塞在 IPC recv 的正常状态字符串，不是卡死。
- **串口成对重复行**：双 sink 正常输出，不是二次引导。
- **`#PF` 风暴（CR2=0x90000000、CPL=3）**：与 idle 跳板设计相符。
- **QEMU 里 `mouse_move` 的负向位移被扭曲**（例如 `-26` 让指针右移 266 px）：核心里 `hid_boot_mouse_decode()` 是 `(INT32)(INT8)` 正确符号扩展，`nd_commit_move()` 也没有加速度，属 QEMU 监视器对相对鼠标的换算问题，**不是驱动缺陷**。

## 本轮镜像演进（每次构建 exit=0）

`2D4281DCC3B5B13A` → `B80CFE2B6FEEC191`（+端口表 dump）→ `6AF5DEA7BD081F83`（+IPC 采样）→ `081D68561834FA82`（+每端口 refused）→ `C01BAF82034D4630`（+B-39/B-40）→ `9649DC0AD75261B4`（鼠标/壳层临时打点）→ `594A77A85D329C9A`（+switch 前 swallow）→ `9D5D0923439B4D02`（清打点后干净重建）→ `99C3D1ECD8548E34`（+B-41）→ `1F2A8F683AC5C139`（+坏消息诊断）→ `CCEBC795C2FE12C5`（+B-39 全量扫尾）→ `035027975A9563B7`（+B-42）→ `87AF9F4F158DFB23`（+B-43）→ `181A5411E28E2B09`（+B-44）→ `2D3388F214C1AF27`（+B-45）→ `7369834405C1B0C5`（B-42 临时置顶试验，已回滚）→ **`2D01A095B6956742`（当前最新：B-32…B-45 全部在内，临时打点已清）**。

镜像不是逐比特可复现的（同一份源码重建哈希会变，`2D3388F214C1AF27` → `2D01A095B6956742` 即为此例），所以哈希只用于"这一次构建的唯一标识"，不能当"回到某个源码状态"的凭据。

## 上游同步：`3814a0d` → `a7c7898`（11 个提交，本地无独有提交）

**为什么先卡在网络** `github.com:443` 在这台机器上被 hosts 里成片的 wg-split 条目钉到 `127.0.0.1`（当时没有任何代理进程在跑，7890/7897/1080/10809/8080 全关），但裸 IP 可达（`140.82.112.4:443` True）。解法是让 git 自己带 Host 头、跳过证书校验、URL 直接写裸 IP：`git -c http.sslVerify=false -c "http.extraHeader=Host: github.com" fetch https://140.82.112.4/chen-xin-Liam/novus-os.git main`。仓库里原本只有 `origin`（`zcy20130330/novus-os`，partial clone `blob:none`），没有 `upstream` 这个 remote，但 `refs/remotes/upstream/main` 存在；用同一个手法逐个试候选路径，**`chen-xin-Liam/novus-os` 返回 200**，其余（`Liam/`、`liam/`、`Novus-OS/`）全 404，上游就是它。

**进来的 11 个提交** `a7c7898 优化开机加载动画`、`68bf136 简单的3D加速`、`de1fef7 优化双缓存帧`、`4b57fd1 添加英文友好`、`1d33785 添加条款`、`253cfe4 优化readme文件`、`1c1704b 细化文档`、`8a154d8 Merge PR #19`（boot-anim）、`35a0b35 自动化测试`、`244134e Merge PR #18`、`7bb747c 可以运行PE32头的exe文件`。

**本地改动怎么合** 本地 96 个文件改动（B-32…B-45）与上游 75 个改动文件重叠 13 个：`Makefile`、`apps/src/bin/dwm.rs`、`apps/src/shell/cmds.rs`、`apps/src/sys.rs`、`graphics/nd_demo.c`、`kernel/apps.c`、`kernel/apps.h`、`kernel/kmain.c`、`kernel/sched.c`、`kernel/sched.h`、`kernel/trap.c`、`tools/tools.mk`、`wm/novus-wm/nwm.c`。做法是先备份（`out/wip-b32-b45.patch` 758,550 B + 未跟踪清单 `out/wip-untracked.txt` 23 项），`git stash push -u` 后 `merge --ff-only upstream/main`，再 `stash pop`；**只有两个文件冲突**：`apps/src/bin/dwm.rs`（三处，双缓冲失败分支/B-37 的 `sleep_ticks(1)` 替换 `yield_now()`，全部按上游新结构合入）和 `kernel/trap.c`（我们的 `SYS_SLEEP 38u` 与上游的 `SYS_PE32_EXEC 41u` 并存，define 与 syscall 表项两条都留）。

**工具链补齐** 上游 `7bb747c` 让 `disk-img`/`iso` 依赖 `out/win32/wowdemo32.exe`（`WOW32_CC := i686-w64-mingw32-gcc`），本机没有 → `pacman -S mingw-w64-i686-gcc`。**PATH 顺序坑**：把 `mingw32\bin` 放最前会让 `gcc`/`objcopy` 变成 32 位，出现 `cc1.exe: error: code model 'small' not supported in the 32 bit mode` 和 `objcopy: file format not recognized`；正确顺序是 `mingw64\bin` 在前、`mingw32\bin` 放最后（已固化在 `C:\msys64\tmp\build_sync.cmd`）。

**未做** 只做了快进与工作区合并，**没有 push、没有新建/更新 PR**（本轮要求是"同步上游"，不是"推回去"）。改动仍全部未提交；`stash@{0}`（"wip b32-b45 before upstream sync"）与 patch 备份保留作回滚点。

## B-46 — `make iso` 一直装不下：ESP 剩余空间归零

**问题** 同步后第一次 `make iso` 直接失败在 `mkfat: out of clusters: image too small`。live ESP 里要装 `NOVUS.EFI` 4,027,076 B、`SYSTEM.IMG` 8 MiB、`ESP.IMG` 16 MiB、`WPAPERS.PKG` 3,812,149 B、`TEST.NKM`、`DEMO.NUC`（8,207 B，落在 cluster 64572），合计约 31.84 MiB，而 `ESP_SIZE_MB ?= 31.5`。

**上限是死的** 再往上不是"加个数字"就完：El Torito 引导目录里的**扇区计数字段只有 16 位**，最多 65535 个 512 B 扇区（32 MiB 差 512 B），正好 32 MiB 会回绕成 0，ISO 直接不可引导。所以 `tools/tools.mk` 里写成 `ESP_SIZE_MB ?= 31.99`（65,516 扇区），并在注释里写明：**只剩约 0.15 MiB 余量，下一次内容增长必须走结构性方案**（例如把 `WPAPERS.PKG` 从 live ESP 里拿掉、改成运行时从 SYSTEM.IMG 里取），不能再往上加数字。

**未做/未验证** 没有做"ESP 瘦身"的结构性改造，只把余量校准到极限；这条是悬着的技术债。

## B-47 — live ISO 在 `beacon()` 的第一笔写就 #GP，崩在进内核之前

**现象** 修完 B-46 的新 ISO 无论 `-cdrom` 引导还是当 `isohybrid` 混合盘引导，都在 `[T] +3844ms splash anim done` 之后立刻 `X64 Exception Type - 0D(#GP)`，`RIP 0x7D57495D`（ImageBase 0x7D545000，偏移 0x2F95D）、`R11=0x6966657500336966`（内存里像 "fi3\0uefi"）。同一个 NOVUS.EFI 从磁盘镜像引导完全正常。

**定位过程** `addr2line` 对 PE 不走；`objdump -h` 给出 `.text VMA 0x02b6011000`（preferred ImageBase 0x2b6000000），把偏移反查落在 `pe_describe+0x1d0`，但 `pe_describe` 全仓只有 `pe_load` 的拒绝分支调用，boot 阶段根本不到那里——符号归因是假的。决定性实验是在 `main.c` 里给 `stage3_load()` / EBS / `stage3_finish()` / 取内存映射 前后各插一条 `boot_timeline("TMP ...")`：**一条 TMP 都没打出来**，说明崩在 `beacon("s3 load start")` 内部（它就是 `splash anim done` 之后的第一条语句）。

**根因** `main.c` 的 `beacon()` 只判 `d->write != 0 && d->sector_count > BEACON_LBA`，就往启动设备 LBA 51500 写一个扇区。live ISO 的启动设备是**只读光盘**，固件照样发布 `WriteBlocks`（调用即失败）；这条写把整台机器的引导打崩。**引导日志不能因为写日志本身而死**，这是设计级错误。

**修法** 选择 beacon 设备时要求介质真的能写：新增 `kernel/blkdev.c` 的 `blkdev_medium_writable()`，UEFI 设备直接问固件的 `Media->ReadOnly`（不是我们自己的 flag 位），分区视图递归问父设备，其余后端才退回 flag 判断；`beacon()` 里换成这个判断，跳过时打一行 `boot beacon: no writable boot medium, no sector log`。

**证据** 同一 ISO 再引导 → `[T] +3841ms boot beacon: no writable boot medium, no sector log` 之后正常 `stage3 load done` → `Exiting boot services` → `kmain start` → `[NWM] first frame`，桌面窗口内容全部绘制（设置页、文件管理器列表、任务栏时钟）；桌面镜像同样一行提示后正常进桌面。

## B-49 — "换一块能写的盘去写 beacon"这个念头，会把已经能引导的镜像打崩

**为什么单独记** B-47 之后我试着让 beacon 在"启动卷是分区、写不进 51500"时爬到父盘或另找一块可写整盘去写，理由是"真机 U 盘的 ESP 分区也写不进自己的 51500"。这条路**错了两次**：

- 第一次按 `Media->Partition` 爬到"父盘"，QEMU 桌面镜像里爬不上去（固件给的 ESP 句柄不带父链，`blkpart_ctx` 里也没有它），依旧跳过——只是白跑一轮。
- 第二次放开成"扫一遍所有整盘，挑一块可写且够长的"，桌面镜像**立刻复现 B-47 形状的 #GP**：`RIP 0x7D55EA5D`、`RDX=0x000000000000C92C`（= 51500）。被挑中的设备在表里报的扇区数覆盖 51500，实际后端只有自己的分区窗口，`WriteBlocks` 一调就崩。

**结论（已落地成代码注释）** beacon 是"诊断用面包屑"，绝对不能为了写它去猜设备。最终规则：**只有当固件说"你就是从这块介质本身启动的"、它可写、且长度覆盖 LBA 51500 时才写**；常见分区化 ESP、光盘一律跳过并打一行日志，**内核自己的 `usblog` 才是真机读数的权威来源**。附带确认：`usblog` 区域是 LBA 51200（头）+ 51201…51455（正文 255 扇区），beacon 的 51500 不与它重叠。

## B-48 — `hex32()` 把 A–F 打成标点

`apps/src/bin/dwm.rs` 的 `hex32()` 原本是 `out[i] = b'0' + (x & 0xF)`，≥10 时落进 `':' ';' '<' '=' '>' '?'`，日志里出现 `[DWM] primary w=400 h=2=0`（768 = 0x300 的 `3` 被打成 `=`）。改成两位分支（`b'0'+d` / `b'a'+(d-10)`）后日志变成 `[DWM] primary w=400 h=2d0`。同一函数在 `[DWM]` 的其它字段也用，修一处即全好。

## B-50 — 设置页"显示"写死 `1280 × 800`，与实际模式不符

**发现方式** 用 1024 × 768 的 live ISO 引导，设置页"设备"卡片写 `显示 1280 × 800 · 32 位色`，而下方的"显示输出"行是 `1 · 1024 × 768 · 主屏`，同一页里自相矛盾。

**修法** `apps/src/bin/settings.rs` 不再写字符串，改成从内核 `REQ_DISPLAY` 的回复里取主屏（`primary()` 为真的那一路，没有主屏标记时退回第 0 路，一路都没有则显示"未检测到"）的 `width × height` 生成这一行。

**证据** 同一 ISO 再引导：`显示 1024 × 768 · 32 位色`，与"显示输出 1 · 1024 × 768 · 主屏"一致；桌面镜像（原本也是 1024 × 768）同样一致。

**未做/未验证** 只在 QEMU 的两种模式（1024 × 768、1280 × 800）下看过；"32 位色"仍是固定文案（当前内核只出一种 framebuffer 格式），多显示器/镜像场景下这行显示主屏而非"镜像"的写法未细究。

## 本轮（同步后）镜像演进

`08E3575C8B4A9EC3`（同步后首建，iso `A5502C6AC6ED3F6D`）→ `66518D1703905BF0`（+B-46/B-47 首版，iso `E6AB71B4C062108A`）→ `2E791F0FFEA3A122`（+B-48，iso `D6226E832A718DF6`，两者均 QEMU 验证进桌面）→ `95DD0D6C943AAE63`（beacon 改问固件 `Media->ReadOnly`，iso `65B5F610332E9344`）→ `A3AE7EF0BB639902`（beacon 尝试爬父盘，iso `93A8A7B173A3A1CC`）→ `D1D18A33C32004B7`（beacon 改扫整盘，**桌面镜像 #GP**，iso `C323EE76D6D8E1A6`）→ `FE696C1E3F8E2E85`（改严格规则，iso `E0ACFAC9EF227B10`，两者均验证）→ **`C7A25DB3CC434707`（当前最新：同步 11 提交 + B-32…B-50，iso `4314F2D71DCCA11D`）**。

两件本轮"查过但不是缺陷"：**旧 `novus-live.iso`（22:31 构建）窗口内容纯白**是那份旧构建特有（当前源码在 1024×768 下内容正常绘制，无法复现）；**QEMU `-cdrom` 引导时 live 菜单默认等约 3.1 s**（磁盘镜像 92 ms）是菜单超时设计，不是卡顿。

## B-51 — 代码可读性整理（第一批）：格式化与"槽位查找"去重

**怎么找的** 全仓按函数体做了一次哈希扫描（去掉空白后比较函数体，只报 ≥120 字符的同体重复），结果只有三组：`usb.c` 的 `bn_dec` 在自己的 `#if` 分支里定义了两份、`nd_input.c` 的 `ban_num` 与 `nwm.c` 的 `cap_num` 同体、`win32.c` 的 `api_draw_menu_bar` 与 `api_update_window` 同体。数量不多，但顺着它们又找到了几处更明显的结构性重复——十进制打印在四个文件里各写一遍、boot 阶段每条日志行都手搓一遍"ASCII→CHAR16"和"数字→CHAR16"、`blkdev.c` 里四处手写"指针落在哪个静态池"的区间比较。

**这一批改了六处，全部保持行为不变**

1. **十进制打印：四份 → 一份。** `kernel/kmain.c` 早就有 `kputs_dec()`，但 `kernel.h` 里没声明，于是 `usb.c` 和 `uhci.c` 各自写了一份 `put_dec()`（连同注释都是复制的）。现在在 `kernel.h` 补上声明，`usb.c`/`uhci.c` 的本地副本删掉，12 处调用改成 `kputs_dec()`。`fs/novusfs/novusfs.c` 的那份**不能直接删**：它同时被宿主工具 `mkfsnovus` 编译，宿主链接里没有 kmain.c，删了就 `undefined reference`（本轮已实际踩到这个链接错误），所以改名 `nvfs_put_dec()` 并加注释说明为什么它必须存在。
2. **boot 阶段的文本拼装。** `arch/x86_64/boot/main.c` 新增 `boot_buf_dec()` / `boot_buf_ascii()`，`boot_timeline()` 里"数字转字符的 do-while + 反转"和两处"ASCII 逐字节转 UTF-16"全部换成这两个函数，日志行格式逐字不变（`[T] +817ms ...` 仍是同样一行）。
3. **`blkdev.c` 的槽位查找。** 新增 `uefi_slot()` / `blkpart_slot()`（"这个指针在不在固定池里，在的话是第几个"），替换掉 4 处手写区间比较：`blkdev_medium_writable()`（原来两个 for 循环各扫一遍池）、`blkdev_partition_parent()`、设备移除里的分区池清理。`uefi_devs[]` 的声明因此上移到文件前部，原位留一行注释指向它。
4. **`settings.rs` 的主屏选择。** "取主屏（无主屏标记时退回第 0 路，无显示返回 None）"从 `page_system()` 里抽成 `primary_output()`；B-50 的显示行和抽屉内的"显示输出"卡片后续可以共用同一判断。
5. **`win32.c` 的两个"只要重画"的 API。** `UpdateWindow()` 与 `DrawMenuBar()` 在本项目里语义相同（合成器持有像素，调用方只能要求重画），原本是两份同体实现；抽出 `api_touch_window()` 并在注释里写明为什么两者可以共用一个实现。
6. **`blkdev_partition_parent()` 简化** 成一次槽位查找，去掉多余的空判断与循环。

**验证** 构建 `exit=0`，除既有的 `net_http.h` 注释告警与 jobserver 告警外无新告警。QEMU（镜像 `5BC15CB7C7A60D71` / iso `8FC94131A72C3592`）：USB 控制器的日志行仍是 `[USB] xhci 0:3.0 ports=8`（十进制输出换了实现但字节相同）、`[INP] usb mice=1 reports=0`、桌面与设置页显示 `1024 × 768` 均与整理前一致。

**未做/未验证** `win32.c` 那一处只有编译层证据——QEMU 这一轮没有跑 PE32 程序，`UpdateWindow()`/`DrawMenuBar()` 的实际调用路径要等 wowdemo32 或真机程序验证。`usb.c` 里 `bn_dec` 的两份分属 `CONFIG_NOVUS_PCI` 的两条构建分支，合并要先拆 `#if` 的边界，风险高于收益，本轮**没动**；`nd_input.c` 与 `nwm.c` 的 `ban_num`/`cap_num` 分属两个模块、各有自己的缓冲区约定，也只记录、未合并。

**过程事故（记在这里，免得下次重犯）** 整理 `usb.c` 时我用 `Set-Content` 整文件重写来做批量改名，把工作区行尾从 CRLF 变成 LF，`git diff` 立刻膨胀成 3667 行；随后为了"确认本来就没改这么多"执行了 `git checkout -- kernel/usb.c`，**把该文件自 B-32 以来的约 3400 行未提交改动全部冲掉**（HEAD 里那份只有 2270 行）。恢复路径是同步前留下的备份：`git apply --include=kernel/usb.c --3way out/wip-b32-b45.patch`，干净应用，行数回到 5738，B-44 的 `g_ctrl_silent_mask` 等标记都在。教训：**批量替换只对单文件做最小 Edit，绝不整文件重写；`git checkout --` 在任何有未提交改动的仓库里都是危险动作**。

## B-52 — “进入内核”改成不确定态转圈（纯几何，无位图/字形）

**起因** chen 提出的启动动画改造：进入内核的这段加载要带“转圈”，且图形必须是纯程序化画出来的（“纯二进制图形”）。原来的 7 帧动画把进度条画到 10% 就停在“进入内核”，剩下的装载时间（冷介质上可能是热介质的十倍）既没有百分比也没有任何动感，看起来就是卡住了。

**改了什么**
1. **`graphics/nd_boot.c` 新增 `nd_boot_spin(UINT32 tick)`。** 复用同一条进度轨，重画轨道底色（`SPLASH_BAR_TRACK`）→ 已完成的百分比用暗一级的 `SPLASH_BAR_TRACK_FILL`（0x2E4570）垫在下面 → 一个短彗星段沿轨道跑（头 `SPLASH_BAR_FILL`，尾 6px `SPLASH_BAR_SPIN_TAIL`）。全部是 `nd_round_rect` 几笔几何，没有位图、没有字形，也没有新分配；`tick` 只要递增即可，帧率由调用方掌握，40 格一圈约 0.6 s。
2. **`graphics/nd.h` 声明该函数**，注释写明“纯几何、caller 拥有帧率、只在整条轨上工作”。
3. **`arch/x86_64/boot/main.c` 的 `boot_anim_play()` 末尾接线。** 第 7 帧（“进入内核”）之后进入循环：一帧转圈 → 给 `stage3_load_pump(8u)` 一个 8 ms 切片 → 补足到 16 ms 帧长；装载报完成即退出。最低 4 帧（约 64 ms，<2% 启动时间）保证热介质上也真的动起来；上限 240 帧只是安全阀，触顶后不再画、直接把装载跑完，死介质不会卡在动画上。
4. 打完一行 `[BA] handoff spin ticks=N`，因为截图证明不了“动”——QEMU 在直接写扫描帧缓冲时会返回过期画面，只有 ticks 计数能说明这一轮真的跑了多少帧。

**遇到并修掉的坑（第一版是错的）** 第一版循环是 `stage3_load_pump(20u)` 打头、失败再补 stall，结果在限速介质上一次 pump 单元（一次 flash 擦除、一次稀疏 FAT 查找）就能吞掉十几帧，彗星反而在最需要它的慢介质上冻住（实测限速 25 IOPS 的磁盘：1.6 s 只走了 5 帧）。改成“先画、给 I/O 8 ms、再补齐 16 ms 帧长”后，动画节奏与 I/O 解耦；单次超预算仍会掉帧（得到正确百分比而不是错误进度），但不会再连片冻结。

**验证** 构建 `exit=0`，无新增告警。镜像 `A1AA19A077DD233C` / iso `BC0AF37619C46118`：
- **ISO 启动到桌面**（`-cdrom`），`[BA] handoff spin ticks=4`、`[T] +3912ms kernel handoff spin done`（比动画结束的 +3848 ms 多 64 ms，即 4 帧×16 ms 的预算）；`usb mice=1`、`[ND] demo desktop painted`。
- **桌面镜像启动到桌面**（GPT 日志盘），同一份 ticks=4，设置页/文件管理器正常、时钟走字、`1024 × 768`。
- **转圈帧的像素证据**：把镜像放在 `throttling.iops-total=25` 的 virtio 盘上跑，装载拖到 3 s，截到彗星帧——轨道 351–671，其中暗蓝完成段铺到 576，正前方 351–425 是亮蓝彗星头（另一轮截面在 369–443，位置不同即说明它确实在动）。

**未做/未验证**
- 只验证了 QEMU；真机（尤其慢 U 盘、SATA 盘）的观感与 ticks 计数还没有读数，笔记本当前离线。
- 没有给“进入内核”之后的**内核侧**阶段（`nd_boot_stage(92…99)`，即探测磁盘/USB/显示、挂载文件系统那几秒）加转圈：那段才是真机上最长且百分比不动的地方，但内核侧此刻还没有稳定的周期性重绘钩子（调度器要到 97% 才起来），要做得先引入一个启动期 tick 源，本轮没动。
- 没改动画的 7 帧时间表（仍是 720 ms），也没动 logo 缩放/淡入曲线。
