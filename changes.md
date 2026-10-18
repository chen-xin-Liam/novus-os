# Changes

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