#!/usr/bin/env bash
#
# Novus OS - M8 application churn test
#
# Starts the machine headless, then repeatedly closes and relaunches the
# built-in applications through the taskbar. This is the sequence that used
# to corrupt user address spaces (each spawn cloned the *running* task's
# page tables) and then halt the scheduler when the last window closed.
#
# It asserts, from the serial log alone:
#   - no "#PF" beyond the two boot-time NX-probe faults,
#   - no "no runnable task" and no "[PMM] WARN double free",
#   - "[SCHED] ticks" keeps advancing while the desktop is idle,
#   - a window closed on purpose really does exit its process.
#
# Usage: scripts/wm-churn.sh [MEM] [ROUNDS] [--iso]
#   MEM     guest memory, defaults to 4G
#   ROUNDS  close/relaunch cycles, defaults to 20
#   --iso   boot out/novus-live.iso instead of the ESP directory

set -euo pipefail

cd "$(dirname "$0")/.."

MEM="4G"
ROUNDS=20
USE_ISO=0

for arg in "$@"; do
    case "$arg" in
    --iso) USE_ISO=1 ;;
    [0-9]*G|[0-9]*M) MEM="$arg" ;;
    [0-9]*) ROUNDS="$arg" ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

MON=/tmp/novus-churn.mon
LOG=/tmp/novus-churn.serial
QEMU_PID=

cleanup() {
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null || true
    rm -f "$MON"
}
trap cleanup EXIT INT TERM

fail() {
    echo "FAIL: $*" >&2
    echo "--- serial tail ---" >&2
    tail -20 "$LOG" >&2 || true
    exit 1
}

[ -x /usr/bin/socat ] || command -v socat >/dev/null || {
    echo "socat is required" >&2
    exit 3
}

rm -f "$MON" "$LOG"

if [ "$USE_ISO" = 1 ]; then
    [ -f out/novus-live.iso ] || { echo "out/novus-live.iso missing: run make iso" >&2; exit 3; }
    BOOT_ARGS=(-cdrom out/novus-live.iso)
else
    [ -f out/NOVUS.EFI ] || { echo "out/NOVUS.EFI missing: run make" >&2; exit 3; }
    mkdir -p esp/EFI/BOOT
    cp out/NOVUS.EFI esp/EFI/BOOT/BOOTX64.EFI
    BOOT_ARGS=(-drive "format=raw,file=fat:rw:esp")
fi

[ -f out/OVMF_VARS.fd ] || cp /usr/share/OVMF/OVMF_VARS_4M.fd out/OVMF_VARS.fd

qemu-system-x86_64 -machine q35 -smp 2 -m "$MEM" \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -drive if=pflash,format=raw,file=out/OVMF_VARS.fd \
    "${BOOT_ARGS[@]}" \
    -serial "file:$LOG" -no-reboot -display none \
    -monitor "unix:$MON,server,nowait" -net none &
QEMU_PID=$!

hmp() {
    printf '%s\n' "$1" | socat - "UNIX-CONNECT:$MON" >/dev/null 2>&1 || true
    sleep 0.18
}

wait_for() {
    local what="$1" limit="$2" i
    for i in $(seq 1 "$limit"); do
        grep -qF "$what" "$LOG" 2>/dev/null && return 0
        sleep 0.5
    done
    return 1
}

# The pointer starts where nd_demo_init() parked it.
CUR_X=640
CUR_Y=266

move_to() {
    local tx=$1 ty=$2 dx dy sx sy

    dx=$((tx - CUR_X))
    dy=$((ty - CUR_Y))
    while [ "$dx" -ne 0 ] || [ "$dy" -ne 0 ]; do
        if [ "$dx" -gt 120 ]; then sx=120; elif [ "$dx" -lt -120 ]; then sx=-120; else sx=$dx; fi
        if [ "$dy" -gt 120 ]; then sy=120; elif [ "$dy" -lt -120 ]; then sy=-120; else sy=$dy; fi
        hmp "mouse_move $sx $sy"
        dx=$((dx - sx))
        dy=$((dy - sy))
    done
    CUR_X=$tx
    CUR_Y=$ty
}

click_at() {
    move_to "$1" "$2"
    hmp "mouse_button 1"
    hmp "mouse_button 0"
}

# Window frames from the M8 sources: fileman(40,396,418,329),
# taskman(470,430,428,311), settings(905,500,358,275). The close box sits
# 18 px in from the right edge, 12 px down the title bar.
close_fileman() { click_at $((40 + 418 - 18)) $((396 + 12)); }
taskbar_files() { click_at 136 17; }
taskbar_tasks() { click_at 236 17; }

wait_for "demo desktop painted" 240 || fail "the desktop never came up"
wait_for "[APP] settings up" 40 || fail "the M8 applications did not start"
sleep 2

echo "churn: $ROUNDS rounds"
for i in $(seq 1 "$ROUNDS"); do
    close_fileman
    sleep 0.4
    taskbar_files
    sleep 0.7
    if grep -q "no runnable task" "$LOG"; then
        fail "scheduler parked at round $i"
    fi
done

# Closing every application must leave a live desktop, and the taskbar has
# to be able to start one again.
close_fileman
sleep 0.4
click_at $((470 + 428 - 18)) $((430 + 12))   # close taskman
sleep 0.4
click_at $((905 + 358 - 18)) $((500 + 12))   # close settings
sleep 0.6
taskbar_tasks
sleep 1.2

# The serial log is written a character at a time, so the last line can be
# caught half-finished. Sample it again a moment later instead of trusting
# one read, and compare as hexadecimal.
last_ticks() {
    grep -o "ticks=0x[0-9A-F][0-9A-F]*" "$LOG" | tail -1 | sed 's/ticks=0x//'
}

ticks_a=$(last_ticks)
[ -n "$ticks_a" ] || fail "no scheduler ticks in the log"

ticking=0
ticks_b=$ticks_a
for _ in 1 2 3; do
    sleep 1
    ticks_b=$(last_ticks)
    if [ -n "$ticks_b" ] && [ $((16#$ticks_b)) -gt $((16#$ticks_a)) ]; then
        ticking=1
        break
    fi
done
[ "$ticking" = 1 ] || fail "scheduler stopped at tick $((16#$ticks_a))"

grep -q "no runnable task" "$LOG" && fail "scheduler reported no runnable task"
grep -q "double free" "$LOG" && fail "double free reported"
grep -q "refusing user mapping" "$LOG" && fail "kernel-half mapping attempted"

faults=$(grep -c "#PF" "$LOG" || true)
[ "$faults" -le 2 ] || fail "unexpected user faults: $faults (expected the 2 NX-probe ones)"

exits=$(grep -c "task fileman exited" "$LOG" || true)
[ "$exits" -ge $((ROUNDS + 1)) ] || fail "only $exits fileman exits for $ROUNDS rounds"

echo "PASS: $ROUNDS rounds, $exits clean exits, ticks $((16#$ticks_a)) -> $((16#$ticks_b)), faults=$faults"
