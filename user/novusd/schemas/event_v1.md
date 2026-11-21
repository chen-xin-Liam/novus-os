# 结构化事件 v1

## 日志级别

`0` debug、`1` info、`2` warn、`3` error、`4` alert。

## HAL 原始事件 `RawEvent`

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| kind | u32 | 1=进程、2=文件、3=系统调用 |
| pid | u32 | 发起进程；文件事件可为 0 |
| object_id | u64 | 对象标识（路径哈希/规则 id） |
| len | u32 | data 有效长度 |
| data | [u8;64] | 事件附带数据（文件名/命令名） |

## 行为决策

`Allow` 放行；`Deny` 拒绝；`Quarantine` 隔离关联文件。

## 去重

`Logger` 在最近 16 条记录内按（级别、消息字节）去重；累计去重次数可通过 `dedup_count()` 读取。
