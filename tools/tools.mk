# ---------------------------------------------------------------------------
# Host image toolchain (tools/)
# ---------------------------------------------------------------------------
# Three host programs produce the artefacts M3 boots from. They are built
# with the host compiler and are not part of the kernel image:
#
#   mkfat      the FAT ESP image holding BOOTX64.EFI, the NovusFS system
#              image and the *.nkm modules. FAT is the one filesystem
#              every UEFI firmware reads without a driver of ours; the
#              type is picked from the image size (FAT16 while the cluster
#              count stays under the FAT32 minimum, FAT32 above it).
#   mkiso      wraps that ESP in an ISO 9660 image with an El Torito boot
#              entry, so the very same ESP also boots from a CD-ROM. The
#              catalog carries the UEFI no-emulation form first - what
#              the specification describes for an EFI boot image and what
#              every distribution ships - and the hard-disk-emulation form
#              of the same image second, for firmwares that look only for
#              that. The ISO's unused system area gets a partition table
#              pointing at the embedded ESP, so the image written straight
#              onto a USB stick is bootable too.
#   mkfsnovus  a NovusFS system-partition image, compiled from the kernel's
#              own fs/novusfs/*.c. Sharing the source is the point: a
#              second implementation of the format could drift from the
#              kernel's and write images the kernel cannot mount.
#   mkdisk     a whole bootable disk (GPT, ESP, NovusFS) assembled on the
#              host from kernel/gpt.c, for `make install`.
#
# Nothing under tools/ is linked into NOVUS.EFI.

HOSTCC     ?= gcc
# _GNU_SOURCE: mkfsnovus sizes its output with ftruncate and seeks with
# fseeko, and -std=c11 would otherwise hide both behind __STRICT_ANSI__.
HOSTCFLAGS ?= -O2 -std=c11 -Wall -Wextra -D_GNU_SOURCE

TOOLS_OUT   := build/tools
MKFAT       := $(TOOLS_OUT)/mkfat
MKISO       := $(TOOLS_OUT)/mkiso
MKFSNOVUS   := $(TOOLS_OUT)/mkfsnovus
MKDISK      := $(TOOLS_OUT)/mkdisk

# fs/novusfs is freestanding and declares memcpy/memset/memcmp itself,
# with a 64-bit length. Renaming them keeps those declarations from
# colliding with glibc's when the same sources are compiled for the host.
HOST_REMAP := -Dmemcpy=novus_memcpy -Dmemset=novus_memset \
              -Dmemcmp=novus_memcmp
HOST_INC   := -Ifs/novusfs -Ikernel -Iarch/x86_64/include -Ilib

NOVUSFS_HOST_SRCS := fs/novusfs/super.c \
                     fs/novusfs/wal.c \
                     fs/novusfs/inode.c \
                     fs/novusfs/balloc.c \
                     fs/novusfs/dir.c \
                     fs/novusfs/novusfs.c \
                     fs/novusfs/merkle.c
NOVUSFS_HOST_OBJS := $(foreach s,$(NOVUSFS_HOST_SRCS),\
                       $(TOOLS_OUT)/$(subst /,_,$(basename $(s))).o)

# Same Rust crate, native target: mkfsnovus needs the very same CRC-32 and
# SHA-256 the kernel uses, and this is how it gets them.
RUST_HOST_LIB := build/rust-host/release/libnovuslib_rs.a

# ---------------------------------------------------------------------------
# Images
# ---------------------------------------------------------------------------
# Sizes in MiB. The live ESP carries, on top of the boot files, a copy of
# itself (NOVUS/ESP.IMG) that the installer writes to a target disk, so
# it is necessarily the largest of the three images.
# mkfat now picks FAT16 for small images, so the install template can
# stay well under the 32 MiB limit imposed by the El Torito hard-disk
# emulation sector-count field. The live ESP is only slightly larger
# because it also carries the template.
SYS_SIZE_MB     ?= 8
ESP_TPL_SIZE_MB ?= 12
ESP_SIZE_MB     ?= 24

ESP_TPL   := out/esp-tpl.img
ESP_IMG   := out/esp.img
NOVUS_IMG := out/novus.img
ISO_IMG   := out/novus-live.iso
NKM_TEST_H := security/nkm/nkm_test_module.h
DISK_IMG  := out/novus-disk.img
NKM_MOD   := out/test.nkm

# Size of the host-built disk image. The ESP template and the system
# partition together need a little over 20 MiB; the rest is room for the
# backup GPT and for whatever the installed system grows into.
DISK_SIZE_MB ?= 64

# Seed of the development signing key. Deterministic on purpose: the
# kernel carries the matching public key, so anyone regenerating the
# module gets the same key pair rather than a silently mismatched one.
NKM_SEED ?= $(shell python3 -c \
  "import hashlib;print(hashlib.sha256(b'novus nkm development signing key v1').hexdigest())")

.PHONY: tools esp novus-img disk-img iso iso-force run-iso install \
        nkm-test-module

tools: $(MKFAT) $(MKISO) $(MKFSNOVUS) $(MKDISK)

$(TOOLS_OUT):
	mkdir -p $(TOOLS_OUT)

$(MKFAT): tools/mkfat/mkfat.c | $(TOOLS_OUT)
	$(HOSTCC) $(HOSTCFLAGS) -o $@ $<

$(MKISO): tools/mkiso/mkiso.c | $(TOOLS_OUT)
	$(HOSTCC) $(HOSTCFLAGS) -o $@ $<

# One rule per kernel filesystem source, flattened like the kernel's own
# object naming, so the host build and the kernel build never share an
# object file.
define HOST_FS_rule
$(TOOLS_OUT)/$(subst /,_,$(basename $(1))).o: $(1) | $(TOOLS_OUT)
	$$(HOSTCC) $$(HOSTCFLAGS) $$(HOST_INC) $$(HOST_REMAP) -c $$< -o $$@
endef
$(foreach src,$(NOVUSFS_HOST_SRCS),$(eval $(call HOST_FS_rule,$(src))))

$(TOOLS_OUT)/tools_mkfsnovus_mkfsnovus.o: tools/mkfsnovus/mkfsnovus.c \
                                          | $(TOOLS_OUT)
	$(HOSTCC) $(HOSTCFLAGS) $(HOST_INC) $(HOST_REMAP) -c $< -o $@

$(TOOLS_OUT)/tools_mkdisk_mkdisk.o: tools/mkdisk/mkdisk.c | $(TOOLS_OUT)
	$(HOSTCC) $(HOSTCFLAGS) $(HOST_INC) $(HOST_REMAP) -c $< -o $@

# The headers matter here: gpt.c reaches the block layer through
# blkdev.h, and part of that interface is inline, so a change to the
# header changes the object even though the .c file is untouched.
$(TOOLS_OUT)/kernel_gpt.o: kernel/gpt.c kernel/gpt.h kernel/blkdev.h \
                           | $(TOOLS_OUT)
	$(HOSTCC) $(HOSTCFLAGS) $(HOST_INC) $(HOST_REMAP) -c $< -o $@

# --gc-sections: the precompiled core rlib carries a DWARF reference to
# Rust's unwind personality even under panic=abort, and nothing defines
# it in a C-hosted binary. Novus never unwinds, so discarding the unused
# debug section is correct - and cheaper than pinning a mangled symbol
# name that changes with the compiler.
$(MKFSNOVUS): $(TOOLS_OUT)/tools_mkfsnovus_mkfsnovus.o $(NOVUSFS_HOST_OBJS) \
              $(RUST_HOST_LIB)
	$(HOSTCC) -Wl,--gc-sections -o $@ $^

# mkdisk writes the same GPT the boot-stage installer writes: kernel/gpt.c
# is linked in unchanged, so a host-built disk and a live-installed disk
# agree on where every partition starts.
$(MKDISK): $(TOOLS_OUT)/tools_mkdisk_mkdisk.o $(TOOLS_OUT)/kernel_gpt.o \
           $(RUST_HOST_LIB)
	$(HOSTCC) -Wl,--gc-sections -o $@ $^

$(RUST_HOST_LIB): $(RUST_DEPS)
	@test -n "$(CARGO)" || { echo "ERROR: cargo not found." >&2; exit 1; }
	cd $(RUST_DIR) && CARGO_TARGET_DIR=$(CURDIR)/build/rust-host \
	    RUSTFLAGS="-C debuginfo=0" $(CARGO) build --release

# ---------------------------------------------------------------------------
# Image targets
# ---------------------------------------------------------------------------

novus-img: $(NOVUS_IMG)

# The system partition: formatted and Merkle-built by the kernel's own
# filesystem code, so its root hash is already in the superblock.
$(NOVUS_IMG): $(MKFSNOVUS) | out
	$(MKFSNOVUS) $@ $(SYS_SIZE_MB)

esp: $(ESP_IMG)

# ESP template: what the installer writes to a target disk's ESP. Built
# separately from the live ESP because an image cannot contain itself.
$(ESP_TPL): $(MKFAT) out/NOVUS.EFI $(NOVUS_IMG) | out
	$(MKFAT) $@ $(ESP_TPL_SIZE_MB) \
	    out/NOVUS.EFI:EFI/BOOT/BOOTX64.EFI \
	    $(NOVUS_IMG):SYSTEM.IMG

# The live ESP. BOOTX64.EFI is the firmware's default fallback path;
# NOVUS/SYSTEM.IMG is the live root; NOVUS/ESP.IMG is the install
# template; NOVUS/TEST.NKM is the signed module the loader verifies.
$(ESP_IMG): $(MKFAT) out/NOVUS.EFI $(NOVUS_IMG) $(ESP_TPL) $(NKM_MOD) | out
	$(MKFAT) $@ $(ESP_SIZE_MB) \
	    out/NOVUS.EFI:EFI/BOOT/BOOTX64.EFI \
	    $(NOVUS_IMG):SYSTEM.IMG \
	    $(ESP_TPL):ESP.IMG \
	    $(NKM_MOD):TEST.NKM

# A signed test module, produced by the same Ed25519 code the kernel
# verifies with. One nkmgen run writes both outputs - the C array the
# kernel compiles in and the plain file the boot stage reads off the ESP -
# so both are listed as targets of the one recipe; naming only the file
# here would leave a deleted header unrecreated.
$(NKM_TEST_H) $(NKM_MOD): $(RUST_DEPS)
	@test -n "$(CARGO)" || { echo "ERROR: cargo not found." >&2; exit 1; }
	cd tools/nkmgen && CARGO_TARGET_DIR=$(CURDIR)/build/rust-host \
	    $(CARGO) run --quiet -- mkmod "$(NKM_SEED)" 1 \
	    ../../$(NKM_TEST_H) ../../$(NKM_MOD)

# 'iso' is phony, so this recipe always runs and naming the artifact is
# what keeps an up-to-date tree from looking like nothing happened. Only
# the rule below rebuilds, and only when a real input changed.
iso: $(ISO_IMG)
	@echo "iso: $(ISO_IMG) ($(shell wc -c < $(ISO_IMG) 2>/dev/null || echo 0) bytes)"

$(ISO_IMG): $(MKISO) $(ESP_IMG) | out
	$(MKISO) $@ $(ESP_IMG) NOVUS_LIVE

# Throw the ISO away and rebuild it whatever make thinks about timestamps.
iso-force:
	rm -f $(ISO_IMG)
	$(MAKE) iso

run-iso: iso
	scripts/run-iso.sh "$(MEM)"

# A complete bootable disk: GPT, ESP and the NovusFS system partition,
# the same layout the live installer writes. Built here so a device or an
# image file can be seeded without a running Novus.
disk-img: $(DISK_IMG)

$(DISK_IMG): $(MKDISK) $(ESP_TPL) $(NOVUS_IMG) | out
	$(MKDISK) $@ $(DISK_SIZE_MB) $(ESP_TPL) $(NOVUS_IMG)

# Write a prepared disk image to a device or to a file. Refuses to touch
# anything that is not explicitly named on the command line.
install: disk-img
	DEVICE="$(DEVICE)" scripts/install.sh

# Regenerate the signed test module by hand after changing the NKM
# layout or the root key. The build already does this through the
# out/test.nkm rule, which is what the ESP carries.
nkm-test-module: $(NKM_TEST_H) $(NKM_MOD)
