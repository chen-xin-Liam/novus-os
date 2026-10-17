# ---------------------------------------------------------------------------
# Host image toolchain (tools/)
# ---------------------------------------------------------------------------
# Three host programs produce the artefacts M3 boots from. They are built
# with the host compiler and are not part of the kernel image:
#
#   mkfat      the FAT32 ESP image holding BOOTX64.EFI, the NovusFS system
#              image and the *.nkm modules. FAT32 is the one filesystem
#              every UEFI firmware reads without a driver of ours.
#   mkiso      wraps that ESP in an ISO 9660 image with an El Torito boot
#              entry, so the very same ESP also boots from a CD-ROM.
#   mkfsnovus  a NovusFS system-partition image, compiled from the kernel's
#              own fs/novusfs/*.c. Sharing the source is the point: a
#              second implementation of the format could drift from the
#              kernel's and write images the kernel cannot mount.
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
SYS_SIZE_MB     ?= 8
ESP_TPL_SIZE_MB ?= 12
ESP_SIZE_MB     ?= 28

ESP_TPL   := out/esp-tpl.img
ESP_IMG   := out/esp.img
NOVUS_IMG := out/novus.img
ISO_IMG   := out/novus-live.iso
NKM_MOD   := out/test.nkm

# Seed of the development signing key. Deterministic on purpose: the
# kernel carries the matching public key, so anyone regenerating the
# module gets the same key pair rather than a silently mismatched one.
NKM_SEED ?= $(shell python3 -c \
  "import hashlib;print(hashlib.sha256(b'novus nkm development signing key v1').hexdigest())")

.PHONY: tools esp novus-img iso run-iso install nkm-test-module

tools: $(MKFAT) $(MKISO) $(MKFSNOVUS)

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

# --gc-sections: the precompiled core rlib carries a DWARF reference to
# Rust's unwind personality even under panic=abort, and nothing defines
# it in a C-hosted binary. Novus never unwinds, so discarding the unused
# debug section is correct - and cheaper than pinning a mangled symbol
# name that changes with the compiler.
$(MKFSNOVUS): $(TOOLS_OUT)/tools_mkfsnovus_mkfsnovus.o $(NOVUSFS_HOST_OBJS) \
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
	    $(NOVUS_IMG):NOVUS/SYSTEM.IMG

# The live ESP. BOOTX64.EFI is the firmware's default fallback path;
# NOVUS/SYSTEM.IMG is the live root; NOVUS/ESP.IMG is the install
# template; NOVUS/TEST.NKM is the signed module the loader verifies.
$(ESP_IMG): $(MKFAT) out/NOVUS.EFI $(NOVUS_IMG) $(ESP_TPL) $(NKM_MOD) | out
	$(MKFAT) $@ $(ESP_SIZE_MB) \
	    out/NOVUS.EFI:EFI/BOOT/BOOTX64.EFI \
	    $(NOVUS_IMG):NOVUS/SYSTEM.IMG \
	    $(ESP_TPL):NOVUS/ESP.IMG \
	    $(NKM_MOD):NOVUS/TEST.NKM

# A signed test module, produced by the same Ed25519 code the kernel
# verifies with. Regenerated whenever the crypto or the loader changes.
$(NKM_MOD): $(RUST_DEPS)
	@test -n "$(CARGO)" || { echo "ERROR: cargo not found." >&2; exit 1; }
	cd tools/nkmgen && CARGO_TARGET_DIR=$(CURDIR)/build/rust-host \
	    $(CARGO) run --quiet -- mkmod "$(NKM_SEED)" 1 \
	    ../../security/nkm/nkm_test_module.h ../../$(NKM_MOD)

iso: $(ISO_IMG)

$(ISO_IMG): $(MKISO) $(ESP_IMG) | out
	$(MKISO) $@ $(ESP_IMG) NOVUS_LIVE

run-iso: iso
	scripts/run-iso.sh "$(MEM)"

# Write a prepared disk image to a device or to a file. Refuses to touch
# anything that is not explicitly named on the command line.
install: novus-img
	scripts/install.sh "$(DEVICE)"

# Regenerate the signed test module by hand after changing the NKM
# layout or the root key. The build already does this through the
# out/test.nkm rule, which is what the ESP carries.
nkm-test-module: $(NKM_MOD)
