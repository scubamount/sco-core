# plugin.ini

Every plugin folder holds a `plugin.ini`. sc-offline reads only `data\plugins\*\plugin.ini`; a folder without one is skipped.

```ini
id = hello              ; lowercase a-z 0-9 _, 1-31 characters; must equal the folder name
name = Hello            ; shown in the menu, mod.log and status
version = 1.0.0
author = you            ; optional
api = 1.0               ; the sco_api version the plugin needs
kind = native           ; native | lua | data
entry = hello.dll       ; native: the DLL; lua: the main script; data: leave it out
requires = teleport, other.svc   ; optional: capabilities, services and plugin ids the plugin can't run without
```

## Format

- One `key = value` per line. Spaces around `=` don't matter.
- `;` or `#` starts a comment at the start of a line or after a space or tab. `a;b` keeps its `;`.
- Blank lines are skipped. CRLF line ends and a UTF-8 byte-order mark are fine.
- Each key at most once. Unknown keys are ignored, so a later SDK can add keys.
- The file is at most 16 KB.

## Keys

| Key | Required | Rule |
|---|---|---|
| `id` | yes | 1-31 characters from `a-z`, `0-9`, `_`. Not `sco`, `host`, `menu` or `game`. Equal to the folder name. Unique among installed plugins. For a native plugin, also equal to `sco_plugin_info.name` |
| `name` | yes | Free text, at most 63 bytes |
| `version` | yes | At most 31 bytes; `1.0.0` style is recommended |
| `author` | no | At most 63 bytes |
| `api` | yes | `<major>.<minor>`. The major must equal the host's; the minor may not be newer than the host's |
| `kind` | yes | `native`, `lua` or `data` |
| `entry` | native, lua | A bare file name in the plugin folder: no `/`, `\` or `:`. Not allowed for `data` |
| `requires` | no | Up to 16 names, comma-separated, no repeats (lowercase dotted, at most 63 bytes each): a capability (`teleport`), a service (`<plugin id>.<name>`, or a host or game service such as `sco.storage` or `game.vehicles`) or a plugin id. See [Load order](#load-order) |

## What the host does with it

| Situation | State in `mod.log` |
|---|---|
| `plugins = off` in `sc-offline.ini` (the default) | `off` |
| The folder holds a file named `disabled` | `disabled` |
| A rule above is broken, the id clashes, `api` doesn't fit or the entry file is missing | `refused: <reason>` |
| A name in `requires` with no dot (a capability or a plugin id) is missing | `refused: missing capability '<name>'` |
| A dotted name in `requires` is no capability, no host or game service and no installed plugin's service | `refused: requires service <name>: no plugin provides it` |
| `requires` leads in a circle (`a` needs `b`, `b` needs `a`; a plugin naming itself counts) | `refused: requires cycle: a -> b -> a` |
| A plugin you require is refused, disabled or fails to load | `refused: requires <id>: plugin '<id>' is <state> (<reason>)` |
| Everything checks out | `loaded` |
| The plugin faulted in a callback | `crashed` (it is never called again this session) |

`sco-plugin-check <folder>` applies the same rules on your machine, except the ones that need the other installed plugins (a missing provider, a cycle between two plugins, a provider that fails to load): only the real host sees those. It refuses a plugin that requires itself.

## Load order

The host loads plugins in folder order, except that a plugin loads **after** every plugin its `requires` names. Name a plugin's id (`requires = nav`) or one of its services (`requires = nav.spatial`: a service name always starts with its provider's id, so the host knows the provider before any code runs). Capabilities and host or game services (`sco.storage`, `sco.ui`, `game.vehicles`) are already there when plugins load and need no ordering; `sco-plugin-check --cap NAME` stands in for them. Plugins unload in the reverse order, so a provider outlives the plugins that need it.

There is no separate `provides` key: a plugin can only publish services named `<its id>` or `<its id>.<name>`. The order guarantees the provider loaded first, not that it published the name (it may publish later); check `query_service`'s result as before. Native, Lua and data plugins follow the same rules. A cycle, or a name nobody provides, refuses the plugin with the reason in the table above; nothing waits or hangs.
