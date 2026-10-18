# Novus OS - top-level Makefile (Milestone M2)
#
# Builds a freestanding UEFI PE32+ application (out/NOVUS.EFI) from the
# M2 source set, using the x86_64-w64-mingw32-gcc cross compiler with
# the MS ABI. fs/novusfs/ is on the include path (-Ifs/novusfs) so the
# kernel stage can pull in the filesystem header directly. On top of the
# M0 flow the image additionally links:
#   - the 16-bit AP trampoline (arch/x86_64/kernel/ap_trampoline.S):
#     assembled with -m16 into build/ap_trampoline.o, stripped to the
#     flat binary build/ap_trampoline.bin via objcopy, and embedded into
#     the image through the generated build/tramp_blob.S (.incbin blob
#     exporting ap_trampoline_bin / ap_trampoline_bin_end);
#   - the Rust static library from lib/novuslib-rs, built by cargo for
#     the x86_64-pc-windows-msvc target. That target emits COFF objects,
#     which the mingw PE linker consumes directly; the ELF objects of
#     x86_64-unknown-none cannot be linked into a PE image.
# 'make run' boots the image under QEMU with OVMF firmware.
#
# Usage:
#   make              build out/NOVUS.EFI
#   make run          build, then boot the image in QEMU (MEM=4G by default)
#   make iso          build the bootable live ISO (out/novus-live.iso);
#                     prints the artifact again when it is already current
#   make iso-force    delete and rebuild the ISO unconditionally
#   make run-iso      build the ISO and boot it in QEMU as a CD-ROM
#   make disk-img     build a bootable disk image (out/novus-disk.img)
#   make install      write that disk image to a device: DEVICE=/dev/sdX
#   make clean        remove build/, out/ and esp/
#
# The image targets (iso / run-iso / disk-img / install / novus-img) are
# defined in tools/tools.mk, which is included at the bottom of this file
# so that 'all' stays the default target.

# ---------------------------------------------------------------------------
# Toolchain
# ---------------------------------------------------------------------------
# Cross toolchain prefix; override on the command line with CROSS=<prefix>.
CROSS ?= x86_64-w64-mingw32-
CC := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy

# Fail early with a clear message when the cross compiler is missing.
ifeq ($(shell command -v $(CC) 2>/dev/null),)
$(error $(CC) not found in PATH. Install the mingw-w64 x86-64 cross toolchain (e.g. package gcc-mingw-w64-x86-64) or set CROSS=<toolchain-prefix>.)
endif

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
# M1 reads a plain shell-style defconfig directly. A full Kconfig parser
# with a menuconfig front-end is planned for a later milestone; keep
# configs/novus_defconfig in sync with the top-level Kconfig file.
-include configs/novus_defconfig

# Guest memory handed to QEMU by 'make run'.
MEM ?= 4G

# ---------------------------------------------------------------------------
# Sources and flags
# ---------------------------------------------------------------------------
# Source list is pinned for M2; extend it in later milestones.
SRC := arch/x86_64/boot/main.c \
       arch/x86_64/boot/stage3.c \
       kernel/kmain.c \
       kernel/vga.c \
       kernel/serial.c \
       kernel/novuslib.c \
       kernel/shared_mem.c \
       kernel/pmm.c \
       kernel/mm.c \
       kernel/vmm.c \
       kernel/heap.c \
       kernel/lapic.c \
       kernel/smp.c \
       kernel/gdt.c \
       kernel/idt.c \
       kernel/trap.c \
       kernel/sched.c \
       kernel/vm.c \
       kernel/nuc.c \
       kernel/blkdev.c \
       kernel/gpt.c \
       kernel/nksym.c \
       graphics/nd_font.c \
       graphics/nd_core.c \
       graphics/nd_hal.c \
       graphics/nd_input.c \
       graphics/nvgui.c \
       graphics/nd_demo.c \
       kernel/ipc.c \
       kernel/apps.c \
       kernel/sysinfo.c \
       wm/novus-wm/nwm.c \
       wm/novus-wm/nwm_demo.c \
       fs/novusfs/super.c \
       fs/novusfs/balloc.c \
       fs/novusfs/inode.c \
       fs/novusfs/dir.c \
       fs/novusfs/wal.c \
       fs/novusfs/merkle.c \
       fs/novusfs/novusfs.c \
       security/nkm/nkm.c

# Assembly sources. Uppercase .S (case-sensitive) makes gcc run the C
# preprocessor over the file first; gcc invokes the assembler itself.
ASM_SRC := arch/x86_64/kernel/entry.S arch/x86_64/kernel/trap.S

INC := -Iarch/x86_64/include -Ikernel -Igraphics -Iwm/novus-wm -Ifs/novusfs \
       -Isecurity/nkm -Ibuild

# Freestanding UEFI environment: no libc, no host runtime, no stack
# protector, no PIC, no Red Zone (interrupts may clobber it), small code
# model, 16-bit wchar_t to match EFI CHAR16.
CFLAGS := $(INC) -ffreestanding -fno-builtin -fno-stack-protector -fno-pic \
          -fshort-wchar -mno-red-zone -mcmodel=small -O2 -g -Wall -Wextra \
          -MMD -MP

# Kconfig-derived preprocessor switches: config 'y' becomes 1, 'n'/unset
# becomes 0, so sources can test them with #if CONFIG_NOVUS_XXX.
CFLAGS += -DCONFIG_NOVUS_A_CORE_DISABLE=$(if $(filter y,$(CONFIG_NOVUS_A_CORE_DISABLE)),1,0) \
          -DCONFIG_NOVUS_SMP=$(if $(filter y,$(CONFIG_NOVUS_SMP)),1,0) \
          -DCONFIG_NOVUS_NOVUSFS=$(if $(filter y,$(CONFIG_NOVUS_NOVUSFS)),1,0) \
          -DCONFIG_NOVUS_WAL=$(if $(filter y,$(CONFIG_NOVUS_WAL)),1,0) \
          -DCONFIG_NOVUS_MERKLE=$(if $(filter y,$(CONFIG_NOVUS_MERKLE)),1,0) \
          -DCONFIG_NOVUS_NKM=$(if $(filter y,$(CONFIG_NOVUS_NKM)),1,0) \
          -DCONFIG_NOVUS_INSTALLER=$(if $(filter y,$(CONFIG_NOVUS_INSTALLER)),1,0) \
          -DCONFIG_NOVUS_SCHED=$(if $(filter y,$(CONFIG_NOVUS_SCHED)),1,0) \
          -DCONFIG_NOVUS_DISPLAY=$(if $(filter y,$(CONFIG_NOVUS_DISPLAY)),1,0) \
          -DCONFIG_NOVUS_GUI=$(if $(filter y,$(CONFIG_NOVUS_GUI)),1,0) \
          -DCONFIG_NOVUS_WM=$(if $(filter y,$(CONFIG_NOVUS_WM)),1,0) \
          -DCONFIG_NOVUS_IPC=$(if $(filter y,$(CONFIG_NOVUS_IPC)),1,0)

# Link as a PE32+ EFI application: subsystem 10 = EFI application,
# image entry point is efi_main.
LDFLAGS := -shared -nostdlib -Wl,-dll -Wl,--subsystem,10 -Wl,-e,efi_main

# Flat object naming: path separators become underscores, e.g.
#   arch/x86_64/boot/main.c -> build/arch_x86_64_boot_main.o
# Every pinned source must map to a unique object name (true for the M1
# list: no two entries collapse onto the same build/xxx.o; the explicit
# per-file rules below never need the reverse mapping).
OBJS := $(foreach src,$(SRC) $(ASM_SRC),build/$(subst /,_,$(basename $(src))).o)

# ---------------------------------------------------------------------------
# Rust static library (lib/novuslib-rs)
# ---------------------------------------------------------------------------
# Built by cargo for the x86_64-pc-windows-msvc target: that target emits
# COFF objects, which the mingw PE linker consumes directly. The ELF
# objects produced for x86_64-unknown-none cannot be linked into a PE
# image, so the msvc target is used even with a mingw toolchain.
RUST_DIR := lib/novuslib-rs
RUST_TARGET := x86_64-pc-windows-msvc
CARGO_TARGET_DIR := build/rust
# Cargo names staticlib output "novuslib_rs.lib" for msvc targets (no lib*.a
# prefix scheme); it is a plain COFF ar archive that mingw ld consumes.
RUST_LIB := $(CARGO_TARGET_DIR)/x86_64-pc-windows-msvc/release/novuslib_rs.lib

# Parse-time notice only (so 'make clean' and friends do not abort); the
# rule below fails with install guidance when the library is needed but
# cargo is genuinely missing.
CARGO := $(shell command -v cargo 2>/dev/null)
ifeq ($(CARGO),)
ifneq ($(wildcard $(HOME)/.cargo/bin/cargo),)
CARGO := $(HOME)/.cargo/bin/cargo
endif
endif
ifeq ($(CARGO),)
$(warning cargo not found in PATH -- $(RUST_LIB) cannot be rebuilt. Install Rust: curl https://sh.rustup.rs | sh -s -- -y && rustup target add x86_64-pc-windows-msvc)
endif

# Prerequisites: every file under RUST_DIR (excluding a local target/).
# find(1) instead of $(wildcard) so a not-yet-populated RUST_DIR yields an
# empty prerequisite list (expected while sources land) instead of a fatal
# "No rule to make target".
RUST_DEPS := $(shell find $(RUST_DIR) -type f -not -path '$(RUST_DIR)/target/*' 2>/dev/null)

# ---------------------------------------------------------------------------
# 16-bit AP trampoline and its embedded binary blob
# ---------------------------------------------------------------------------
# ap_trampoline.S runs in 16-bit real mode on the AP core before paging is
# enabled, so it is assembled with dedicated flags: -m16 (none of the
# 64-bit -mcmodel/-fshort-wchar knobs) and no unwind tables / stack
# protector. The object is stripped to a flat binary and embedded into the
# image via a generated .incbin blob bracketed by the global symbols
# ap_trampoline_bin / ap_trampoline_bin_end.
TRAMPOLINE_SRC := arch/x86_64/kernel/ap_trampoline.S
TRAMPOLINE_OBJ := build/ap_trampoline.o
TRAMPOLINE_BIN := build/ap_trampoline.bin
TRAMP_BLOB_S := build/tramp_blob.S
TRAMP_BLOB_OBJ := build/tramp_blob.o

# ---------------------------------------------------------------------------
# Rules
# ---------------------------------------------------------------------------
.PHONY: all run clean banner

all: banner out/NOVUS.EFI

# Configuration summary printed before anything is built.
banner:
	@echo "Novus OS build: arch=$(CONFIG_NOVUS_ARCH) min_mem=$(CONFIG_NOVUS_MIN_MEMORY_MB)MB smp=$(CONFIG_NOVUS_SMP) a_core_disable=$(CONFIG_NOVUS_A_CORE_DISABLE) novusfs=$(CONFIG_NOVUS_NOVUSFS) wal=$(CONFIG_NOVUS_WAL) merkle=$(CONFIG_NOVUS_MERKLE) nkm=$(CONFIG_NOVUS_NKM) installer=$(CONFIG_NOVUS_INSTALLER) display=$(CONFIG_NOVUS_DISPLAY) gui=$(CONFIG_NOVUS_GUI) ipc=$(CONFIG_NOVUS_IPC) wm=$(CONFIG_NOVUS_WM)"

# Link the final EFI image. Static archives go after the plain objects so
# the linker extracts members on demand; the tramp_blob object carries the
# embedded trampoline binary.
out/NOVUS.EFI: $(OBJS) $(TRAMP_BLOB_OBJ) $(RUST_LIB) | out
	$(CC) $(LDFLAGS) $(OBJS) $(TRAMP_BLOB_OBJ) $(RUST_LIB) -o $@

# Build the Rust static library with cargo (see the section above for why
# the msvc target). CARGO_TARGET_DIR is passed as an absolute path because
# the recipe cd's into RUST_DIR first.
$(RUST_LIB): $(RUST_DEPS)
	@test -n "$(CARGO)" || { \
	    echo "ERROR: cargo not found in PATH; cannot build $(RUST_LIB)." >&2; \
	    echo "Install Rust: curl https://sh.rustup.rs | sh -s -- -y && rustup target add x86_64-pc-windows-msvc" >&2; \
	    exit 1; }
	@test -f $(RUST_DIR)/Cargo.toml || { \
	    echo "ERROR: $(RUST_DIR)/Cargo.toml not found; cannot build $(RUST_LIB)." >&2; \
	    exit 1; }
	cd $(RUST_DIR) && CARGO_TARGET_DIR=$(CURDIR)/$(CARGO_TARGET_DIR) $(CARGO) build --release --target $(RUST_TARGET)

# ---------------------------------------------------------------------------
# M8 application images: Rust -> flat blob -> embedded C array
# ---------------------------------------------------------------------------
# The three system applications are ordinary Rust programs built for
# x86_64-unknown-none with apps/link.ld, which places _start at 0x400000
# and emits one flat image per app. objcopy drops the ELF wrapper,
# nuc_spawn_blob() maps the result at the same address, and the
# generated header turns each blob into a C array the kernel links
# (kernel/apps.c is the table).
APPS_DIR    := apps
APPS_TARGET := $(CURDIR)/build/rust-apps
APPS_STAMP  := build/apps.stamp
APPS_NAMES  := fileman taskman settings
APPS_BINS   := $(addprefix out/apps/,$(addsuffix .bin,$(APPS_NAMES)))
APPS_BLOB_H := build/apps_blobs.h
APPS_DEPS   := $(shell find $(APPS_DIR)/src -type f 2>/dev/null) \
               $(APPS_DIR)/Cargo.toml $(APPS_DIR)/link.ld \
               $(APPS_DIR)/.cargo/config.toml

$(APPS_STAMP): $(APPS_DEPS) | build
	@test -n "$(CARGO)" || { echo "ERROR: cargo not found in PATH." >&2; exit 1; }
	cd $(APPS_DIR) && CARGO_TARGET_DIR=$(APPS_TARGET) $(CARGO) build --release \
	    --target x86_64-unknown-none
	@touch $@

out/apps/%.bin: $(APPS_STAMP) | out
	@mkdir -p out/apps
	$(OBJCOPY) -O binary $(APPS_TARGET)/x86_64-unknown-none/release/$* $@
	@printf 'apps: %s %s bytes\n' $* "$$(wc -c < $@)"

$(APPS_BLOB_H): $(APPS_BINS) | build
	@{ echo '/* Generated by the build: M8 application blobs. */'; \
	   echo '#ifndef NOVUS_APPS_BLOBS_H'; \
	   echo '#define NOVUS_APPS_BLOBS_H'; \
	   for n in $(APPS_NAMES); do xxd -i -n k_app_$$n out/apps/$$n.bin; done; \
	   echo '#endif'; } > $@

# apps.c includes the generated header; nothing else does.
build/kernel_apps.o: $(APPS_BLOB_H)

# 16-bit AP trampoline: real-mode code, so dedicated flags (no -mcmodel,
# no -fshort-wchar, no unwind tables, no stack protector).
$(TRAMPOLINE_OBJ): $(TRAMPOLINE_SRC) | build
	$(CC) -m16 -ffreestanding -fno-asynchronous-unwind-tables -fno-stack-protector -c $< -o $@

# Flat binary of the trampoline (loaded as raw bytes by the BSP).
$(TRAMPOLINE_BIN): $(TRAMPOLINE_OBJ)
	$(OBJCOPY) -O binary $< $@

# Generated assembly embedding the trampoline binary via .incbin,
# bracketed by the global symbols ap_trampoline_bin / ap_trampoline_bin_end.
# The blob references the binary by absolute path so the assembler finds it
# regardless of its source-directory-relative search rules.
$(TRAMP_BLOB_S): $(TRAMPOLINE_BIN)
	@printf '\t.globl ap_trampoline_bin\nap_trampoline_bin:\n\t.incbin "%s"\n\t.globl ap_trampoline_bin_end\nap_trampoline_bin_end:\n' '$(abspath $<)' > $@

$(TRAMP_BLOB_OBJ): $(TRAMP_BLOB_S) | build
	$(CC) $(CFLAGS) -c $< -o $@

# Compile every C/asm source into a flat object under build/. One explicit
# rule per source is generated from $(SRC) and $(ASM_SRC) (a single pattern
# rule cannot portably reverse the flattened stem back to the real source
# path):
#   arch/x86_64/boot/main.c -> build/arch_x86_64_boot_main.o
define COMPILE_rule
build/$(subst /,_,$(basename $(1))).o: $(1) | build
	$$(CC) $$(CFLAGS) -c $$< -o $$@
endef
$(foreach src,$(SRC) $(ASM_SRC),$(eval $(call COMPILE_rule,$(src))))

# Header dependencies: -MMD writes a .d next to each object, and the
# include below makes a changed header rebuild its users. Without it a
# protocol or config header edit silently left stale objects behind -
# exactly the kind of bug that only shows up as "the kernel ignores my
# new limit".
-include $(OBJS:.o=.d)

# nkm.c embeds the generated test module array. Without this edge a
# regenerated module (make nkm-test-module) leaves the object untouched
# and the kernel keeps carrying a module image that exists nowhere else.
build/security_nkm_nkm.o: security/nkm/nkm_test_module.h

# Output directories.
out build:
	mkdir -p $@

# Build, then boot the image in QEMU.
run: all
	scripts/run.sh "$(MEM)"

# Remove all build artifacts.
clean:
	rm -rf build out esp

# ---------------------------------------------------------------------------
# Host image toolchain (tools/, see tools/tools.mk)
# ---------------------------------------------------------------------------
# Included last, after every kernel rule: make takes the first target it
# sees as the default, and 'all' must stay that target. The host rules
# reuse CARGO, RUST_DIR and RUST_DEPS from the section above.
include tools/tools.mk
