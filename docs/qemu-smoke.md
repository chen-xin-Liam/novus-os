# QEMU 冒烟测试（真机之外）

`scripts/qemu-smoke.sh` 用一台普通 Linux 主机上的 QEMU 启动已经发布出来的
Novus 磁盘镜像，只通过串口日志和 QEMU monitor（HMP）验证桌面、PS/2 输入和
USB 枚举。它不编译、不改源码、不依赖 VNC、`vncdotool` 或 `socat`，可以在同一台
机器上反复执行。

## 适用范围

- 验证对象是构建产物 `out/novus-disk.img` 上传后的镜像，不是当前工作树里的
  半成品；脚本不会调用 `make`。
- 场景一：UEFI 启动到桌面，串口 marker 和 PS/2 初始化全部通过。
- 场景二：用 HMP 的相对鼠标事件和 `sendkey` 驱动 guest，逐像素比对截图。
- 场景三：挂 xHCI 控制器和 USB 键鼠，检查控制器行和枚举成功行。
- 只新增本脚本和本文档；仓库里的源码、构建脚本和其他文档均不修改。

## 前置条件

- Linux x86_64，`bash`，`python3` 以及 Pillow：
  `python3 -m pip install pillow`。
- 可执行的 QEMU。默认优先使用 `/home/user/local/bin/qemu-novus`，否则使用
  `PATH` 里的 `qemu-system-x86_64`。
- OVMF 拆分镜像：`OVMF_CODE.fd` 和 `OVMF_VARS.fd`。默认目录
  `/home/user/local/usr/share/OVMF`。
- QEMU 的 `-L` 数据目录，默认 `/home/user/local/usr/share/qemu`。缺少它时
  QEMU 会报 `could not load PC BIOS 'bios-256k.bin'`。
- 下载镜像需要 `curl` 或 `wget`；本地镜像模式不需要。
- 主机至少留出 3 GiB 给 guest，另加 QEMU 自身开销。

## 快速开始

```bash
cd /path/to/novus
bash scripts/qemu-smoke.sh
```

只跑场景一和场景二：

```bash
bash scripts/qemu-smoke.sh --scenario ps2
```

用已经下载好的镜像，保留 `/tmp` 工作目录：

```bash
bash scripts/qemu-smoke.sh --image out/novus-disk.img --keep
```

覆盖镜像地址：

```bash
bash scripts/qemu-smoke.sh --url https://your-cdn.example/novus/novus-disk.img
```

退出码为 `0` 表示所有检查 PASS，`1` 表示至少一项 FAIL，`2` 表示环境或参数
错误（例如缺 Pillow、OVMF、QEMU、镜像不是 raw 磁盘）。

## 镜像地址优先级

脚本按以下顺序取镜像，先命中者生效：

1. `--url URL`
2. 环境变量 `NOVUS_IMAGE_URL`
3. 仓库根目录的 `out/latest-image-url.txt`（取第一行，去掉空白）
4. 脚本内置的 `DEFAULT_IMAGE_URL`，当前为
   `https://cdn.tracy.ink/novus/novus-disk.img`
5. `--image PATH` 则完全跳过下载，直接使用本地文件

`--image` 的相对路径按仓库根目录解析；`--output` 也按仓库根目录解析，默认
`out/qemu-smoke`。如果上传后的 CDN key 变了，改脚本里的 `DEFAULT_IMAGE_URL`
一行，或写 `out/latest-image-url.txt`，或每次传 `--url`。

脚本会先用 Python 检查镜像是至少 16 MiB 的 raw 磁盘且带有 MBR 签名，避免把
HTML 错误页或半截文件送进 QEMU。

## 固定的 QEMU 配置

两个场景都使用同一套基础参数，只在 USB 控制器上分流：

```text
-machine pc,vmport=off                   # i440fx，关闭 VMware absolute pointer
-accel tcg -smp 2 -m 3072
-drive if=pflash,format=raw,readonly=on,file=OVMF_CODE.fd
-drive if=pflash,format=raw,file=<work>/vars-<scenario>.fd
-drive file=<image>,format=raw,if=ide,index=0,media=disk
-snapshot -boot order=c
-serial file:<output>/serial-<scenario>.log
-monitor unix:<work>/hmp-<scenario>.sock,server=on,wait=off
-L /home/user/local/usr/share/qemu
-nic none -display none -no-reboot
```

- `OVMF_VARS.fd` 每个场景启动前复制一份，NVRAM 不跨场景、不跨重复运行污染。
- `vmport=off` 关掉 QEMU 的 VMware 绝对指针。guest 一旦探测到它，就会把
  `nd_input` 切成 absolute 模式并丢弃 PS/2 相对包；那样 `mouse_set 2` 和
  `mouse_move` 都不会移动光标，像素比对只剩时钟跳动，必然误报。
- `-snapshot` 保证磁盘镜像不被 guest 写回；`--keep` 只保留 `/tmp` 工作目录，
  镜像本身也不变。
- 串口写文件，不占用终端；HMP 走 Unix socket，Python 直接连，不依赖 `socat`。
- 场景一和场景二追加：

  ```text
  -usb -device usb-kbd -device usb-mouse
  ```

- 场景三追加：

  ```text
  -device qemu-xhci -device usb-kbd -device usb-mouse
  ```

## 时序

- QEMU 启动后轮询串口里的 `[ND] demo desktop painted`，最多等
  `--boot-timeout` 秒（默认 90）。
- 桌面出现后，保证从 QEMU 启动到截图至少经过 `--settle` 秒（默认 75，允许
  60..90），然后再发 `screendump`。这样既不会在启动动画中途截屏，也不会因为
  机器快而把等待时间压到 60 秒以下。
- 超过 `--boot-timeout` 仍没有桌面时记录 `boot.desktop-timeout`，继续生成报告
  并最终以退出码 1 结束。

## 场景一：串口 marker 与 PS/2 全 ACK

串口日志 `out/qemu-smoke/serial-ps2.log` 必须出现：

| 检查 | 必须出现 |
| --- | --- |
| `boot.identity-map` | `[MM] identity map ... RAM WB <十进制> bytes, MMIO UC <十进制> bytes`，两个数字都大于 0 |
| `boot.paging` | `[MM] paging enabled` |
| `boot.desktop-painted` | `[ND] demo desktop painted` |
| `boot.timeline` | 至少 3 条 `[T] +<数字>ms`，数值单调不减且最后一条大于第一条 |
| `input.ps2-all-ack` | 13 条 PS/2 必须项全部出现，且没有坏 verdict |
| `guest.no-fatal` | 没有 `[FATAL]`、`[TRAP] kernel fault`、`[ND] FAIL:`、`[HALT] panic` |

PS/2 必须项覆盖控制器自检、aux 口测试、鼠标复位/自检、设备 ID、采样率、
分辨率、缩放、开上报、IRQ 配置以及 `[PS2] keyboard IRQ1 + mouse IRQ12 enabled`。
任何 `no reply`、`FAIL`、`unexpected`、`resend requested`、`WARN` 都判 FAIL；
ACK 行少于 7 条也判 FAIL。坏 verdict 的原始串口行会进失败报告，不会被吞掉。

## 场景二：鼠标与键盘的像素比对

同一台 ps2 机器上，HMP 按顺序执行：

```text
screendump ps2-desktop.ppm
mouse_set 2
screendump ps2-pointer-a.ppm
mouse_move -180 120
sleep 0.7
screendump ps2-pointer-b.ppm
sendkey tab
sleep 0.9
screendump ps2-tab.ppm
sendkey ret
sleep 0.9
screendump ps2-ret.ppm
```

`mouse_set 2` 选的是 QEMU 注册的 PS/2 鼠标（`info mice` 里的
`* Mouse #2: QEMU PS/2 Mouse`），不是 USB HID 鼠标；`-usb` 场景下 USB 鼠标是
另一个编号。`mouse_move` 发相对位移，guest 的光标 overlay 会跟着动，所以
A/B 两张图像能逐像素比较。

比对规则（Python + Pillow）：

1. 两张 PPM 尺寸必须一致。
2. 任一 RGB 通道非零即算变化像素，生成二值 mask。
3. 按 dock 几何生成时钟排除区，把该矩形从 mask 清零。
4. 剩余 mask 的 bbox 必须非空，且 bbox 不能与时钟矩形相交。

时钟排除区由 `graphics/nd_demo.c` 的常量推导：

```text
dock_w = 660, dock_h = 56, gap = 10
tx = (W - 660) / 2, ty = H - 56 - 10
clock = (tx + 660 - 112 - 4, ty + 9 - 4, 96 + 8, 48)
```

1024x768 下即 `(726, 707, 104, 48)`，覆盖右对齐的时间标签和下面的日期行。
排除它是因为时钟每秒刷新会制造与输入无关的变化；如果不排除，鼠标没动也会
被误判为 PASS。

三组比对分别记为：

- `pointer.mouse-move`：A 对 B，必须看到光标位移。
- `keyboard.tab`：B 对 C，Tab 必须产生可见焦点变化。
- `keyboard.ret`：C 对 D，Return 必须产生可见变化（例如打开菜单/选中项）。

任何一组 bbox 为空、只落在时钟区、或截图缺失，都判 FAIL。报告 detail 会给出
完整 bbox、时钟区内被忽略的像素数和排除矩形。

## 场景三：USB 控制器与枚举

串口日志 `out/qemu-smoke/serial-xhci.log` 必须出现：

| 检查 | 必须出现 |
| --- | --- |
| `usb.controller` | `[USB] xhci <bus>:<dev>.<fn> ports=<hex>` 或 `[USB] BAR0 moved to ...` |
| `usb.enumeration` | 至少一条 `[USB] port 0x... connected, speed=...` 且至少一条 `[USB] HID interface ready` |
| `usb.no-errors` | 没有 USB 失败行 |

`usb.controller` 或 `usb.enumeration` 缺失一律 FAIL，不会降级成 WARN。内核不会
打印 `[USB] ... attached`，所以枚举成功以“端口 connected + HID interface
ready”这一对为准；只要缺任意一半，就算枚举未完成。

`usb.no-errors` 扫描这些失败串：`no reachable BAR0`、`no DMA memory`、
`enumeration failed`、`ep0 rc`、`bulk transfer failed`、`HID report descriptor
read failed`、`HID interface ignored`。

## 报告与失败信息

脚本最后打印一张表，并在表格下方逐条列出失败检查对应的原始串口行：

```text
SCENARIO  CHECK                   RESULT  DETAIL
--------  ----------------------  ------  ------
ps2       guest.no-fatal          PASS    no [FATAL]/[TRAP]/[ND] FAIL marker
ps2       boot.identity-map       PASS    RAM WB 3221225472 bytes, MMIO UC 268435456 bytes
...
ps2       pointer.mouse-move      FAIL    no pixel change at all between the two screendumps (clock=726,707,104,48)

FAILED CHECKS - raw log lines
[ps2] pointer.mouse-move: no pixel change at all between the two screendumps (clock=726,707,104,48)
    raw: (no serial line for this check)

RESULT: FAIL - 1 failed, 13 passed (of 14)
artifacts: out/qemu-smoke
```

- `report.txt` 是同样的完整文本；`checks.jsonl` 是每条检查的机器可读记录。
- 失败检查的 `raw` 优先取相关串口行；纯像素检查没有串口行时会明确写
  `(no serial line for this check)`。
- 进程级失败（QEMU 提前退出）会记录 `qemu.process`，并指向
  `qemu-<scenario>.stderr.log`。
- 所有检查 PASS 时退出码 `0`；任意 FAIL 时 `1`；参数/环境错误 `2`。

## 产物

默认输出目录 `out/qemu-smoke/`：

| 文件 | 内容 |
| --- | --- |
| `serial-ps2.log` / `serial-xhci.log` | 每个场景的完整串口日志 |
| `ps2-*.ppm` / `xhci-*.ppm` | HMP `screendump` 原始截图 |
| `hmp-ps2.jsonl` / `hmp-xhci.jsonl` | 每条 HMP 命令和回显 |
| `qemu-*.stdout.log` / `qemu-*.stderr.log` | QEMU 进程输出 |
| `checks.jsonl` | 每条检查的结构化结果 |
| `report.txt` | PASS/FAIL 表和失败原始行 |

每轮开始会清掉这些已知产物和 `checks.jsonl`，避免上一轮的记录混进本轮。
`/tmp/novus-qemu-smoke.XXXXXX` 工作目录默认在退出时删除；加 `--keep` 可以保留
其中的 OVMF VARS 副本和 HMP socket 便于排查。

## 可重复性

- 每个场景复制一份新的 `OVMF_VARS.fd`，不共享 NVRAM。
- 磁盘始终 `-snapshot`，guest 写入不落盘。
- 输出目录在开始时清理已知文件；临时目录用 `mktemp -d` 独立命名。
- QEMU 退出时先走 HMP `quit`，再 `TERM`，最后 `KILL`，不残留进程和 socket。
- 同一镜像、同一 QEMU 版本下可以连续跑；时钟区已排除，不会因为跨秒刷新而
  随机 PASS/FAIL。

## 排障

| 现象 | 处理 |
| --- | --- |
| `accel_init_ops_interfaces: assertion failed (ops != NULL)` | 使用 `--qemu /home/user/local/bin/qemu-novus`；直接跑该目录下的 `qemu-system-x86_64` 在 QEMU 6.2 会崩 |
| `libfuse3.so.3` / `libpmem.so.1` 找不到 | 脚本对 `/home/user/local/*` 的 QEMU 自动加 `LD_LIBRARY_PATH`；自装 QEMU 时手动补 |
| `could not load PC BIOS 'bios-256k.bin'` | 确认 `-L` 指向 `/home/user/local/usr/share/qemu`，或传 `--qemu-share` |
| 下载 403/404 | 用 `--url` 指向真实 CDN，或先下载再用 `--image`；也可写 `out/latest-image-url.txt` |
| `image is not a raw Novus disk` | 镜像不是至少 16 MiB 的 MBR raw 磁盘，可能下载到了错误页 |
| 串口日志为空 | 先看 `qemu-*.stderr.log`；多数是 OVMF/`-L` 路径不对或 QEMU 没起来 |
| 截图空白或颜色很少 | 确认 guest 真的到了桌面；`screen.desktop-capture` 会检查尺寸和颜色数 |
| 鼠标比对为空 | 先看 `hmp-ps2.jsonl` 里 `mouse_set 2` 是否被接受；再用 `info mice` 确认 2 号是 PS/2 鼠标；串口若出现 `[ND] absolute pointer ready (VMware mouse protocol)`，说明 `vmport=off` 没生效，PS/2 相对包会被忽略 |
| 鼠标只改了时钟区 | 光标落进了 dock 时钟；换一个起始位置或增大相对位移，不要放宽排除规则 |
| Tab/Return 没有像素变化 | 说明当前桌面没有焦点环或菜单响应；这是真实 FAIL，先看 `ps2-tab.ppm` / `ps2-ret.ppm` |
| `usb.controller` PASS 但 `usb.enumeration` FAIL | 检查 QEMU 是否真的加了 `-device qemu-xhci -device usb-kbd -device usb-mouse`，以及 `[USB] port ... connected` 后是否跟着 `HID interface ready` |
| `[USB] ep0 rc ... code 0x...6` | 控制传输被设备 STALL，枚举未完成；这是真实 FAIL，检查描述符请求/端点配置，不要改成 WARN |
| USB 行完全没有 | 场景三才算 USB；`--scenario ps2` 不要求 `[USB]` |

## 备注

- 该脚本不替代 `scripts/qemu_smoke.py` 的 RFB/VNC 基线比对；后者是另一套
  q35 + VNC + QMP 流程。本脚本刻意只使用串口和 HMP，便于在无图形桌面的 CI
  机器上跑。
- 默认 CDN 地址是脚本常量，上传路径变化时请用 `--url` 覆盖，不要把真实地址
  硬编码进文档或 CI。
- 本脚本只读运行镜像；不会构建、刷盘或修改仓库里的任何源码文件。
