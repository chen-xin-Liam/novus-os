# Novus OS Roadmap

> Version: v1.0 (based on 2026-11-27 worktree)
> Companion: [README.md](README.md), [SECURITY.md](SECURITY.md), [CHANGELOG.md](CHANGELOG.md)
> 中文版本：[../ROADMAP.md](../ROADMAP.md) · [Index](INDEX.md)
>
> Inventory of `arch/`, `kernel/`, `fs/`, `security/`, `graphics/`, `wm/`, `apps/`, `user/`, `tools/`, `scripts/`, `templates/`, `configs/`, `assets/`, all `docs/`, `.trae/specs/` and `.codebuddy/plans/`. Describes the **real current state**, known gaps and phased plan.

---

## Contents

1. [Reading rules and authority](#1-reading-rules-and-authority)
2. [Current snapshot](#2-current-snapshot)
3. [Milestone overview (M0–M12+)](#3-milestone-overview)
4. [Implemented capabilities](#4-implemented-capabilities)
5. [Gap analysis](#5-gap-analysis)
6. [Phased roadmap (P0–P5)](#6-phased-roadmap)
7. [Frozen boundaries](#7-frozen-boundaries)
8. [Testing and quality evolution](#8-testing-and-quality-evolution)
9. [Risk register](#9-risk-register)
10. [Maintenance rules](#10-maintenance-rules)

---

## 1. Reading rules and authority

### Fact priority

1. Current source and build results
2. Frozen format specs (NUC/NKM/NUP/INS/IPC/etc.)
3. changes.md / CONTRIBUTING.md
4. README.md (contains early planning text)
5. `.trae/specs/`, `.codebuddy/plans/` (process docs, not specs)

Status marks: **[Done]**, **[Partial]**, **[Declared-unfinished]**, **[Not implemented]**, **[Needs verification]**.

### Known doc/code drift (resolve before planning)

| Item | Conflict | Adopted view |
|---|---|---|
| WM protocol version | docs/header comment say 1.0; `nwm_proto.h:26` is `0x00020000` | **Code 2.0**; doc erratum P0-01 |
| Window limit | spec says 8; code `NWM_MAX_WINDOWS 10` | **10** |
| Minimum RAM | README/defconfig 2 GiB; CONTRIBUTING early 4 GiB | 2 GiB boot refusal, 4 GiB working line |
| NKM header | prose 128 B; `nkm.h` and spec 208 B | **208 B** |
| Update header | kernel frozen 128 B; `novupkg` uses 144 B | **Contract conflict, P0-02** |
| rootfs dir count | header 78; fsinfo/check 77 | **78** is source of truth |
| Multi-monitor | changes.md says unimplemented and implemented in two entries | Mirrored 4 outputs implemented, not independent desktops |
| Win32 capability | CONTRIBUTING says GUI/TLS unimplemented; `win32.c` has GUI/ws2_32 prototypes | Prototype-level coverage |
| NUC stack | kernel 64 KiB–4 MiB; cppcc README 64–256 KiB | **Kernel** |
| M10 status | README line lacks mark but text says implemented | Mostly landed, stability work ongoing |
| A/B cores | README says two-phase required; `smp.c` default 0, defconfig `y` | Enabled by default; SIPI failure degrades to single-core |

---

## 2. Current snapshot

### Hard constraints

- From-scratch, no external open-source kernels/libs; Unifont glyph data is a documented exception.
- 2 GiB minimum (refuse boot below); LowMem 2–6 GiB, Full ≥6 GiB.
- Single syscall-visible VFS root `/`; no `..` traversal; no `..` entries; paths 256 B; I/O 4096 B.
- Built-in apps are flat NUC blobs embedded in the kernel; `no_std`, no allocator; blob <128 KiB.
- nsh is a windowed terminal (144×26 grid), 35 builtins, engine in `apps/src/shell/`.

### Real source map and artifacts

`arch/x86_64/` boot; `kernel/` core+drivers+network+Win32/PE+install; `fs/` VFS/NovusFS/FAT/ISO/ext4; `security/` NKM/TLS/update/CA; `graphics/` display/widgets/desktop/fonts/themes/IME; `wm/novus-wm/` compositor+protocol; `apps/` Rust apps; `user/` Nuomi_pl/cppcc/win32compat/novusd; `tools/` host tools; `scripts/` validation.

No `system/`, `drivers/`, `user/shell/`, `user/micropython/`.

`make` → `out/NOVUS.EFI`; `make iso` → live ISO; `make disk-img` → 64 MiB GPT disk; generators (theme/lang/font/IME/wallpaper/CA/NKM) committed.

### Capability overview

| Area | Status | Evidence |
|---|---|---|
| UEFI boot, splash, cross-phase log | [Done] | `boot/main.c`, `stage3.c`, `klog.c`, `nd_boot.c` |
| A/B two-phase start, AP wake | [Partial] | `smp.c`, AP trampoline; multi-AP missing |
| PMM/MM/heap/per-process paging/COW | [Done] | `pmm.c`, `mm.c`, `heap.c`, `vm.c` |
| IDT/IRQ/LAPIC, preemptive scheduling | [Done] | `idt.c`, `irq.c`, `sched.c`, `trap.c` |
| Syscall ABI (1–7/60 + 8–19/40/DWM) | [Partial] | `g_syscalls[]`; CAP not wired |
| NUC loader | [Partial] | `nuc.c`; unsigned, W^X not fully enforced |
| IPC + Novus-WM + Mini-DWM software compositing | [Partial] | `ipc.c`, `nwm.c`, `dwm.rs` |
| NovusFS (WAL+Merkle+fsck priorities) | [Partial] | WAL errors only WARN; not strict crash consistency |
| VFS mounts, FAT/ISO/ext4 read-only | [Partial] | NTFS enum only |
| Self-developed TLS 1.2/1.3 + CA + updates | [Partial] | Not full WebPKI |
| NKM Ed25519 signing | [Partial] | One dev key, no rotation |
| Network and four QEMU NICs | [Partial] | Single TCP, polling, WiFi detect only |
| xHCI mass storage, HID, touchpad | [Partial] | BOT not UASP, no EHCI |
| 8+ GUI apps, nsh, Nuomi, device manager | [Partial] | apps/src/bin/*, shell, nuomi.rs |
| Graphical installer, i18n, session split | [Partial] | setup.rs, install.c, lang.c |
| Win32 PE32+ prototype | [Partial] | pe.c, pe32.c, wow32.c, win32.c |
| Self-tests/QEMU smoke/baselines | [Partial] | scripts/*; no hosted CI |

---

## 3. Milestone overview

| Milestone | Theme | Status | Unfinished |
|---|---|---|---|
| M0 | Minimal UEFI boot, A-core BSP, VGA text | [Done] | — |
| M1 | Memory manager, paging, SMP wake B core | [Done] | Multi-AP, full dual-core semantics |
| M2 | NovusFS minimal + WAL | [Done (code)] | M2 spec checklists not checked off |
| M3 | Merkle, NKM, ISO Live, installer | [Done] | Key rotation, anti-rollback |
| M4 | Scheduler, process creation, syscalls | [Done] | CAP, W^X, exec, true concurrent pipes |
| M5 | NovusDisplay 2D + widgets | [Done (base)] | Hardware 2D, OpenGL/Vulkan/DX |
| M6 | Novus-WM + IPC | [Done] | Shared-memory path, strong auth, doc update to 2.0 |
| M7 | cppcc C++ subset | [Partial] | C++11 phase 2/3, multi-TU, generic exec |
| M8 | Built-in system apps | [Done] | Settings persistence (RO FS constraint) |
| M9 | Win32 prototype + Win11 Chinese desktop | [Partial] | Arbitrary-path EXE, 32-bit PE32, DLL/TLS/SEH, sandbox |
| M10 | Fluent theme + graphical installer | [Partial] | NTFS/ext4 install, USB hub/UASP, post-install ESP payload |
| M11 | i18n, session split, I/O | [Partial] | USB bulk transfer, independent multi-monitor |
| M12 | Device manager + Mini-DWM | [Done] | `power_state` mostly empty, no user sleep threshold |
| Unnumbered | GPU 2D, native DP/eDP/HDMI, NTFS/ext4 write, DWM HW backend | [Not implemented] | See Phase 3 |

---

## 4. Implemented capabilities

### Boot, memory, processes

- `efi_main` → serial/console → memory self-test (<2 GiB refuse) → `stage3_prepare/menu/load/finish` → `ExitBootServices`; built-in FAT32 reader fallback.
- Boot splash reads 4 KiB log ring in shared memory; kernel inherits into 32 KiB ring.
- `pmm.c` 256 KiB bitmap (~8 GiB); `kmalloc` page-granularity 256 MiB; `vm.c` per-task CR3/COW; `mm.c` UC framebuffer mapping + `wbinvd`.
- `SCHED_MAX_TASKS=10`, 64 KiB ring-0 stacks, 4-tick slice; `sched_create_user_ex()` with ABI whitelist; user exceptions terminate only that process.
- Syscalls: `rax` number, `rdi/rsi/rdx/r10`, negative = error; 8 `open`, 9 `read`, 10 `fs_write`, 11 `close`, 12 `stat`, 13 `opendir`, 14 `readdir`, 15 `mkdir`, 16–19 power/session/setup, 20–24 unlink/rename/mount/umount/mounts.

### Filesystem and storage

- NovusFS V3 (`0x00030000`): 4 KiB blocks, dual superblocks, 1024 inodes × 128 B, 56 B names, 7 direct + single/double indirect; WAL 64 blocks CRC-32 + commit + replay; Merkle 64 B SHA-256 nodes, 64 per block, covering bitmap+inode+data.
- VFS single root + 8-entry mount table; 6 `mounts.json` whitelist paths; FAT12/16/32, ISO9660/Joliet, ext4 read-only backends; rootfs layout 78 directories.
- Block layer unified 512 B sectors; AHCI/NVMe/IDE(PIO)/USB BOT/GPT/devtab; `qtest.sh` covers four disk types.

### Security and updates

- NKM V1.0: 208 B header, SHA-256 with `[0x50,0xB0)` zeroed, Ed25519 over digest, ≤4 keys/16 modules, only `R_X86_64_64`.
- Self-developed TLS 1.3 1-RTT / TLS 1.2 ECDHE+AEAD, X.509 chain, AES-GCM/ChaCha20-Poly1305, P-256/X25519/RSA, CSPRNG.
- novu updates: 128 B frozen header, four magics, three patch types + full `.novu`, five priority queues, 8-step verification, snapshot + atomic replace + reverse rollback + Merkle rebuild, persistent journal; `updatesrv`.
- NUC unsigned, ring-3 isolation; Win32 ABI whitelist (only 40/60).

### Graphics, window, input

- Framebuffer HAL: up to 4 GOP outputs, EDID preferred timing, primary + mirrored rest; offscreen composite → whole-frame present; cursor as overlay; VGA `0x3DA` retrace probe + bounded wait.
- Full Unicode: 114,397 glyphs, 3.5 MB `nd_font_data.bin`, sparse page index; strict UTF-8; `Ctrl+Shift+U` direct code point; `Ctrl+Space` Pinyin IME (single syllable, ≤6 candidates); TTF glyf only.
- Theme tokens (light/dark/time-based), CN/EN switching with persistence, `NWPK` wallpaper (currently gradient fallback due to JPEG decoder UB).
- nvgui pool of 192 widgets; WM protocol 2.0 with 10 windows, 120 shapes/160 texts; frame-end `INVALIDATE`.

### Network and peripherals

- ARP/IP/UDP/gateway forwarding; single TCP client; single-transaction DNS A query (source port 5353); DHCP; HTTP+TLS; NTP; NCSI.
- NICs: e1000/e1000e, I21x, igb (real hardware), legacy virtio-net, rtl8139 — all pass DHCP/DNS/NCSI; unified PCI ID table.
- USB: xHCI 1.1 command/event rings, BOT CBW/CSW READ10/WRITE10, UHCI HID boot, HID/touchpad; `sysinfo` device tree.

### User space and languages

- `apps/src/bin/`: `fileman`, `taskman`, `settings`, `netman`, `session`, `setup`, `nsh`, `devmgmt`, `dwm`; plus `novusd`.
- nsh: 35 builtins (`ls cd pwd cat ... nuomi update novusd run install`), pipes, redirection, `$(...)`, vars, glob, history, aliases, `; && ||`.
- Nuomi: `apps/src/nuomi.rs` no_std heapless port (256 KiB arena, 6 KiB source, ~500 calls); `user/Nuomi_pl/` C reference with GC + NUC toolchain.
- cppcc: host C++ subset compiler with NUC/PE32+/ELF64 backends; C++11 phase 2 in progress.
- novusd: 6 delivery stages; five capabilities degraded on device (file events/process tracing/audit/sandbox/argv).
- Win32: PE32+ at `0x50000000`, DIR64, name-based IAT, 32 B syscall thunks, 141 APIs, simulated registry.

### Tools and validation

- Host tools: mkfat/mkiso/mkfsnovus/mkdisk/nkmgen/novupkg/wpkg/fontgen/langgen/themegen/imegen/wpgen/cabundle/updatesrv/tlstest.
- Self-tests: display/irq/sched/dwm/memory; qemu-smoke + VNC baselines; ci_smoke.py; run-install-test.sh; wm-churn.sh; check_rootfs.py; inspect_img.py.
- Local CI: `make check`/`check-lint`/`smoke`/`pipeline`; no hosted CI.

---

## 5. Gap analysis

### Correctness and consistency

| ID | Gap | Impact | Priority |
|---|---|---|---|
| G-C01 | 12 doc/code drifts (§1) | Planning based on wrong status | **P0** |
| G-C02 | Update header 128 B vs 144 B | Update packages may be rejected | **P0** |
| G-C03 | rootfs count 78 vs 77 | Image validation mismatch | **P0** |
| G-C04 | Scripts hardcode `/home/dev/...` and OVMF paths | Cannot run on other machines/CI | **P0** |
| G-C05 | JPEG wallpaper decoder broken (toolchain UB), gradient fallback | Photo wallpaper unusable | P1 |
| G-C06 | nsh QEMU interaction test not done; install shutdown/reboot unverified | Interaction paths lack regression | P1 |
| G-C07 | Settings changes not persisted (RO FS) | Incomplete UX | P1 |
| G-C08 | Kconfig placeholder, `CONFIG_USER_MOCHI=y` with no submodule | Config/source mismatch | P1 |
| G-C09 | M2 spec checklists not filled; one pending AB-core todo | Process docs stale | P1 |
| G-C10 | `qtest.sh`/`rebuild-kernel.sh` swallow failures with `|| true` | False green regression | P1 |

### Security

| ID | Gap | Impact | Priority |
|---|---|---|---|
| G-S01 | CAP 128-bit model only spec, no `cap_check` | Cannot do least-privilege | **P0/P1** |
| G-S02 | IPC ports lack strong auth | WM not a security boundary | P1 |
| G-S03 | NKM one dev key, no rotation/revocation | No production key governance | P1 |
| G-S04 | No Secure Boot / signed firmware chain | Firmware-layer attacks undefended | P2 |
| G-S05 | TLS not full WebPKI | Cannot claim arbitrary public update service | P1 |
| G-S06 | CSPRNG deterministic fallback, no TRNG | Key material quality limited | P2 |
| G-S07 | NUC segment flags not enforcing W^X | Code injection surface | P1 |
| G-S08 | Merkle excludes WAL/self; WAL replay not strict gate | No full crash consistency | P2 |
| G-S09 | Win32 not a sandbox | Cannot run untrusted EXEs | P2 |
| G-S10 | Update center scheduling/gray-release/blacklist/reporting missing | Management loop incomplete | P1/P2 |
| G-S11 | Hardcoded dev keys in `update.rs`, `novu_verify.h` | Replace before shipping | **P0** |

### Functionality

| ID | Gap | Priority |
|---|---|---|
| G-F01 | Generic `./app.nuc` exec unimplemented; only fixed `APPS/DEMO.NUC` | **P0/P1** |
| G-F02 | nsh missing `grep/find/sort/kill/ps/date/uname/man` etc.; `help <name>` has no detail | P1 |
| G-F03 | Pipelines and `$(...)` sequential capture, no fork/exec | P2 |
| G-F04 | File API lacks `seek`/`truncate`/owner-rwx; cross-backend rename | P1 |
| G-F05 | Installer only NovusFS; no USB hub/UAS/hotplug/USB3; no post-install ESP payload | P1 |
| G-F06 | novusd five capabilities degraded; no kernel file events/audit/enforced sandbox | P1/P2 |
| G-F07 | cppcc phase 2/3 incomplete (classes/templates/exceptions/RTTI/new/float/STL/multi-TU) | P2 |
| G-F08 | Win32: no disk EXE/PE32/DLL/TLS/multithreading/SEH | P2/P3 |
| G-F09 | Nuomi host missing HTTP/process (non-Windows)/ZIP/SHA-1/concurrent download/GUI window | P2/P3 |
| G-F10 | MicroPython only roadmap, no source; Mochi submodule missing | P3 |
| G-F11 | C++98 STL port: 122 imported APIs, ~20 implemented, 85 `msvcrt.dll` missing | P3 |
| G-F12 | Debug manual's `log_print`/`panic_handler`/assert/log persistence absent | P1 |

### Hardware/performance

| ID | Gap | Priority |
|---|---|---|
| G-H01 | Vendor GPU 2D entries all `ENODEV`; CPU only | P2 |
| G-H02 | Mini-DWM HW backend missing | P2 |
| G-H03 | No hardware flip/vblank; tearing not eliminated | P2 |
| G-H04 | Multi-output mirrored only, not independent desktops | P2 |
| G-H05 | Storage: no AHCI NCQ/IRQ completion, NVMe PRP/deeper queues (4), IDE DMA, USB BOT reset recovery, ATAPI, `power_state` | P2 |
| G-H06 | NICs: no igc/r8168/8125/vmxnet3/ne2k/pcnet; virtio 1.0-only | P2 |
| G-H07 | WiFi detection only, no firmware/802.11/WPA2 | P2/P3 |
| G-H08 | No USB3/UASP/hub; EHCI-only high-speed unsupported | P2 |
| G-H09 | Single TCP session, no window scaling/congestion control/listener; DNS single transaction; polling RX | P2 |
| G-H10 | Only A/B cores; SIPI failure degrades; no multi-AP | P2 |
| G-H11 | No measured performance baseline (WC throughput, frame budget, boot time) | P2 |
| G-H12 | No OpenGL/OpenCL/Vulkan/DX11/DX12 | P3 |

---

## 6. Phased roadmap

Exit conditions are mandatory; do not skip.

### Phase 0 — Baseline hardening (P0, release blocking)

Exit: docs/code consistent; update contract unified; hardcoded keys/paths removed; all required checks runnable in a clean environment.

| ID | Task | Deliverable/acceptance |
|---|---|---|
| P0-01 | Doc errata (WM 2.0/10 windows, memory, NKM 208B, dirs 78, multi-monitor, Win32, NUC stack, M10) | Errata PR; README/docs aligned with code constants |
| P0-02 | Unify update header contract (128 vs 144) | Kernel/novupkg/spec share `HDR`; end-to-end `.novu` verify passes |
| P0-03 | Remove hardcoded dev keys/test anchors | Production keys from cert store; E2E isolated from production CA |
| P0-04 | Sync rootfs count and validation tools | `check-rootfs` green |
| P0-05 | De-hardcode script paths | Env vars/repo-relative OVMF/QEMU; machine-path list cleared |
| P0-06 | Decide JPEG wallpaper decoder fate | Fix + QEMU screenshot verification, or remove dead code/resources |
| P0-07 | Add key interaction regressions | QEMU nsh/nuomi, install shutdown/reboot, VNC account/start-menu/app/keyboard |
| P0-08 | Clean config system | Remove/complete `user/mochi` dangling config; Kconfig ↔ defconfig 1:1 |
| P0-09 | Close process docs | Fill M2 checklists; resolve pending AB-core todo |
| P0-10 | Remove false-green gates | No `|| true`; baseline updates explicit + manual review record |

### Phase 1 — Functional closure and developer experience (P1)

Exit: any NUC can be launched from shell; terminal is daily-usable; update/install/security daemon loop closes; debug interfaces land.

| ID | Task | Acceptance |
|---|---|---|
| P1-01 | Generic `./app.nuc` exec | nsh loads NUC from any VFS path; demo slot kept as fallback |
| P1-02 | NUC W^X | TEXT RX, non-TEXT non-X; selftest checks RW segments not executable |
| P1-03 | Fill nsh builtins | At least grep/find/sort/date/uname/ps/kill/sleep/printf/test/exit; `help <name>` details |
| P1-04 | Shell UX | Tab completion, Ctrl+U/K, incremental history, alias persistence; assess concurrent pipes |
| P1-05 | File API completion | `seek`/`truncate`/unlink + owner/rwx model; ext4 rename |
| P1-06 | CAP least privilege | `SYS_CAP`/`cap_check()` in `g_syscalls[]` + user headers; default-minimum privileges |
| P1-07 | IPC port auth | Port registration/lookup capability checks; WM sole `"nwm"` server |
| P1-08 | NKM multi-key/rotation | `key_id` or equivalent, runtime import/revocation with admin session auth |
| P1-09 | TLS/WebPKI key gaps | CRL/OCSP hard-fail, name constraints, path length, full keyUsage; public hostname E2E |
| P1-10 | Update center policy | Idle/night scheduling, tiered retry/blacklist, server gray release/reporting; settings subpages |
| P1-11 | Installer capabilities | NTFS/ext4 writable target (or remove "planned" options); USB hub/UASP/hotplug/USB3; post-install ESP payload |
| P1-12 | novusd full capabilities | Kernel file events/audit source/enforced sandbox/argv; close five degradations |
| P1-13 | Debug/log interfaces | `log_print`/level filtering/`panic_handler`/assert/log persistence; update Debug manual |
| P1-14 | Settings persistence | User config area on writable NovusFS; theme/lang/wallpaper persist without breaking Merkle |
| P1-15 | Installer UX closure | A-core switch/memory mode/display output cards tied to real kernel settings |

### Phase 2 — Security hardening and reliability (P1/P2)

Exit: security boundary is clearly described; power-loss/crash behavior has verifiable guarantees.

- P2-01 Secure Boot / signed firmware chain.
- P2-02 CSPRNG/hardware TRNG (remove deterministic fallback outside self-test).
- P2-03 Merkle/WAL crash consistency (WAL replay strict gate, anti-rollback counter, fsck automation).
- P2-04 Win32 sandbox hardening (full isolation, safe DLL loading, SEH, threads, file perms).
- P2-05 Persist user-space crash reports to NovusFS logs + GUI popup.
- P2-06 Unattended regression matrix (`make pipeline` as merge gate).

### Phase 3 — Hardware, graphics and performance (P2)

Exit: usable frame rate and correct display on real hardware; major hardware classes covered.

- P3-01 Vendor GPU 2D backend (GTT/ring/blit for at least one vendor, CPU fallback).
- P3-02 Mini-DWM HW compositing backend.
- P3-03 True vblank / hardware flip, tear-free with measurable metric.
- P3-04 Independent multi-monitor workspaces, EDID and DP/eDP/HDMI identification.
- P3-05 Storage: AHCI NCQ/IRQ, NVMe PRP/deeper queues, IDE DMA, BOT reset recovery, ATAPI, `power_state`, user sleep threshold.
- P3-06 NICs: igc, r8168/8125, vmxnet3/ne2k/pcnet, virtio 1.0; igb without LU.
- P3-07 Wireless: firmware loading, 802.11 association, WPA2, scan/connect UI.
- P3-08 USB3/UASP/hub; EHCI path.
- P3-09 General network stack: multiple TCP connections, window scaling, congestion control, listener, concurrent DNS, interrupt RX.
- P3-10 Multi-AP SMP with topology and load balancing.
- P3-11 Performance baselines (real-hardware WC throughput, frame budget, boot time) in regression.

### Phase 4 — Language, compatibility layer and ecosystem (P2/P3)

Exit: developers can write/distribute apps without reading the kernel; language/compat boundaries documented.

- P4-01 cppcc C++11 phase 2 (classes/ctors/access control, inheritance/vtables, templates, full overload resolution, multi-TU, trailing return, `constexpr`).
- P4-02 cppcc phase 3 (exceptions/RTTI/new/delete/float/minimal STL runtime).
- P4-03 `nucdump` tool for any NUC.
- P4-04 Nuomi host capabilities (HTTP/process/ZIP/SHA-1/concurrent download/GUI window; macros/modules/exceptions).
- P4-05 Win32 expansion (arbitrary-path EXE, PE32 executor, real DLL mapping, TLS/SEH, more APIs).
- P4-06 C++98 port compatibility (122 APIs prioritized; nested exception propagation and `catch` type matching).
- P4-07 MicroPython runtime (Issue first for self-development boundary and NUC packaging).
- P4-08 Mochi toolchain (submodule or remove config).
- P4-09 NUP/INS consumption closure (app installation/shortcut creation/removal end-to-end).
- P4-10 Templates and SDK (IPC/network/update/theme templates).

### Phase 5 — Long-term vision (P3)

P5-01 3D/performance graphics APIs (OpenGL/Vulkan/DX subsets); P5-02 full OOM/priority reclamation; P5-03 multi-user and file permissions; P5-04 app store and signed distribution; P5-05 full crash-consistent filesystem; P5-06 platform CI, reproducible builds, release branches with security backports.

---

## 7. Frozen boundaries

| Spec | Version / frozen point | Change rule |
|---|---|---|
| NUC | V1.0, 48 B header, `NUC!`, ABI `0x0100` | Field/semantic change → ABI RFC + major bump |
| NKM | V1.0, 208 B header, SHA-256 with `[0x50,0xB0)` zeroed, Ed25519 | Sync static assertions, nkmgen, spec, docs |
| NUP/INS | V1.0 frozen | Any extension bumps Version |
| WM IPC / kernel IPC | code `0x00020000`; magic `0x4E495043`, 32 B header, payload ≤128 B | No private message/layout changes |
| Debug interface | file V1.0 / content V1.1 | Parameter/error-code level frozen |
| Update patches | 128 B header, four magics, three patch types + `.novu`, five channels permanently frozen | Fix contract conflict (P0-02), not the frozen definition |
| rootfs layout | `fs/novusfs/rootfs_layout.h` truth | Sync all images and checks |
| Shared memory | 8 KiB @ `0x7E000`, NVSH v1.1 | New fields only in `0x2F0` reserved tail |
| Syscalls | Sparse frozen numbers | Append only; sync user headers |

ABI change PR must include: spec, kernel impl, host tools, test fixtures, version number, compatibility note.

---

## 8. Testing and quality evolution

| Phase | Goal | Actions |
|---|---|---|
| Phase 0 | Reproducible | De-hardcode paths; remove `|| true`; inject OVMF/QEMU via params; `check-rootfs` green |
| Phase 1 | Coverage | nsh/Nuomi interaction, post-install shutdown/reboot, CAP rejection cases, update header consistency; generator `--check` gate |
| Phase 2 | Trustworthy | `make pipeline` as merge gate; screenshot baseline manual review; "bad input rejected" regression for security fixes |
| Phase 3 | Quantified | Real-hardware FPS/throughput/boot-time baselines; hardware matrices with JSON results |
| Phase 4 | Releasable | Hosted CI, clean rebuilds, release branches, security backport policy (or explicitly none) |

Existing entry points: display/irq/dwm selftests, qemu-smoke.sh, qemu_smoke_test.py, ci_smoke.py, run-install-test.sh, wm-churn.sh, check_rootfs.py, inspect_img.py, novupkg/nkmgen/updatesrv selftests, cppcc cargo test.

---

## 9. Risk register

| Risk | Trigger | Mitigation |
|---|---|---|
| Frozen ABI conflict | New feature needs format change | Append fields/messages; RFC + six-piece sync |
| Toolchain UB | JPEG decoder class of problem | Host stub comparison + kernel-side output validation; QEMU pass ≠ real hardware |
| Real-hardware corruption/stall | Framebuffer cache attributes, WB/UC alias | Always `MM_MAP_UC` + `wbinvd`; real-hardware verification |
| Update package rejection | 128/144 header conflict, test root contamination | P0-02/P0-03; E2E vs production CA isolation |
| Stale docs mislead implementation | README retains early plans | CONTRIBUTING priority rule; errata per change |
| Over-claimed security | Treating CAP/Secure Boot/WebPKI as implemented | SECURITY.md is truth; no relaxed checks in PRs |
| Baseline drift | Arbitrary `--update-baseline` | Forbidden in normal regression; manual per-image review |
| Closed vendor hardware | GPU/wireless firmware undocumented | Keep software/degraded paths; never "pretend to work" |
| Resource limit breach | blob >128 KiB, stack >4 MiB, display list overflow | Build/runtime assertions; apps.stamp and nwm_proto.h constants as gates |
| No allocator constraint | `Vec`/`String`/`Box` in apps | Template and code review; fixed buffers |

---

## 10. Maintenance rules

1. Every status change must include evidence (source path, build/self-test command and output).
2. At each phase exit, update the milestone table, gap table (remove/downgrade) and task table (mark completion date), and append to `changes.md` in "topic → path-level bullets → verification" format.
3. Never treat spec text as implementation; if no function/config/build target exists, mark [Declared-unfinished] or [Needs verification].
4. P0 is release-blocking; P1/P2 follow dependency order.
5. Frozen-format changes do not bypass §7.
6. Security truth is [SECURITY.md](SECURITY.md); this roadmap must not call spec goals current capabilities.
7. The drift list (§1) converges with errata — record which file is authoritative when closing each item.
