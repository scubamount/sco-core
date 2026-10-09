# toml++ (vendored)

[toml++](https://github.com/marzer/tomlplusplus) by Mark Gillard, the TOML parser behind DataCore data packs ([docs/datacore.md](../../docs/datacore.md), design decision 4). The two files are byte-for-byte the release's; nothing here is edited.

| | |
|---|---|
| Version | 3.4.0 (tag `v3.4.0`, the latest release on 2026-10-09) |
| Source | <https://raw.githubusercontent.com/marzer/tomlplusplus/v3.4.0/toml.hpp> and <https://raw.githubusercontent.com/marzer/tomlplusplus/v3.4.0/LICENSE> |
| `toml.hpp` SHA-256 | `6b5172ad4dd6519aec67b919181fa7a38a2234131e5b2afa232dfe444819783e` (17,748 lines, the single-header build) |
| `LICENSE` SHA-256 | `529bc3900a9571e49db285b0df432397e70b881cc3bf48de6667ae74ff4b06d8` |
| License | MIT ([LICENSE](LICENSE)), compatible with sco-core's GPL-3.0: the combined work is distributed under GPL-3.0 and the MIT notice ships with it |

`tools/test.sh` checks both SHA-256 sums before it builds anything that uses the header.

## How it is used

Only `src/datacore/pack.cpp` includes it, through `src/datacore/toml.h`, so it is compiled into the `sco_datacore` library (and through it `sco-dcb`) and nowhere else. That wrapper:

- defines `TOML_EXCEPTIONS 0`: a parse error is a `toml::parse_result` turned into a pack refusal reason, never an exception crossing sco-core;
- turns the header's own warnings off around the include (`#pragma warning(push, 0)` on MSVC, `#pragma GCC diagnostic` on clang and gcc), and the include directory is a SYSTEM one, so sco-core's code keeps `/W4 /WX` and `-Wall -Wextra -Werror`.

toml++ needs C++17; sco-core is C++20. It has no network or file code that sco-core calls: packs are read by sco-core and handed to `toml::parse` as text.

## Updating

1. Download `toml.hpp` and `LICENSE` from the new release tag.
2. Replace both files and update the table above and the sums in `tools/test.sh`.
3. Run `tools/test.sh` and the CMake build: `test_datacore_pack` holds the golden parses of valid and invalid packs, so a parsing change shows there.
