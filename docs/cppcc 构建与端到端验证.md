# cppcc 构建与端到端验证

## 构建

`CONFIG_USER_CPPCC=y` 时，`make` 会在主机端构建 Rust 工具并产出 `build/tools/cppcc`（Windows 为 `cppcc.exe`）。单独构建编译器可运行 `make cppcc`；只编译 smoke 样例可运行 `make cppcc-demo`，产物为 `out/cppcc/hello.nuc`。

`make run` 会构建样例、将其复制为 `esp/APPS/DEMO.NUC`，再调用 `scripts/run.sh` 启动 q35 + OVMF。`make iso` 也会把同一文件打入 ESP 镜像。若同时启用 Mochi，cppcc 优先占用 boot-stage 固定的 `APPS/DEMO.NUC` 槽位；设置 `CONFIG_USER_CPPCC=n` 后，原 Mochi 打包规则恢复。

## 运行路径

启动时 `arch/x86_64/boot/stage3.c` 通过 UEFI FAT 读取 `APPS/DEMO.NUC` 到 reserved pages，并写入 shared memory 的 `app_img_base/app_img_size`。`kernel/kmain.c` 在 scheduler 初始化后读取该镜像并调用 `nuc_load(..., "demo")`。因此无需内核改动，也无需运行后从 shell 手动 `exec`。

串口预期能看到 boot-stage 的 `[NUC ] APPS/DEMO.NUC staged`、内核的 `[NUC] demo: ... entry ...`，随后用户 syscall 输出 `[USER] Hello, Novus!`。loader 启动中的 NUC selftest 也会先输出其测试结果。

## 格式校验与限制

cppcc 的 nucgen 在写文件前校验冻结 Header、段表、符号表、重定位表、String Table、段页对齐、USER flags、BSS 无文件载荷和 relocation 类型 1/2。`cargo test --manifest-path user/cppcc/Cargo.toml` 覆盖格式字段和从 Hello 源码到 NUC 的端到端生成；仓库当前没有 `nucdump`。

当前 boot-stage 只读取固定文件名 `APPS/DEMO.NUC`，`scripts/run.sh` 本身只启动已有 `esp/` 目录；所以请通过 `make run` 进行自动 staging。测试程序运行后由 `_start` 调用 `main`，再显式发起 `SYS_EXIT=60`。

## 本次验证记录（2026-10-18）

工作区静态诊断未报告 cppcc 源码错误。当前 Windows 环境中 `make`、`cargo`、`rustc`、`qemu-system-x86_64`、`xxd` 和 MinGW x86-64 GCC 均未出现在 PATH，因此本次无法执行 Cargo 测试、编译 NUC、逐字节查看实际产物或启动 QEMU；不能将预期串口日志视为已实测结果。工具链可用后，按上面的 `make run` 路径完成运行验证，并以 `[NUC ] APPS/DEMO.NUC staged`、`[NUC] demo: ...` 和 `[USER] Hello, Novus!` 为检查点。