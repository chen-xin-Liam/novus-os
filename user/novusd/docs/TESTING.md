# 测试分层

## 命令

```bash
make test             # C 单测 + Rust 单测/集成测试
make test-e2e         # 端到端脚本
make check-static     # 静态链接校验
make check-core-purity
make size-check       # Novus 镜像 < 128 KiB
scripts/test-all.sh   # 全部
```

## C 单元测试

`c/core/tests` 使用极简 harness（无 cmocka 等外部依赖），覆盖：

- AC：重叠模式、跨块边界、坏 CRC
- 哈希：FNV 空串、CRC32 标准向量、SHA-256 空串与 `abc`
- 启发式：空输入、可疑样本、良性样本
- 模糊：相同/不同/空输入
- 扫描：EICAR 命中与良性负例

## Rust 集成测试

`rust/nd-tests/tests`：

- `detection.rs`：EICAR、良性、wildcard 展开、调度优先级、行为升级
- `e2e.rs`：目录树扫描同时检出 EICAR 与忽略良性文件
- `quarantine.rs`：stash → restore 一致性与 delete
- `update.rs`：合法更新、旧版本拒绝、损坏包、回滚
- `dropped_eicar.rs`：使用外部拖入的 `eicar.com`，验证扫描命中 → 隔离删除 → 恢复 → 再次删除

## 端到端脚本

- `tests/e2e/scan_flow.sh`：生成 EICAR/良性样本 → `selftest` → `scan --quarantine` → 校验输出 → `quarantine list`
- `tests/e2e/dropped_eicar_flow.sh`：拖入的 `eicar.com` → 命中并删除 → 恢复 → 删除
- `tests/e2e/update_flow.sh`：`make-update` → `update` → `status`

## 样本

`tests/data/eicar.txt` 为 EICAR 标准测试串；`tests/data/benign.txt` 为误报对照。EICAR 是业界通用无害正例，不包含真实恶意代码。

外部拖入样本路径（集成测试与端到端脚本会自动探测，不存在时跳过）：

`/mnt/c/Users/陈信/AppData/Local/Temp/codebuddy-dropped-files/7b17c74e-9e4c-4fc2-9293-37006c114d20/eicar.com`

测试只操作该样本在 `/tmp` 下的副本，不会删除或改动原始拖入文件。
