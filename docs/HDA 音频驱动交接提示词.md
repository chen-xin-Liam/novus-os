# 任务提示词：Novus-OS Intel HDA 音频驱动收尾

> 历史说明：`hda_banner()` 与 `[BOOT] diag:` 诊断条已随启动诊断页一并移除；文中相关 API 说明仅作历史记录。

> 直接把你（接手方）要执行的完整上下文写在下面，照着做即可，不需要再问需求方。

## 0. 一句话目标

把已经写了一半的 Intel HD Audio 播放驱动跑通，并留下**可验证的取证**：QEMU 里用 `-audiodev wav` 把实际播放的 PCM 录成 wav 文件，boot 日志出现 `[HDA] played lpib=<非零> of cbl=192000`，wav 里能测出 440 Hz。做完把结果回填进 `docs/系统能力完成度总表.md`。

## 1. 仓库与环境（硬约束）

- 源码在 **Windows 侧** `C:\novus`（junction 指向 `C:\Users\张辰宇\Downloads\novus-os-main`），分支 `b/boot-anim`，**全部改动未提交**，工作区里还有前几轮大量改动，**严禁 `git add -A` / `git commit`**，只改自己需要的文件。
- 双环境：Windows 环境只有 `PowerShell`（Bash 在该环境被禁用）；Linux 云主机环境 `vm` 才有 `qemu-system-x86_64`，只有 `Bash`。用 `manage_environment(action="switch", env_id="client:582478:714055298858128"|"vm")` 切换。
  - **`Read`/`Edit`/`Write` 只能改当前环境里的文件**：在 vm 环境里编辑 `C:\novus\...` 会报 `file_path must be absolute`，先切回 Windows。
  - `present_files` 只在 Windows 环境可用。
- 当前环境默认是 Windows。

## 2. 已经完成的代码（不要重复写，直接在其上改）

| 文件 | 状态 | 内容 |
|---|---|---|
| `kernel/hda.h` | 新增 | `hda_init()` / `hda_selftest()`，`NOVUS_HDA_SELFTEST` 默认 0 |
| `kernel/hda.c` | 新增，**有 bug 待修** | HDA 控制器探测、GCTL 复位、STATESTS 读 codec、CORB/RIRB 建环、节点枚举、converter+pin 配置、BDL 播放 440 Hz 方波 |
| `Makefile` | 已改 | `SRC` 加了 `kernel/hda.c`；CFLAGS 加了 `-DNOVUS_HDA_SELFTEST=$(if $(filter 1,$(HDA)),1,0)` |
| `kernel/kmain.c` | 已改 | 顶部 `#include "hda.h"`；`i2c_hid_init();` 之后加了 `(void)hda_init();` 与 `#if NOVUS_HDA_SELFTEST (void)hda_selftest(); #endif` |

## 3. 已经验证到的（QEMU 实测，可复现）

用 `intel-hda` + codec 的 QEMU 跑出来：

```
[HDA] controller 0x8086:0x2668 bar0=0xC1030000
[HDA] gcap=0x4401 output streams=0x1
[HDA] statests=0x1
[HDA] codec=0x0
[HDA] corb+rirb at 0x395000/0x396000
[HDA] dbg gctl=0x1 corbctl=0x1 rirbctl=0x1 corbrp=0x8000 corbsize=0x42 rirbsize=0x42 corbbase=0x395000 rirbbase=0x396000
[HDA] root subnode query got no answer
[HDA] dbg corbwp=0x1 rirbwp=0x0 intsts=0x0
```

结论：控制器/复位/CORB-RIRB 建环都对（寄存器读回与写入一致，CORBWP 已推进到 1），**但 codec 一个 verb 都没回，RIRBWP 始终 0**。

## 4. 根因判断（已定位，就差这一步）

verb 用错了。HDA 的 0xF00 不是「取子节点数」的固定 verb，而是 **`AC_VERB_PARAMETERS`（0x0F00）+ payload 为参数 ID**：

- 取节点数 → payload `0x04`（`AC_PAR_NODE_COUNT`），回包 `count | 首个节点号 << 8`
- 取 widget 能力 → payload `0x09`（`AC_PAR_AUDIO_WIDGET_CAP`），回包 bits 31..20 = widget type（0 = audio output converter，4 = pin complex）
- 原来写的是「0xF00 payload 0x00」和「0xF09 payload 0」，QEMU 的 codec 对不认识的 verb 直接不回，所以 RIRBWP 不前进。

**当前文件里已经把常量改成正确的了**（`HDA_V_PARAMS 0x0F00`、`HDA_PAR_NODE_COUNT 0x04`、`HDA_PAR_WIDGET_CAP 0x09`），但**调用点还没跟着改**，这是接手要做的第一件事：

1. `hda_init()` 里两处 `hda_send(..., HDA_V_SUBNODES, 0x00u, &resp)` → 改成 `hda_send(..., HDA_V_PARAMS, HDA_PAR_NODE_COUNT, &resp)`（root nid 0 一处、AFG 那处一处）。
2. widget 能力那处 `hda_send(g_codec, nid, HDA_V_WIDGET_CAPS, 0x00u, &caps)` → `hda_send(g_codec, nid, HDA_V_PARAMS, HDA_PAR_WIDGET_CAP, &caps)`。
3. 若编译报 `HDA_V_SUBNODES` / `HDA_V_WIDGET_CAPS` 未定义，说明常量已删、只剩调用点，按上面替换即可。

改完再跑的判定：

- 成功枚举 → 日志出现 `[HDA] root reply=0x... subnodes=0x...`、`[HDA] afg nid=0x1 subnodes=0x...`、`[HDA] dac nid=0x2 pin nid=0x3`（QEMU `hda-output` 的节点号不一定是 2/3，以日志为准）。
- 然后 converter 配置（`0x706` 流标签、`0x200` 格式 0x11、`0x300` amp、`0x707` pin 输出使能）发下去，再 RUN 流描述符。

## 5. 构建与验证步骤（照抄）

**构建（Windows 侧）**

```
cmd /c C:\msys64\tmp\build_hda.cmd
```

该脚本内容等价于：

```
cd /d C:\novus
make.exe CROSS= disk-img -j8 HDA=1 > C:\msys64\tmp\mhda.log 2>&1
```

产物 `C:\novus\out\novus-desktop.img`，恒 67,108,864 B；取 sha256 前 12 位当版本号。

**传到 vm**

1. Windows 侧 `FileBatchUpload` 传 `C:\novus\out\novus-desktop.img` → 得到 CDN url；
2. 切到 vm，`curl -sSL -o /tmp/deskN.img <url>`，`sha256sum` 核对前 12 位；
3. `cp /tmp/vars59.fd /tmp/varsN.fd`（OVMF 变量盘，里面有能启动的 Boot 项，**必须复用**，不要新建空 vars）。

**起 QEMU（关键参数，错一个就掉进 UEFI Shell 或没声音）**

```
/home/user/local/usr/bin/qemu-system-x86_64 \
 -m 3072 -smp 2 -L /home/user/local/usr/share/qemu \
 -drive if=pflash,format=raw,readonly=on,file=/home/user/local/usr/share/OVMF/OVMF_CODE.fd \
 -drive if=pflash,format=raw,file=/tmp/varsN.fd \
 -drive file=/tmp/deskN.img,format=raw,if=virtio \
 -netdev user,id=n0 -device e1000,netdev=n0,addr=0x7,romfile=/tmp/pxe-e1000.bin \
 -audiodev wav,id=snd0,path=/tmp/hdaN.wav \
 -device intel-hda,addr=0x8 -device hda-output,audiodev=snd0 \
 -device qemu-xhci -device usb-mouse -device usb-kbd \
 -vnc 127.0.0.1:85 -monitor unix:/tmp/monN,server,nowait \
 -serial file:/tmp/sxN.log -display none
```

要点：

- **`addr=0x7`（e1000）和 `addr=0x8`（intel-hda）必须显式写**：否则 PCI 槽位变动，OVMF 里的 `PciRoot(0x0)/Pci(0x4,0x0)` 启动项失效，进 UEFI Shell。
- **codec 要用 `hda-output`，不要 `hda-duplex`**：`-audiodev wav` 只做输出，duplex 的 ADC voice 创建失败会报 `audio: Failed to create voice 'adc'`。
- 长跑用 `nohup bash /tmp/runN.sh > /tmp/qN.out 2>&1 &`，别在 Bash 里 `sleep 25 && grep ...`（会被拦）。

**取证**

```
grep -an 'HDA' /tmp/sxN.log
ls -l /tmp/hdaN.wav
```

- 成功标志：`[HDA] played lpib=<非零> of cbl=192000`，且 wav 文件大小 > 0（0.5 s 立体声 48k/16 约 192 KB）。
- 用 python 复核 wav：读 `wave` 模块取 nframes/声道/采样率，统计过零次数 ≈ 880/s（440 Hz 方波），RMS 非零。**只看日志不够，wav 必须实测有波形。**

**清理**

- 杀 QEMU：`for p in $(pgrep -a qemu | awk '$2 ~ /qemu-sys/ {print $1}'); do kill -9 $p; done`
- **绝对不要 `pkill -f '<镜像名>'`**：命令行里含该字符串会把执行命令的 shell 自己杀掉（返回 137）。

## 6. 做完后的交付

1. 出**正式版镜像**：`cmd /c C:\msys64\tmp\build_main.cmd`（不带 `HDA=1`，即默认不响），取 sha256，`FileBatchUpload` 拿 CDN 链接。
2. 回填 `docs/系统能力完成度总表.md`：
   - 相关条目：第二部分专项一「七、多媒体 & 音频」里的音频行（如「音频设备驱动/HD Audio 播放」）；第十五章「电源 & 热管理」不受影响，别乱动。
   - 判定口径：✅ = 有代码 **且** 有 QEMU 日志/wav 取证；只有代码记 🟡。
   - 总览计数**用脚本按状态单元统计**，禁止手填（主清单/专项一/专项二/专项三各一段，注意专项三里有 `✅ 遵守` 这类非标准写法，沿用已有数字）。
   - 第五部分「变更日志」追加一行：日期 / 条目 / 🟡→✅ / 证据片段 / 镜像 sha256 前 12 位；「附：核心证据索引」补日志与 wav 路径。
3. 用 `present_files` 交付更新后的总表。

## 7. 已知风险 / 未做

- 现有实现只覆盖 QEMU 的 codec 行为；真机（HP EliteBook 640 G10 的 Sunrise Point-LP HD Audio）还需要：codec 上电（`0x705` D0 已发）、amp gain 具体 index、pin 的具体输出选择（HP out vs speaker），以及 AC'97 之外的机型兜底。
- 没有中断（IRQ）驱动：靠轮询 LPIB 300 ms 判断是否在跑；后续可以接 IOAPIC。
- 没有混音/多流/录音（ADC）、没有音量控制接口、没有和用户态（syscall）打通。
- 原 `hda_banner()` 只服务于已删除的全屏启动诊断页，现一并移除；真机音频状态请从串口 `[HDA]` 日志读取。
