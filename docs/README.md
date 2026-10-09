# sco-core documentation

Start with the [README](../README.md) for what sco-core is. Pick your path below. Where sco-core and sc-offline are heading: [Framework plan](framework.md).

## Writing a plugin

| Doc | What |
|---|---|
| [Plugin SDK](../sdk/README.md) | Build the examples, start from the template, check and install a plugin |
| [Plugin API v1](api-v1.md) | `sco_api.h`: exports, the host table, commands, events, capabilities, threading, compatibility, layout |
| [plugin.ini](../sdk/docs/plugin-ini.md) | Every key of the plugin manifest |
| [Data packs](../sdk/docs/data-packs.md) | Missions, rules, scripts and lists without code |
| [Lua plugins](../sdk/docs/lua.md) | The `sco` table, the sandbox and its limits |
| [Plugin rules](../sdk/docs/plugin-rules.md) | What plugins may and may not do, and what the host guarantees |

## Hosting plugins (sc-offline and sco-core developers)

| Doc | What |
|---|---|
| [Plugins](plugins.md) | Discovery, `plugin.ini` checks, the native loader, crash containment, Lua, data packs, status |
| [sco-lua](../plugins/lua/README.md) | The Lua runtime: sandbox, step and memory limits, updating Lua |
| [How it works](architecture.md) | The runtime (tasks, events, commands, owners), threading and limits |
| [C++ API reference](api.md) | Every function and type in `include/sco/`, including `caps`, `host` and the runtime |

## Game core

| Doc | What |
|---|---|
| [How it works](architecture.md) | Signature rows, results, resolving, the report, startup |
| [Adding a signature](adding-signatures.md) | Moving a feature's addresses into a table, and the rules rows follow |
| [Checking a game build](sigcheck.md) | `sco-sigcheck`: output, exit codes, the patch-day routine |

## Working on sco-core

| Doc | What |
|---|---|
| [Building and testing](building.md) | `tools/test.sh`, CMake and CTest, the Windows run, the SDK zip, CI |
| [Contributing](../CONTRIBUTING.md) | What fits, how to make a change, how it reaches sc-offline |
| [Changelog](../CHANGELOG.md) | What changed |
