#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
cd "$ROOT"
BIN=$(find target/x86_64-unknown-none -maxdepth 3 -type f -name novusd 2>/dev/null | head -n1 || true)
if [[ -z "$BIN" ]]; then
  echo "No Novus app artifact found (skeleton build is allowed to skip this check)."
  exit 0
fi
SIZE=$(stat -c %s "$BIN")
if [[ "$SIZE" -ge 131072 ]]; then
  echo "FAIL: image $SIZE bytes exceeds 128 KiB budget"; exit 1
fi
echo "OK: image size $SIZE bytes < 128 KiB"
