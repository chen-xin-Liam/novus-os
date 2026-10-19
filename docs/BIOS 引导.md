# Novus OS BIOS 引导

状态：BIOS MVP 源码已加入；本机缺少 GNU build/QEMU 工具链，尚未完成镜像构建和启动验收。请将下文的启动日志视为预期标记，不是已取得的验证结果。

## 构建与运行

在具备 host `gcc`、GNU `objcopy`、Cargo（含 `x86_64-unknown-none` target）和 QEMU 的 Linux 环境中：

```sh
make bios-img
make run-bios
```

`make bios-img` 生成 `out/bios.img`，`make run-bios` 通过 SeaBIOS 启动并把 COM1 接到终端。可用 `bash scripts/run-bios.sh 8G` 修改来宾内存，或设置 `QEMU` 指定 QEMU 可执行文件。UEFI 默认构建和运行仍为 `make` / `make run`，后者继续使用 OVMF。

BIOS 路径使用 host GCC 输出 ELF，C 内核以 MS x64 ABI 编译，Rust 库以 `x86_64-unknown-none` 输出 ELF，并启用 `bios-ms-abi` feature 通过 `win64` FFI 与 C 互调。UEFI 与 host 工具构建不启用该 feature，继续使用目标 C ABI。stage1/stage2 是 `.code16` 汇编，不依赖 `-m16`。BIOS ELF 对象位于独立 `build/bios/`，不复用 PE/COFF 对象。

## 启动链和镜像布局

```text
SeaBIOS -> LBA 0 bootsect -> LBA 1..64 stage2
         -> E820/VBE + 内核分块读取 -> A20/GDT/4GiB identity paging
         -> long mode -> 0x100000 legacy_entry -> legacy_main
         -> EFI_MEMORY_DESCRIPTOR[40-byte] -> kernel_entry -> kmain/PMM
```

| LBA | 内容 |
| --- | --- |
| 0 | 512 字节 bootsect，446 字节以内代码、64 字节空分区表、`55 AA` |
| 1..64 | 固定 32 KiB stage2 |
| 65 起 | raw kernel image，按 512 字节向上补齐 |

stage2 尾部 `NVB2` 头由 `tools/mkboot/mkboot.c` 写入 kernel sector count 和实际字节数。内核 ELF 的 `legacy_entry` 被链接脚本断言在 `0x100000`，因此 stage2 可直接跳到该地址。磁盘采用无分区表 superfloppy，不包含 FAT/GPT 或 ISO。

## 当前能力边界

- stage2 在实模式采集 E820 和 VBE，使用 16-byte EDD DAP 分块读入低内存 bounce buffer，再短暂进入保护模式复制到 1 MiB 以上，避免依赖 EDD 3.0 的 64-bit buffer pointer。
- BIOS 只选择报告为 1280×800×32、线性 framebuffer 的 VBE 模式；没有完全匹配模式或颜色 mask 不受 ND 支持时回退 VGA 文本。模式号不固定写死为 `0x118`。
- legacy C 入口将 E820 转成与仓库 `uefi.h` 一致的 40-byte EFI 描述符，按页拆分 loader/kernel/framebuffer 保留区，避免 PMM 将启动期间仍使用的物理页分配出去。
- 当前内存管理位图覆盖上限为 8 GiB；BIOS 合成 map 只表示低 8 GiB。低于 2 GiB Conventional Memory 会拒绝启动。
- 当前 MVP 不读取 ESP，不提供 UEFI stage3 的 live root、Merkle 磁盘验证、NKM/NUC 暂存、菜单或安装器。对应 shared-memory 字段保持空/未知，内核应走既有降级路径。

## 故障排查

| 现象 | 检查方向 |
| --- | --- |
| `NO STAGE2` | 确认镜像 LBA 0 签名、stage2 恰为 64 扇区、BIOS EDD AH=42 可用 |
| `STAGE2 ERROR` | 当前错误统一显示；检查 E820 返回 `SMAP`、VBE BIOS、EDD、A20 及 `NVB2` patch header |
| 无 `[BIOS] legacy boot` | 检查 stage2 long-mode far jump、CR4.PAE、CR3、EFER.LME、CR0.PG、低 4 GiB 映射及 `legacy_entry == 0x100000` |
| PMM 报空 map / 低内存 | 核对 `bios_boot_info` magic、E820 count/stride、描述符大小 40、页对齐与 2 GiB 最低值 |
| ND 无 framebuffer | 确认 VBE mode info 的 pitch 是字节、RGB mask 与 framebuffer 范围有效；否则 VGA 是预期回退 |
| Kernel ELF 链接失败 | 检查 host GCC 是否支持 `-mabi=ms`、Rust unknown-none target 是否安装、Rust 导出是否为 `win64` ABI，以及未解析运行时符号 |

完成 Ubuntu/QEMU 验收后，应把串口关键行和 UEFI 回归结果补入 `changes.md`，并将 `docs/BIOS 引导方案.md` 中的偏离项逐项标记为已验证或待处理。