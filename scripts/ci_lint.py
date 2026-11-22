#!/usr/bin/env python3
"""Clippy/rustfmt regression gate for `make check-lint`.

The tree carries a large amount of pre-existing clippy/rustfmt debt, so a
plain `-D warnings` gate would fail on every commit. Instead the current
per-crate/per-lint/per-file counts, unformatted files and stale generated
files are stored in tools/ci/lint-baseline.json; the gate fails only when a
*new* issue shows up. Refresh the baseline deliberately with
`make check-lint-update` after fixing debt or changing the Rust toolchain.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

from ci_common import (
    BASELINE, ROOT, cargo_env, dump_json, ensure_out,
    generated_drift, report, run,
)

TARGET = "x86_64-unknown-none"
CRATES = (
    ("apps", Path("apps/Cargo.toml"), ("--lib", "--bins")),
    ("novuslib-rs", Path("lib/novuslib-rs/Cargo.toml"), ("--lib",)),
)


def relative(path: Path) -> str:
    try:
        return str(path.resolve().relative_to(ROOT))
    except ValueError:
        return str(path)


def collect_clippy(cargo: str, env, lint_dir: Path) -> dict[str, int]:
    counts: dict[str, int] = {}
    for name, manifest, targets in CRATES:
        result = run(
            [cargo, "clippy", "--manifest-path", str(manifest), "--message-format=json",
             *targets, "--target", TARGET],
            env=env,
        )
        rendered: list[str] = []
        for line in result.stdout.splitlines():
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                message = json.loads(line)
            except json.JSONDecodeError:
                continue
            if message.get("reason") != "compiler-message":
                continue
            diagnostic = message.get("message", {})
            if diagnostic.get("level") not in ("warning", "error"):
                continue
            code = (diagnostic.get("code") or {}).get("code")
            if not code:
                continue
            spans = diagnostic.get("spans") or []
            primary = next((span for span in spans if span.get("is_primary")), None)
            if primary is None:
                location = "<no-span>"
            else:
                source = Path(primary["file_name"])
                if not source.is_absolute():
                    source = (ROOT / manifest).parent / source
                location = relative(source)
            key = f"{name}|{code}|{location}"
            counts[key] = counts.get(key, 0) + 1
            text = diagnostic.get("rendered")
            if text:
                rendered.append(text.rstrip())

        (lint_dir / f"clippy-{name}.txt").write_text(
            "\n".join(rendered) + "\n", encoding="utf-8"
        )
        if not rendered and result.returncode != 0:
            raise RuntimeError(
                f"clippy could not run for {name}: {(result.stderr or result.stdout).strip()}"
            )
    return counts


def collect_fmt(cargo: str, env, lint_dir: Path) -> list[str]:
    drift: set[str] = set()
    for name, manifest, _ in CRATES:
        result = run(
            [cargo, "fmt", "--manifest-path", str(manifest), "--all", "--", "--check"],
            env=env,
        )
        (lint_dir / f"fmt-{name}.txt").write_text(
            result.stdout + result.stderr, encoding="utf-8"
        )
        for match in re.findall(r"^Diff in (.+?):\d+:$", result.stdout, re.M):
            path = Path(match)
            drift.add(relative(path if path.is_absolute() else ROOT / path))
    return sorted(drift)


def rustc_version(env) -> str:
    result = run(["rustc", "--version"], env=env)
    return result.stdout.strip() or "unknown"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--update-baseline", action="store_true")
    args = parser.parse_args()

    ensure_out()
    lint_dir = ROOT / "out" / "ci" / "lint"
    lint_dir.mkdir(parents=True, exist_ok=True)
    try:
        cargo, env = cargo_env()
        clippy = collect_clippy(cargo, env, lint_dir)
        fmt = collect_fmt(cargo, env, lint_dir)
        generated = generated_drift()
        version = rustc_version(env)
    except Exception as exc:
        report(False, "clippy/fmt collection", f"{type(exc).__name__}: {exc}")
        return 1

    current = {
        "rustc": version,
        "target": TARGET,
        "clippy": clippy,
        "fmt": fmt,
        "generated": generated,
    }
    dump_json(lint_dir / "current.json", current)

    if args.update_baseline:
        BASELINE.parent.mkdir(parents=True, exist_ok=True)
        dump_json(BASELINE, current)
        print(f"baseline updated: {relative(BASELINE)} "
              f"({len(clippy)} diagnostics, {len(fmt)} unformatted files, "
              f"{len(generated)} stale generated files)")
        return 0

    if not BASELINE.is_file():
        report(False, "lint-baseline", f"missing {relative(BASELINE)}; run make check-lint-update")
        return 1

    baseline = json.loads(BASELINE.read_text(encoding="utf-8"))
    if baseline.get("rustc") != version:
        print(f"note: baseline was recorded with {baseline.get('rustc')}, current is {version}")

    old_clippy = baseline.get("clippy", {})
    new_diagnostics = sorted(
        ((key, count - old_clippy.get(key, 0)) for key, count in clippy.items()
         if count > old_clippy.get(key, 0)),
        key=lambda item: (-item[1], item[0]),
    )
    fixed_diagnostics = sum(
        max(0, old_clippy.get(key, 0) - count)
        for key, count in old_clippy.items()
        if count > clippy.get(key, 0)
    )
    old_fmt = set(baseline.get("fmt", []))
    new_fmt = sorted(set(fmt) - old_fmt)
    fixed_fmt = sorted(old_fmt - set(fmt))
    old_generated = set(baseline.get("generated", []))
    new_generated = sorted(set(generated) - old_generated)
    fixed_generated = sorted(old_generated - set(generated))

    ok = not new_diagnostics and not new_fmt and not new_generated
    detail = (f"{len(clippy)} clippy/rustc diagnostics, {len(fmt)} unformatted files, "
              f"{len(generated)} stale generated files")
    if fixed_diagnostics or fixed_fmt or fixed_generated:
        detail += (f" (fixed: {fixed_diagnostics} diagnostics, {len(fixed_fmt)} files, "
                   f"{len(fixed_generated)} generated)")
    report(ok, "clippy-fmt-baseline", detail if ok else "")

    for key, added in new_diagnostics[:25]:
        print(f"  NEW {key} (+{added})")
    if len(new_diagnostics) > 25:
        print(f"  ... and {len(new_diagnostics) - 25} more")
    for path in new_fmt[:25]:
        print(f"  NEW unformatted: {path}")
    if len(new_fmt) > 25:
        print(f"  ... and {len(new_fmt) - 25} more")
    for path in new_generated[:25]:
        print(f"  NEW stale generated file: {path}")

    dump_json(lint_dir / "regressions.json", {
        "ok": ok,
        "new_clippy": [{"key": key, "added": added} for key, added in new_diagnostics],
        "new_fmt": new_fmt,
        "new_generated": new_generated,
        "fixed_clippy": fixed_diagnostics,
        "fixed_fmt": fixed_fmt,
        "fixed_generated": fixed_generated,
    })
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
