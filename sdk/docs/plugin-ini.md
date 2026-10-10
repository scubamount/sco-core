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

[settings]              ; optional: values the player can change in the menu (see below)
speed = int default 5 min 1 max 10 label "Walk speed"
```

## Format

- One `key = value` per line. Spaces around `=` don't matter.
- `;` or `#` starts a comment at the start of a line or after a space or tab. `a;b` keeps its `;`.
- Blank lines are skipped. CRLF line ends and a UTF-8 byte-order mark are fine.
- Each key at most once. Unknown keys are ignored, so a later SDK can add keys.
- The file is at most 16 KB.
- A line `[name]` starts a section. `[settings]` is the only one defined; put every key above it. Other sections are skipped.

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

## Settings

A native or Lua plugin can let the player change values in the game menu without writing any UI. Declare them in a `[settings]` section, one per line:

```ini
[settings]
god_mode   = bool   default false label "God mode" help "Take no damage"
speed      = int    default 5 min 1 max 10 label "Walk speed"
fov        = float  default 90 min 60 max 120
nickname   = string default "Pilot"
difficulty = enum(easy,normal,hard) default normal
```

Each line is `name = type [default V] [min N] [max N] [label "text"] [help "text"]`, the fields in any order, each at most once. Values are bare words or text in double quotes (no quote inside). `;` and `#` start a comment outside quotes.

| Type | Value | Default if you leave it out |
|---|---|---|
| `bool` | `true` or `false` | `false` |
| `int` | An integer; `min`/`max` optional | 0, or the nearest bound |
| `float` | A plain decimal (`1.5`, `-2e-3`); `min`/`max` optional | 0, or the nearest bound |
| `string` | Up to 255 bytes of printable text | empty |
| `enum(a,b,c)` | One of up to 16 choices, each 1-31 of `a-z 0-9 _` | The first choice |

- The name is 1-31 of `a-z 0-9 _`, unique in the plugin; at most 32 settings. `label` is what the player sees (1-63 characters; the name if you leave it out), `help` a hint (1-255). The default must lie inside min and max. A data pack can't have settings.
- Read them in your code: C `get_int(self, "speed", &v)` and friends in [`sco_settings.h`](../../include/sco_settings.h) (`sco.settings`), C++ `sco::sdk::Settings`, C# `Sco.Sdk.Settings`, Lua `sco.settings.get(name)`. A read gives the default until the player changes it, and always a value your declaration allows. You can't write settings from the plugin. Subscribe to the event `settings.changed` to react to a change.
- The host keeps the values between launches. If an update of your plugin changes a setting's type, range or choices, a value saved for the old declaration is dropped and the new default applies (`mod.log` says so).
- A typo is an error, not skipped: an unknown field, a default outside its range, an unknown type or a repeated name refuses the plugin, and `sco-plugin-check` tells you the line.
- A game build older than this feature doesn't know the section and refuses the plugin ("expected key = value").

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
