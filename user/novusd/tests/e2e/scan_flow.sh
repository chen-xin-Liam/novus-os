#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-$ROOT/out/novusd}"
WORK="/tmp/nd_scan_flow_$$"
mkdir -p "$WORK"
rm -f /var/lib/novus/sigdb.bin 2>/dev/null || true

printf '%s' 'X5O!P%@AP[4\PZX54(P^)7CC)7}$EICAR-STANDARD-ANTIVIRUS-TEST-FILE!$H+H*' > "$WORK/eicar.txt"
printf '%s' 'this is a harmless sample' > "$WORK/benign.txt"

"$BIN" selftest
"$BIN" scan "$WORK" --quarantine | tee "$WORK/report.txt"
grep -q 'MALICIOUS' "$WORK/report.txt"
grep -q 'CLEAN' "$WORK/report.txt"
"$BIN" quarantine list

rm -rf "$WORK"
echo "OK: scan -> detect -> quarantine flow"
