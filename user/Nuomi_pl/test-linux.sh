#!/bin/sh
# Nuomi + NUC toolchain - Linux smoke test (needs gcc/cc + make).
# Verifies: full build via Makefile, all interpreter samples, NUC pipeline,
# and Route-A compile round-trip (nuomi -c -> nucutil run) which now uses mmap on Linux.
set -e
cd "$(dirname "$0")"

echo "[build] make"
make

echo "[1] interpreter samples"
for f in samples/*.nuomi; do
  if ./nuomi "$f" >/dev/null; then echo "  ok   $f"; else echo "  FAIL $f"; exit 1; fi
done

echo "[2] NUC emit-demo / validate / run (expect entry 0x2A=42)"
./nucutil emit-demo /tmp/nuomi-demo.nuc >/dev/null
./nucutil validate /tmp/nuomi-demo.nuc
./nucutil run /tmp/nuomi-demo.nuc

echo "[3] Route-A compile round-trip (nuomi -c -> nucutil run, expect 142)"
./nuomi -c samples/compile-demo.nuomi -o /tmp/cd.nuc && ./nucutil run /tmp/cd.nuc

rm -f /tmp/nuomi-demo.nuc /tmp/cd.nuc
echo "LINUX TEST: ALL OK"
