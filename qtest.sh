#!/bin/bash
set -e
cd /home/dev/novus-os/novus-os
export PATH="$HOME/.cargo/bin:$PATH"
qemu-img create -f raw /tmp/disk.img 64M >/dev/null 2>&1
OVMF=/usr/share/OVMF
CODE=$OVMF/OVMF_CODE_4M.fd
VARS=/tmp/ovmf.fd
# OVMF persists BootOrder in the VARS file; restart from a clean one each run
# so a failed boot never poisons the next test.
cp "$OVMF/OVMF_VARS_4M.fd" "$VARS"

run() {
  local mc="$1" log="$2" extra="$3"
  timeout 75 qemu-system-x86_64 -machine "$mc" -smp 2 -m 4G \
    -drive if=pflash,format=raw,readonly=on,file="$CODE" \
    -drive if=pflash,format=raw,file="$VARS" \
    -cdrom out/novus-live.iso -boot order=d -display none \
    -serial file:"$log" -no-reboot -net none $extra || true
}

run pc   /tmp/b_ide.log  "-drive if=ide,index=0,media=disk,format=raw,file=/tmp/disk.img"
echo IDE_DONE
run pc   /tmp/b_ahci.log "-device ahci,id=ahci -drive if=none,format=raw,file=/tmp/disk.img,id=d0 -device ide-hd,bus=ahci.0,drive=d0"
echo AHCI_DONE
run pc   /tmp/b_nvme.log "-drive if=none,format=raw,file=/tmp/disk.img,id=nv -device nvme,drive=nv,serial=test"
echo NVME_DONE
run pc   /tmp/b_usb.log  "-device qemu-xhci,id=xhci -drive if=none,format=raw,file=/tmp/disk.img,id=u0 -device usb-storage,bus=xhci.0,drive=u0"
echo USB_DONE
