# Novus OS Driver License Exception

> Version: v1.0 (2026-11-27)
> Applies to: third-party developers and vendors who wish to write **closed-source / proprietary device drivers** for Novus OS
> Companion: `../LICENSE`, `../COPYING`, [SECURITY.md](SECURITY.md), [COMPATIBILITY.md](COMPATIBILITY.md)
> 中文版本：[../DRIVER-LICENSE.md](../DRIVER-LICENSE.md) · [Index](INDEX.md)

---

## 1. Why this document exists

Novus OS itself is **GPLv2** (`../LICENSE`, `../COPYING`). Section 10 of GPLv2 permits copyright holders to grant **additional permissions** without modifying the GPLv2 text itself.

This document is such an **additional permission**: it allows third parties to distribute **closed-source device drivers**, subject to the boundaries here. It does **not** modify, replace or weaken GPLv2. Where it conflicts with GPLv2, the interpretation more favorable to the permission granted here applies — only within the explicitly authorized scope.

> [!NOTE]
> This is part of the project license, not legal advice. Before commercial distribution assess your target markets and consult counsel if needed.

## 2. Scope of the grant

You may write, distribute and sell a driver under a **proprietary license (including closed source)** without publishing source, provided that:

1. The driver is independently created and does not contain/copy/derive from Novus OS GPLv2 code (`kernel/`, `fs/`, `security/`, `graphics/`, `wm/`, `apps/`, `tools/`, `lib/`, etc.).
2. It interacts **only through published, frozen ABIs**: syscall ABI ([ABI.md](ABI.md) §2), NKM format and `nksym_table` symbols (§4), Novus-WM/IPC protocol (§5), VFS/block device public interfaces (§6).
3. It is distributed as an NKM module, a NUC application, or both; it does not modify kernel source and does not statically link kernel code into the driver binary.
4. It is signed with a project-recognized or vendor-managed key following [SECURITY.md](SECURITY.md) §4.1. Closed source ≠ exempt from signing.
5. It explicitly declares: name, version, supported ABI/kernel versions, vendor/author, proprietary license name, and "This driver is not an official Novus OS component."
6. It does not relicense GPLv2 code as closed source and does not close-source project trademarks, logos or documentation.

Under these conditions the driver binary may contain only machine code and data (**no source required**); you may charge for it, bundle it or ship it as firmware; GPLv2 does not extend to the driver.

## 3. Explicitly out of scope

| Action | Why |
|---|---|
| Modifying/forking/recompiling kernel or any GPLv2 component | Derived works must stay GPLv2 |
| Copying GPLv2 source into a closed-source driver repo | Even "reference implementation" is a copy |
| Bypassing NKM signature checks or enabling `NKM_FLAG_ALLOW_UNSIGNED` in production | Security violation |
| Using unpublished kernel-internal symbols, layouts or private protocols | Only `nksym_table` and frozen ABIs allowed |
| Presenting a closed driver as an official built-in component | Trademarks/attribution not granted |
| Using this exception for non-driver user apps | Ordinary apps may use NUC but gain no closed-source exception |

> If a driver needs a new symbol or ABI, **open an Issue/RFC and publish the interface first**, then follow the ABI change process in [CHANGELOG.md](CHANGELOG.md). Do not extend the kernel via private patches.

## 4. Signing and trust model

Closed-source and open-source drivers are treated **identically** for security:

1. **NKM drivers must be signed**: SHA-256 with `[0x50,0xB0)` zeroed, plus Ed25519.
2. **Keys**: development seed for QEMU/self-test only (never production); vendors generate their own production keys and submit root public keys through the defined process (current implementation has only a development key, no runtime rotation — a **known limitation**, not a capability of this exception).
3. **ABI version** in the driver header must match the kernel.
4. **Fault isolation**: driver crash must not bring down the kernel; user drivers are process-isolated, Ring 0 drivers follow NKM lifecycle.

> Closed-source drivers cannot be source-audited, so they **should** use least privilege, fixed boundaries and complete logging. The project may refuse to load drivers that compromise stability or security.

## 5. Distribution and labeling

Every closed-source driver package must include a `DRIVER_NOTICE` (plain text or Markdown) with at least:

```text
Driver:       <name>
Version:      <version>
Vendor:       <vendor/author>
License:      <proprietary license name>
ABI Version:  <e.g. NUC 0x0100 / NKM major 1>
Kernel:       <supported V1.6-FinalSpec or specific version>
Signature:    <signing key fingerprint or vendor key ID>
Notice:       This is a third-party closed-source driver.
              It is NOT an official Novus OS component.
              Licensed under <license>, not GPLv2.
              See DRIVER-LICENSE.md for the granted exception.
```

Also include a copy of this file and `../LICENSE`; preserve other components' notices; return a clear error code on load failure — never silently fall back to an unsafe state.

## 6. Compatibility responsibilities

- Frozen ABIs mean drivers **should** work across kernel minor versions in the same major; major bumps require revalidation.
- On **[ABI BREAK]** the project bumps the major version and announces; driver authors must release a compatible version.
- No undocumented debug interfaces, private sysinfo request numbers or QEMU-specific behavior.
- Declare required memory mode (LowMem/Full) and required peripherals.

## 7. Relationship to GPLv2

| File | License | Notes |
|---|---|---|
| `LICENSE`/`COPYING` | GPLv2 verbatim | **Not modified** |
| Project source | GPLv2 | Derived works remain GPLv2 |
| Drivers meeting this document | Proprietary | Additional permission granted |
| Drivers not meeting it | No exception | Assessed under GPLv2 |

Irrevocable for released compliant driver versions; the project may adjust terms for future versions.

## 8. Official compatibility list application

1. Open an Issue with `DRIVER_NOTICE`, signing key fingerprint, ABI version and test report.
2. Provide reproducible load/functionality tests (QEMU + real hardware logs).
3. Pass security review: no unpublished symbols, no signature bypass, no OOB access.
4. Once approved, an entry is added to [COMPATIBILITY.md](COMPATIBILITY.md) linking to your docs.

Listing does **not** endorse safety or quality — only ABI/process compliance.
