# Novus OS Glossary

> Version: v1.0 (2026-11-27)
> A "slang dictionary" for newcomers: custom names, abbreviations, module names and filename prefixes.
> 中文版本：[../GLOSSARY.md](../GLOSSARY.md) · [Index](INDEX.md)
>
> Status marks: **[implemented]** / **[spec goal]** / **[prototype]**.

---

## Contents

1. [Project and product names](#1-project-and-product-names)
2. [File formats and containers](#2-file-formats-and-containers)
3. [Boot, kernel and memory](#3-boot-kernel-and-memory)
4. [Graphics, windows and input](#4-graphics-windows-and-input)
5. [Filesystems and storage](#5-filesystems-and-storage)
6. [User space, apps and languages](#6-user-space-apps-and-languages)
7. [Network and security](#7-network-and-security)
8. [Build, images and verification](#8-build-images-and-verification)
9. [Filename prefixes](#9-filename-prefixes)
10. [Easily confused terms](#10-easily-confused-terms)

---

## 1. Project and product names

| Term | Meaning | Notes |
|---|---|---|
| **Novus OS** | Project name | x86_64 UEFI desktop OS, GPLv2, self-developed |
| **V1.6-FinalSpec** | Current spec version | Frozen release, 19 parts |
| **M0–M12** | Development milestones | **Not** release version numbers |
| **NovusFS** | Self-developed filesystem | Dual superblock + WAL + Merkle; magic `NOVS`, version `0x00030000` |
| **NovusDisplay / nd** | Graphics stack | Framebuffer HAL → 2D renderer → nvgui → desktop; prefix `nd_*` |
| **Novus-WM / nwm** | Window manager | In-kernel compositor + IPC server; port `"nwm"` |
| **Novus Fluent** | UI design language | Semantic tokens generated from `theme.toml` |
| **NovusDefender / novusd** | Built-in security daemon | EICAR detection, scan, quarantine, incremental update, directory snapshot |
| **nsh** | Novus Shell | Windowed terminal, 144×26 grid; engine in `apps/src/shell/`; 35 builtins |
| **nvgui** | Novus GUI widget library | Fixed pool of 192 widgets |
| **Mini-DWM** | Mini desktop window manager | Surface redirection + software compositing + display syscalls |
| **Nuomi** | Custom Lisp-like language | Host C reference (`user/Nuomi_pl`) + device-side `no_std` interpreter |
| **cppcc** | C++ subset compiler | Host Rust tool; NUC/PE32+/ELF64 targets |
| **Mochi** | Another Lisp-like language | Spec only, no `user/mochi` submodule **[spec goal]** |
| **MicroPython** | User-space Python runtime | Roadmap only, no source **[spec goal]** |
| **nuc_spawn_blob** | Kernel entry to launch built-in app | Maps flat image at `0x400000`; blob ≤128 KiB |

## 2. File formats and containers

| Term | Meaning | Notes |
|---|---|---|
| **NUC** | Native executable (`.nuc`) | 48 B header, `NUC!`, ABI `0x0100`; native x86_64 code, no VM |
| **NKM** | Kernel module format | 208 B header, `NKM!`, Ed25519; Ring 0, must be signed |
| **NUP** | App install package | V1.0 frozen, 64 B header, `NUP!` |
| **INS** | Shortcut format | V1.0 frozen, INI-style UTF-8 text |
| **novu** | Update system | `.novu` full + `.hotpth`/`.npth`/`.apth` patches; five priority channels |
| **NWPK** | Wallpaper package | `WALLPAPERS.PKG`, 40 B index + raw JPEG |
| **NVSH** | Shared memory layout version | v1.1, 8 KiB @ `0x7E000` |
| **ESP** | EFI System Partition | FAT16/32, 8.3 names only; live ESP 31.5 MiB |
| **El Torito** | CD boot spec | no-emulation EFI; 16-bit sector count ⇒ boot item <32 MiB |
| **isohybrid** | ISO usable as USB disk | Same image written to USB boots on firmware |
| **Joliet** | ISO9660 long-name extension | Preferred over plain ISO tree |
| **PE32+/COFF/ELF64** | External target formats | cppcc outputs PE32+/ELF64; kernel Rust lib uses msvc target → COFF |
| **APPS/DEMO.NUC** | Fixed boot-stage demo slot | Only auto-executed NUC path; generic exec not implemented |

## 3. Boot, kernel and memory

| Term | Meaning | Notes |
|---|---|---|
| **A core / BSP** | Bootstrap processor | Early self-check + Merkle verification, then sleeps |
| **B core / AP** | Application processor | Woken by A core, runs main kernel |
| **SIPI** | Startup IPI | Wakes AP; failure degrades to single-core, no panic |
| **AP trampoline** | AP boot stub | 16-bit, <4 KiB, no relocations; last 24 B = CR3/AP stack/`ap_main` |
| **efi_main** | UEFI entry point | PE32+ entry of `NOVUS.EFI` |
| **kmain** | Kernel init | Fixed order: shared-mem → PMM → MM → heap → display → storage → devtab → IPC → FS → NKM → SMP → scheduler |
| **kputs** | Unified logging | Mirrors to VGA, COM1, `klog` ring and optional fb console; locked |
| **klog** | Log ring | 4 KiB boot (shared memory tail) + 32 KiB kernel |
| **Shared memory handoff** | Boot↔kernel block | 8 KiB @ `0x7E000`; hardware info, self-check, Merkle root, live root, wallpaper, install payload |
| **PMM** | Physical memory manager | 256 KiB bitmap, ~8 GiB; DMA needs explicit PMM |
| **MM** | Paging manager | Low-4GiB identity map + physmap; framebuffer UC mapping |
| **VM / vmm** | Virtual memory / page allocator | Per-task CR3; `vm.c` supports COW |
| **kmalloc / heap** | Page-granularity allocator | 4 KiB rounding, ≤256 MiB / 256 records; **not** a slab |
| **LowMem / Full** | Memory modes | 2–6 GiB reduced features; ≥6 GiB full |
| **OOM** | Out-of-memory handling | Reclaims user processes by priority, protects kernel |
| **IDT/GDT/IRQ/LAPIC** | Interrupt system | IDT handles exceptions and syscall (vector `0x100`) |
| **CR3** | Page-table base register | One per task, process isolation |
| **NX / W^X** | Memory protection | Only TEXT executable; full W^X still roadmap |
| **tick / time slice** | Scheduling | Default 4 ticks, `SCHED_MAX_TASKS=10`, 64 KiB kernel stack |
| **stamp** | Build marker | Records flags or source changes; `apps.stamp` tracks four app paths |

## 4. Graphics, windows and input

| Term | Meaning | Notes |
|---|---|---|
| **`nd_*`** | NovusDisplay prefix | `nd_hal.c`, `nd_core.c`, `nd_boot.c`, `nd_font_uni.c`, `nd_ime.*` |
| **Framebuffer HAL** | Display abstraction | Enumerates up to 4 GOP outputs, first primary, rest mirrored |
| **Compositing buffer / nd_canvas** | Offscreen canvas | All drawing goes offscreen; `nd_present()` presents whole frame |
| **damage** | Dirty rectangle | Cursor and window redraw track changed rectangles |
| **Retrace wait** | Tearing mitigation | Probes VGA port `0x3DA`, bounded wait; no hardware flip/vblank |
| **Display list** | GUI client draw commands | `CLEAR` → `SHAPE`/`TEXT` → `INVALIDATE`; ≤120 shapes / ≤160 texts |
| **INVALIDATE** | Frame commit message | **Required** at end of frame; missing it causes half-frame flicker |
| **Surface redirection** | WM v2 | Client passes vidmem handle instead of pixels; zero-copy flip |
| **flip / FLIP_PRESENT** | Front/back buffer swap | v2 client submits new surface handle |
| **vidmem / CRTC / VSync** | Mini-DWM abstractions | Software backend only currently |
| **nwm** | WM port | `"nwm"`; `NWM_PID_SHELL=0` = desktop itself |
| **IPC** | Inter-process communication | 32 B header, magic `0x4E495043`, FNV-1a, payload ≤128 B, queue 8, ports 16 |
| **IME** | Input method | Single-syllable Pinyin, ≤6 candidates; `Ctrl+Space`, `Ctrl+Shift+U` |
| **ND_KEY_BASE** | Input key code base | `0x110000`; character keys carry Unicode code points |
| **backing store** | Widget pool | nvgui fixed 192 widgets |
| **Semantic tokens** | UI color roles | `ui::bg()`, `card_bg()`, `text()`, `accent()` etc.; no literal colors |
| **Wallpaper gradient fallback** | Current wallpaper | JPEG decoder has toolchain UB; desktop uses programmatic gradient |

## 5. Filesystems and storage

| Term | Meaning | Notes |
|---|---|---|
| **VFS** | Virtual filesystem | Single root `vfs_root()`; mount table switches backends by prefix |
| **Mount table** | 8 entries | Absolute unique paths; `/` cannot unmount; cross-mount rename refused |
| **rootfs layout** | Standard directory tree | Source of truth `fs/novusfs/rootfs_layout.h`, 78 directories |
| **mounts.json** | Mount config | ≤4 KiB / ≤8 entries / whitelist 6; runtime `mount` no write-back |
| **`..` traversal elimination** | Path normalization | **Not done**; directories have no `..` entries |
| **WAL** | Write-ahead log | 64-block ring, CRC-32 + commit, replay on mount; errors only WARN |
| **Merkle** | Integrity tree | 64 B SHA-256 nodes, 64 per block; covers bitmap+inode+data |
| **Dual superblock** | Atomic root update | Primary/backup + version counter |
| **fsck priorities** | Repair order | superblock → WAL → inode isolation |
| **blkdev** | Block device abstraction | Unified 512 B sectors; 4Kn via bounce buffer |
| **devtab** | Device table | `devtab_refresh()` generated; device manager only observes it |
| **devmgmt/devmgr** | Device manager | App `devmgmt`, module `apps/src/devmgr/` |
| **BOT** | USB mass storage | Bulk-Only Transport (CBW/CSW, READ10/WRITE10); not UASP |
| **xHCI/UHCI/EHCI** | USB controllers | xHCI 1.1 full; UHCI HID-only; EHCI-only high-speed unsupported |
| **power_state** | Disk power query | AHCI nulled, USB unimplemented, IDE unavailable on QEMU **[partial]** |

## 6. User space, apps and languages

| Term | Meaning | Notes |
|---|---|---|
| **apps/** | Built-in app sources | Rust `no_std`, no allocator; flat NUC blobs in kernel |
| **g_apps[]** | App registry | `kernel/apps.c`; **append only** — indices reused by old sysinfo/clients |
| **apps_launch_name()** | Launch by name | Preferred over index-based launch |
| **bootstrap apps** | Special launch apps | Only `session` and `setup`; others require login |
| **sinfo / sysinfo** | System info service | Port `"sys"` provides tasks/files/stats/network/lang/theme/display |
| **nsh builtins** | 35 commands | `ls cd pwd cat ... nuomi update novusd run install`; registered in `BUILTINS` |
| **Sequential-capture pipeline** | nsh pipes | No fork/exec; captures whole stage output; ≤8 stages, 4 KiB buffer |
| **cmd aliases** | DOS-style aliases | `dir`/`copy`/`del`, off by default |
| **bump arena** | Nuomi device memory | 256 KiB (128 KiB env reclaimable), no GC |
| **MAX_DEPTH** | Nuomi recursion limit | 1500 eval levels ≈ 500 function calls |
| **Three targets** | cppcc outputs | NUC V1.0 / Win64 PE32+ / Linux ELF64 |
| **C ABI library** | cppcc C interface | `cppcc-cabi`, stable header |
| **novusd command channel** | File-based daemon protocol | `/temp/novusd/req.bin`, `rsp.bin` (kernel spawn has no argv) |
| **6 stages (novusd)** | Porting delivery stages | Detection core, Rust platform, CLI, quarantine, update, E2E; 5 capabilities degraded |
| **templates/** | Three extension paths | GUI app / shell command / Nuomi script skeletons |

## 7. Network and security

| Term | Meaning | Notes |
|---|---|---|
| **NCSI** | Network connectivity status | Custom check fetching `connecttest.txt`; distinguishes no-link/DNS/TCP/HTTP/timeout |
| **Single-transaction DNS** | DNS implementation | Fixed source port 5353, one A query at a time |
| **Single TCP session** | TCP implementation | One connection, no window scaling/congestion control; no listener |
| **CAP** | Capability permission model | Spec defines 128-bit `cap_check()`, not implemented **[spec goal]** |
| **NKM hash rule** | Signature digest rule | SHA-256 over whole file with `[0x50,0xB0)` zeroed; Ed25519 over 32 B digest |
| **Development key** | Current signing root | One dev key for NKM, hardcoded update key; no rotation **[known limit]** |
| **NKM_FLAG_ALLOW_UNSIGNED** | Debug switch | "admin session only" is spec goal; no full admin auth |
| **Six-piece set** | ABI change checklist | spec, kernel, host tools, test fixtures, version number, compatibility note |
| **ABI BREAK** | Incompatible change | OS-level major event; highlight, RFC, major version bump |
| **WebPKI** | Public key infrastructure | Current TLS is not full WebPKI: no CRL/OCSP, name constraints **[partial]** |
| **fail-closed** | Security default | Empty CA root set refuses connections |
| **EICAR** | Antivirus test marker | Used by novusd for signature self-test, not real malware |
| **Explicit rejection** | Security principle | Unimplemented features must diagnose, not silently accept |

## 8. Build, images and verification

| Term | Meaning | Notes |
|---|---|---|
| **defconfig** | Default config | `configs/novus_defconfig` is the only file `-include`d by Makefile |
| **Kconfig** | Config declarations | Placeholder only; **no parser, no menuconfig** |
| **`y`/`n`** | Config values | Converted to C macros `1`/`0`; `depends on` not auto-checked |
| **CROSS** | Cross toolchain prefix | Default `x86_64-w64-mingw32-` |
| **E2E** | End-to-end build flag | `E2E=1` adds test roots and HTTPS probe; no mixed objects |
| **Generated files** | Committed derived files | Theme/lang/font/IME/wallpaper/CA/NKM test modules |
| **mkfat/mkiso/mkfsnovus/mkdisk** | Host image tools | Self-developed; mkfsnovus/mkdisk compile kernel FS/GPT source |
| **nkmgen/novupkg** | Signing/packaging tools | Rust host tools |
| **novus.img/esp.img/iso/disk.img** | Four image types | 8 MiB system / 31.5 MiB live ESP / live ISO / 64 MiB GPT disk |
| **qemu-smoke** | QEMU smoke test | VNC screenshots vs baselines; VNC only on 127.0.0.1:5901 |
| **baseline** | Screenshot regression baseline | Missing/mismatch fails; no `--update-baseline` in normal runs |
| **wm-churn** | App lifecycle test | 20 rounds of close/restart, checks ticks and no double-free |
| **OVMF** | Open-source UEFI firmware | Required for QEMU; temp VARS + `-snapshot` |
| **check-rootfs** | Image validation target | Validates ISO/Joliet/ESP 8.3/system tree/fonts |
| **ci.mk entries** | Local CI | `make check`/`check-lint`/`smoke`/`pipeline`; **no hosted CI** |
| **machine-specific paths** | Need adjustment | Scripts contain `/home/dev/...` or fixed OVMF paths |
| **degradation (WARN)** | Fault policy | Minor errors don't panic; PRs must state which WARNs appeared |

## 9. Filename prefixes

| Prefix | Subsystem | Examples |
|---|---|---|
| `nd_` | NovusDisplay | `nd_hal.c`, `nd_core.c`, `nd_font_uni.c` |
| `nwm_` | Novus-WM | `nwm_proto.h`, `nwm.c` |
| `nuc_` | NUC loader | `nuc.c`, `nuc_spawn_blob()` |
| `nkm_` | NKM modules | `nkm.c`, `nkm_test_module.h` |
| `novu_` | Update system | `novu_verify.c`, `novu_queue.c` |
| `net_` | Network | `net_e1000.c`, `net_ncsi.c` |
| `vfs_` | VFS | `vfs.c`, `vfs_mount.c` |
| `novusfs_` | NovusFS | `novusfs_mount()`, `novusfs_selftest()` |
| `blkdev_` | Block layer | `blkdev_register()`, `blkdev_power_state()` |
| `devtab_` | Device table | `devtab_refresh()`, `devtab_init()` |
| `sys_` | Syscall implementations | `sys_fs_open()`, `sys_ipc_send()` |
| `cmd_` | shell builtins | `cmd_ls()`, `cmd_nuomi()`, `BUILTINS` |
| `k*` | Kernel common | `kmain()`, `kputs()`, `klog.c` |
| `g_*` | Globals | `g_syscalls[]`, `g_apps[]` |

## 10. Easily confused terms

| Confused pair | Difference |
|---|---|
| **`write(2)` vs `fs_write(10)`** | #2 is serial log, **not** file write; file write is #10 |
| **NUC vs NKM** | NUC = user program (unsigned, page-table isolation); NKM = Ring 0 module (Ed25519 signed) |
| **NovusDisplay vs nvgui** | `nd_*` = kernel graphics/renderer; `nvgui` = user-space widgets |
| **Novus-WM vs Mini-DWM** | former = protocol server/window manager; latter = v2 surface redirection/compositor |
| **nwm vs nsh** | `nwm` = WM port; `nsh` = terminal app |
| **nsh vs sinfo/sysinfo** | `nsh` = shell; `sinfo` = IPC client querying kernel info |
| **devmgmt/devmgr/devtab** | app / app module / kernel device table |
| **Nuomi (device) vs Nuomi_pl (host)** | former = `no_std` heapless interpreter; latter = C reference with GC and NUC toolchain |
| **cppcc vs nucgen** | compiler front+codegen vs NUC serializer backend |
| **E2E=1 vs production** | Test anchors only in E2E builds; `make clean` before switching |
| **M0–M12 vs V1.6** | milestones ≠ release versions |
| **Shared log ring vs klog** | 4 KiB boot ring in shared memory tail vs 32 KiB kernel ring |
| **WARN vs failure** | Most self-test failures log WARN and continue; WARN ≠ pass |
| **spec goal vs implemented** | CAP, Secure Boot, TRNG, MicroPython, generic exec are roadmap only |
