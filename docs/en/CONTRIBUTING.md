# Novus OS Contributing Guide

> Version: v1.0 (2026-11-27)
> 中文版本：[../CONTRIBUTING.md](../CONTRIBUTING.md) · [Index](INDEX.md)
>
> Organized as "get running → understand structure → make changes → validate". Start with §2.2 for a first build, §10 for task recipes, §11 for troubleshooting.

---

## 1. Before you start

### 1.1 Required reading by direction

| Direction | Must read |
|---|---|
| Everyone | [README.md](README.md), [templates README](../templates/README.md), [CHANGES.md](CHANGES.md), `../LICENSE` |
| Kernel boot/drivers/IRQ | `../docs/中断体系.md`, `../docs/Novus OS Debug 级接口手册 V1.0.md` |
| FS/directory tree/images | `../docs/根目录结构与挂载规范.md`, `../docs/用户会话与文件API.md` |
| Terminal/built-in apps | `../docs/终端与命令解释器设计.md`, `../docs/M8 内置系统应用.md`, `../docs/设备管理器.md` |
| GUI/themes/fonts/Chinese | `../docs/Novus-WM 与 IPC 协议规范 V1.0.md`, `../docs/Novus UI 设计语言规范 V1.0.md`, `../docs/中文支持与 Win11 桌面.md` |
| NUC/NKM/packages/shortcuts | corresponding `../docs/*V1.0.md` specs |
| cppcc/Nuomi/Win32 | `../docs/M7 cppcc 实现说明.md`, `user/cppcc/README.md`, `../docs/Mochi语言规范.md`, `../docs/M9 Win32 兼容层原型.md` |
| Installer/session/i18n/update | `../docs/图形安装向导设计与实现.md`, `../docs/多语言、会话与 M11 输入输出.md`, `../docs/Novus OS 自研分级增量更新体系完整规范.md` |

**Spec priority**: source/build results > frozen specs > changes.md/CONTRIBUTING > README early plans > `.trae/`/`.codebuddy/` process docs.

### 1.2 Real source map

```text
arch/x86_64/   UEFI entry, boot, kernel entry, AP trampoline, assembly
kernel/        scheduler, syscalls, NUC loader, memory, IRQ, devices, network, Win32 loader
fs/            VFS, NovusFS, FAT, ISO9660, ext4, mount config
security/      CAP, TLS, update verification, NKM loader
graphics/      NovusDisplay, widgets, desktop, fonts, themes, IME, wallpaper
wm/novus-wm/   in-kernel compositor and window protocol; nwm_proto.h is truth
apps/          Rust no_std runtime, built-in apps, nsh, Nuomi
templates/     GUI app / nsh command / Nuomi script templates
user/          Nuomi_pl reference, cppcc, win32compat, ssl
tools/         host image/package/signing/generator tools + tools.mk
scripts/       build helpers, image checks, self-tests, QEMU tests
configs/       effective defconfig
assets/        wallpapers and resources
docs/          design and frozen specs
```

No `system/`, `drivers/`, `user/shell/`, `user/micropython/`. Built-in apps are in `apps/src/bin/`, shell in `apps/src/shell/`, NIC drivers in `kernel/`. MicroPython is roadmap only — discuss design in an Issue first.

### 1.3 Documentation navigation

| Task | Read first | Then |
|---|---|---|
| Understand layering/data flow | [ARCHITECTURE.md](ARCHITECTURE.md) | WM/IPC spec |
| Done vs not done | [ROADMAP.md](ROADMAP.md) §2/§4 | [CHANGES.md](CHANGES.md) |
| Is a feature really implemented? | [SECURITY.md](SECURITY.md) §5 + source | ROADMAP §0.3 drift list |
| Boot/driver/IRQ | ARCHITECTURE §2 | IRQ doc, Debug manual |
| GUI/themes | templates README | UI design spec |
| nsh commands | terminal design doc | shell-command template |
| Compiler/language | `user/cppcc/README.md` | C++11 support matrix |
| Prepare a PR | §9 | §13 |

Three doc classes: frozen format specs (immutable fields/versions), design docs (descriptions; file errata on conflict), process docs (historical plans, not specs).

---

## 2. Environment setup

### 2.1 Required tools

- x86_64 cross toolchain: `x86_64-w64-mingw32-gcc`, `objcopy`
- Rust + `rustup` targets:
  ```sh
  rustup target add x86_64-pc-windows-msvc   # kernel Rust static lib: COFF
  rustup target add x86_64-unknown-none      # built-in apps: flat NUC blob
  ```
- `make`, `gcc`, Python 3, `xxd`
- QEMU, OVMF, `qemu-img`
- `bison`, `flex`, `libelf-dev` (devcontainer list)

Ubuntu/Debian:

```sh
sudo apt update
sudo apt install -y build-essential gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64 \
  binutils-mingw-w64-x86-64 mingw-w64 xorriso mtools grub-pc-bin \
  qemu-system-x86 ovmf bison flex libelf-dev python3 xxd
```

Optional: `socat` (wm-churn), `openssl` (update/E2E), Pillow (wallpaper), GNU Unifont hex + ibus Pinyin table (font/IME generators, not distributed), PowerShell 7 (Windows QEMU tests).

WSL: `source "$HOME/.cargo/env"` or add the toolchain directory to PATH explicitly. Override prefix with `make CROSS=<prefix>`.

### 2.2 First build, step by step

```sh
# ① verify tools
x86_64-w64-mingw32-gcc --version
rustup target list --installed | grep -E 'x86_64-pc-windows-msvc|x86_64-unknown-none'
qemu-system-x86_64 --version

# ② host self-tests (no QEMU)
bash scripts/display-selftest.sh
bash scripts/irq-selftest.sh

# ③ kernel + apps
make

# ④ image validation
make check-rootfs

# ⑤ boot
make run
```

Expected serial markers: `[BOOT] UEFI boot started` → `[SHM] shared memory verified` → `[BOOT] mounts applied` → `[M3] integrity: PASS` → `[ND] demo desktop painted` → `[HALT] entering the scheduler`.

Recommended first change: compile a `templates/gui-app/` app standalone → add an nsh command (§10.1) → plug an app into the system (§10.2).

---

## 3. Configuration and build

### 3.1 Configuration mechanism

No Kconfig parser, no `make menuconfig`:

- `configs/novus_defconfig` is the real config source (`-include`d by Makefile).
- Top-level `Kconfig` is declaration-only; sync new options manually.
- `y`/`n` become C macros `1`/`0`; source lists are pinned; gate features with `#if CONFIG_NOVUS_*`.
- `depends on` is not validated: ensure `IRQ→SCHED`, `GUI→DISPLAY`, `WM→GUI+IPC`.
- `CONFIG_USER_MOCHI`/`CONFIG_USER_CPPCC` may both occupy the `APPS/DEMO.NUC` slot; no `user/mochi` submodule currently.
- `E2E=1` adds test roots and HTTPS probes; all related `make` calls must use the same flag.
- `CONFIG_NOVUS_NTP=y` by default: NTP via UDP/123 hourly after DHCP.

### 3.2 Common commands

```sh
make                 # out/NOVUS.EFI
make run             # build + QEMU, MEM=4G
make iso             # out/novus-live.iso
make run-iso         # boot from ISO
make disk-img        # out/novus-disk.img
make check-rootfs    # validate ISO/ESP/system tree and fonts
make clean           # remove build/, out/, esp/
```

Host tools: `make tools`, `make cppcc`, `make cppcc-demo`, `make nkm-test-module`, `make wpkg-img`, `make theme`, `make novus-img`, `make esp`, `make iso-force`.

`make install DEVICE=/dev/sdX` uses `dd` to overwrite the whole device — explicit DEVICE and manual `yes` required. Prefer `scripts/run-install-test.sh` for testing (writes `/tmp/novus-install-target.img`).

### 3.3 Overridable variables

| Variable | Default | Purpose |
|---|---|---|
| `CROSS` | `x86_64-w64-mingw32-` | cross prefix |
| `MEM` | `4G` | QEMU memory |
| `E2E` | `0` | E2E test anchors |
| `HOSTCC`/`HOSTCFLAGS` | `gcc` / `-O2 -std=c11 -Wall -Wextra -D_GNU_SOURCE` | host tools |
| `SYS_SIZE_MB`/`ESP_TPL_SIZE_MB`/`ESP_SIZE_MB`/`DISK_SIZE_MB` | 8/16/31.5/64 | image sizes |
| `NKM_SEED` | fixed | test key seed — changing breaks embedded public key |
| `DEVICE` | none | `make install` target |
| `NOVUS_NIC` | `e1000` | QEMU NIC model |
| `DISPLAY` | empty | gtk when set, else headless |

### 3.4 Artifacts

`out/NOVUS.EFI` (PE32+ subsystem 10, entry `efi_main`), `out/apps/<name>.bin` (flat blob → `k_app_<name>`), `out/novus.img` (8 MiB NovusFS), `out/esp-tpl.img` (16 MiB), `out/esp.img` (31.5 MiB), `out/novus-live.iso` (ISO9660+Joliet+El Torito+isohybrid), `out/novus-disk.img` (64 MiB GPT+ESP+NovusFS), `out/qemu-smoke*`.

El Torito sector count is 16-bit × 512 B ⇒ boot item must be <32 MiB; live ESP = 31.5 MiB. FAT ESP uses 8.3 names only. Never commit `build/`, `out/`, `esp/`, `target/`.

---

## 4. Testing and validation

### 4.1 Minimum required by change type

| Change | Minimum | Additional |
|---|---|---|
| Any | `make`, `make check-rootfs` | list commands in PR |
| Kernel boot/scheduler/IRQ | + `irq-selftest.sh` | QEMU smoke, `irq-qemu-test.ps1` |
| FS/dirs/images | + `inspect_img.py out/novus.img` | `make disk-img`, smoke, `font_smoke.sh` |
| GUI/themes/fonts/IME/i18n | full QEMU smoke | `wm-churn.sh 4G 20`, generator `--check`, screenshot review |
| Window/app lifecycle | full smoke | `wm-churn.sh 4G 20 [--iso]` |
| Storage drivers | `make iso` | `qtest.sh`, manually check `[MOUNT]` |
| Installer | `make iso` | `run-install-test.sh 256`, `EN=1`, `FLOW=desktop` |
| Update/TLS/CA | normal build | `updatesrv/selftest.py`, `novupkg selftest`, E2E |
| NKM/NUC/ABI | normal build | `nkmgen selftest`, `make nkm-test-module` |
| cppcc | `cargo test --manifest-path user/cppcc/Cargo.toml` | `make cppcc-demo` + `make run` serial markers |
| Nuomi | host reference tests + nsh | `test-linux.sh`, `tests/nuc/run-tests.py` |

### 4.2 Standard regression

```sh
bash scripts/display-selftest.sh
bash scripts/irq-selftest.sh
python3 -m unittest discover -s scripts -p qemu_smoke_test.py
python3 -m unittest discover -s scripts -p novusimg_test.py
cargo test --manifest-path user/cppcc/Cargo.toml

make clean && make iso
python3 scripts/inspect_img.py out/novus.img
python3 scripts/check_rootfs.py out

make disk-img
bash scripts/qemu-smoke.sh
bash scripts/qemu-smoke.sh --no-root --output out/qemu-smoke-no-root
scripts/wm-churn.sh 4G 20   # needs socat
```

Tool self-tests: `make tools`, `build/tools/novupkg selftest`, `(cd tools/nkmgen && cargo run --quiet -- selftest)`, `python3 tools/updatesrv/selftest.py`, generator `--check` flags.

QEMU smoke uses temp firmware vars and `-snapshot`; VNC only on `127.0.0.1:5901`. Baselines in `scripts/baselines/qemu/{disk,no-root}`; missing/mismatch fails; **never** use `--update-baseline` in normal regression.

### 4.3 Machine-specific scripts

These contain `/home/dev/...` or fixed OVMF paths: `qtest.sh`, `scripts/build-all.sh`, `scripts/build-img.sh`, `scripts/rebuild-kernel.sh`, `tools/updatesrv/e2e-run.sh`, `scripts/run.sh`, `run-iso.sh`, `font_smoke.sh`, `wm-churn.sh`, `run-install-test.sh`. `qtest.sh` uses `|| true` — manually check `/tmp/b_*.log` for `[MOUNT]`. No hosted CI: state unexecuted checks in the PR.

### 4.4 Logs and debugging

`kputs()` mirrors to VGA, COM1, klog ring and optional fb console. Prefixes: `[BOOT]`, `[SHM]`, `[M3]`/`[MRKL]`, `[ND]`/`[CRTC]`, `[NOVS]`/`[WAL]`, `[BLK]`/`[USB]`/`[NET]`/`[DNS]`/`[NCSI]`, `[SMP]`/`[PMM]`/`[HEAP]`, `[APP]`, `[WARN]`.

Debug order: host self-tests → `make` → `check-rootfs` → QEMU serial → screenshots → interaction scripts. No kernel addresses/backtraces/key material/reproducible payloads in public logs.

---

## 5. Kernel, drivers and low-level development

### 5.1 Boot and init order

1. `efi_main`: serial/console, block devices, memory self-test (<2 GiB refuses), `stage3_prepare/menu/load/finish`, `ExitBootServices()`.
2. `kernel_entry()`: 64 KiB BSP stack, clear `.bss`, call `kmain()` (never returns).
3. `kmain()`: PMM → MM → heap → devices/block → IPC → NovusFS/VFS → NKM → update/CA → LAPIC/SMP → scheduler/IRQ/NUC tasks/lang/session/network/WM → `sched_start()`.

Hard constraints: NKM needs heap/PMM; NovusFS self-test before AP wake (no second core touching block layer/WAL); PCI BAR probing after storage drivers, before network stack.

### 5.2 New C sources or drivers

- Explicitly add `.c` to Makefile `SRC`, assembly to `ASM_SRC`, new dirs to `INC` — no auto-discovery.
- Add config switches in `Kconfig`, defconfig, `#if CONFIG_NOVUS_*`, and docs.
- Block devices: `blkdev_register()`, static storage, 512 B sectors.
- NICs: `net_register()`, probe in `net_init()`; first UP device is active.
- IRQ: `irq_register(line, flags, isr, device, name)`; ≤4 handlers/GSI, shared lines same trigger.
- ISR: no `sti`, sleep, scheduling, allocation, protocol stack or logging; only clear own source; EOI by framework.
- Device manager observes `devtab_refresh()` tree only.
- NKM-resolvable symbols must be in `nksym_table`.
- Insert `kmain()` init call by hardware class — adding a source file alone does nothing.
- AP trampoline: 16-bit, <4 KiB, no relocations; last 24 B = CR3/AP stack/`ap_main`; SIPI failure degrades to single-core.

### 5.3 Syscall ABI

- Single table `g_syscalls[]` in `kernel/trap.c`; sparse frozen numbers — never reuse/reorder.
- Convention: `rax` number, `rdi/rsi/rdx/r10` args (4th **must** be `r10`), negative return = error.
- Copy user pointers page-by-page under task CR3.
- Paths ≤256 B, I/O ≤4096 B, fd from 3.
- `ipc_recv` blocking relies on rewinding return RIP by 2 bytes — the `syscall; ret` layout is ABI.
- Win32 tasks only `SYS_WIN32(40)` and `SYS_EXIT(60)`.
- `user/Nuomi_pl/include/novus_syscall.h` must stay in sync.

### 5.4 Process, memory and stack

- One task/CR3/kernel stack/trap frame per NUC process; `SCHED_MAX_TASKS=10`, 64 KiB ring-0 stack, 4-tick slice.
- `kmalloc()` page-granularity, 4 KiB rounding, ≤256 MiB / 256 records — not a slab.
- Shared heap window `0xFFFF810000000000`, 256 MiB, supervisor-only.
- PMM 256 KiB bitmap, ~8 GiB; DMA via explicit PMM.
- NUC stack header-clamped 64 KiB–4 MiB; top `0xA0000000`, trampoline `0x90000000`.
- NUC ≤16 segments, only TEXT executable, rest NX; only `R_X86_64_32` and `R_X86_64_PC32`; no imports. NUC unsigned (page-table isolation); NKM signed.

### 5.5 Filesystem and root directory

- Single syscall root `vfs_root()`; mount table switches backends by prefix; no multiple root trees.
- Truth source for tree/mounts: `fs/novusfs/rootfs_layout.h`; readable doc in `../docs/根目录结构与挂载规范.md`.
- 8 mount entries; absolute unique paths; `/` cannot unmount; cross-mount rename refused.
- No `..` traversal elimination; no `..` entries; paths 256 B.
- `/.novusfs/mounts.json` ≤4 KiB / ≤8 / whitelist; runtime `mount`/`umount` no write-back.
- NovusFS writable root: dual superblock, 64 WAL blocks, reserved Merkle area, block bitmap, inode table, data area; frozen format.
- FAT/ISO9660 read-only: FAT 8.3 only, ISO prefers Joliet; writes/deletes/renames fail.
- ext4 supported; NTFS is enum only.

---

## 6. Security, NKM and frozen formats

- CAP is defined as 128-bit mask in specs but `cap_check` is **not** exposed; never treat spec text as implemented.
- NKM Ring 0 modules require Ed25519; digest = SHA-256 over whole file with `[0x50,0xB0)` zeroed.
- Limits: `NKM!`, ABI major 1, 208 B header, 1–16 segments, deps ≤8, symbols ≤64, relocations ≤256, modules ≤16, root keys ≤4; 64-bit absolute relocations only.
- Load order: bounds → digest → signature → dependencies → map → relocate → init.
- `NKM_FLAG_ALLOW_UNSIGNED` is debug-only for the current admin session; never persisted.
- Offsets fixed by `_Static_assert` in `security/nkm/nkm.c`; layout changes sync loader, `nkmgen`, spec, docs.
- `nkm_exit()` frees memory, IRQ and device registrations; refuse unload if depended upon.

Frozen formats: NUC (major 1), NKM (frozen), NUP/INS (bump version), WM IPC (`0x00020000` in code — doc still says 1.0), Debug interface, update patches, rootfs layout (`rootfs_layout.h`). ABI change PRs must update spec, kernel, host tools, fixtures, version and compatibility note. Text errata state "field offsets and ABI major unchanged".

Closed-source drivers: [DRIVER-LICENSE.md](DRIVER-LICENSE.md) grants a GPLv2 §10 additional permission; still requires signatures and frozen-ABI-only access.

---

## 7. GUI, windows and built-in apps

### 7.1 Graphics stack and protocol

- `graphics/` layers: framebuffer HAL → 2D software renderer → widgets → desktop; pixel `0xAARRGGBB`; offscreen compositing.
- `wm/novus-wm/` is in-kernel compositor; clients use only `ipc_send/ipc_recv/ipc_lookup` on port `"nwm"` — no shared memory, no kernel headers.
- IPC header 32 B, magic `0x4E495043`, FNV-1a, payload ≤128 B, queue 8, ports 16, inbox port = pid.
- Per-window display list hard limits: 120 shapes, 160 texts, text ≤96 B, title ≤24 B, ≤10 windows. Older 40/48 budgets were suggestions, not protocol limits.
- Frame: `CLEAR` → `SHAPE`/`TEXT` → `INVALIDATE`; missing INVALIDATE causes flicker; retry `IPC_EBUSY`.
- Draw only on events/theme changes; idle `sys::yield_now()`.
- Text width via `nd_text_width()`/`ui::width()`; no "bytes × 6"; Chinese via code points.
- Colors only via semantic tokens `ui::bg()`, `card_bg()`, `row_sel()`, `text()`, `dim()`, `accent()`, `line()`.

### 7.2 Adding a GUI app

Nine bins currently: `fileman`, `taskman`, `settings`, `netman`, `session`, `setup`, `nsh`, `devmgmt`, `dwm`; plus `novusd`.

1. `apps/src/bin/<name>.rs`.
2. `apps/Cargo.toml` add `[[bin]]`.
3. `Makefile` `APPS_NAMES` → `out/apps/<name>.bin` → `k_app_<name>`.
4. `kernel/apps.c` `g_apps[]` **append at end** (indices reused; prefer `apps_launch_name()`).
5. Desktop entry in `graphics/nd_demo.c` and icon type in `nvgui.*` if needed.

Names: letters/digits/underscore only, **no `-`**. Only `session`/`setup` are bootstrap apps; others need login. Blob must be <128 KiB.

### 7.3 User-space runtime constraints

- `no_std`, no allocator, no third-party deps: no `Vec`/`String`/`Box`; fixed arrays/slices.
- `panic` prints `[APP] panic` to COM1 and `exit(3)`; no backtrace — use `sys::write(b"...\n")`.
- Build: `cargo build --release --target x86_64-unknown-none` → `objcopy -O binary` → `xxd -i`; link at `0x400000`.
- Blob <128 KiB; stack 64 KiB–4 MiB; `apps.stamp` tracks `apps/src/**`, `apps/Cargo.toml`, `apps/link.ld`, `apps/.cargo/config.toml`.

### 7.4 nsh and commands

`apps/src/shell/`: `path.rs` (absolute paths), `parse.rs` (tokenization/redirection), `mod.rs` (state/pipelines), `cmds.rs` (builtins), `update.rs`/`crypto/` (update/signatures).

Two steps to add a builtin: write `fn cmd_xxx(...)` after other `cmd_*` before `BUILTINS`; register `("name", cmd_name)`.

```rust
fn cmd_xxx(shell: &mut Shell, cmd: &Cmd, stdin: &[u8], out: &mut dyn Sink) -> i32
```

Args already expanded (variables/glob/quotes); `MAX_ARGS=40`, word ≤256 B; `stdin` is a complete buffer ≤4 KiB; output only via `out.write()` (never `println!`); return = `$?`; environment via `shell.set_var()`; launch apps via `sinfo::spawn_name()` (no fork/exec); pipelines ≤8 stages sequential capture; `$(...)` nesting ≥3 fails; `>` truncates via `unlink+open(O_CREAT)`, `>>` reads then rewrites, both ≤4 KiB; `dir`/`copy`/`del` aliases off by default.

### 7.5 Nuomi two implementation lines

- `user/Nuomi_pl/`: host C reference + NUC toolchain with GC; `nuomi -c` only here; language spec in `user/Nuomi_pl/docs/Nuomi语言规范.md`.
- `apps/src/nuomi.rs`: nsh built-in `no_std`, heapless, libm-free port; `nuomi file.nuomi`, `nuomi -e "(+ 1 2)"`.

Device limits: source 6 KiB, bump arena 256 KiB (128 KiB env reclaimable, no GC), depth `MAX_DEPTH=1500` (~500 calls), 24 args, 24 bindings, 64 literal elements, 1024 B format buffer. Platform differences: no JSON/HTTP/process/window primitives; no `input`, `key` is `nil`; `pow` integer exponent only, `sqrt` self-computed; repeated `cons` in loops exhausts arena.

### 7.6 cppcc, Win32 and roadmap

- **cppcc**: host C++ subset compiler, no LLVM/external assembler. Single TU, POD structs, pointers/1-D arrays, limited preprocessing; no classes/templates/exceptions/STL/floats/function pointers/multi-TU; ≤6 args. `cargo test`, `make cppcc-demo`. Samples only boot-stage fixed `APPS/DEMO.NUC`; generic exec not implemented.
- **Win32**: prototype, PE32+ at `0x50000000`, 141 APIs (console/tick/Sleep/registry + GUI/GDI/files/resources/ws2_32 handshake). Separate `g_win32[]` — never shift `g_apps[]`. Add APIs by syncing thunk encoding, `SYS_WIN32=40`, `task_t.abi` whitelist and registry layout. Full isolation/DLL/TLS/threads still unimplemented.
- **Mochi**: spec only, no submodule; targets defined only when source exists.
- **MicroPython**: roadmap only — open an Issue first.

---

## 8. Generated files and assets

| Source | Command | Output |
|---|---|---|
| `tools/themegen/theme.toml` | `make theme` | `graphics/nd_theme.[ch]`, `apps/src/theme.rs` |
| `tools/langgen/strings*.toml` | `python3 tools/langgen/langgen.py` | `graphics/nd_lang_dict.c`, `apps/src/lang_dict.rs` |
| Unifont hex | `python3 tools/fontgen/fontgen.py ...` | `graphics/nd_font_data.bin`, `nd_font_uni.c`, `apps/src/unicode_data.rs` |
| `tools/imegen/pinyin.txt` | `python3 tools/imegen/imegen.py --gen` | `graphics/nd_ime_dict.c` |
| `assets/` wallpapers | `python3 tools/wpgen/wpgen.py`, `make wpkg-img` | `assets/backgrounds-screen/`, `out/wallpapers.pkg` |
| Update roots | `python3 tools/cabundle/cabundle.py ...` | `security/novu/cabundle.c` |
| NKM seed | `make nkm-test-module` | `security/nkm/nkm_test_module.h`, `out/test.nkm` |

Generated files are committed; regenerate after source changes. New Chinese strings usually need font regeneration. `--check` detects drift. E2E certs only affect `cabundle_e2e.c` (`E2E=1`). Never commit private keys/seeds; `.gitignore` covers `private/`, `tools/updatesrv/*.key`, `security/tls/*.key`, `user/ssl/*.key`.

---

## 9. Code style and submission

No unified `clang-format`/`rustfmt.toml`/`.editorconfig`/CI/CODEOWNERS/PR templates, so:

- Follow the file's existing style; no mass reformatting.
- No new `-Wall -Wextra` warnings; self-tests use `-Werror`.
- Rust 2021 edition; app profile `panic=abort, opt-level=s, lto=true, strip=true`; run `cargo fmt`; avoid unnecessary `unsafe`.
- No external open-source dependencies without an Issue discussing self-development alternatives and license boundaries. Unifont is a documented exception.
- One PR per change; title/description state motivation, implementation, compatibility impact, tests and unexecuted items.
- Update README/docs/changes.md. `changes.md` convention: topic → path-precise bullets → verification results, listing unrun items honestly.
- Never commit `build/`, `out/`, `esp/`, `target/`, `*.o`, OVMF VARS copies, temp logs or machine-specific paths.

### PR pre-merge checklist

```text
[ ] make and make check-rootfs pass
[ ] Matrix-required unit/self/QEMU tests run
[ ] Generated files regenerated, --check clean
[ ] Image/blob/stack/IPC/display-list limits respected
[ ] ABI/protocol changes versioned and synced everywhere
[ ] Boot order, mount rules, signature chain, launch-name indices intact
[ ] README/docs/changes.md updated to real implementation
[ ] No build artifacts, private keys, machine paths or temp changes
[ ] PR lists commands run and explicitly states unexecuted checks
```

---

## 10. Common task recipes

### 10.1 Add an nsh builtin

```rust
fn cmd_hello(shell: &mut Shell, cmd: &Cmd, stdin: &[u8], out: &mut dyn Sink) -> i32 {
    let _ = (shell, stdin);
    out.write(b"hello\n");
    0
}
```

Register `("hello", cmd_hello)` in `BUILTINS`; `help`/`which`/pipes/redirection work automatically. Output only via `out.write()`; return is `$?`; launch apps via `sinfo::spawn_name()`. Verify with `hello`, `hello | cat`, `$(hello)`.

### 10.2 Add a GUI app

Copy `templates/gui-app/src/main.rs`; change four places (§7.2). Append to `g_apps[]`. Frame: `CLEAR` → shapes/texts → `INVALIDATE`; retry `IPC_EBUSY`. Text width via `ui::width()`; colors via tokens. Verify blob <128 KiB; `make` + smoke + `wm-churn`.

### 10.3 Add C source/driver

Add to Makefile `SRC`/`ASM_SRC`/`INC`; sync Kconfig/defconfig/`#if`/docs; register block device (`blkdev_register()`) or NIC (`net_register()`) or IRQ (`irq_register()`); ISR only clears its source; insert `kmain()` init call; add symbols to `nksym_table`. Verify `make`, `qtest.sh`, `irq-selftest.sh`.

### 10.4 Add a syscall

Allocate a new number (append only) in `kernel/trap.c`, add to `g_syscalls[]`, implement `sys_*`; copy user pointers page-by-page; sync `user/Nuomi_pl/include/novus_syscall.h` and `apps/src/sys.rs`; add rejection tests for bad input.

### 10.5 Add an IPC port service

Use a shared header for port name; 32 B header/magic/FNV-1a/payload ≤128 B/queue 8/ports 16; no strong port auth currently — don't treat as a security boundary; blocking receive relies on `syscall; ret` layout.

### 10.6 Modify generated assets

Edit TOML/source, **not** generated files; regenerate and commit; `--check` clean; new Chinese strings need font regeneration; verify `font_smoke.sh` + screenshot review.

### 10.7 Add an NKM module

Use `tools/nkmgen`; `NKM!`, ABI major 1, 208 B header, 1–16 segments, deps ≤8, symbols ≤64, relocations ≤256, modules ≤16, root keys ≤4; SHA-256 with `[0x50,0xB0)` zeroed + Ed25519; 64-bit absolute relocations only; load order fixed; `nkm_exit()` frees resources. Verify `make nkm-test-module`, `nkmgen selftest`.

---

## 11. Troubleshooting

| Symptom | Likely cause | First action |
|---|---|---|
| `undefined symbol k_app_*` | App missing one of Cargo/APPS_NAMES/g_apps | Check four registration points |
| App blob >128 KiB | `Vec`/`String` or large static buffer | Fixed arrays; check `apps.stamp` |
| Boot stuck at splash after "mapping memory" | Paging/framebuffer mapping failure | Check `[ND]` lines and UC mapping |
| No serial output | `CONFIG_NOVUS_DEBUG_SERIAL=n` or QEMU redirection | Check defconfig and run.sh |
| White/black bands, half-frame flicker | Display list missing `INVALIDATE` or drawing outside compositor | Add INVALIDATE; draw only `nd_canvas()` |
| Chinese shows boxes | Glyph missing or generated assets stale | Regenerate fonts; check code-point boundaries |
| check-rootfs dir count mismatch | `rootfs_layout.h` (78) vs fsinfo/check (77) | Sync to header (ROADMAP P0-04) |
| QEMU NIC no address | Model mismatch or DHCP retry state not reset | Try `NOVUS_NIC=` models; igb on QEMU is known |
| `[MRKL] MISMATCH` | Root hash mismatch or runtime wrote `mounts.json` | Check protected metadata and A-core log |
| Smoke baseline mismatch | UI change without baseline update or dynamic content | Never `--update-baseline` in normal runs |
| check-lint new warnings | clippy/rustfmt diff from baseline | Fix new items only |
| qtest.sh "passes" but no mount | `|| true` swallows failure | grep `/tmp/b_*.log` for `[MOUNT]` |
| Generator `--check` stale | TOML changed without regeneration | Regenerate and commit |
| Win32 sample exits 0 but no output | thunk/API whitelist missing | Check `kernel/win32.c` API table |

Order: host self-tests → `make` → check-rootfs → QEMU serial → screenshots → interaction scripts.

---

## 12. FAQ

**Q: Why no `Vec`/`String`/`Box` in apps?** A: `no_std` allocator-free flat NUC images embedded in the kernel; fixed buffers; panic prints COM1 and exits.

**Q: Why aren't nsh pipelines parallel?** A: Phase-1 simplification: no fork/exec, sequential capture (≤8 stages, 4 KiB). True concurrency needs process-model work (ROADMAP Phase 1).

**Q: Why no `make menuconfig`?** A: `Kconfig` is declaration-only; Makefile reads `configs/novus_defconfig`; `y/n` → macros; `depends on` unchecked. Full parser is a later milestone.

**Q: Why does README mention `system/`/`drivers/`?** A: Early planning text. Real map in §1.2; file errata welcome.

**Q: Does QEMU passing mean real hardware works?** A: No — known differences: framebuffer cache attributes, JPEG decoder toolchain UB, igb without `STATUS.LU`. State real-hardware validation in PRs.

**Q: Why not `--update-baseline` freely?** A: Baselines are regression gates; only for intentional UI changes with manual per-image review.

**Q: Why not treat CAP/Secure Boot as implemented?** A: [SECURITY.md](SECURITY.md) §5 lists them as spec goals; treating them as implemented creates false security promises.

**Q: Why are some self-tests WARN not ERROR?** A: "Minor errors don't panic" principle; WARN still must be stated in PRs.

**Q: Must contributors run every test?** A: Run the matrix minimum in §4.1; no hosted CI — state unexecuted checks in the PR.

---

## 13. PR description template

```markdown
## Motivation
Problem before the change. Related Issue / ROADMAP gap ID (e.g. G-F02).

## Implementation
- Files, key functions, data flow (path-level)
- ABI/config/frozen format changes?

## Compatibility
- [ ] No ABI/protocol/disk format change
- [ ] Changed: version bumped and six-piece sync done
- [ ] Boot order, mount rules, signature chain, g_apps[] indices intact

## Tests
Commands actually run (with arguments):
...
Results (log markers, screenshot paths, JSON reports):

## Unexecuted checks
List explicitly with reasons.

## Docs
- [ ] README/docs/changes.md updated to real implementation
- [ ] Doc conflicts: state authoritative file
- [ ] Generated files regenerated, --check clean

## Hygiene
- [ ] No build/out/esp/target/*.o, private keys, machine paths or temp changes
```

One PR per change — "fix a typo" and "add a syscall" are separate PRs.

---

GPLv2; contributors retain copyright. See `../LICENSE` and `../COPYING`. Closed-source drivers may use the GPLv2 §10 additional permission in [DRIVER-LICENSE.md](DRIVER-LICENSE.md), still subject to signature and frozen-ABI requirements.
