"""Shared helpers for the Novus CI make targets.

Standard library only. Every script in this directory can import it directly
because Python puts the script's own directory on sys.path.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CI_OUT = ROOT / "out" / "ci"
BASELINE = ROOT / "tools" / "ci" / "lint-baseline.json"
DEV_HOME = Path("/home/dev")

GENERATORS = ("tools/themegen/themegen.py", "tools/langgen/langgen.py")


def ensure_out() -> None:
    CI_OUT.mkdir(parents=True, exist_ok=True)


def cargo_env() -> tuple[str, dict[str, str]]:
    """Return (cargo path, environment) with the Rust bin dir on PATH."""
    env = os.environ.copy()
    cargo = None
    for candidate in (DEV_HOME / ".cargo" / "bin" / "cargo",
                      Path.home() / ".cargo" / "bin" / "cargo"):
        if candidate.is_file():
            cargo = str(candidate)
            break
    if cargo is None:
        cargo = shutil.which("cargo")
    if cargo is None:
        raise RuntimeError("cargo not found: add ~/.cargo/bin to PATH")

    bindir = str(Path(cargo).resolve().parent)
    env["PATH"] = bindir + os.pathsep + env.get("PATH", "")
    for name, preferred, fallback in (
        ("CARGO_HOME", DEV_HOME / ".cargo", Path.home() / ".cargo"),
        ("RUSTUP_HOME", DEV_HOME / ".rustup", Path.home() / ".rustup"),
    ):
        if not env.get(name):
            env[name] = str(preferred if preferred.is_dir() else fallback)
    return cargo, env


def generated_drift() -> list[str]:
    """Return generated files reported stale by the in-tree generators."""
    drift: set[str] = set()
    for generator in GENERATORS:
        result = run([sys.executable, generator, "--check"])
        stale = [
            line.split(":", 1)[1].strip()
            for line in (result.stdout + result.stderr).splitlines()
            if line.strip().startswith("stale:")
        ]
        if stale:
            drift.update(stale)
        elif result.returncode != 0:
            drift.add(f"{generator}: exited {result.returncode}")
    return sorted(drift)


def run(cmd, *, cwd: Path = ROOT, env=None, check: bool = False) -> subprocess.CompletedProcess:
    result = subprocess.run(
        [str(part) for part in cmd],
        cwd=str(cwd),
        env=env,
        text=True,
        capture_output=True,
    )
    if check and result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()
        raise RuntimeError(f"command failed: {' '.join(str(p) for p in cmd)}\n{detail}")
    return result


def dump_json(path: Path, data) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )


def report(ok: bool, name: str, detail: str = "") -> None:
    tag = "PASS" if ok else "FAIL"
    line = f"[{tag}] {name}"
    if detail:
        line += f" - {detail}"
    print(line, flush=True)
