/**
 * Novus OS - native IDE/ATA (PIIX3/ICH9) PIO driver (storage milestone).
 *
 * The kernel stage exposes AHCI/SATA, NVMe and USB storage, but the classic
 * onboard IDE controller - the QEMU "pc" machine's default disk path - is
 * detached from the firmware block-device list at ExitBootServices and so
 * vanishes unless the kernel drives it directly. ide_init() finds the PCI IDE
 * controller and drives the compatibility-mode I/O ports with PIO28/PIO48.
 */
#ifndef NOVUS_IDE_H
#define NOVUS_IDE_H

/* Detect the PCI IDE controller and register every attached disk (master and
 * slave on both channels) as a "ideN" block device. Returns the number of
 * disks registered. Safe to call once; later calls are no-ops. */
int ide_init(void);

#endif /* NOVUS_IDE_H */
