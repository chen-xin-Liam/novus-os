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

## 多目标验证

```sh
make cppcc-tools
build/tools/cppcc examples/cxx11_smoke.cc -o /tmp/smoke.nuc
build/tools/cppcc_to_win64 examples/cxx11_smoke.cc -o /tmp/smoke.exe
build/tools/cppcc_to_linux examples/cxx11_smoke.cc -o /tmp/smoke.elf
chmod +x /tmp/smoke.elf && /tmp/smoke.elf
cargo test --manifest-path user/cppcc/Cargo.toml --release
```

`cppcc_to_win64` 产物可用 `x86_64-w64-mingw32-objdump -x` 检查 PE32+ 头、节表和 kernel32 导入表；`cppcc_to_linux` 产物可用 `readelf -h -l -d` 检查 ELF64 程序头、PT_INTERP 与 dynamic 段。

## 本次验证记录（2026-11-20）

- `make -j2 cppcc-tools` 成功，产出 cppcc、cppcc_to_win64、cppcc_to_linux 与 libcppcc_cabi.a。
- `cargo test --release` 全部通过：59 个单元测试、4 个编译 API 测试、PE32+ 结构测试与 ELF64 结构测试。
- `examples/cxx11_smoke.cc` 已用三个目标成功编译；生成的 ELF64 在本机实际执行并输出 `ok`，退出码 0。
- NUC/QEMU 启动路径仍按上文 `make run` 检查点验证；本次未在 QEMU 中重复启动。