# Local IPC: `sco.ipc` and `sc_ipc.h`

A bridge links sc-offline to another program on the same PC (another game, a mod of it) through one named shared-memory mapping. Two headers:

| Header | License | Who includes it |
|---|---|---|
| [`include/sc_ipc.h`](../include/sc_ipc.h) | **MIT** (the [interface exception](../LICENSE)) | Both sides. The other program needs **only this file**: plain C11 / C++20, freestanding headers, no sco-core dependency. |
| [`include/sco_ipc.h`](../include/sco_ipc.h) | GPL-3.0 | The sc-offline plugin: the host service `sco.ipc` 1.0, the only way a plugin creates a channel. |

sc-offline's two bridges also publish their channel layouts as MIT headers ([Bridge layouts](#bridge-layouts)).

Design: [multiplayer.md § 3.3](design/multiplayer.md#33-sc_ipch-the-wire-mit-and-the-scoipc-service-the-in-game-wrapper).

## The channel

The plugin creates it; the other program (the **peer**) opens it by name and never creates it.

- **Name:** `Local\SCO_<plugin id>.<channel>`, built by the host. A plugin can't choose `Global\`, another plugin's prefix or any other name.
- **Access:** the current user only (the mapping's security descriptor names that user as owner and grants nobody else). If a mapping of that name already exists, the host uses it only when the current user owns it and it is large enough.
- **Sizes:** 4 KiB to 256 MiB per channel, 512 MiB over a plugin's open channels, 16 channels per plugin, 32 rings per channel.
- **Lifetime:** when the plugin closes the channel, unloads or crashes, the host marks it closed (`SC_IPC_GONE` for the peer at once), stops its heartbeat and unmaps it.

```text
offset 0     sc_ipc_hdr   64 bytes: magic "SCIO", version, owner/peer pid, epoch, heartbeats,
                          size, layout_id / layout_version, state
offset 64..  the bridge's layout: sc_ipc_ring (at multiples of 64) and sc_ipc_block (multiples
             of 8) regions at offsets both sides agree on; layout_id/version name that agreement
```

Every struct's size and offsets are `static_assert`ed in `sc_ipc.h`, so a layout drift fails to compile on both sides.

## The peer's side (the other program)

```c
#include "sc_ipc.h"                                    /* nothing else from sco-core */

HANDLE h = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, L"Local\\SCO_voxel_bridge.link");
void* base = MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
MEMORY_BASIC_INFORMATION mbi;
VirtualQuery(base, &mbi, sizeof mbi);                  /* the view's size from the OS, never from the mapping */

sc_ipc_chan ch;
sc_ipc_ring_view in, out;
if (sc_ipc_attach(base, mbi.RegionSize, MY_LAYOUT_ID, 1, &ch) == SC_IPC_OK &&
    sc_ipc_ring_attach(&ch, IN_OFFSET, &in) == SC_IPC_OK &&     /* SC_IPC_TO_PEER: sc-offline pushes */
    sc_ipc_ring_attach(&ch, OUT_OFFSET, &out) == SC_IPC_OK) {   /* SC_IPC_FROM_PEER: we push */
    sc_ipc_peer_beat(&ch, GetCurrentProcessId(), GetTickCount64());   /* at least once a second */
    uint8_t msg[256]; uint32_t type, n = sizeof msg;
    while (sc_ipc_ring_pop(&in, &type, msg, &n) == SC_IPC_OK) { handle(type, msg, n); n = sizeof msg; }
    sc_ipc_ring_push(&out, MSG_POSE, &pose, sizeof pose);
    sc_ipc_block_read(&ch, POSE_BLOCK, &snapshot, sizeof snapshot);  /* seqlock: SC_IPC_BUSY = retry */
}
```

- **Results** are `int`: `SC_IPC_OK` (0), `FULL`, `EMPTY`, `TOO_SMALL` (pop: `*inout_size` holds the size needed, the record stays queued), `BAD_ARG`, `CORRUPT` (the other side wrote something impossible; nothing is read), `EPOCH` (the channel or ring was re-created: attach again), `BUSY` (a block or the header is mid-write: retry), `NOT_READY`, `MISMATCH` (another wire version, layout or block size), `GONE` (closed).
- **Epoch:** sc-offline bumps the header's epoch each time it re-creates the channel (a restart while you kept the mapping). Your views remember the epoch they attached under; `SC_IPC_EPOCH` means re-attach (`sc_ipc_check` tells without touching a ring).
- **Heartbeats** are `GetTickCount64()` milliseconds, one clock for both processes. sc-offline beats from its host tick; `sc_ipc_owner_age_ms` tells how long ago. Pick a timeout (a few seconds) and treat the link as down past it, or at `SC_IPC_GONE`.
- **Never block:** every call returns at once; poll from your own loop or frame.

## Rings and blocks

- **Ring:** single producer, single consumer, one direction (`SC_IPC_TO_PEER` or `SC_IPC_FROM_PEER`, fixed when the plugin lays it out). Byte capacity a power of two (at least 64); records are an 8-byte header (`size`, `type`) plus the payload padded to 8. A record never wraps: when it doesn't fit before the end, the producer fills the end with one pad record (type `0xFFFFFFFF`) and starts at 0. Head and tail are 64-bit byte counters on their own cache lines; they wrap at 2^64.
- **Block:** a seqlock snapshot (`seq`, `size`, then the bytes) with one writer. `seq` is odd while being written and 0 until first written; a reader that sees it change re-reads (`SC_IPC_BUSY`).

## Trust: a hostile peer

Either side may be buggy or hostile and can write anything into the mapping at any time. `sc_ipc.h` reads the geometry once, at attach, validates it against the view size the OS reported, and keeps it in the caller's own memory (`sc_ipc_chan`, `sc_ipc_ring_view`). From then on every counter, length and record header read from the mapping is checked against that cached geometry before use (read once into a local, so it can't change between the check and the use): an impossible value is `SC_IPC_CORRUPT`, never an access outside the mapping. The host logs the first such refusal per channel. Only plain bytes cross: no pointer, handle or code address is ever put in a channel.

## Bridge layouts

The layout of a channel (which blocks and rings sit at which offsets, and the structs and messages inside them) belongs to the bridge, not to `sc_ipc.h`. sc-offline's two bridges ship theirs in the SDK as standalone headers, so the program on the other side builds against the published SDK alone:

| Header | License | Channel | `layout_id` | Version constant | Other side |
|---|---|---|---|---|---|
| [`include/sc_titanlink.h`](../include/sc_titanlink.h) | **MIT** (the [interface exception](../LICENSE)) | `Local\SCO_titanlink.link`, 64 KiB | `0x314B4C54` ("TLK1") | `TL_LAYOUT_VERSION` = 1 | The Northstar plugin for Titanfall 2 (its own repository) |
| [`include/sc_voxel_bridge.h`](../include/sc_voxel_bridge.h) | **MIT** (the [interface exception](../LICENSE)) | `Local\SCO_voxel_bridge.link`, 8 MiB | `0x31425856` ("VXB1") | `VX_LAYOUT_VERSION` = 1 | The voxel game's mod |

- **Both sides vendor the header from the SDK.** sc-offline's built-in (the owner) and the other program (the peer) each copy the same header from a published SDK; neither keeps its own version of the structs.
- **The version must match.** The owner creates the channel with the header's `layout_id` and `*_LAYOUT_VERSION`; the peer passes its copy's values to `sc_ipc_attach`, which answers `SC_IPC_MISMATCH` for any other. Any change to a struct, offset or constant bumps the version, so an old peer is refused instead of misreading the channel.
- **Pinned twice.** Each header `static_assert`s every struct size and field offset and that the regions fit the channel without overlapping; [`tests/abi_titanlink.c`](../tests/abi_titanlink.c) and [`tests/abi_voxel_bridge.c`](../tests/abi_voxel_bridge.c) pin the same numbers and every constant (C11, C++20, `-fshort-enums`, x86_64-pc-windows-msvc, and each header on its own).
- Plain C11 / C++20, `<stddef.h>` and `<stdint.h>` only, no sco-core include (not even `sc_ipc.h`: a peer includes both). Little-endian, natural alignment.

What each field means and what the other side must do (beats, timeouts, the checks sc-offline applies) is in sc-offline's `docs/bridges.md`.

## The plugin's side (`sco.ipc` 1.0)

| Function | What |
|---|---|
| `create(self, name, bytes, layout_id, layout_version, &channel)` | Creates (or re-opens) `Local\SCO_<id>.<name>`, lays out the header with a new epoch |
| `ring_init(self, channel, offset, capacity, direction)` | Lays out an empty ring (again after every `create`) |
| `ring_push` / `ring_pop` | `TO_PEER` rings only / `FROM_PEER` rings only; `ring_pop` uses the size handshake |
| `block_write` / `block_read` | Seqlock snapshots |
| `peer_age_ms` | Milliseconds since the peer's last beat (`SCO_NOT_FOUND` before the first) |
| `view` | The plugin's own mapping, for bulk regions (video frames); valid until close or unload |
| `close` | Marks it closed for the peer and unmaps it |

Any thread; calls never block. Channels are `uint64_t` ids. C++: [`scosdk/ipc.hpp`](../include/scosdk/ipc.hpp) (`sco::sdk::Ipc`, `IpcChannel`, which closes on destruction). Lua plugins get no `sco.ipc` ([plugin rules](../sdk/docs/plugin-rules.md)). Host side: [`sco/ipc.h`](../include/sco/ipc.h); the host kit starts it, ticks the heartbeat and stops it after every plugin unloads.
