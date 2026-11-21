#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-$ROOT/out/novusd}"
SRC="/mnt/c/Users/陈信/AppData/Local/Temp/codebuddy-dropped-files/7b17c74e-9e4c-4fc2-9293-37006c114d20/eicar.com"
HASH="275a021bbfb6489e54d471899f7db9d1663fc695ec2fe2a2c4538aabf651fd0f"
WORK="/tmp/nd_dropped_flow_$$"

if [ ! -f "$SRC" ]; then
  echo "SKIP: dropped EICAR sample not found"
  exit 0
fi

mkdir -p "$WORK"
cp "$SRC" "$WORK/eicar.com"
rm -f /var/lib/novus/sigdb.bin 2>/dev/null || true

"$BIN" scan "$WORK" --quarantine | tee "$WORK/report.txt"
grep -q 'MALICIOUS' "$WORK/report.txt"
test ! -e "$WORK/eicar.com" || { echo "FAIL: detected file was not removed"; exit 1; }

"$BIN" quarantine list | grep -q "$HASH"
"$BIN" quarantine restore "$HASH"
test -f "$WORK/eicar.com" || { echo "FAIL: restore did not recreate the file"; exit 1; }

"$BIN" scan "$WORK" --quarantine >/dev/null
"$BIN" quarantine delete "$HASH"
test ! -e "$WORK/eicar.com" || { echo "FAIL: deleted file still present"; exit 1; }

rm -rf "$WORK"
echo "OK: dropped EICAR found, deleted, restored and deleted again"
