# Changelog

sco-core has no releases yet. sc-offline pins a commit; this file lists what each change means for sc-offline.

## Unreleased

- **Docs**: README, [How it works](docs/architecture.md), [Adding a signature](docs/adding-signatures.md), [Checking a game build](docs/sigcheck.md), [API reference](docs/api.md), CONTRIBUTING.md and this changelog.

## 2026-10-08: first commit (`01d579c`)

- Scanners (`FindPattern`, `FindUniquePattern`, `BytesMatch`, `FindCString`, `FindRipLea`, `Rel32`, `RipTarget`) moved from sc-offline's `src/common.cpp`. One behavior change: a pattern longer than 96 bytes now matches nothing instead of matching only its first 96 bytes.
- Signature registry: named rows, dependency order, `Missing` / `Ambiguous` / `Failed` / `Blocked` results, and the `[core] signatures: N/M OK` report.
- Status channel: `sco::Status()` and `sco::GetStatus()` replace sc-offline's `SetMenuStatus`.
- Teleport's four addresses as `teleport.*` rows, moved byte for byte from sc-offline's `src/teleport.cpp`.
- `tools/sco-sigcheck`: runs every table against a `StarCitizen.exe` on disk.
- `tools/test.sh`: 42 unit checks with AddressSanitizer and UBSan.
