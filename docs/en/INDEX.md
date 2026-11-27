# Novus OS — English Documentation

> This directory contains English versions of the project-level documents. Chinese originals remain at the repository root.
> Maintained as of 2026-11-27 (V1.6-FinalSpec baseline).

## Documents

| Document | Description |
|---|---|
| [README.md](README.md) | Project overview, quick start, documentation map |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Environment, validation matrix, PR rules |
| [CODESTYLE.md](CODESTYLE.md) | C/Rust coding conventions |
| [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) | Community behavior rules |
| [SECURITY.md](SECURITY.md) | Security boundaries, vulnerability reporting, SLA, CVE, disclosure, 0day policy |
| [ROADMAP.md](ROADMAP.md) | Status, gaps, phased plan (M0–M12 and beyond) |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Layering, boot, kernel, VFS, GUI, network, build flows with Mermaid diagrams |
| [COMPATIBILITY.md](COMPATIBILITY.md) | Hardware, media, filesystem, ABI, language and toolchain compatibility |
| [BUILD.md](BUILD.md) | Toolchain, targets, images, verification and troubleshooting |
| [CHANGELOG.md](CHANGELOG.md) | Version history with highlighted ABI BREAKs |
| [GLOSSARY.md](GLOSSARY.md) | Project-specific terms, abbreviations and filename prefixes |
| [ABI.md](ABI.md) | Implemented frozen ABI quick reference |
| [DRIVER-LICENSE.md](DRIVER-LICENSE.md) | Closed-source driver license exception (GPLv2 §10) |
| [CHANGES.md](CHANGES.md) | Development change log summary |

## Reading path for newcomers

1. [README.md](README.md) — what the project is
2. [GLOSSARY.md](GLOSSARY.md) — decode the custom names
3. [ARCHITECTURE.md](ARCHITECTURE.md) — how it fits together
4. [BUILD.md](BUILD.md) — get it running
5. [CONTRIBUTING.md](CONTRIBUTING.md) — start contributing

## Notes

- Frozen format specifications (NUC/NKM/NUP/INS/IPC/update) remain in `docs/` in Chinese and are authoritative.
- Where a translation conflicts with source code or a frozen spec, the source/spec wins.
- Chinese originals: `../` + filename (e.g. `../SECURITY.md`).
