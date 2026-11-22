#!/usr/bin/env python3
"""Fast hard-gating source checks run by `make check`.

Covers things that are cheap and unambiguous: Python/shell syntax, generated
file drift, obvious repository hygiene problems and a real `cargo check` of
both freestanding Rust crates. Style debt (clippy/rustfmt) is handled by
ci_lint.py with a baseline so existing warnings do not block every change.
"""

from __future__ import annotations

import ast
import json
from pathlib import Path

from ci_common import (
    BASELINE, ROOT, cargo_env, dump_json, ensure_out,
    generated_drift, report, run,
)

SKIP_DIRS = {".git", "build", "out", "esp", "target", ".codebuddy", ".vscode", ".trae"}
MAX_TRACKED_BYTES = 10 * 1024 * 1024


def tracked_files() -> list[Path] | None:
    result = run(["git", "ls-files", "-z"])
    if result.returncode != 0:
        return None
    return [Path(item) for item in result.stdout.split("\0") if item]


def source_files(files: list[Path], suffix: str | None = None) -> list[Path]:
    chosen = []
    for path in files:
        if suffix and path.suffix != suffix:
            continue
        if any(part in SKIP_DIRS for part in path.parts):
            continue
        chosen.append(path)
    return chosen


def check_python(files: list[Path]) -> tuple[bool, str]:
    broken = []
    for path in source_files(files, ".py"):
        try:
            ast.parse(path.read_bytes(), filename=str(path))
        except (SyntaxError, UnicodeDecodeError) as exc:
            broken.append(f"{path}: {exc}")
    ok = not broken
    return ok, f"{len(source_files(files, '.py'))} files" if ok else "; ".join(broken[:5])


def check_shell(files: list[Path]) -> tuple[bool, str]:
    broken = []
    scripts = source_files(files, ".sh")
    for path in scripts:
        result = run(["bash", "-n", str(path)])
        if result.returncode != 0:
            broken.append(f"{path}: {result.stderr.strip().splitlines()[0]}")
    return not broken, f"{len(scripts)} files" if not broken else "; ".join(broken[:5])


def check_generated() -> tuple[bool, str]:
    current = generated_drift()
    if not BASELINE.is_file():
        ok = not current
        return ok, "baseline missing; run make check-lint-update" if not ok else "theme + language tables current"
    known = set(json.loads(BASELINE.read_text(encoding="utf-8")).get("generated", []))
    new = sorted(set(current) - known)
    fixed = sorted(known - set(current))
    if new:
        return False, "new drift: " + ", ".join(new)
    detail = "theme + language tables current"
    if fixed:
        detail += f" (fixed: {', '.join(fixed)})"
    if current:
        detail += f" ({len(current)} known stale file(s) baselined)"
    return True, detail


def check_hygiene(files: list[Path]) -> tuple[bool, str]:
    problems: list[str] = []
    for path in files:
        if any(part in SKIP_DIRS for part in path.parts) or not path.is_file():
            continue
        try:
            raw = path.read_bytes()
        except OSError as exc:
            problems.append(f"{path}: unreadable ({exc})")
            continue

        name = path.name.lower()
        if name.endswith((".key", ".p12", ".pfx", ".p8")) or name.startswith("id_rsa"):
            problems.append(f"{path}: private key material")
        elif name.endswith(".pem") and b"PRIVATE KEY-----" in raw:
            problems.append(f"{path}: PEM contains a private key")
        if len(raw) > MAX_TRACKED_BYTES:
            problems.append(f"{path}: {len(raw)} bytes exceeds the {MAX_TRACKED_BYTES} limit")
        if b"\0" in raw:
            continue
        text = raw.decode("utf-8", errors="replace")
        if "<<<<<<<" in text or ">>>>>>>" in text:
            problems.append(f"{path}: unresolved merge conflict marker")
        if path.suffix == ".sh" and b"\r\n" in raw:
            problems.append(f"{path}: CRLF line endings in a shell script")
    return not problems, "no keys, oversized blobs or conflict markers" if not problems else "; ".join(problems[:8])


RUST_CRATES = (
    ("apps", Path("apps/Cargo.toml"), ("--lib", "--bins")),
    ("novuslib-rs", Path("lib/novuslib-rs/Cargo.toml"), ("--lib",)),
)


def check_rust() -> tuple[bool, str]:
    cargo, env = cargo_env()
    failures = []
    for name, manifest, targets in RUST_CRATES:
        result = run(
            [cargo, "check", "--manifest-path", str(manifest), *targets,
             "--target", "x86_64-unknown-none"],
            env=env,
        )
        if result.returncode != 0:
            failures.append((result.stderr + result.stdout).strip()[-1200:])
    return not failures, "apps + novuslib-rs compile for x86_64-unknown-none" if not failures else " | ".join(failures)


def main() -> int:
    ensure_out()
    files = tracked_files()
    if files is None:
        report(False, "git", "cannot list tracked files")
        return 1

    results = []
    for name, func in (
        ("python-syntax", lambda: check_python(files)),
        ("shell-syntax", lambda: check_shell(files)),
        ("generated-drift", check_generated),
        ("repo-hygiene", lambda: check_hygiene(files)),
        ("rust-check", check_rust),
    ):
        try:
            ok, detail = func()
        except Exception as exc:  # keep the other checks useful
            ok, detail = False, f"{type(exc).__name__}: {exc}"
        results.append({"name": name, "ok": ok, "detail": detail})
        report(ok, name, detail if not ok else "")

    dump_json(ROOT / "out" / "ci" / "check.json", {"results": results})
    failed = [item["name"] for item in results if not item["ok"]]
    print(f"check: {len(results) - len(failed)}/{len(results)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
