# Plugin rules

sc-offline is an offline mod: while it runs, the game never connects to Cloud Imperium Games' servers. Plugins follow the same scope as sc-offline's own code ([CONTRIBUTING](https://github.com/scubamount/sc-offline/blob/main/CONTRIBUTING.md#what-fits-this-project)). Offline play stays the default.

## What a plugin may connect to

Only these two, and only through the host's services:

- **Other processes on the same PC, by local IPC** (shared memory), for example a bridge to Titanfall 2 (Northstar) or Minecraft.
- **Other players running sc-offline, privately, over a LAN or a VPN they choose** (Tailscale, ZeroTier), through sco-core's `sco.net` service: co-presence. Each player runs their own offline game. Peers exchange their own authenticated messages (poses, spawn announcements), and each game shows remote players as "ghost" entities it spawns itself. Co-presence doesn't use or change the game's own netcode or servers.

Neither service has shipped yet; the design is in review in [sco-core PR #37](https://github.com/scubamount/sco-core/pull/37). Until they ship, a plugin connects to nothing. A native plugin can technically open its own sockets or shared memory (it runs with the game's rights), so this is a review rule, not a sandbox.

## What a plugin may not do

These are permanent:

- Connect to Cloud Imperium Games' servers or online services, or take part in public or official online play.
- Get around anti-cheat or skip any anti-cheat step, or bypass signature checks.
- Tamper with accounts or entitlements.
- Force the game's host type or override its network context.
- Run console commands or other commands from a peer without authentication.
- Send telemetry.
- Help anyone cheat in the official game.
- Open network connections or shared memory of its own, outside `sco.net` and the IPC service.
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
- A Lua plugin runs in a sandbox with no file, process or network access, and an instruction budget per callback. Once `sco.net` ships, one exception: a Lua plugin whose `plugin.ini` declares `requires = sco.net` may read the peer list and publish and subscribe to `<plugin-id>.<channel>` messages. It never gets a socket, an address or session control (hosting, joining, admitting or removing peers), and gets no IPC.
- A data pack never runs code.
- sc-offline's self-update never touches `data\plugins\`.

## License

Plugins built against `sco_api.h` are GPL-3.0, like sco-core and sc-offline. There is no linking exception.
