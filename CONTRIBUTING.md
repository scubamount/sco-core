# Contributing to sco-core

sco-core is the game-facing core of [sc-offline](https://github.com/scubamount/sc-offline). Most changes start there: a feature moving its addresses here, or a game patch breaking a row. Read sc-offline's [CONTRIBUTING.md](https://github.com/scubamount/sc-offline/blob/main/CONTRIBUTING.md) first; its rules apply here too.

## What fits

- **Offline, single-player only.** Nothing that connects to Star Citizen's servers, changes online play, or helps cheating or anti-cheat bypass. PRs like that are closed.
- **No game files.** Byte patterns and the names of strings the game references are fine. Don't commit pieces of `StarCitizen.exe`, dumps or anything extracted from the game.
- **No secrets or personal data** in code, tests or logs you paste.
- **Portable core.** Only `src/sco_image_win.cpp` may include Windows headers. Everything else must build with clang or gcc on macOS and Linux, so the tests and `sco-sigcheck` keep running without Windows.
- **No new dependencies** without discussing it in an issue first.

## Making a change

1. Open an issue first for anything bigger than a small fix.
2. Follow [Adding a signature](docs/adding-signatures.md) for new rows; its rules (move without changing, one address per row, unique or nothing) are checked in review.
3. Run `tools/test.sh`. It must end with `0 failed`. New registry or scanner behavior gets a test in `tests/test_core.cpp`; runtime behavior (tasks, events, commands) in `tests/test_runtime.cpp`.
4. Run `tools/sigcheck.sh` against a real `StarCitizen.exe` if you touched a table, and paste the `[core]` lines and the game build into the PR.
5. Update the docs your change affects (`README.md`, `docs/`, header comments) and add a line to [CHANGELOG.md](CHANGELOG.md).
6. Open the PR against `main`. CI (`test`) must pass.

## Reaching sc-offline

sc-offline pins sco-core as a submodule at `external/sco-core`. After a sco-core change merges, a sc-offline PR moves the pin (`git -C external/sco-core checkout <sha>` then commit `external/sco-core`) along with any code that uses the change. That PR is where the Windows build and in-game testing happen.

## Code style

- C++20 in the style of the surrounding code. Code the DLL compiles stays small: no `std::regex`, iostreams or other heavy headers outside `tools/`.
- Resolvers return `SigFail("<static reason>")` with a reason a maintainer can act on: which check failed, at which offset.
- Comments say why, not what.

## License

GPL-3.0 ([LICENSE](LICENSE)). By opening a pull request you agree your contribution is licensed under GPL-3.0.
