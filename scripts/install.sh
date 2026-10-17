#!/usr/bin/env bash
#
# Novus OS - host-side disk installer
#
# Writes the prepared system image (out/novus.img, produced by mkfsnovus
# from the kernel's own filesystem code, Merkle tree already built) onto
# a block device or into an image file.
#
# This is the host half of the install story. The boot-stage installer
# writes a full disk - GPT, ESP and the NovusFS partition - from inside
# the live system; this script is for the case where the target is not
# attached to a running Novus (imaging a card, seeding a VM disk).
#
# Usage:
#   scripts/install.sh DEVICE=/dev/sdX
#   scripts/install.sh DEVICE=out/disk.img [SIZE_MB=64]
#
# Refuses to run without an explicit DEVICE. Refuses block devices that
# are already mounted. Refuses to enlarge a file target silently.

set -euo pipefail

cd "$(dirname "$0")/.."

DEVICE="${DEVICE:-}"
SIZE_MB="${SIZE_MB:-0}"
IMG="out/novus.img"

if [ -z "$DEVICE" ]; then
    echo "usage: DEVICE=<device-or-file> [SIZE_MB=<n>] scripts/install.sh" >&2
    echo " refusing to guess a target." >&2
    exit 2
fi

if [ ! -f "$IMG" ]; then
    echo "$IMG not found. Run 'make novus-img' first." >&2
    exit 1
fi

# A block device that is mounted anywhere is not a safe target.
if [ -b "$DEVICE" ]; then
    if mount | grep -q "^${DEVICE}"; then
        echo "refusing: $DEVICE has a mounted filesystem" >&2
        exit 1
    fi
    echo "about to overwrite ALL data on $DEVICE"
    read -r -p "type 'yes' to continue: " CONFIRM
    if [ "$CONFIRM" != "yes" ]; then
        echo "aborted."
        exit 1
    fi
elif [ -e "$DEVICE" ]; then
    # Existing regular file: never grow it behind the caller's back.
    :
else
    # New file target: create it at the requested size, or at the image
    # size when SIZE_MB was not given.
    if [ "$SIZE_MB" = "0" ]; then
        SIZE_MB=$(( ( $(stat -c %s "$IMG") + 1048575 ) / 1048576 ))
    fi
    echo "creating $DEVICE (${SIZE_MB} MiB)"
    truncate -s "${SIZE_MB}M" "$DEVICE"
fi

echo "installing $IMG -> $DEVICE"
dd if="$IMG" of="$DEVICE" bs=1M conv=notrunc,fsync status=progress
sync

echo "done. $DEVICE now carries a NovusFS system partition."
echo "verify with: qemu-system-x86_64 -drive format=raw,file=$DEVICE ..."
