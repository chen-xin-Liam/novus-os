# Novus OS Development Changes (English Summary)

> Version: v1.0 (2026-11-27)
> This is an English **summary** of the root `changes.md`, which remains the authoritative detailed log.
> 中文版本：[../changes.md](../changes.md) · [Index](INDEX.md)
>
> Format convention in the Chinese original: topic title → path-precise bullets → verification results, listing unexecuted items honestly.

---

## Contents

1. [Boot splash shows logs directly](#1-boot-splash-shows-logs-directly)
2. [QEMU NIC support](#2-qemu-nic-support)
3. [NCSI connectivity detection](#3-ncsi-connectivity-detection)
4. [Full UTF-8/Unicode](#4-full-utf-8unicode)
5. [Developer templates](#5-developer-templates)
6. [Nuomi in nsh](#6-nuomi-in-nsh)
7. [Display refactor: no flicker/tearing](#7-display-refactor-no-flickertearing)
8. [M9 Win32 compatibility prototype](#8-m9-win32-compatibility-prototype)
9. [M11 phase 1: session, bilingual, installer](#9-m11-phase-1-session-bilingual-installer)
10. [M11 phase 1 continued: multi-output](#10-m11-phase-1-continued-multi-output)
11. [M10 UI unification and graphical installer](#11-m10-ui-unification-and-graphical-installer)
12. [cppcc M7 docs and build integration](#12-cppcc-m7-docs-and-build-integration)

---

## 1. Boot splash shows logs directly

- New `kernel/klog.c/h`: one log ring spanning both stages. Boot stage writes `boot_puts()`/`boot_timeline()` lines into a 4 KiB ring in the shared-memory reserved carve-out (no frozen field changed); `kmain()` inherits it with `klog_init()` then appends to a 32 KiB ring. `klog_snapshot()` reads the shared ring before inheritance, so splash shows firmware logs in phase one.
- `graphics/nd_boot.c`: semi-transparent log panel between logo and status line. Pure overlay — logo/status rectangles, fonts and positions unchanged; only the panel rectangle is repainted.
- `shared_mem_bootstage_init()` moved before the first log in `efi_main()` so its `memset()` does not erase early logs.
- Verification: `make` passed; QEMU 1280×800 screenshot confirmed the panel; 60 s headless boot showed no new kernel exceptions (the only `#PF` is the existing nxprobe user-space self-test). No git commit created.

## 2. QEMU NIC support

- `kernel/net_rtl8139.c`: I/O BAR driver, 8 KiB RX ring with CBR/CAPR handshake, four TX descriptors using TOK bit. **CAPR must be written as read-pointer − 16**; writing the read pointer itself makes QEMU consider the ring full. DHCP/DNS/NCSI verified.
- `kernel/net_igb.c`: fixed three real bugs — advanced RX descriptor layout (`status_error` at 0x18, `length` at 0x1C), `SRRCTL0` offset (0x0C00C, not 0x02100), missing `RXDCTL0`/`TXDCTL0` queue enables with Linux-like init order. Kept `STATUS.LU` gate: QEMU's igb never sets LU, so "not connected" is honest there; real 82576/82580/I350 work.
- `kernel/net_e1000.c`: MAC from RAL/RAH first (firmware-authoritative), EERD fallback; e1000e NVM is flash and the old order read `52:54:00:00:00:00`. Interface names by registration order (`ethN`).
- Fixed DHCP timeout retry loop: timeout branch did not clear `g_phase`, so every tick re-sent DISCOVER and re-ran NCSI probe.
- `scripts/run.sh`: `NOVUS_NIC` variable, default `e1000`.
- QEMU 10.0.13 matrix: `e1000`/`e1000e` pass; `virtio-net-pci` pass; `rtl8139` pass; `igb` loads but QEMU drops frames (no LU); `vmxnet3`/`ne2k_pci`/`pcnet` not implemented.
- Also added `kernel/net_ids.c` unified PCI ID table, `net_virtio.c` (legacy), WiFi identification (`wlanN`, `NET_STATE_NODRV`), `netdev_t.model[32]`, `NET_MAX_DEVS` 4→8, network panel "model" row.
- Not done: I225/I226 (igc), RTL8168/8125 (r8169), USB/WWAN, real wireless (firmware + 802.11 + WPA2).

## 3. NCSI connectivity detection

- `kernel/net_dns.c`: single-transaction A record query from UDP 5353; DNS server from DHCP option 6; fallback `10.0.2.3` on timeout.
- `kernel/net_ncsi.c`: fetches `http://ncsi.novus-os.ryzn.pro/connecttest.txt` in three phases (resolve → connect → read), distinguishing no-link/DNS/TCP/HTTP/timeout.
- `kernel/net_tcp.c`: from echo probe to general single-session client; fixed pseudo-header byte-order checksum bug.
- `kernel/net.c`: gateway forwarding for off-subnet destinations; UDP 5353 routed to DNS; tick drives DNS and NCSI state machines.
- `sysinfo`: `SYSINFO_REQ_NCSI(0x031A)` and snapshot fields `ncsi`/`http_code`/`ncsi_ms`; `apps/src/bin/netman.rs` connectivity card.
- `scripts/run.sh`: from `-net none` to `-netdev user -device e1000`, so `make run` has DHCP/DNS/NAT by default.
- Verification: `[DNS] answer` → `[NET] tcp established` → `[NCSI] connected`.

## 4. Full UTF-8/Unicode

- Glyph source changed from "UI corpus" to all Unicode: `tools/fontgen/fontgen.py` reads GNU Unifont hex + upper hex, generating `graphics/nd_font_data.bin` (3.5 MB, 114,397 glyphs × 32 B), sparse-page index `nd_font_uni.c` and `apps/src/unicode_data.rs`. Old 2,451-glyph CJK corpus deleted.
- Storage: sparse pages `g_uni_page[cp >> 8]` → slot with `valid`/`wide` bitmaps + dense glyph block; lookup = page → bit → popcount rank. Blob embedded via `.incbin` in `nd_font_blob.S`; `NOVUS.EFI` grew 2.5 MB → 6.4 MB (still under El Torito 32 MB). Unifont source not in repo; download URL in script header.
- Metrics from glyph width: ASCII 5×7/6 px, others 8 px half-width / 16 px full-width; Mn/Me/Cf combining marks step 0. `nd_utf8_next()` rejects overlong/surrogate/>U+10FFFF; added `nd_utf8_encode()` (1–4 B).
- Event ABI: `ND_KEY_*` moved above `0x110000` (`ND_KEY_BASE`); character keys carry Unicode code points; control keys no longer collide with `U+0100..U+010A`; Rust side uses `wm::KEY_*`.
- `nd_input.c`: Alt modifier and `Ctrl+Shift+U` code-point entry (1–6 hex digits + Enter/Space, Esc cancels), validating surrogates and upper bound.
- IME service: `nd_ime_key_ex()` decouples Pinyin state machine from text box; `nwm` feeds keys to IME before forwarding; candidate window drawn in focused window; `Ctrl+Space` toggles CN/EN; `ng_edit_insert_cp()` uses `nd_utf8_encode()`.
- Terminal `nsh`: 1× text (ASCII 6 px, Chinese 16 px, 144×26), pixel-based line/scroll/cursor, backspace/Delete/left/right by code point; scroll buffer wraps at code-point boundaries.
- Rust side: `ui::char_adv()` looks up `unicode_data`, `width()` drops the "-1 last cell" rule, `width_big()` scales all characters; `wm::copy_title`/`text_at` cut at code-point boundaries (`fit_bytes`).
- Shell/fs: `glob_match` `?` matches one code point, `*` aligns at code-point boundaries; history/vars/ls names/parse word buffers/fileman names/path copies retreat to code-point boundaries; `VFS_PATH_MAX` (256) used instead of component limit 56; `user_path()` rejects invalid UTF-8; user names accept any non-control UTF-8.
- Verification: host `_ref` comparison of `nuomi.rs` samples (see §6) and QEMU screenshots with `Ελληνικά Русский é ñ ← ⚙ 😀`; `Ctrl+Shift+U 0410 Enter` produced `А`; `Ctrl+Space` + `nihao` produced candidate "鸟" → `user@novus:/$ А鸟`. Temporary test changes reverted.

## 5. Developer templates

- New `templates/`: three minimal skeletons — GUI app, nsh command, Nuomi script. `templates/README.md` is the index with integration steps, per-frame budgets and no-allocator constraints, COM1 debugging.
- `templates/gui-app/`: full desktop window app (page/card/button/click counter/Esc close/theme follow), builds standalone with `cargo build` and `verify.sh` checks entry address; integration = 4 edits.
- `templates/shell-command/`: `hello`/`where`/`count` examples; integration = function + one `BUILTINS` line.
- `templates/nuomi/`: language features (recursion/map-reduce/vector/closure/while/raw draw) and file operations scripts.
- Verification: template app compiled standalone (entry `0x400000`, blob 30,700 B), temporarily integrated into `apps/` + Makefile + `kernel/apps.c` with full `make` passing, then reverted; command template compiled after paste; both Nuomi scripts ran via the same `apps/src/nuomi.rs`.

## 6. Nuomi in nsh

- New `apps/src/nuomi.rs`: C interpreter from `user/Nuomi_pl` ported to `no_std`, heapless Rust. Syntax/special forms/builtins match the C version; `print` quotes strings, `str`/`draw` output raw.
- `apps/src/shell/cmds.rs`: `nuomi script.nuomi` and `nuomi -e expr`; `help`/`which` list it automatically.
- Storage: bump arena allocated with the command stack frame, split by object, no GC; environment in reclaimable area with conservative no-reclaim when closures capture it.
- Intentional differences from C version: no JSON/HTTP/process/window primitives; no `input`, `key` always `nil`; `sqrt`/`pow` locally implemented (`pow` integer exponent only); recursion depth limited (~500 function calls); `reverse` truly reverses and `assoc` preserves table order.
- Limits: 256 KiB objects (128 KiB env reclaimable), 6 KiB source, 24 args, 64 literal elements.
- Kernel support: `nuc_spawn_blob()` buffer 64 KiB → 128 KiB (nsh with interpreter ≈69 KiB); built-in app stack 64 KiB → `NUC_STACK_MAX` 4 MiB.
- Verification: host-compiled `nuomi.rs` with mocks compared byte-for-byte against C reference on samples (closures/factorial/fib/lists/logic/scope/stdlib/strings/strings2/vectors/while) except documented differences; file primitives, `-e`, syntax errors/unbound var/arity errors covered; full `make` passed. QEMU terminal interaction not yet done at time of writing.

## 7. Display refactor: no flicker/tearing

- **Real-hardware corruption root cause**: framebuffer accessed as cacheable (WB) memory. `nd_init()` used identity mapping for low-4GiB framebuffers (WB) and `MM_MAP_UC` only for high. QEMU TCG ignores cache attributes; real hardware showed stale/out-of-order pixels. Now primary and mirror framebuffers always use `mm_map_phys(..., MM_MAP_UC)` with 2 MiB page alignment compensation, plus `wbinvd` after mapping to flush stale WB alias lines.
- **Wallpaper color issue**: host stub decoded the same JPEG correctly (RGB≈71,129,174) but kernel-side pipeline produced ≈0x2C dark/wrong image — toolchain-related UB. Since the 920-line decoder is risky to blind-debug, desktop background changed to programmatic vertical gradient (`nd_wallpaper_paint()` with `wp_lerp()` from theme tokens); photo encode/decode retained for later.
- **Retrace wait**: `ND_BLANK_SPINS=800` (~0.4 ms), `ND_BLANK_MIN_AREA=0x20000`; above cursor/icon size so pointer moves don't wait, but typical window drags do; runs in interrupt-disabled timer tick so bound must be small.
- Display layer changed from direct framebuffer drawing to Windows-style compositing: `nd_init()` allocates offscreen buffer, `nd_canvas()` is the only draw target, `nd_present()` copies completed rectangles once.
- Cursor changed from save-under to compositing overlay with damage; no hide/draw flash and no stale saved pixels.
- New `graphics/nd_boot.c`: splash with dark gradient, N mark, Novus OS text, rounded progress bar and phase text. State in `.data` (kernel_entry clears `.bss`); drawing uses only render primitives, no heap.
- Boot stage reordered: `stage3_prepare()` only probes block devices and opens ESP; new `stage3_load()` reads payloads after `nd_capture_boot()`; `boot_quiet()` disables text console/firmware cursor and logs to COM1.
- Fixed `kernel/shared_mem.h`: `wpkg_img_base/size` static assertions existed but fields missing → link failure; added at 0x308/0x310, `reserved` shortened 16 B, block still 8 KiB.
- Verification: `make` passed; `CONFIG_NOVUS_DISPLAY/WM/GUI=n` files compiled standalone; QEMU q35+OVMF showed `[ND] compositor: off-screen buffer, vsync available` and `[ND] boot splash kept until the desktop presents`; screenshots confirmed gradient background, first desktop frame and cursor without residue.

## 8. M9 Win32 compatibility prototype

- `kernel/pe.h/pe.c`: PE32+ loader at `0x50000000`, DIR64 relocations, name-based IAT, rejects DLL/TLS/delay imports/ordinal imports; maps headers/sections/thunks/command line/64 KiB stack/return trampoline; `sched_create_user_ex(..., TASK_ABI_WIN32)`.
- `kernel/win32.h/win32.c`: API gate and simulated registry; originally 15 APIs, later expanded to 141 (console/tick/Sleep/error codes/command line/module base/registry/heap/VirtualAlloc/user32 windows/messages/timers/coordinates/cursor/move/focus/gdi32 drawing/TEB/TLS/screen+font metrics/text layout/DT_WORDBREAK/file+dir/LoadLibrary/GetProcAddress/controls/MessageBoxA/menus/resources/version compatibility/ws2_32 handshake).
- `task_t` gains `abi`, `win32_last_error`, `win32_image_base`; dispatcher only allows `SYS_WIN32` and `SYS_EXIT` for `TASK_ABI_WIN32`.
- Sample `user/win32compat/samples/hello.c` (no windows.h/CRT); Makefile cross-compiles with mingw and `xxd -i` into `build/win32_blobs.h`; `CONFIG_NOVUS_WIN32` default y.
- sysinfo `/exe` virtual directory and file manager `/exe` tab; `KIND_EXE` optionally runs.
- Verification: .reloc DIR64; imports KERNEL32/ADVAPI32 nine functions; QEMU `/exe` run produced load line, three Win32 outputs and `task hello.exe exited with code 0`; `CONFIG_NOVUS_WIN32=n` still builds.

## 9. M11 phase 1: session, bilingual, installer

- Live media boots into `live` session without username/password; first account creation only after installation to writable NovusFS; install mode starts only `setup`.
- CN/EN bilingual: `tools/langgen/strings*.toml` generates `graphics/nd_lang_dict.c` and `apps/src/lang_dict.rs`; `nd_text`/`nd_text_width` translate at render layer, `ui::width()` uses the same table.
- `SYSINFO_REQ_LANG(0x030F)` and `kernel/lang.c`: immediate switching, persists to `/language.cfg` on installed systems, installer writes choice to new system disk.
- Installer expanded to seven pages: language, EULA, disk, scheme, confirm, progress, complete/fail. EULA and filesystem choice (NovusFS available; NTFS/ext4 "planned" and refused by kernel with `INSTALL_ERR_FS`).
- `kernel/usb.c`: xHCI 1.1 (command/event rings, port reset, slot/address, descriptors, SET_CONFIGURATION, Configure Endpoint) and BOT (CBW/CSW, READ CAPACITY, READ10/WRITE10) registered as `usb00`.
- `kernel/pci.c`: 64-bit BAR support, `pci_bar_assign()`; firmware BARs above 4 GiB moved into 32-bit PCI hole.
- Boot stage resolves USB ports from boot device path and publishes to shared memory; installer refuses the boot-only disk.
- Verification: standard AHCI install end-to-end, English install, post-install `[LANG] loaded: en`, live `[SESSION] live session: no account required`; USB install to `usb00` with Merkle PASS and boot from USB.

## 10. M11 phase 1 continued: multi-output

- `graphics/nd_hal.c` enumerates all firmware GOP outputs (≤4), reads EDID preferred timing and digital/analog link, switches to preferred mode when controller supports it; first output primary, rest mirror the same frame (`nd_mirror_rect()`, red/blue byte swap when needed).
- `nd_outputs()` reports one entry per framebuffer; firmware heads pointing to same memory are not treated as separate displays; Settings → System gains "display output" card.
- `kernel/sysinfo.c` adds `SYSINFO_REQ_DISPLAY(0x0318)`/`RSP(0x0319)`; client `sinfo::display()`.
- Fixed window redraw: CLEAR/SHAPE/TEXT no longer trigger per-message redraw; frame-end `INVALIDATE` marks the whole client area once (previously 50+ messages per frame caused half-frame flashes).
- `nd_selftest` adds three mirror byte-order round-trip checks (37 total pass).

## 11. M10 UI unification and graphical installer

- Single source `tools/themegen/theme.toml` generates `graphics/nd_theme.[ch]` and `apps/src/theme.rs`; `make theme` regenerates; light/dark/time-follow (dark 19:00–07:00).
- `nvgui` themes from `nd_tokens()`; shell rebuilds widget tree on version change; WM title bars follow theme; sysinfo theme reply appends `mode/resolved/version`.
- WM display list adds rounded fill/stroke ops 6/7 (radius in `pad`); `apps/src/ui.rs` upgraded to page/card/nav/row/button/ghost/progress/empty.
- Settings/file manager/task manager/network/login use role colors and poll theme version; settings adds theme mode, accent color and "open installer" entry.
- Boot stage stages `ESP.IMG` into reserved pages and publishes `esp_img_base/size/install_flags` in shared memory tail; menu 3 and `INSTALL.YES` set `boot_mode=INSTALL` and enter graphical install; text install kept under `CONFIG_NOVUS_INSTALLER_TEXT`.
- New `kernel/install.[ch]` and `SYS_SETUP=19`: candidate-disk detection, GPT, ESP/system image staged copy, NovusFS mount and Merkle verification, setup-pid authorization; `blkdev_release()` returns partition-view slots.
- New `apps/src/install.rs` and six-page `apps/src/bin/setup.rs`; app table launch by name (`apps_launch_name`, `SYSINFO_REQ_SPAWN_NAME`); new windows get focus immediately.
- New `scripts/run-install-test.sh`, UI design spec and graphical installer design doc; Chinese font data and theme files regenerated.
- Fixed: `nd_tokens()` lazy-built palette caused all-zero `g_current` in light mode (black desktop); tile titles use 1× font; reinstall allowed for exact Novus layout (ESP + NovusFS only) so retry works.
- Verification: `make` + `make iso`, mingw cross gcc; QEMU ISO menu 3 → six pages → `partition table written`/`ESP written`/`system image written`/`verify root ok`/`install complete`; boot from target disk with `[MRKL] PASS`; AUTO theme resolved dark with `-rtc base=localtime`.

## 12. cppcc M7 docs and build integration

- New `user/cppcc/README.md` (supported C++ subset, CLI, syscall builtin, known limits); new `docs/M7 cppcc 实现说明.md` (direct AST backend, x86_64 codegen, NUC V1.0 mapping, boot-stage load path).
- Revised `docs/M7 cppcc 实施方案.md` status and IR/stack/syscall descriptions to match actual codegen; retained as historical plan.
- README §7 and top summary changed to x86_64 native machine-code loading with fixed-path staging and generic exec still missing; M8/WM/NKM docs note M7 delays as historical; NKM spec clarifies cppcc output NUC, `nkmgen` makes test NKM images.
- New `user/cppcc/examples/hello.cc` calling `novus_write`.
- Make/Kconfig add `CONFIG_USER_CPPCC`; `make`/`make cppcc` build host tool, `make cppcc-demo` generates `out/cppcc/hello.nuc`.
- `make run` stages smoke NUC as `esp/APPS/DEMO.NUC`; ESP image rule packages same path; cppcc-enabled build takes the fixed demo slot.
- Verification at time of writing: cppcc static diagnostics clean; tests added for NUC byte layout, hello generation, control flow/functions and syntax error line/column; host lacked cargo/rustc/make/mingw/QEMU/xxd so `cargo test`, artifact byte checks and QEMU boot were **not executed**; no `nucdump` in repo; no git commit.
