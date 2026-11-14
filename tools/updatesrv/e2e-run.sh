#!/usr/bin/env bash
# Novus OS HTTPS end-to-end test (kernel-side, real vendor roots).
#
# Boots the kernel with E2E=1 under QEMU user-mode networking and runs the
# in-kernel one-shot HTTPS probe against the reference update server.
#
# Trust anchors in the E2E image:
#   * the real vendor root set in security/novu/cabundle.c, generated from
#     tools/cabundle/roots.pem (Debian/Mozilla + curl CA bundle + ICANN +
#     ISRG; count is read dynamically below) - the production trust set
#   * 1 QEMU test anchor (security/novu/cabundle_e2e.c, E2E=1 only)
# The probe connects to 10.0.2.2:8443; updatesrv presents server.pem signed
# by the test CA. Multi-anchor chain validation must select the right root by
# issuer/subject Name, which is exactly what this test exercises.
#
# All assets are in the repository. No certificate generation, no network.
# The probe waits for the NCSI connectivity check to finish before using the
# single TCP session, so allow enough wall time (default 90 s).
# Usage: tools/updatesrv/e2e-run.sh [SECONDS]
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO"
export PATH=/home/dev/.rustup/toolchains/stable-x86_64-unknown-linux-gnu/bin:/home/dev/.cargo/bin:/usr/bin:/bin
LOGDIR="${NOVUS_E2E_LOGDIR:-/tmp/novus-e2e}"
SECONDS="${1:-90}"
mkdir -p "$LOGDIR/pkgs"

# Verify the checked-in PKI before spending time on a build.
openssl verify -CAfile tools/updatesrv/test_ca.pem tools/updatesrv/server.pem >/dev/null
openssl x509 -in tools/updatesrv/server.pem -noout -ext subjectAltName \
  | grep -q "10.0.2.2"
test -s tools/cabundle/roots.pem

ROOT_COUNT=$(grep -c -- "-----BEGIN CERTIFICATE-----" tools/cabundle/roots.pem)
echo "[e2e] production vendor roots: $ROOT_COUNT (+1 QEMU test anchor, E2E only)"

# One make invocation, E2E=1 on EVERY target. A separate `make iso` would
# re-evaluate CFLAGS with E2E=0 and (via build/.cflags) recompile objects
# without the probe/anchor, silently baking a production EFI into the ISO.
make E2E=1 iso >"$LOGDIR/build.log" 2>&1

# Boot the live ISO (the validated path: El Torito exposes a proper FAT ESP
# with SYSTEM.IMG/ESP.IMG, so the live root is mounted). A vvfat directory
# presents as a 504 MiB disk whose FAT Novus' built-in reader cannot always
# parse, which silently drops the live root and stalls boot.
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$LOGDIR/OVMF_VARS.fd"

python3 tools/updatesrv/updatesrv.py --https 8443 \
  --cert tools/updatesrv/server.pem --key tools/updatesrv/server.key \
  --dir "$LOGDIR/pkgs" --quiet >"$LOGDIR/srv.log" 2>&1 &
SRV=$!
sleep 1

rm -f "$LOGDIR/serial.log"
qemu-system-x86_64 -machine q35 -smp 2 -m 4G \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file="$LOGDIR/OVMF_VARS.fd" \
  -cdrom out/novus-live.iso \
  -serial file:"$LOGDIR/serial.log" -no-reboot -display none \
  -netdev user,id=novusnet -device e1000,netdev=novusnet &
QEMU=$!

sleep "$SECONDS"
kill "$QEMU" 2>/dev/null || true
kill "$SRV" 2>/dev/null || true

SERIAL="$LOGDIR/serial.log"
echo "=== cabundle / anchor ==="
grep -a "\[NET\] https\|\[E2E\] QEMU test anchor" "$SERIAL" || true
echo "=== [E2E] probe ==="
grep -a "\[E2E\]" "$SERIAL" || echo "NO E2E MARKER"

RC=0
# Assert the positive evidence, not merely the absence of a FAIL line.
grep -q "vendor roots=" "$SERIAL" || { echo "ASSERT FAIL: vendor root set not loaded"; RC=1; }
grep -q "QEMU test anchor added" "$SERIAL" || { echo "ASSERT FAIL: E2E anchor missing"; RC=1; }
grep -q "\[E2E\] OK" "$SERIAL" || { echo "ASSERT FAIL: HTTPS probe did not succeed"; RC=1; }
if grep -aq "cert chain verify FAIL\|chain FAIL\|\[E2E\] FAIL\|\[TLS13\].*FAIL" "$SERIAL"; then
  echo "ASSERT FAIL: TLS/chain error in log"
  grep -a "cert chain verify FAIL\|chain FAIL\|\[E2E\] FAIL\|\[TLS13\].*FAIL" "$SERIAL"
  RC=1
fi
if [ "$RC" -eq 0 ]; then
  echo "E2E PASS: real vendor roots + isolated test anchor + HTTPS GET"
else
  echo "E2E FAILED"
fi
exit "$RC"
