#!/usr/bin/env bash
# Regenerate the QEMU-only test PKI (OPTIONAL maintenance helper).
#
# The generated assets are checked into tools/updatesrv/ and
# security/novu/cabundle_e2e.c, so a normal E2E run needs NO generation step
# and NO network access. Run this only when rotating the test CA.
#
# IMPORTANT: this script NEVER touches security/novu/cabundle.c. That file is
# the production trust set (real vendor roots, see roots.pem for the current
# count) generated from tools/cabundle/roots.pem. Only cabundle_e2e.c - linked exclusively into
# E2E=1 builds - is rewritten here.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
DST="$REPO/tools/updatesrv"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

# Self-signed test CA (CA:TRUE, RSA-PKCS1-SHA256).
openssl genrsa -out ca.key 2048 2>/dev/null
openssl req -x509 -new -key ca.key -days 3650 -subj "/CN=NovusTestCA" \
  -addext "basicConstraints=critical,CA:TRUE" \
  -addext "keyUsage=critical,keyCertSign,cRLSign" -out ca.pem 2>/dev/null

# Server certificate signed by the test CA. SAN MUST contain the QEMU
# user-net gateway 10.0.2.2: the kernel verifies an iPAddress SAN.
openssl genrsa -out server.key 2048 2>/dev/null
openssl req -new -key server.key -subj "/CN=updatesrv" -out server.csr 2>/dev/null
cat > san.cnf <<'EOF'
[svr]
subjectAltName=IP:10.0.2.2
basicConstraints=CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=serverAuth
EOF
openssl x509 -req -in server.csr -CA ca.pem -CAkey ca.key -CAcreateserial \
  -days 3650 -sha256 -extfile san.cnf -extensions svr -out server.pem 2>/dev/null

openssl verify -CAfile ca.pem server.pem >/dev/null
openssl x509 -in server.pem -noout -ext subjectAltName | grep -q "10.0.2.2"

cp ca.pem      "$DST/test_ca.pem"
cp ca.key      "$DST/test_ca.key"
cp server.pem  "$DST/server.pem"
cp server.key  "$DST/server.key"
chmod 600 "$DST/test_ca.key" "$DST/server.key"

python3 "$REPO/tools/cabundle/cabundle.py" --pem "$DST/test_ca.pem" \
  --prefix novus_e2e_root --out "$REPO/security/novu/cabundle_e2e.c"

echo "E2E test PKI rotated. Production cabundle.c is unchanged."
