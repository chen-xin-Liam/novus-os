#!/usr/bin/env bash
set -euo pipefail
BIN="${1:?usage: check-static.sh <binary>}"
OUT="$(ldd "$BIN" 2>&1 || true)"
if echo "$OUT" | grep -q 'statically linked'; then
  echo "OK: static binary confirmed"
  exit 0
fi
if command -v readelf >/dev/null 2>&1 && readelf -l "$BIN" | grep -q 'INTERP'; then
  echo "FAIL: dynamic interpreter present"; exit 1
fi
if echo "$OUT" | grep -q '=> /'; then
  echo "FAIL: dynamic dependencies present"; echo "$OUT"; exit 1
fi
echo "OK: no dynamic dependencies detected"
