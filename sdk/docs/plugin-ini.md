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
requires = teleport, spawn.ship   ; optional: capabilities the plugin can't run without
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
| `requires` | no | Up to 16 capability names, comma-separated (lowercase dotted, at most 63 bytes each) |

## What the host does with it

| Situation | State in `mod.log` |
|---|---|
| `plugins = off` in `sc-offline.ini` (the default) | `off` |
| The folder holds a file named `disabled` | `disabled` |
| A rule above is broken, the id clashes, `api` doesn't fit or the entry file is missing | `refused: <reason>` |
| A capability in `requires` is missing on this game build | `refused: missing capability '<name>'` |
| Everything checks out | `loaded` |
| The plugin faulted in a callback | `crashed` (it is never called again this session) |

`sco-plugin-check <folder>` applies the same rules on your machine.
