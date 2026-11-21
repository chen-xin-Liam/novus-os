#!/usr/bin/env bash
# Shared environment setup. Usage: source scripts/env.sh [check]
set -euo pipefail

export CARGO_HOME="${CARGO_HOME:-/home/dev/.cargo}"
export RUSTUP_HOME="${RUSTUP_HOME:-/home/dev/.rustup}"
export PATH="$CARGO_HOME/bin:$PATH"

check() {
  command -v gcc >/dev/null || { echo "gcc missing"; return 1; }
  command -v ar >/dev/null || { echo "ar missing"; return 1; }
  command -v cargo >/dev/null || { echo "cargo missing"; return 1; }
  local v
  v="$(rustc --version)"
  case "$v" in
    rustc\ 1.99.*) ;;
    *) echo "WARNING: expected rustc 1.99.x, got: $v" ;;
  esac
}

if [[ "${1:-}" == "check" ]]; then
  check
  echo "env ok: $(rustc --version)"
fi
