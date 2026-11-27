# Novus OS Architecture

> Version: v1.0 (2026-11-27, based on current worktree)
> Companion: [README.md](README.md), [ROADMAP.md](ROADMAP.md), [COMPATIBILITY.md](COMPATIBILITY.md)
> 中文版本：[../ARCHITECTURE.md](../ARCHITECTURE.md) · [Index](INDEX.md)
>
> Describes the **real architecture**: layering, dependency direction, data flows, ABI boundaries and key constants. Mermaid diagrams render in Mermaid-capable Markdown viewers.

---

## Contents

1. [Design principles and layering](#1-design-principles-and-layering)
2. [Boot architecture](#2-boot-architecture)
3. [Kernel architecture](#3-kernel-architecture)
4. [Filesystem and storage](#4-filesystem-and-storage)
5. [Graphics, windows and compositing](#5-graphics-windows-and-compositing)
6. [Network and security](#6-network-and-security)
7. [User space](#7-user-space)
8. [Build and images](#8-build-and-images)
9. [End-to-end data flows](#9-end-to-end-data-flows)
10. [Constraint quick reference](#10-constraint-quick-reference)

---

## 1. Design principles and layering

### Hard constraints

1. **Self-developed only**: no external open-source kernels/libs (Unifont glyph data is a documented exception).
2. **Unidirectional dependencies**: top-down calls only; no cycles.
3. **Frozen ABI**: NUC/NKM/NUP/INS/IPC/Debug fields and versions frozen; change requires RFC + version bump.
4. **Single-root VFS**: one syscall-visible `/`; mounts switch backends by prefix; no `..` entries.
5. **Isolation**: NUC uses ring-3 page tables (unsigned); NKM is Ring 0 and must be Ed25519-signed.
6. **Degradation**: user exceptions kill only the process; most kernel self-test failures are WARN, not panic.

```mermaid
flowchart TB
    subgraph APP["User space (ring 3)"]
        A1["Built-in apps<br/>fileman / taskman / settings<br/>netman / session / setup / devmgmt / dwm / nsh"]
        A2["Runtime<br/>apps/src: ui wm sys sinfo fs<br/>shell nuomi devmgr install"]
        A3["Toolchain<br/>cppcc / Nuomi_pl / win32compat / novusd"]
        A1 --> A2 --> A3
    end
    subgraph ABI["Syscall ABI (g_syscalls[])"]
        S1["1-7 yield/write/ipc<br/>8-15 / 20-24 files"]
        S2["16-19 power/session<br/>25-27 update/https/console<br/>28-37 DWM"]
        S3["40 Win32 / 41 PE32"]
    end
    subgraph KERN["Kernel (ring 0)"]
        K1["scheduler/trap/IRQ/IDT"]
        K2["memory: pmm/mm/heap/vm"]
        K3["FS: VFS + NovusFS(WAL,Merkle)<br/>FAT / ISO9660 / ext4"]
        K4["devices: PCI/AHCI/NVMe/IDE/USB<br/>NICs / HID / power"]
        K5["graphics: NovusDisplay / nvgui / nwm<br/>Mini-DWM / gpu2d"]
        K6["network & security<br/>IP/UDP/TCP/DNS/DHCP/HTTP<br/>TLS / NKM / novu / CA"]
    end
    subgraph HW["Hardware / firmware"]
        H1["UEFI: GOP / Block I/O / memory map"]
        H2["x86_64: LAPIC / paging / syscall"]
        H3["storage / NICs / USB / input / framebuffer"]
    end
    APP -->|syscall| ABI --> KERN -->|drivers| HW
    H1 -->|handoff| KERN
```

Directory-to-layer map: `arch/x86_64/` boot; `kernel/` core; `fs/` filesystems; `security/` signing/TLS/update; `graphics/` display; `wm/novus-wm/` compositor; `apps/` Rust apps; `user/` language/tools; `tools/` host tools; `scripts/` validation.

> No `system/`, `drivers/`, `user/shell/`, `user/micropython/` — those were early plans.

---

## 2. Boot architecture

### A/B two-phase boot

A core (BSP) does firmware handoff and integrity self-check then sleeps; B core (AP) loads the main kernel. `CONFIG_NOVUS_A_CORE_DISABLE` skips A-core checks; SIPI failure degrades to single-core.

```mermaid
sequenceDiagram
    autonumber
    participant FW as UEFI firmware
    participant A as A core / BSP
    participant SHM as Shared memory 8 KiB @ 0x7E000
    participant B as B core / AP
    participant K as kmain

    FW->>A: efi_main
    A->>A: serial/console, memory scan (<2 GiB refuses)
    A->>A: stage3_prepare: block devices + ESP
    A->>SHM: hardware info / self-check / Merkle root
    A->>A: stage3_menu + stage3_load
    Note over A: Merkle verify, stage live root / ESP.IMG / NUC / wallpaper
    A->>B: wake AP (SIPI → ap_trampoline.S)
    A->>FW: ExitBootServices
    B->>K: kernel_entry → kmain()
    K->>SHM: shared_mem_verify() (NVSH v1.1)
    K->>K: PMM → paging → heap → display → devices
    K->>K: NovusFS/VFS → NKM → update/CA
    K->>K: LAPIC → IPC → scheduler/IRQ → tasks
    K->>A: park A core
    K->>K: sched_start()
```

### Shared memory handoff

8 KiB @ `0x7E000`, layout frozen; new fields only appended to `0x2F0` reserved tail. Contains: hardware info / memory map, self-check status, Merkle root, `live_root_base/size`, `esp_img_base/size/install_flags`, `wpkg_img_base/size`, NUC demo image, 4 KiB log ring.

### kmain init order (hard constraint)

```mermaid
flowchart TD
    S0["shared_mem_verify()"] --> S1["pmm_init + selftest<br/>(page tables come from PMM)"]
    S1 --> S2["mm_init + selftest<br/>low-4GiB identity + physmap<br/>framebuffer UC + wbinvd"]
    S2 --> S3["heap_init + selftest"]
    S3 --> S4["nd_init / splash<br/>no framebuffer → halt"]
    S4 --> S5["power / ahci / nvme / usb / ide<br/>blkdev idle timeout"]
    S5 --> S6["devtab_init + touchpad_probe<br/>(only safe PCI BAR scan moment)"]
    S6 --> S7["ipc_init + selftest<br/>(task inboxes depend on it)"]
    S7 --> S8["NovusFS selftest / live root mount<br/>fonts / icons / VFS / ext4 selftest"]
    S8 --> S9["mountcfg_apply() / mounts.json"]
    S9 --> S10["Merkle verdict / NKM / update / CA"]
    S10 --> S11["lapic_init + smp_wake_ap"]
    S11 --> S12["sched / IRQ / network / lang / session / NUC tasks / WM"]
    S12 --> S13["sched_start() (never returns)"]
```

Why order is fixed: `pmm_init` before `mm_init` (page-table pages); NovusFS self-test and live-root mount before AP wake (no second core touching block layer/WAL); `devtab_init` after storage drivers and before network (BAR probing writes `0xFFFFFFFF`).

---

## 3. Kernel architecture

```mermaid
flowchart TB
    CORE["kernel.h / kputs (VGA + COM1 + klog + fbcon)"]
    subgraph BASE["Base layer"]
        PMM["pmm.c: physical page bitmap"]
        MM["mm.c: paging / UC-WB / huge pages"]
        HEAP["heap.c: page-granularity kmalloc"]
        VM["vm.c/vmm.c: per-process space / COW"]
        IRQ["idt/gdt/irq/trap"]
    end
    subgraph MID["Middle layer"]
        SCHED["sched.c: preemptive tasks / ABI kinds"]
        IPC["ipc.c: ports / queues / checksums"]
        BLK["blkdev + GPT + mountcfg"]
        VFS["fs/vfs.c + vfs_mount.c"]
        NUC["nuc.c: NUC loader"]
        NKM["security/nkm: signed modules"]
    end
    subgraph HIGH["Service layer"]
        SESS["session/sysinfo/lang"]
        NET["net*.c: IP/UDP/TCP/DNS/DHCP/HTTP/NTP/NCSI"]
        WM["wm/novus-wm: windows / compositing"]
        DWM["gpu/gpu2d/vidmem/crtc/dwmsys"]
        INSTALL["install / power"]
        WIN32["pe/pe32/wow32/win32"]
    end
    CORE --> BASE
    PMM --> MM --> HEAP --> VM
    MM --> IRQ --> SCHED --> IPC
    BLK --> VFS --> NUC
    HEAP --> NKM
    SCHED --> SESS
    VFS --> SESS
    VFS --> INSTALL
    IRQ --> NET --> SESS
    IPC --> WM --> DWM
    VFS --> WIN32
    SCHED --> WIN32
```

### Memory

| Region | Mechanism | Key constants |
|---|---|---|
| Physical pages | `pmm.c` 256 KiB bitmap, ~8 GiB | `kernel/pmm.c` |
| Kernel heap | `kmalloc` 4 KiB rounding, ≤256 MiB / 256 records | `kernel/heap.c` |
| Heap window | `0xFFFF810000000000`, 256 MiB, shared CR3s, supervisor-only | CONTRIBUTING §5.4 |
| Process space | Per-task CR3; COW; only TEXT executable | `kernel/vm.c`, `nuc.c` |
| Kernel stack | 64 KiB ring-0 per task | `kernel/sched.h` |
| NUC stack | top `0xA0000000`, trampoline `0x90000000`, header-clamped 64 KiB–4 MiB | CONTRIBUTING §5.4 |
| Shared area | 8 KiB @ `0x7E000`, NVSH v1.1 | `kernel/shared_mem.h` |

### Task state machine

```mermaid
stateDiagram-v2
    [*] --> Created: sched_create_user_ex / nuc_load / pe32_exec
    Created --> Runnable: CR3 + kernel stack + trap frame + inbox
    Runnable --> Running: timeslice (default 4 ticks)
    Running --> Runnable: timeslice exhausted (LAPIC timer preempts)
    Running --> Blocked: ipc_recv (RIP rewound 2 bytes)
    Blocked --> Runnable: message arrives
    Running --> Exited: SYS_EXIT / user exception
    Exited --> [*]
    note right of Running
      SCHED_MAX_TASKS = 10
      TASK_ABI_WIN32 allows only SYS_WIN32 / SYS_EXIT
    end note
```

### Syscall ABI

`syscall` → `kernel/trap.c` → `g_syscalls[]` → `sys_*`. Convention: `rax` number, `rdi/rsi/rdx/r10` args, negative return = error. User pointers copied page-by-page; unknown numbers logged once per number then refused. Full table in [ABI.md](ABI.md) §2.

---

## 4. Filesystem and storage

```mermaid
flowchart TB
    SYS["syscall layer: path ≤256 B, I/O ≤4096 B, fd from 3"]
    VFS["VFS dispatch: lookup/creat/mkdir/unlink/read/write/stat/readdir/sync/rename"]
    MOUNT["Mount table: 8 entries, absolute unique paths, / cannot unmount, cross-mount rename refused"]
    ROOT["vfs_root() single root; no .. traversal; no .. entries"]
    SYS --> VFS --> MOUNT --> ROOT
    ROOT --> N["NovusFS (writable): dual superblock + WAL + Merkle"]
    ROOT --> E["ext4: read + mkfs + mount"]
    ROOT --> F["FAT12/16/32 (RO, 8.3 names)"]
    ROOT --> I["ISO9660/Joliet (RO, Joliet preferred)"]
    ROOT --> NT["NTFS: enum only, no backend"]
    CFG["/.novusfs/mounts.json ≤4 KiB / ≤8 / whitelist 6<br/>read at boot, no runtime write-back"] -.-> MOUNT
```

Mount whitelist (`rootfs_layout.h:138-144`): `/`, `/disk0/1`, `/media/cdrom0/usb0/usb1`. Runtime `mount`/`umount` do not write back `mounts.json` (would break Merkle).

NovusFS: magic `NOVS`, version `0x00030000`, 4 KiB blocks, 1024 inodes × 128 B, 56 B names, 7 direct + single/double indirect; WAL 64 blocks with CRC-32 and commit; Merkle 64 B SHA-256 nodes, 64 per block, covering bitmap+inode+data (not WAL or itself).

```mermaid
flowchart LR
    subgraph DISK["NovusFS volume"]
        SB1["Superblock primary"] --- WAL["WAL: 64-block ring, CRC-32, commit"] --- MR["Merkle: 64 B nodes, 64/block"] --- BM["block bitmap"] --- IN["inode table"] --- DATA["data blocks"] --- SB2["Superblock backup"]
    end
```

Block device stack: `blkdev` abstracts 512 B sectors with UEFI bounce; AHCI, NVMe, IDE(PIO), USB xHCI+BOT, UHCI HID-only and memimg (live root staged by boot) backends.

---

## 5. Graphics, windows and compositing

```mermaid
flowchart TB
    subgraph CLIENT["User-space GUI apps (apps/)"]
        UI["ui.rs (Novus Fluent) page/card/nav/row/button/progress"]
        WMCL["wm.rs (v1.x display list + v2 surface messages)"]
        UI --> WMCL
    end
    subgraph IPC["Kernel IPC"]
        P["port nwm: 32 B header, magic 0x4E495043<br/>FNV-1a, payload ≤128 B, queue 8, ports 16"]
    end
    subgraph SERVER["Kernel services"]
        NWM["Novus-WM 2.0 (0x00020000)<br/>10 windows, Z-order, focus, input routing<br/>CLEAR/SHAPE/TEXT → INVALIDATE"]
        DWM["Mini-DWM: vidmem / CRTC/VSync<br/>surface redirection / flip / thumbnails"]
        HW2D["gpu2d: CPU software backend only"]
        NWM --> DWM --> HW2D
    end
    subgraph GFX["NovusDisplay"]
        HAL["nd_hal.c: GOP framebuffer (≤4 outputs)<br/>EDID, mirroring, UC mapping"]
        CORE["nd_core.c: rect/line/circle/rounded/gradient/blend"]
        COMP["Offscreen nd_canvas → nd_present<br/>cursor overlay + damage"]
        TXT["Fonts/Unicode/IME: 114,397 glyphs<br/>Pinyin IME, Ctrl+Shift+U"]
        TH["Themes / i18n / wallpaper NWPK"]
        HAL --> CORE --> COMP
        TXT --> CORE
        TH --> CORE
    end
    CLIENT -->|IPC| IPC --> SERVER -->|draw| GFX -->|framebuffer| SCREEN["Screen / mirrored outputs"]
```

### One GUI frame

```mermaid
sequenceDiagram
    participant App as App (e.g. fileman)
    participant W as WM port nwm
    participant I as Kernel inbox
    participant C as Compositor
    App->>W: NWM_REQ_CLEAR (0x010D)
    loop ≤120 shapes
        App->>W: NWM_REQ_SHAPE (0x010E)
    end
    loop ≤160 texts (≤96 B each)
        App->>W: NWM_REQ_TEXT (0x010F)
    end
    App->>W: NWM_REQ_INVALIDATE (0x0110)
    W->>C: draw client area from display list
    C->>C: offscreen composite → retrace wait → present
    W-->>I: NWM_EV_KEY etc.
    I-->>App: event drives next frame
```

### Mini-DWM v2 surface redirection

v2 messages are additive — v1.x clients keep display lists:

```mermaid
flowchart LR
    A["v2 client"] -->|SYS_VMEM_ALLOC| V["vidmem object"]
    A -->|SURFACE_CREATE + handle| W["WM / Mini-DWM"]
    A -->|write pixels| V
    A -->|FLIP_PRESENT + new handle| W
    W -->|zero-copy front-buffer swap| C["CRTC / compositor"]
    C -->|present| FB["framebuffer"]
    A2["v1.x client"] -->|CLEAR/SHAPE/TEXT/INVALIDATE| W
```

---

## 6. Network and security

```mermaid
flowchart TB
    APP["User space: netman / nsh / novusd / Win32 ws2_32"]
    SYS["SYS_HTTPS(26) / sysinfo network queries"]
    HTTP["net_http.c: single-session HTTP client"]
    TLS["security/tls: TLS 1.3 1-RTT / TLS 1.2 ECDHE+AEAD<br/>X.509 chain, AES-GCM, ChaCha20-Poly1305"]
    L4["net_tcp.c (single connection)<br/>net_dns.c (single transaction, port 5353)"]
    L3["net.c: ARP/IP/UDP/gateway<br/>net_dhcp/net_ntp/net_ncsi"]
    DRV["net_ids.c: unified PCI ID table"]
    NIC["e1000/e1000e/I21x/igb<br/>virtio-net(legacy)/rtl8139/wifi(detect only)"]
    APP --> SYS --> HTTP --> TLS --> L4 --> L3
    L3 --> DRV --> NIC
    NIC -->|net_tick poll, ≤8 frames/tick| L3
```

```mermaid
flowchart TB
    subgraph TRUST["Trusted side"]
        FW["UEFI boot + Merkle check (A core)"]
        KR["Kernel Ring0: page tables / syscalls / IPC / block layer"]
        NKM["NKM: SHA-256 with hash/sig zeroed + Ed25519"]
        UPD["Update: 128 B frozen header + SHA-256 + Ed25519"]
    end
    subgraph UNTRUST["Untrusted side"]
        NUC["NUC: unsigned, ring3 page-table isolation"]
        W32["Win32 EXE: ABI whitelist 40/60 only"]
        IPC_C["Ordinary IPC clients (no strong port auth)"]
    end
    FW -->|integrity verdict in shared memory| KR
    NKM -->|signed modules only| KR
    UPD -->|8-step verify → snapshot → atomic replace| KR
    KR -->|syscall boundary + page-wise user copy| NUC
    KR -->|whitelist thunks| W32
    KR <-->|port messaging, not a security boundary| IPC_C
```

Spec goals not yet landed: CAP 128-bit least privilege, Secure Boot, strong IPC port auth, runtime key rotation, hardware TRNG. See [SECURITY.md](SECURITY.md) §5.

---

## 7. User space

`apps/` is `no_std`, allocator-free, dependency-free: fixed arrays/slices; panic prints COM1 and `exit(3)`; flat image linked at `0x400000`; `objcopy -O binary` → `xxd -i` → `k_app_<name>`.

```mermaid
flowchart TB
    subgraph BINS["apps/src/bin/ (embedded NUC blobs)"]
        B1["fileman / taskman / settings / netman"]
        B2["session / setup / nsh / devmgmt / dwm"]
        B3["novusd (from user/novusd)"]
    end
    subgraph LIB["apps/src/ shared library"]
        L1["sys.rs: raw syscall wrappers"]
        L2["fs.rs: file syscall client"]
        L3["sinfo.rs: sysinfo service (port sys)"]
        L4["wm.rs + ipc.rs: WM protocol client"]
        L5["ui.rs + theme.rs + lang.rs + unicode_data.rs"]
        L6["shell/: mod cmds parse path update crypto"]
        L7["nuomi.rs / devmgr/ / dwm/ / install.rs"]
    end
    BINS --> LIB
    LIB -->|SYS_*| K["kernel syscalls"]
    LIB -->|port sys| S["kernel/sysinfo.c"]
    LIB -->|port nwm| W["Novus-WM"]
```

Registration chain (four edits to add an app): `apps/src/bin/name.rs` → `apps/Cargo.toml [[bin]]` → `Makefile APPS_NAMES` → `kernel/apps.c g_apps[]` **append at end** → optional `graphics/nd_demo.c` desktop entry.

nsh: keyboard events → shell line editor/history/vars/aliases → `parse.rs` (tokens, quotes, glob, redirection, pipes, `$(...)`) → 35 builtins writing via `out.write()` → display list. External apps launched via `sinfo::spawn_name()` — no fork/exec.

Multi-language execution: cppcc → NUC/PE32+/ELF64; boot-stage reads fixed `APPS/DEMO.NUC`; generic `./app.nuc` exec not implemented. nsh `nuomi` command runs `apps/src/nuomi.rs` directly.

---

## 8. Build and images

```mermaid
flowchart TB
    CFG["configs/novus_defconfig (Kconfig declaration-only)"]
    SRC["C sources + Rust kernel staticlib (msvc target → COFF) + Rust apps (unknown-none → flat blobs)"]
    GEN["Generators: theme/lang/font/IME/wallpaper/CA/NKM"]
    CFG --> MK["Top-level Makefile: explicit SRC/ASM_SRC/INC"]
    SRC --> MK
    GEN --> MK
    MK --> EFI["out/NOVUS.EFI (PE32+ subsystem 10, entry efi_main)"]
    MK --> APPS["out/apps/<name>.bin → k_app_<name>"]
    MK --> TOOLS["mkfat/mkiso/mkfsnovus/mkdisk/nkmgen/novupkg/wpkg"]
    EFI --> IMG["out/novus.img (8 MiB NovusFS)"]
    APPS --> IMG
    TOOLS --> IMG
    IMG --> ESP["out/esp.img (31.5 MiB) / esp-tpl.img (16 MiB)"]
    ESP --> ISO["out/novus-live.iso (ISO9660+Joliet+El Torito+isohybrid)"]
    IMG --> DISK["out/novus-disk.img (64 MiB GPT+ESP+NovusFS)"]
    ISO --> CHECK["make check-rootfs"]
    DISK --> CHECK
```

El Torito has a 16-bit sector count ⇒ boot item <32 MiB; live ESP = 31.5 MiB. Full details in [BUILD.md](BUILD.md).

---

## 9. End-to-end data flows

### Key press to screen pixel

```mermaid
sequenceDiagram
    participant HW as keyboard/mouse
    participant IRQ as IRQ / nd_input
    participant W as Novus-WM
    participant SH as nsh inbox
    participant SHL as shell engine
    participant K as kernel syscalls
    participant C as compositor
    HW->>IRQ: scancode (with Ctrl/Alt)
    IRQ->>W: input event (IME handles composition first)
    W->>SH: NWM_EV_KEY (Unicode code point)
    SH->>SHL: line edit → parse → builtins
    SHL->>K: syscall
    K-->>SHL: result → out.write()
    SHL->>W: CLEAR/SHAPE/TEXT/INVALIDATE
    W->>C: draw from display list
    C->>C: composite → retrace wait → present
    C-->>HW: pixels updated
```

### File open to disk I/O

```mermaid
sequenceDiagram
    participant A as app fs.rs
    participant T as kernel/trap.c
    participant V as VFS + mount table
    participant N as NovusFS
    participant B as blkdev
    participant D as AHCI/NVMe/USB
    A->>T: SYS_OPEN(path, flags) (page-wise user copy)
    T->>V: longest-prefix mount selection (≤256 B)
    V->>N: component-wise lookup (no .. traversal)
    N->>B: read superblock/inode/data blocks
    B->>D: 512 B sector I/O (UEFI bounce)
    D-->>B: data
    B-->>N: blocks
    N-->>V: inode
    V-->>T: fd (from 3)
    T-->>A: fd
```

---

## 10. Constraint quick reference

| Constraint | Value | Source |
|---|---|---|
| Tasks / kernel stack / timeslice | 10 / 64 KiB / 4 ticks | `kernel/sched.h` |
| Path / I/O / fd base | 256 B / 4096 B / 3 | `kernel/trap.c:78-80` |
| Mounts / whitelist | 8 / 6 entries | `fs/vfs_mount.h:17`, `rootfs_layout.h` |
| NovusFS block/inode/name | 4 KiB / 1024×128 B / 56 B | `fs/novusfs/novusfs.h` |
| WAL / Merkle | 64 blocks / 64 B nodes, 64/block | `fs/novusfs/novusfs.h` |
| IPC header/payload/queue/ports | 32 B / ≤128 B / 8 / 16 | `kernel/ipc.h` |
| WM version/windows/shapes/texts | `0x00020000` / 10 / 120 / 160 | `nwm_proto.h:26-42` |
| GOP outputs / glyphs | 4 / 114,397 | `graphics/nd_hal.c`, `nd_font_uni.c` |
| App blob / stack | <128 KiB / 64 KiB–4 MiB | CONTRIBUTING §7.3 |
| nsh history/vars/aliases/pipes | 64/32/16/≤8 stages | `apps/src/shell/mod.rs` |
| Nuomi source/arena/depth | 6 KiB / 256 KiB / ~500 calls | `apps/src/nuomi.rs` |
| Live ESP | <32 MiB (31.5 MiB) | `tools/tools.mk` |

Known boundaries (see ROADMAP §4): two-core SMP only; single TCP/DNS-transaction and polling network; CPU-only 2D, no vblank, mirrored multi-output; IDE PIO/NVMe queue depth 4/USB BOT; CAP/Secure Boot/port auth/key rotation unimplemented; no generic `./app.nuc` exec; Win32 prototype.
