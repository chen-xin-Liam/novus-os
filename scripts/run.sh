#!/usr/bin/env bash
#
# Novus OS - QEMU runner
#
# Boots the UEFI image (out/NOVUS.EFI) under QEMU with OVMF firmware and
# wires the guest serial port (COM1) to this terminal's stdio. The guest
# gets two vCPUs for the AB dual-core SMP boot (README part 2): vCPU 0 is
# the A core (BSP), vCPU 1 the B core (AP).
#
# Usage: scripts/run.sh [MEM]    (guest memory size, defaults to 4G)

set -euo pipefail

MEM="${1:-4G}"

# Always run from the repository root so relative paths are stable.
cd "$(dirname "$0")/.."

# Lay out the ESP and make sure a built image exists.
mkdir -p esp/EFI/BOOT out
if [ ! -f out/NOVUS.EFI ]; then
    echo "out/NOVUS.EFI not found. Run 'make' first." >&2
    exit 1
fi
cp out/NOVUS.EFI esp/EFI/BOOT/BOOTX64.EFI

# Stage the wallpaper package on the ESP so the live desktop shows a photo
# rather than a flat colour. The boot stage reads WPAPERS.PKG off the ESP
# before ExitBootServices; without it the loader falls back to a solid fill.
if [ -f out/wallpapers.pkg ]; then
    cp out/wallpapers.pkg esp/WPAPERS.PKG
else
    echo "warning: out/wallpapers.pkg not built; desktop will be a flat colour" >&2
fi

# Copy pristine OVMF non-volatile variables on first use only, so boot
# entries and firmware state persist across runs.
if [ ! -f out/OVMF_VARS.fd ]; then
    cp /usr/share/OVMF/OVMF_VARS_4M.fd out/OVMF_VARS.fd
fi

# Show a window when a display is available; otherwise run headless.
# Serial output on stdio is fully functional either way.
if [ -n "${DISPLAY:-}" ]; then
    DISPLAY_ARGS="-display gtk"
else
    DISPLAY_ARGS="-display none"
fi

# The vvfat drive is mounted read-write so the EFI application can
# persist state on the ESP if it wants to.
#
# Networking: QEMU's user-mode stack gives the guest DHCP (10.0.2.2), DNS
# (10.0.2.3) and NAT to the real internet, which is what the NCSI probe in
# kernel/net_ncsi.c needs to reach ncsi.novus-os.ryzn.pro.
#
# NIC models the kernel drives: e1000, e1000e, virtio-net-pci, rtl8139.
# Pick one with NOVUS_NIC, e.g.  NOVUS_NIC=virtio-net-pci make run
NIC="${NOVUS_NIC:-e1000}"
exec qemu-system-x86_64 -machine q35 -smp 2 -m "$MEM" \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -drive if=pflash,format=raw,file=out/OVMF_VARS.fd \
    -drive format=raw,file=fat:rw:esp \
    -serial stdio -no-reboot $DISPLAY_ARGS \
    -netdev user,id=novusnet -device "$NIC",netdev=novusnet
