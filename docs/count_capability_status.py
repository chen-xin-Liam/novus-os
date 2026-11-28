#!/usr/bin/env python3
"""按「状态单元」统计《系统能力完成度总表》各清单的 ✅/🟡/⬜/⚪ 数量。

一个状态单元 = 表格里一行，其第 3 个单元格正好是状态标记
（`✅`、`🟡`、`⬜`、`⚪`，以及专项三历史写法 `✅ 遵守`）。
判定口径表、总览表、变更日志里的其它写法一律不计。

用法：
    python docs/count_capability_status.py [文档路径]
"""

import re
import sys

PARTS = [
    ("主清单（二~二十章）", "# 第一部分"),
    ("专项一：可执行程序 & 二进制 ABI", "# 第二部分"),
    ("专项二：底层图形基础设施 & 3D", "# 第三部分"),
    ("专项三：Windows PE / Win32 兼容", "# 第四部分"),
]

STATUS = {"✅": "done", "🟡": "partial", "⬜": "todo", "⚪": "deferred"}
DEFERRED_MARK = "✅ 遵守"


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else "docs/系统能力完成度总表.md"
    lines = open(path, encoding="utf-8").read().splitlines()

    starts = []
    for _, header in PARTS:
        idx = next((i for i, line in enumerate(lines) if line.startswith(header)), None)
        if idx is None:
            print("missing part header: %s" % header)
            return 1
        starts.append(idx)
    starts.append(len(lines))

    total = {"done": 0, "partial": 0, "todo": 0, "deferred": 0, "units": 0}
    for i, (name, _) in enumerate(PARTS):
        counts = {"done": 0, "partial": 0, "todo": 0, "deferred": 0}
        units = 0
        for line in lines[starts[i] : starts[i + 1]]:
            if not line.startswith("|"):
                continue
            cells = [c.strip() for c in line.split("|")]
            if len(cells) < 5:
                continue
            # cells[0] is empty (leading pipe), cells[1] id, cells[2] text,
            # cells[3] status.
            status = cells[3]
            if status == DEFERRED_MARK:
                counts["done"] += 1
            elif status in STATUS:
                counts[STATUS[status]] += 1
            else:
                continue
            units += 1
        total["units"] += units
        for key in counts:
            total[key] += counts[key]
        print(
            "%-34s units=%d  done=%d partial=%d todo=%d deferred=%d"
            % (name, units, counts["done"], counts["partial"],
               counts["todo"], counts["deferred"])
        )

    print(
        "%-34s units=%d  done=%d partial=%d todo=%d deferred=%d"
        % ("合计", total["units"], total["done"], total["partial"],
           total["todo"], total["deferred"])
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
