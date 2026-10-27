# NUC 一致性测试向量（Nuomi × novus-os 共享）

一套"跑一下就知道"的 `.nuc` fixtures，用于把规范争议降级成可执行断言。**novus-os 的加载器与 Nuomi 的编译器/校验器应给出相同结果。**

| fixture | 操作 | 期望 | 规范依据 |
|---|---|---|---|
| `golden-valid.nuc` | run | `entry returned: 42` | §9 入口加载流程 |
| `bss-valid.nuc` | run | `entry returned: 42` | §4.4 BSS 不占文件空间、加载时清零 |
| `overflow-mul.nuc` | run | `entry returned: 10000000000` | 64 位算术 + 64 位入口返回（`(* 100000 100000)`） |
| `bad-reserved.nuc` | validate | `[-1]` reserved 非 0 | §3.2 |
| `bad-no-vmuser.nuc` | validate | `[-1]` 缺 VM_USER | §4.3 bit3 必置 |
| `bad-text-noread.nuc` | validate | `[-1]` TEXT 不可读 | §4.4 |
| `bad-static-extern.nuc` | validate | `[-1]` STATIC 含 EXTERN | §10 |
| `bad-bssfileoffset.nuc` | validate | `[-1]` BSS 占文件空间 | §4.2/§4.4 |
| `bad-seg-oob.nuc` | validate | `[-11]` 段数据越界 | §2/§12 EOVERFLOW |
| `bad-rel-symi.nuc` | validate | `[-11]` reloc sym_idx 越界 | §7.3/§12 EOVERFLOW |

## 用法

```bash
python tests/nuc/gen-fixtures.py   # 生成/刷新全部 fixtures（需 nuomi + nucutil）
python tests/nuc/run-tests.py      # 跑全部 fixtures 并比对预期（可挂 CI）
```

## 背景

- 2026-10-18 跨仓核验发现两处争议待 liam 侧确认：
  1. **P0 Header 48 vs 64**（规范 §3.1 结构体与"48 字节"自相矛盾）——本仓库实现按 48；liam 内核加载器若按 `sizeof(nuc_hdr_t)=64` 读段表会整体错 16 字节。待 liam 拍板（倾向改规范为 48 + `uint32_t reserved[4]`）。
  2. **P2 规范 §11 最小示例 hex 自身 4 处不自洽**（rel/strtab 偏移违反布局铁律、entry_point 与正文不符、compile_flag 注释错）——勿把 §11 hex 当黄金样本，本目录的 fixtures 才是。
- `overflow-mul.nuc` 锁死两件事：后端 64 位算术（历史 bug：32 位 `imul eax,edx` 截断）、宿主 runner 64 位入口返回（历史 bug：`int rc = fn()` 截断）。
