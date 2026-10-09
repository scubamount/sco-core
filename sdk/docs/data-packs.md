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

Extensions match in any case. Everything else in the folder is ignored. Links (symlinks, junctions) are skipped, so a pack can't point outside its own folder. A pack holds at most 4096 content files; more refuses the whole pack.

## How features use it

At startup sc-offline builds a content index: one entry per file, with its kind, the pack's id and the path relative to the pack (`lists/locations.txt`). Features ask the index for their kind and read the files themselves. Several packs may ship a file with the same name; features get them in plugin order (folder name order).

Each feature decides which files it reads and in what format. A list file uses the format of the sc-offline file it extends: [`examples/travel_pack/lists/locations.txt`](../examples/travel_pack/lists/locations.txt) uses the `system | name | entity | radius` lines of sc-offline's `data\locations.txt`. Which features read pack content, and from which names, is listed in sc-offline's docs as each feature gains support.

## Build and check

A pack needs no compiler. `sco_add_pack(<id> DIR <folder>)` in CMake copies it into `data/plugins/<id>/` on install, or copy the folder by hand.

```sh
out/bin/sco-plugin-check out/data/plugins/travel_pack
```

lists every file the host would index and fails if there are none.
