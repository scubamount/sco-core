# Plugin rules

sc-offline is an offline, single-player mod. Plugins follow the same scope as sc-offline's own code ([CONTRIBUTING](https://github.com/scubamount/sc-offline/blob/main/CONTRIBUTING.md#what-fits-this-project)).

## What a plugin may not do

- Connect to anything: no network access, no Star Citizen servers, no online play of any kind.
- Help cheating or anti-cheat bypass, or change how the game behaves online.
- Ship pieces of the game: no parts of `StarCitizen.exe`, dumps, or files extracted from `Data.p4k`. Entity and class names in text files are fine.
- Collect or send personal data.

## What a plugin should do

- Use only `sco_api.h` (native) or the `sco` table (Lua). The C++ headers in sco-core's `include/sco/` are internal and change without notice.
- Check `has()` before offering a feature, and say why it is off, instead of failing.
- Keep callbacks short. `tick` runs about 10 times a second on the game's main thread; slow work there stalls the game.
- Name everything with its id: commands are `<id>.<action>`, and the host prefixes log lines and status messages with it.

## What the host guarantees

- Nothing loads unless the player sets `plugins = on`. Each plugin can be switched off on its own.
- Every plugin is listed in `mod.log` and `sc-offline.exe status` with its state.
- A native plugin that faults inside a callback is disabled and never called again; everything it registered is removed and the game keeps running. This limits damage; it is not a sandbox. A native plugin runs with the game's full rights.
- A Lua plugin runs in a sandbox with no file, process or network access, and an instruction budget per callback.
- A data pack never runs code.
- sc-offline's self-update never touches `data\plugins\`.

## License

Plugins built against `sco_api.h` are GPL-3.0, like sco-core and sc-offline. There is no linking exception.
