#!/usr/bin/env python3
"""Sequential stage runner behind `make pipeline`.

Runs the independent make targets in a fixed order, tees each stage's output
to out/ci/stages/<stage>.log and writes a small report even when a stage
fails, so the failing step and its duration are obvious.
"""

from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path

from ci_common import ROOT, dump_json, ensure_out

STAGES = (
    ("check", ("check",)),
    ("check-lint", ("check-lint",)),
    ("build", ("out/novus-live.iso",)),
    ("smoke", ("smoke",)),
)


def run_stage(name: str, targets, log_dir: Path, make: str) -> dict:
    log_path = log_dir / f"{name}.log"
    command = [make, *targets]
    print(f"\n=== {name}: {' '.join(command)} ===", flush=True)
    started = time.monotonic()
    with log_path.open("w", encoding="utf-8") as log:
        process = subprocess.Popen(
            command, cwd=str(ROOT), stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1,
        )
        assert process.stdout is not None
        for line in process.stdout:
            log.write(line)
            print(line, end="", flush=True)
        returncode = process.wait()
    return {
        "stage": name,
        "status": "PASS" if returncode == 0 else "FAIL",
        "returncode": returncode,
        "duration_s": round(time.monotonic() - started, 2),
        "log": str(log_path.relative_to(ROOT)),
    }


def main() -> int:
    ensure_out()
    stages_dir = ROOT / "out" / "ci" / "stages"
    stages_dir.mkdir(parents=True, exist_ok=True)
    make = os.environ.get("MAKE", "make")
    records = []

    for name, targets in STAGES:
        record = run_stage(name, targets, stages_dir, make)
        records.append(record)
        dump_json(ROOT / "out" / "ci" / "report.json", {
            "status": "PASS" if all(r["status"] == "PASS" for r in records) else "FAIL",
            "stages": records,
        })
        if record["status"] != "PASS":
            break

    total = round(sum(item["duration_s"] for item in records), 2)
    lines = [f"{item['stage']:<14} {item['status']:<5} {item['duration_s']:>8.2f}s  {item['log']}"
             for item in records]
    summary = "\n".join(lines) + f"\ntotal            {total:.2f}s\n"
    (ROOT / "out" / "ci" / "report.txt").write_text(summary, encoding="utf-8")
    print("\n=== pipeline summary ===")
    print(summary, end="")
    return 0 if all(item["status"] == "PASS" for item in records) and len(records) == len(STAGES) else 1


if __name__ == "__main__":
    raise SystemExit(main())
