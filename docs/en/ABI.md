# Novus OS ABI Reference (Implemented Parts)

> This document lists only **implemented and frozen** interfaces. Planned/prototype/unlanded capabilities are excluded.
> Sources of truth: `kernel/trap.c`, `kernel/ipc.h`, `wm/novus-wm/nwm_proto.h`, `kernel/nuc.h`, `security/nkm/nkm.h`, `fs/vfs.h`, `kernel/shared_mem.h`.
> Full matrix: [COMPATIBILITY.md](COMPATIBILITY.md); history: [CHANGELOG.md](CHANGELOG.md); style: [CODESTYLE.md](CODESTYLE.md).
> 中文版本：[../ABI.md](../ABI.md) · [Index](INDEX.md)

---

## 1. Syscall calling convention

| Item | Convention |
|---|---|
| Entry | `syscall` instruction, vector `0x100` |
| Call number | `rax` |
| Args | `rdi`, `rsi`, `rdx`, **`r10`** (4th arg must be `r10`, not `rcx`) |
| Return | `rax`; negative = error, zero/positive = success or length |
| User pointers | Never dereferenced directly; copied page-by-page under the task's CR3 |
| Path limit | 256 bytes |
| Single I/O | 4096 bytes |
| fd base | 3 |
| Unknown numbers | Logged once per number, then refused |

## 2. Implemented syscalls

The single dispatch table is `g_syscalls[]` in `kernel/trap.c`. Numbers are sparse and frozen; **append only, never reuse or reorder**.

| No. | Name | Notes |
|---|---|---|
| 1 | `yield` | Yield CPU |
| 2 | `write` | Serial log output (**not** file write) |
| 3 | `getpid` | Get pid |
| 4 | `ipc_send` | Send IPC message |
| 5 | `ipc_recv` | Receive; blocking relies on rewinding return-frame RIP by 2 bytes |
| 6 | `ipc_lookup` | Look up port by name |
| 7 | `ipc_stats` | IPC stats |
| 8 | `open` | Open file |
| 9 | `read` | Read file |
| 10 | `fs_write` | Write file |
| 11 | `close` | Close fd |
| 12 | `stat` | File info |
| 13 | `opendir` | Open directory |
| 14 | `readdir` | Enumerate directory |
| 15 | `mkdir` | Create directory |
| 16 | `power_off` | Power off |
| 17 | `reboot` | Reboot |
| 18 | `session` | Session control |
| 19 | `setup` | Installer service |
| 20 | `unlink` | Delete |
| 21 | `rename` | Rename (NovusFS root only) |
| 22 | `mount` | Mount |
| 23 | `umount` | Unmount |
| 24 | `mounts` | Query mount table |
| 25 | `update` | Update service control |
| 26 | `https` | HTTPS client control |
| 27 | `console` | Serial console read/write |
| 28–37 | DWM | `vmem_alloc/free/map/grant`, `surface_present`, `wait_vsync`, `set_exclusive`, `set_cursor`, `dwm_query`, `set_mode` (gated by `CONFIG_NOVUS_DWM`) |
| 40 | `win32` | Win32 API gate (`TASK_ABI_WIN32` only) |
| 41 | `pe32_exec` | Execute PE32 image from VFS |
| 60 | `exit` | Exit process |

Win32-ABI tasks may call **only 40 and 60**; all others are refused by the dispatcher.

## 3. NUC executable format (V1.0 / ABI `0x0100`)

| Field | Value |
|---|---|
| Magic | `NUC!` (`4E 55 43 21`) |
| Header | 48 bytes (`0x30`), `reserved` 16 bytes |
| `abi_version` | major=1 (high byte), minor=0 (low byte) |
| Segments | ≤16; only `TEXT` executable, rest NX |
| Relocations | `R_X86_64_32`, `R_X86_64_PC32`; type 3 reserved → `NOVUS_EINVAL` |
| Stack | requested in header, clamped to 64 KiB–4 MiB; top fixed `0xA0000000`, return trampoline `0x90000000` |
| Entry | `entry_rip = text.vaddr + entry_point` (byte offset into `.text`) |
| Signature | none; isolation via page tables (unlike NKM) |
| Load path | boot-stage reads fixed `APPS/DEMO.NUC`, kernel calls `nuc_load()` |

**Compatibility**: major ≠ 1 refused; minor higher or lower than kernel accepted (unknown fields ignored). Generic `./app.nuc` exec is not implemented.

## 4. NKM module format (V1.0 / ABI major 1)

| Field | Value |
|---|---|
| Magic/header | `NKM!` / 208 bytes (`0xD0`) |
| Digest | SHA-256 over whole file with `[0x50,0xB0)` (32 B hash + 64 B sig) zeroed first |
| Signature | Ed25519 over the 32-byte raw digest |
| Limits | segments 1–16, deps ≤8, symbols ≤64, relocations ≤256, modules ≤16, root keys ≤4 |
| Relocations | 64-bit absolute only; PC32 refused |
| Load order | bounds → digest → signature → dependencies → map → relocate → init |

Offsets/lengths fixed by `_Static_assert` in `security/nkm/nkm.c`. Currently one development key, no runtime rotation.

## 5. IPC and Novus-WM protocol

### 5.1 Kernel IPC

| Item | Value |
|---|---|
| Header | 32 bytes, magic `0x4E495043` (`NIPC`) |
| Checksum | FNV-1a |
| Payload | ≤128 bytes |
| Queue/ports | 8 per port; 16 static ports; task inbox port = pid |
| Named port base | `IPC_PORT_BASE = 0x1000` (avoids pid collision) |
| Flags | `IPC_F_SYNC 0x1`, `IPC_F_REPLY 0x2`, `IPC_F_BLOCK 0x4` |
| Return codes | `0` OK; `-1` EBADF, `-2` EBADMSG, `-3` EMSGSIZE, `-4` EBUSY, `-5` ENOENT, `-6` EAGAIN, `-7` ERETRY, `-8` ETIMEDOUT |

### 5.2 Novus-WM (code version `0x00020000` / 2.0)

Port `"nwm"`; `NWM_PID_SHELL = 0` means the desktop itself (local inline call, no inbox).

| Class | Message numbers |
|---|---|
| Requests v1.x | `0x0101`–`0x0110`: CREATE/DESTROY/MOVE/RESIZE/RAISE/MINIMIZE/MAXIMIZE/RESTORE/FOCUS/DRAW/TITLE/EFFECT/CLEAR/SHAPE/TEXT/INVALIDATE |
| Requests v2 | `0x0111`–`0x0116`: SURFACE_CREATE/DESTROY, FLIP_PRESENT, BLT_SUBMIT, SET_EXCLUSIVE, THUMBNAIL |
| Replies | `0x0001` OK, `0x0002` ERR |
| Events | `0x0201`–`0x0206`: CREATED/MOUSE/KEY/FOCUS/RESIZE/CLOSE |

| Limit | Value |
|---|---|
| Windows | 10 |
| Shapes / texts per window | 120 / 160 |
| Text run / title | 96 bytes / 24 bytes |
| Frame protocol | `CLEAR` → `SHAPE`/`TEXT` → **`INVALIDATE`** (missing INVALIDATE causes half-frame flicker) |
| Modal flag | `NWM_F_CREATE_MODAL = 0x1` |

v2 messages are additive: clients allocate vidmem objects with `SYS_VMEM_ALLOC` and hand WM only handles — pixels never cross IPC.

## 6. File and directory ABI

| Item | Value |
|---|---|
| Root | single `vfs_root()`; mount table switches backends by path prefix |
| Mount table | 8 entries; paths absolute and unique; `/` cannot be unmounted; cross-mount `rename` refused |
| Path limit | 256 bytes; no `..` traversal elimination; directories have no `..` entries |
| File type bits | `VFS_MODE_FILE=1`, `VFS_MODE_DIR=2` |
| Backends | NovusFS (writable), FAT (RO 8.3), ISO9660/Joliet (RO), ext4 (read+mkfs), NTFS (enum only) |
| Metadata | `/.novusfs/mounts.json` ≤4 KiB / ≤8 entries / whitelist; runtime `mount` does not write back |

**Not implemented**: `seek`, `truncate`, owner/rwx permissions, cross-backend `rename`.

## 7. Other frozen constants

| Item | Value |
|---|---|
| Shared memory | 8 KiB @ `0x7E000`, NVSH v1.1; 4 KiB log ring carved at tail (frozen fields untouched) |
| Tasks | `SCHED_MAX_TASKS=10`, 64 KiB ring-0 stack each, default 4-tick slice |
| Kernel heap | `kmalloc` 4 KiB granularity, ≤256 MiB / 256 records |
| Built-in blobs | <128 KiB, linked at `0x400000`; stack 64 KiB–4 MiB |
| Input events | `ND_KEY_*` above `0x110000` (`ND_KEY_BASE`); character keys carry Unicode code points |
| sysinfo requests | `LANG 0x030F`, `DISPLAY 0x0318`, `RSP_DISPLAY 0x0319`, `NCSI 0x031A` |
| Graphics | 4 GOP outputs (mirrored); 114,397 Unicode glyphs |

## 8. Change rules

Any change to a frozen format (NUC/NKM/NUP/INS/IPC/update/rootfs/shared-memory/syscalls) affecting fields, offsets, encoding or semantics requires an ABI RFC and a major version bump, plus the six-piece sync: spec, kernel, host tools, test fixtures, version number, compatibility note.

Append-only changes are minor versions; text errata must state "field offsets and ABI major unchanged". See [CHANGELOG.md](CHANGELOG.md).
