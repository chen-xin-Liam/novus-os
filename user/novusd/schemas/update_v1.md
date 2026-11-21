# 增量更新包格式 v1

所有整数为小端。

## 头部（52 字节）

| 偏移 | 长度 | 字段 | 说明 |
| --- | --- | --- | --- |
| 0 | 8 | magic | ASCII `NDSUPD01` |
| 8 | 4 | format_version | 固定为 1 |
| 12 | 4 | new_version | 必须大于当前版本 |
| 16 | 4 | payload_len | 新特征库字节数 |
| 20 | 32 | payload_sha256 | 对 payload 的 SHA-256 |

## 载荷与尾部

| 偏移 | 长度 | 字段 | 说明 |
| --- | --- | --- | --- |
| 52 | payload_len | payload | 完整的 `sigdb_v1` 文件 |
| 52+payload_len | 4 | crc32 | 覆盖除尾部 CRC 外的全部字节 |

## 应用流程

1. 校验魔数、格式版本、长度、新版本。
2. 校验 CRC32 与 payload SHA-256。
3. 写入 `/var/lib/novus/sigdb.tmp`。
4. 将当前库重命名为 `sigdb.bak`，临时库重命名为 `sigdb.bin`。
5. 失败时可用 `rollback` 将备份换回。
