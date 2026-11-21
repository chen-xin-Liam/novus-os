# C/Rust 职责边界与 FFI 约定

## 职责边界

| 领域 | C（`c/core`） | Rust（`rust/nd-core`） |
| --- | --- | --- |
| 系统调用 | 不调用 | 不调用；仅经 HAL 调用平台实现 |
| 样本解析 | 头部、字节分布、熵近似、可疑字符串、AC 匹配、哈希、模糊指纹 | 规则解释、置信度融合、判定策略、结果聚合 |
| 内存 | 调用方提供的固定缓冲与静态对象池 | RAII 句柄、边界检查、生命周期约束 |
| 错误 | `i32`：0 成功、负值错误、正值计数 | 映射为 `Result<T, i32>` 并记录上下文 |
| 并发 | 无共享可变状态；实例由调用方独占 | 队列、优先级、背压、取消与限流 |
| 跨边界 | 只暴露 C ABI 与定长结构 | 只在 `ffi.rs` 使用 `unsafe`，外部为安全接口 |

## ABI 规则

1. 跨边界结构全部 `#[repr(C)]`，只含定长整数、指针和长度；禁止 `String`、`Vec`、`Box`、trait 对象越界。
2. 字符串/字节串统一用「指针 + 长度」；不传 C 字符串，不假设 NUL 结尾。
3. 不透明句柄 `nd_engine` 由 C 静态池创建，Rust 用 `Engine` RAII 包裹；禁止跨层释放对方内存。
4. 输入缓冲只在调用期间有效；回调不得递归进入同一引擎实例，不得长时间阻塞。
5. 所有公共 C 函数校验空指针、长度与容量上限。
6. C 核心编译使用 `-ffreestanding -fno-builtin -fno-stack-protector`，不链接 libc。

## 数据结构

```c
typedef struct nd_engine nd_engine;

typedef struct {
    int32_t  verdict;
    int32_t  rule_id;
    uint64_t offset;
    uint32_t confidence;
    uint32_t reserved;
} nd_scan_result;

typedef struct {
    int32_t  kind;
    int32_t  pid;
    uint64_t object_id;
    uint32_t len;
    const uint8_t *data;
} nd_event;

typedef int32_t (*nd_event_cb)(const nd_event *event, void *ctx);
```

## 函数签名

```c
int32_t nd_engine_create(nd_engine **out, const uint8_t *cfg, uint32_t cfg_len);
void    nd_engine_destroy(nd_engine *engine);
int32_t nd_db_load(nd_engine *engine, const uint8_t *db, uint32_t db_len);
int32_t nd_db_rule_count(const nd_engine *engine);
int32_t nd_scan_chunk(nd_engine *engine, const uint8_t *buf, uint32_t len,
                      int is_last, nd_scan_result *out, uint32_t cap);
int32_t nd_register_event_cb(nd_engine *engine, nd_event_cb cb, void *ctx);

uint32_t nd_fnv1a32(const uint8_t *buf, uint32_t len, uint32_t seed);
uint32_t nd_crc32(const uint8_t *buf, uint32_t len, uint32_t seed);
void     nd_sha256(const uint8_t *buf, uint32_t len, uint8_t out32[32]);
int32_t  nd_heuristic_score(const uint8_t *buf, uint32_t len, uint32_t *score);
int32_t  nd_fuzzy_digest(const uint8_t *buf, uint32_t len, uint8_t out32[32]);
int32_t  nd_fuzzy_similarity(const uint8_t *a, const uint8_t *b, uint32_t *pct);
int32_t  nd_db_verify_header(const uint8_t *db, uint32_t len,
                             uint32_t *version, uint32_t *count);
uint32_t nd_db_crc(const uint8_t *db, uint32_t len);
```

## 错误码

`0` 成功；`-1` 无效参数；`-2` 容量不足；`-3` 未找到；`-4` 版本不符；`-5` 校验失败；`-6` 状态错误；`-7` 能力不支持。正值表示写入结果数量。

## 分块扫描语义

`nd_scan_chunk` 在引擎内保留 `max_pattern_len - 1` 字节重叠窗口，跨调用命中不会丢失；`is_last` 表示流结束并清空尾部。命中数组由调用方提供，容量不足时返回已写入数量，超出部分被丢弃（Rust 侧按固定容量聚合）。
