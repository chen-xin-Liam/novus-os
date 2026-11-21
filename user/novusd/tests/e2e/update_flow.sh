#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-$ROOT/out/novusd}"
PKG="/tmp/nd_update_$$.bin"

"$BIN" make-update "$PKG" 2
"$BIN" update "$PKG" | tee /tmp/nd_update_report_$$.txt
grep -q 'version=2' /tmp/nd_update_report_$$.txt
"$BIN" status

rm -f "$PKG" /tmp/nd_update_report_$$.txt
echo "OK: incremental update flow"
