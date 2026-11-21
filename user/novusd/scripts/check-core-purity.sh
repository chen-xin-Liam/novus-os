#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BAD=0
scan() {
  local dir="$1"
  [[ -d "$dir" ]] || return 0
  if grep -RInE '#include[[:space:]]*<(linux/|sys/|unistd|fcntl|pthread|asm/|bpf/)|fanotify|inotify|ptrace|seccomp|prctl|/proc/|std::os::unix|libc::' "$dir"; then
    echo "FAIL: platform-dependent reference in $dir"; BAD=1
  fi
}
scan c/core
scan rust/nd-core/src
scan rust/nd-platform-api/src
if [[ $BAD -ne 0 ]]; then exit 1; fi
echo "OK: core is free of platform dependencies"
