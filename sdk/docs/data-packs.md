# Data packs

A data pack adds content files and nothing else. It never runs code, so it is the safest kind of plugin to install.

```
data\plugins\travel_pack\
  plugin.ini          kind = data, no entry
  lists\locations.txt
```

## What a pack can carry

| Folder | Files | Kind in the content index |
|---|---|---|
| `missions\` | `*.cwmission` | `mission` |
| `rules\` | `*.rules` | `rules` |
| `scripts\` | `*.xml`, in any subfolder depth | `script` |
| `lists\` | `*.txt` | `list` |
| `datacore\` | `*.toml` | `datacore`: changes to game data ([below](#game-data-datacore)) |

Extensions match in any case. Everything else in the folder is ignored. Links (symlinks, junctions) are skipped, so a pack can't point outside its own folder. A pack holds at most 4096 content files; more refuses the whole pack.

## How features use it

At startup sc-offline builds a content index: one entry per file, with its kind, the pack's id and the path relative to the pack (`lists/locations.txt`). Features ask the index for their kind and read the files themselves. Several packs may ship a file with the same name; features get them in plugin order (folder name order).

Each feature decides which files it reads and in what format. A list file uses the format of the sc-offline file it extends: [`examples/travel_pack/lists/locations.txt`](../examples/travel_pack/lists/locations.txt) uses the `system | name | entity | radius` lines of sc-offline's `data\locations.txt`. Which features read pack content, and from which names, is listed in sc-offline's docs as each feature gains support.

## Game data: `datacore\`

A pack can change the game's own data (`Data\Game2.dcb`, the DataCore database) with `.toml` files in `datacore\`: set a record's field, add an object cloned from another and point a field at it, append to an array. Records and fields are named, not located by byte offset, so a pack keeps working across game patches as long as the records and fields it names still exist. [`examples/quantum_pack`](../examples/quantum_pack/datacore/eos.toml) is a complete example:

```toml
format = 1

[[set]]
record = "EntityClassDefinition.QDRV_RSI_S01_Eos_SCItem"
guid   = "08a5bfdb-1972-421f-83fe-be03b7ac5222"
field  = "Components[SCItemQuantumDriveParams].params.spoolUpTime"
value  = 3.5
```

The whole format (`[[set]]`, `[[instance]]`, `[[append]]`, value forms, `atomic`) is in sco-core's [DataCore packs reference](../../docs/datacore.md#pack-format). The rules that matter most:

- **All or nothing per file.** If any override in a file can't be applied (a record or field the game no longer has, a value of the wrong type), none of that file applies, and `mod.log` says why. `atomic = false` lets each override apply on its own, for lists of unrelated tweaks.
- **Plugin order decides.** When two packs set the same field, the one later in plugin order (folder name order) wins, and `mod.log` names both.
- **The patch is applied when the game loads its data**, at startup. Changing a pack takes effect at the next launch.

To find names and paths, extract `Game2.dcb` from `Data.p4k` and use sco-core's `sco-dcb` tool ([how](../../docs/datacore.md)): `sco-dcb show Game2.dcb <record>` prints every field as the path a pack writes. Before you ship, and again after each game patch:

```sh
sco-dcb lint my_pack                  # syntax and shape, no game file
sco-dcb check Game2.dcb my_pack       # every override resolved against the game's file; exit 0 = all apply
```

## Build and check

A pack needs no compiler. `sco_add_pack(<id> DIR <folder>)` in CMake copies it into `data/plugins/<id>/` on install, or copy the folder by hand.

```sh
out/bin/sco-plugin-check out/data/plugins/travel_pack
```

lists every file the host would index and fails if there are none. For `datacore\` files it only reports that they were indexed; `sco-dcb lint` and `sco-dcb check` check their content.
