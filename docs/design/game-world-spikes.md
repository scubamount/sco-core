# Design: game.world and event spikes (Appendix B)

Status: spike results of 2026-10-10. No code yet. This is the "Appendix B" that [game services](game-services.md) PR 6 requires before any `game.world` or `game.*` event code: for each item, what to read or hook, how it is located, the byte checks that pin what the code relies on, the arguments, and proof that the locator is unique.

**Build checked:** StarCitizen.exe 4.10.196.36804, `E:/Games/Star Citizen/StarCitizen/LIVE/Bin64/StarCitizen.exe`, **PE timestamp `0x6ac64e6f`** (printed by both tools below). `.text` is `0x7e8ae9c` bytes. Every count below comes from that file, on sco-core `029c5b3`.

## How the evidence was produced

Two tools, both run on this build. Every count quoted in a section is copied from their output.

1. **sigcheck**, for rows that already exist: `bun ~/.omo/tmp/sigcheck-local.js <sco-core worktree> -v`. This builds `tools/sco-sigcheck` with Zig and runs it on the exe. Output: `image: timestamp 0x6ac64e6f ...` and `[core] signatures: 300/300 OK`. Saved as `~/sc-baseline/4.10.196.36804/sigcheck-spikes-029c5b3.txt`.
2. **A throwaway probe** (not committed), built against sco-core's own scanners: `zig c++ -target x86_64-windows-gnu -std=c++20 -O2 -I include -o probe.exe probe.cpp src/sco_scan.cpp src/sco_pe_file.cpp`, then `probe.exe StarCitizen.exe script.txt`. It maps the exe with `sco::FileImage` and runs one command per script line:
   - `str <text>`: how many NUL-bounded copies of the string are in `.rdata`.
   - `lea <r0> <r1> <r2> <text>`: how many RIP-relative LEAs with that encoding point at the string. This is the `FindRipLea` match, counted over all of `.text`.
   - `pat <pattern>`: `sco::FindPattern` over `.text`, with the match count.
   - `at <rva> <pattern>`: `sco::BytesMatch` at an address.
   - `fn <rva>`: `sco::FunctionStart`, which walks `.pdata` the same way a row would.

   Output saved as `~/sc-baseline/4.10.196.36804/spikes-game-world-probe.txt`.

The RVAs in the evidence only identify this build's matches. A row never uses an RVA: it finds the anchor or pattern, then applies the offsets and checks listed here.

Disassembly was read with a local Capstone script, to understand the code only. Uniqueness is never claimed from it.

## Verdicts

| # | Item | Verdict | Locator (count on 4.10.196) | Capability |
|---|---|---|---|---|
| B1 | Raycast | **FOUND**: existing rows | `build.ground_ray` site pattern: 1 | `game.world.raycast` |
| B2 | Camera position and rotation | **FOUND**: existing rows | `CmdTeleportToCamera` LEA: 1; camera accessor pattern: 1 | `game.world.camera` |
| B2 | Camera FOV | **BLOCKED** | candidate `CalculateFOV` LEA: 1; the value it returns is not pinned | none yet |
| B3 | Entity enumeration (`query_radius`) | **FOUND locator**, in-game check pending | `EntitySystemDump Begin` LEA: 1; ForEach call-site pattern: 1 | `game.entities.query_radius` |
| B4 | Actor health | **FOUND** | HealthPool read pattern: 1; status accessor pattern: 1 | `game.actors.health` |
| B4 | Actor state (alive / incapacitated / dead) | **FOUND locator**; which predicate is dead and which is incapacitated is pending | state-check pattern: 1; `IsDeadConfirmed` LEA: 1 | `game.actors.state` |
| B5 | `game.player.spawned` | **FOUND** | `QueueEvent<SPI_Player_OnSpawn>` LEA: 1; prologue pattern: 1 | `game.events.player_spawned` |
| B6 | `game.player.died` | **FOUND** | `QueueEvent<SPI_Player_OnDeath>` LEA: 1 | `game.events.player_died` |
| B7 | `game.zone.changed` | **FOUND**: poll on existing rows, no new hook | teleport rows: 300/300 OK; zone-slot anchors: 1 and 1 | `game.events.zone_changed` |
| B8 | `game.vehicle.boarded` / `exited` | **FOUND locator**, payload pending | `CSCActorResultStateLinked::Enter` LEA: 1, `::Exit` LEA: 1 | `game.events.vehicle_seat` |
| B9 | Entity `streamed_in` / `streamed_out` | **FOUND locator**, payload and thread pending | `CallOnSpawnSinks` label LEA: 1; `DeleteEntity` labels LEA: 1 and 1 | `game.entities.watch` |

**What the verdicts mean.**
- **FOUND**: every locator matches exactly once (tool output below), the byte checks match, and the arguments come from the disassembly.
- **FOUND locator**: the address is proven the same way, but part of the payload can only be confirmed in game. Examples are which actor fired, the vector layout, and the calling thread. Each section lists what the maintainer's in-game run has to confirm before PR 6 ships that item. Until then, the capability stays off.
- **BLOCKED**: no locator whose result we can trust. Nothing is published for it.

Adversarial notes:
- *misleading success output*: no item is FOUND on reasoning alone. Each FOUND line cites a probe or sigcheck count of exactly 1.
- *stale state*: every run is on the 4.10.196.36804 exe, with its timestamp printed above.
- The other adversarial classes don't apply to a docs-only spike.

## B1. Raycast: `game.world.raycast`

**Verdict: FOUND, on existing rows.** sc-offline's build mode already casts this ray. The rows are on main in `src/game/world_sigs.cpp` (capability `build.ground_ray`): `build.ray_tag`, `build.ground_ray`, `build.phys_world`, `build.release_grid`, plus the entity vtable slots `build.entity_ray_proxy` (0x208) and `build.entity_skip_add` (0x430).

- **What it calls:** the physical world's ray query. The world object is the global at `build.phys_world`, and the query is its vtable slot `0x1C0`, called as `int (*)(world, ray_params* rp, int 30)`. `rp` is the 0xA8-byte block that `build.cpp` `CastInZone` fills. Its fields:
  - `+0x18`: origin, three doubles, zone-local.
  - `+0x30`: direction, three floats.
  - `+0x3C`: `0x101`.
  - `+0x40`: `0x20F`.
  - `+0x48`: the hit buffer, 0x60 bytes per hit.
  - `+0x50`: max hits.
  - `+0x70`: the skip list.
  - `+0x80`: the zone's grid.
  - `+0x88`: the `PlanetRayIntersection` tag.

  The zone's grid comes from zone vtable slot `0x30`. Release it afterwards with `build.release_grid`. Casts are zone-local, so walk up `ZoneParent`, as `RayHit` does.
- **Locator:** the string anchor `PlanetRayIntersection`, plus a RIP-relative LEA that passes the site checks. The checks are in `GroundRaySite`: `L-0xBF` `48 8B 3D` (the world global), `L-0x42` `48 8B 98 C0 01 00 00` (slot 0x1C0), `L+0x07`, `L+0x26`, `L+0x80` `FF 50 30 41 B8 1E 00 00 00` (the 30), `L+0x9A` `FF D3`, and `L+0xF2` `33 D2 E8`. The release function must load the same world global.
- **Uniqueness evidence:** the string has 3 LEAs, and exactly one of them passes the checks. That one site also matches a pattern exactly once:
  ```
  str  count=1 first=0x83a80e0  "PlanetRayIntersection"
  lea  48 8D 05 count=3 at 0x26d3096 0x271c36f 0x2f88c7d  "PlanetRayIntersection"
  pat  count=1 at 0x2f88c7d  "48 8D 05 ?? ?? ?? ?? C7 85 9C 00 00 00 01 01 00 00 48 89 85 E8 00 00 00"
  at   0x2f88c3b MATCH  "48 8B 98 C0 01 00 00"
  at   0x2f88cfd MATCH  "FF 50 30 41 B8 1E 00 00 00"
  pat  count=1 at 0x613b10  "48 8B D1 48 8B 0D ?? ?? ?? ?? 48 8B 01 48 FF A0 28 02 00 00"
  sigcheck: OK build.ground_ray 0x2f88c7d, OK build.phys_world 0x9e3f788, OK build.release_grid 0x613b10,
            OK build.entity_ray_proxy 0x6897780, OK build.entity_skip_add 0x6873990
  ```
- **What v1 can return:** the hit position (`from + dir * dist`, where `dist` is the float at hit `+0x10`) and whether anything was hit (the u64 at hit `+0`). The design also asks for the hit entity and the normal. Their fields in the 0x60-byte hit are not pinned by any read the game makes at this site. Verify them in game, or v1 leaves them zeroed and says so in the header.

## B2. Camera: `game.world.camera`

**Verdict: position and rotation FOUND on existing rows. FOV BLOCKED.**

- **What it reads:** `actor + 0x208` is a per-actor object (`build.cpp` names it `cam`). Inside it:
  - position: three doubles at `+0x6D20`;
  - rotation: floats x, y, z, w at `+0x6D38`.

  These are `kCameraPosition` and `kCameraRotation` in `include/sco/game/world.h`. The actor comes from `reads::LocalPlayer`.
- **Locator:** row `build.camera_fields` (capability `build.camera`), at `teleport.to_camera + 0x17A`. `teleport.to_camera` is found through the string `CmdTeleportToCamera`, whose LEA is unique. The row's checks pin all six loads, offsets included: `vmovss xmm6,[rax+6D44]`, `xmm7,[+6D38]`, `xmm8,[+6D3C]`, `xmm9,[+6D40]`, `vmovups xmm10,[+6D20]`, `vmovsd xmm11,[+6D30]`. `teleport.to_camera + 0x151`, `48 8B 83 08 02 00 00`, pins `actor+0x208`. There is a second, independent pin: the game's own accessor `mov rax,[rcx+208h]; add rax,6D00h; ret` matches exactly once. That puts the camera block at `+0x6D00` of the same object.
- **Uniqueness evidence:**
  ```
  str  count=1 first=0x850d308  "CmdTeleportToCamera"
  lea  48 8D 15 count=1 at 0x2e27298  "CmdTeleportToCamera"
  at   0x2e27291 MATCH  "48 8B 83 08 02 00 00"
  at   0x2e272ba MATCH  "C5 FA 10 B0 44 6D 00 00"   ... (all six loads MATCH)
  pat  count=1 at 0x2e77f10  "48 8B 81 08 02 00 00 48 05 00 6D 00 00 C3"
  sigcheck: OK teleport.to_camera 0x2e27140, OK build.camera_fields 0x2e272ba
  ```
- **FOV, BLOCKED.** The only candidate is `CameraViewFirstPersonBase::CalculateFOV`: its label is unique (`lea 4C 8D 05 count=1 at 0x401ba04`), and `fn 0x401ba04 -> FunctionStart 0x401b980`. It calls the camera accessor above and returns a float in `xmm0`. But it computes a value instead of storing one: it blends two inputs and scales by two constants. Its `this` is a first-person camera view, not the actor. Nothing in the exe ties its result to the view actually rendered (third person, turrets, free cam). Reading it would mean hooking a per-frame function whose units and scope aren't proven, so FOV stays out of `camera()` v1. The header should say "fov: not available" rather than return a guess.
- **Found on the way:** sc-offline `main`'s `build.cpp` still reads `cam+0x6D18` and `+0x6D30`, the offsets from before 4.10.196. sc-offline PR #85 (`rows-combined`) already switches it to `kCameraPosition`, so the fix lands with #85.

## B3. Entity enumeration: `game.entities.query_radius`

**Verdict: FOUND locator. In-game check pending: the thread the callback runs on.**

- **What it calls:** the game's own "for each entity" path, used by the console dump `EntitySystemDump`. Without a name filter, the dump calls `ForEach(*g_entityIndex, &callable)`. `callable` is `{ void* ctx; void (*fn)(void* ctx, uint64_t entityHandle) }`. The game's lambda masks the handle with `0xFFFFFFFFFFFF` to get the entity pointer, then reads the entity's vtable slots `+0x20` and `+0x398`. The dump's `this` is the entity system, which is the same global as row `teleport.entity_system` (`0x9e3f808` on this build). `[es+0x128]` is the total entity count it prints. `query_radius` would walk the list, filter by zone and distance with the existing `EntityZone` and `EntityLocalPos` reads, and filter by class with `class_of`.
- **Locator:** this pattern inside `EntitySystemDump`, unique on its own:

  `48 8B 0D ?? ?? ?? ?? 48 8D 55 E7 48 89 45 F7 41 80 E0 01 48 8D 45 B7`

  It contains:
  - `mov rcx, [entity index]`, the RIP operand at `+3`.
  - the callable's lambda LEA at `+0x3B` (`48 8D 05`).
  - the ForEach call at `+0x46` (`E8`).

  Cross-check: the string anchor `EntitySystemDump Begin` is unique and sits in the same function.
- **Uniqueness evidence:**
  ```
  str  count=1 first=0x8c6b4b0  "EntitySystemDump Begin"
  lea  48 8D 0D count=1 at 0x688cac0  "EntitySystemDump Begin"
  pat  count=1 at 0x688c918  "48 8B 0D ?? ?? ?? ?? 48 8D 55 E7 48 89 45 F7 41 80 E0 01 48 8D 45 B7"
  at   0x688c953 MATCH  "48 8D 05"
  at   0x688c95e MATCH  "E8"
  at   0x688cb65 MATCH  "44 8B 86 28 01 00 00"
  ```
- **Pending in game:** ForEach wraps the callable in the engine's job system (it builds a job with id `0x7831`). The callback may therefore run on worker threads, or in parallel. Before PR 5 ships `query_radius`, the maintainer's run must log the thread id inside the callback and confirm it runs synchronously on the game thread. If it doesn't, the table function has to collect handles under a lock and filter after ForEach returns.

## B4. Actor health and state: `game.actors.health`, `game.actors.state`

**Verdict: health FOUND. State: locator FOUND, but which predicate means dead and which means incapacitated is pending.**

- **What it reads:** `status = actor[0x208] + 0x74A0` (`CSCActorStatus`), through the game's accessor `mov rax,[rcx+208h]; add rax,74A0h; ret`.
  - **Health:** `float GetStat(status, 0x0B)`. The game logs `"... IncapacitatedStatusEffect: %s, HealthPool: %.4f, Stun: %.4f"` from two calls: `GetStat(status, 6)` goes to the 5th argument (Stun) and `GetStat(status, 0x0B)` to `r9`, the 4th (HealthPool). Both `GetStat` and the accessor are RIP-followed from the same pattern, at `+0x00` (accessor `E8`) and `+0x0D` (`GetStat` `E8`).
  - **State:** the game decides "not fully alive" as `P2(status) || P1(status)`:
    - `P2` is a direct call to the function that names itself `CSCActorStatus::IsDeadConfirmed` (its label LEA is unique; `FunctionStart` of it is the called function).
    - `P1` is the status vtable slot `0x60`.

    Both are `bool (*)(status)`. The log that follows prints "Dead: [$$], Incapacitated: [$$]" from these two results. The function's own name supports P2 = dead, which would make P1 = incapacitated, but the argument order in that log isn't pinned by any byte. "Alive" (neither predicate true) is proven. The dead/incapacitated split needs one in-game check: down an NPC, kill an NPC, log P1 and P2 each time. Until then, `state()` returns alive or not-alive only, or the capability stays off.
- **Uniqueness evidence:**
  ```
  pat  count=1 at 0x5bd3fe0  "E8 ?? ?? ?? ?? BA 06 00 00 00 48 8B C8 E8 ?? ?? ?? ?? 48 8B 8E F8 00 00 00 C5 F8 28 F0 E8 ?? ?? ?? ?? BA 0B 00 00 00 48 8B C8 E8"
  pat  count=1 at 0x2e77ef0  "48 8B 81 08 02 00 00 48 05 A0 74 00 00 C3"
  str  count=1 ... "Actor is requesting the incapacitated state ... HealthPool: %.4f, Stun: %.4f"
  lea  48 8D 15 count=2 at 0x5bd4029 0x62279ff   (the string alone is NOT unique; the pattern above is)
  pat  count=2 at 0x400a702 0x6224c05  "E8 ?? ?? ?? ?? 48 8B C8 48 8B D8 E8 ?? ?? ?? ?? 84 C0 75 14 48 8B 0B 48 8B 51 60 48 8B CB FF D2 84 C0"
  pat  count=1 at 0x400a702  "<same> 0F 84 ?? ?? ?? ?? 48 8D 44 24 70"
  str  count=1 first=0x87b6810  "CSCActorStatus::IsDeadConfirmed"
  lea  48 8D 05 count=1 at 0x3fec92a  "CSCActorStatus::IsDeadConfirmed"
  fn   0x3fec92a -> FunctionStart 0x3fec460
  ```
  The state row uses the second, longer pattern, which is unique. From it: the accessor at `+0x00` (`E8`), `P2` at `+0x0B` (`E8`, whose target must equal `FunctionStart` of the `IsDeadConfirmed` label), and slot `0x60` checked at `+0x17` (`48 8B 51 60`).
- **Thread:** read only, game thread, like the other `reads` functions.

## B5. `game.player.spawned`

**Verdict: FOUND (hook).**

- **What it hooks:** the function that queues the game's own `SPI_Player_OnSpawn` event. Its label `void __cdecl SCigEventDispatcher::QueueEvent<struct SPI_Player_OnSpawn>(...)` is loaded at `+0x60`.
- **Arguments:** `void fn(actor* rcx, bool dl, void* r8)`. `rcx` is an actor: the function masks `[rcx+8]` with `0xFFFFFFFFFFFF` (actor to entity, the same shape as `reads::LocalPlayer`) and calls entity slot `0x7A8`. The caller passes `[obj+8] & mask`, `byte [obj+0x18]` and `[obj+0x20]`. The event's other fields are not needed: the bus payload is the actor's entity id and its zone, read through the existing `reads`. Bus events are low volume, so the host compares the actor with `LocalPlayer` and publishes only the local player's spawn.
- **Locator:** this prologue pattern, unique on its own:

  `48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 40 48 8B F9 48 B8 FF FF FF FF FF FF 00 00 48 8B 49 08 49 8B D8 48 23 C8`

  Checks: the label LEA at `+0x60`, and `FF 90 A8 07 00 00` at `+0x2C`. Cross-check: `FunctionStart` of the label LEA is the pattern's match.
- **Uniqueness evidence:**
  ```
  str  count=1 first=0x8bb4c30  "void __cdecl SCigEventDispatcher::QueueEvent<struct SPI_Player_OnSpawn>(const struct SPI_Player_OnSpawn &)"
  lea  48 8D 05 count=1 at 0x602a7f0  (same string)
  pat  count=1 at 0x602a790  "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 40 48 8B F9 48 B8 FF FF FF FF FF FF 00 00 48 8B 49 08 49 8B D8 48 23 C8"
  at   0x602a7bc MATCH  "FF 90 A8 07 00 00"
  fn   0x602a7f0 -> FunctionStart 0x602a790
  ```

## B6. `game.player.died`

**Verdict: FOUND (hook).**

- **What it hooks:** the function that builds and queues `SPI_Player_OnDeath`. Its label `void __cdecl SCigEventDispatcher::QueueEvent<struct SPI_Player_OnDeath>(...)` is loaded at `+0x414`.
- **Arguments:** `void fn(actor* rcx, const hit_info* rdx, void* r8)`. At `+0x22` it saves them, `48 8B DA 4C 8B F9` (`rbx = rdx`, `r15 = rcx`). At `+0x69` it passes `[r15+8]`, the actor's entity handle, on (`4D 8B 47 08`). `[rdx+0x78]` is a damage type (compared with 2, 0x13 and 0x16). Callers: 2. The payload is the victim's entity id. The killer's id is not pinned, so v1 leaves it 0. As with B5, the host publishes only for the local player.
- **Locator:** string anchor plus `FunctionStart` (`.pdata`), with checks at `+0x00` (prologue `4C 89 44 24 18 48 89 54 24 10 55 53 57 41 54 41 55 41 57`), `+0x22` and `+0x69`.
- **Uniqueness evidence:**
  ```
  str  count=1 first=0x851cd60  "void __cdecl SCigEventDispatcher::QueueEvent<struct SPI_Player_OnDeath>(const struct SPI_Player_OnDeath &)"
  lea  48 8D 05 count=1 at 0x2edbae4  (same string)
  fn   0x2edbae4 -> FunctionStart 0x2edb6d0
  at   0x2edb6d0 MATCH  "4C 89 44 24 18 48 89 54 24 10 55 53 57 41 54 41 55 41 57"
  at   0x2edb6f2 MATCH  "48 8B DA 4C 8B F9"
  at   0x2edb739 MATCH  "4D 8B 47 08"
  ```

## B7. `game.zone.changed`

**Verdict: FOUND. A poll on existing rows, no new hook.**

- **Decision:** the event fires when the local player's zone id differs from last tick's. The game pack's per-tick task reads `reads::LocalPlayer`, `EntityZone` (entity slot `0x6B8`) and `ZoneId` (zone slot `0x58`). Those come from rows `teleport.to_camera`, `teleport.client_mgr`, `teleport.handle_from_id` and `teleport.entity_system`, with the slot checks `VerifyEntityPositionSlots` and `VerifyZoneSlots`. That is two virtual calls a tick, already proven. The only hook-shaped candidate, `CExtrapolatePredictor::OnZoneChange`, belongs to network extrapolation of remote entities and isn't tied to the local player. A hook there would need its own payload spike and buys nothing over the poll. Rejected.
- **Uniqueness evidence:**
  ```
  sigcheck: [core] signatures: 300/300 OK; OK teleport.to_camera 0x2e27140, OK teleport.client_mgr 0xa1ae208,
            OK teleport.handle_from_id 0x2d42630, OK teleport.entity_system 0x9e3f808
  lea  4C 8D 05 count=1 at 0x5372fc2  "Zone: %s, ZonePos:(%f.2,%f.2,%f.2), WorldPos((%f.2,%f.2,%f.2)"   (VerifyEntityPositionSlots: slot 0x6B8)
  lea  48 8D 15 count=1 at 0x51e93ec  "Changing reference point to unstreamable parent zone: ..."         (VerifyZoneSlots: slot 0x58)
  lea  4C 8D 05 count=1 at 0x68262b4  "CExtrapolatePredictor::OnZoneChange"                                  (rejected candidate)
  ```
- **Payload:** `{ size, old_zone_id, new_zone_id }`. Zone ids are session handles, as the design requires.

## B8. `game.vehicle.boarded` / `exited`

**Verdict: FOUND locator. Payload pending: which actor, and whether the link is a vehicle seat.**

- **What it hooks:** the seated actor-state's `Enter` and `Exit`, the virtual functions of `CSCActorResultStateLinked`. Each loads its own label, so `FunctionStart` of the label's LEA is the function. Both have no direct callers, because they are reached through the vtable. The sibling state's label spells out the signature: `void __cdecl CSCActorResultStatePlayerShadow::Exit(class CSCActorResultStateHost &,const struct SCActorStateData::SData &)`. So both functions are `void (state* rcx, CSCActorResultStateHost* rdx, const SData* r8)`.
- **Locator:** string anchor plus `FunctionStart`, with prologue checks at `+0x00`:
  - `Enter`: `48 89 5C 24 10 4C 89 44 24 18 55 56 57 41 54 41 55 41 56 41 57`.
  - `Exit`: `4C 89 44 24 18 48 89 54 24 10 48 89 4C 24 08 55 53 56 57`.

  `CSCActorResultStateLinked::OnEnterSeat` (unique LEA) is a seat-specific alternative for "boarded".
- **Uniqueness evidence:**
  ```
  lea  4C 8D 05 count=1 at 0x6133a5a  "CSCActorResultStateLinked::Enter"    fn 0x6133a5a -> FunctionStart 0x6133960   at 0x6133960 MATCH
  lea  4C 8D 05 count=1 at 0x6133f06  "CSCActorResultStateLinked::Exit"     fn 0x6133f06 -> FunctionStart 0x6133e40   at 0x6133e40 MATCH
  lea  48 8D 0D count=1 at 0x6142ba8  "CSCActorResultStateLinked::OnEnterSeat"
  str  count=1 first=0x8ba1e40  "void __cdecl CSCActorResultStatePlayerShadow::Exit(class CSCActorResultStateHost &,const struct SCActorStateData::SData &)"
  ```
- **Pending in game:** nothing in the exe ties the state host to an actor, or proves that a "linked" state means a vehicle seat (it may also cover chairs and beds). The maintainer's run must log, for both hooks: the host pointer, `LocalPlayer`'s actor, and the actor's parent entity class. Do this in a pilot seat, a turret, a station chair and a bed. Two rules follow from that run:
  - The host fires `boarded` and `exited` only when the hook's actor is the local player and the linked entity is a vehicle.
  - The payload is `{ size, vehicle_entity_id, seat_index }`. `seat_index` comes from `game.vehicles` (PR 3) once that exists. Until then it is -1.

## B9. Entity `streamed_in` / `streamed_out`: `game.entities.watch`

**Verdict: FOUND locator. The vector layout and the calling thread are pending.**

- **streamed_in hook:** `CEntitySystem::CallOnSpawnSinks(es* rcx, const std::vector<IEntityPtr>* rdx)`. It loads its own label at `+0x17`, so `FunctionStart` is the function. It hands `rdx` to every registered sink. The sink list is `[es+0x30 .. es+0x38]`, called as `sink->vtbl[1](sink, rdx)`. There is 1 caller (in the spawn update), and it runs once per spawned batch. The host's watch matches each entity's class against the plugin's `type` filter before calling into the plugin, as the design requires.
- **streamed_out hook:** `CEntitySystem::DeleteEntity(es* rcx, CEntityPtr* rdx)`. Both of its labels are unique. The entity is `*rdx & 0xFFFFFFFFFFFF`. The caller stores the handle at `[rsp+0x30]` and passes its address (`48 8D 54 24 30 ... E8`), and the function itself does `48 8B 1F 49 23 DC`. Its one caller loops over the frame's deleted entities, so every removal path ends here. That is why `DeleteEntity` is used instead of the three `RemoveEntity*` entry points.
- **Locators and checks:**
  - `CallOnSpawnSinks`: `+0x00` `4C 8B DC 49 89 5B 10 49 89 6B 18 49 89 73 20 57 48 81 EC 80 00 00 00`; `+0x70` `48 8B 5F 30 33 C9 48 8B 7F 38` (sink list); `+0xF0` `48 8B 0B 48 8B D5 48 8B 01 FF 50 08` (sink call).
  - `DeleteEntity`: `+0x00` `48 8B C4 48 89 58 10 48 89 70 20 57 41 54 41 55 41 56 41 57`; `+0x26` `48 8B FA`; `+0xBB` `48 8B 1F 49 23 DC`.
- **Uniqueness evidence:**
  ```
  lea  48 8D 05 count=1 at 0x687d967  "void __cdecl CEntitySystem::CallOnSpawnSinks(const class std::vector<class IEntityPtr,struct TempAllocator<class IEntityPtr> > &)"
  fn   0x687d967 -> FunctionStart 0x687d950    at 0x687d950 / 0x687d9c0 / 0x687da40 MATCH
  lea  4C 8D 05 count=1 at 0x6886c8c  "CEntitySystem::DeleteEntity"
  lea  48 8D 05 count=1 at 0x6886d8b  "void __cdecl CEntitySystem::DeleteEntity(class CEntityPtr)"
  fn   0x6886c8c -> FunctionStart 0x6886c20    at 0x6886c20 / 0x6886c46 / 0x6886cdb MATCH; caller at 0x686c5d4 MATCH
  ```
- **Pending in game:**
  1. **The vector layout.** Is it `begin` at `[rdx]` and `end` at `[rdx+8]`? The `TempAllocator` could be stateful. The run must log both and the element count for one batch.
  2. **The thread** both hooks run on. The game logs "RemoveEntity ... on batch thread, deferring", so spawning has a batch thread. If either hook isn't on the game thread, the host queues the matches and calls plugins from its game-thread tick. The design's "fn runs on the game thread" rule then holds by construction.

  "Streamed out" here means deleted on this client. Offline that is the same thing; with multiplayer later, that is worth restating.

## What PR 6 builds from this

- **`game.world`:** `raycast` (position and hit flag; entity and normal only after their field check) and `camera` (position and rotation; no FOV). Both on existing rows. No new rows.
- **Bus events:**
  - `game.player.spawned` and `game.player.died`: two new hook rows, B5 and B6.
  - `game.zone.changed`: a poll, B7.
  - `game.vehicle.boarded` / `exited`: two hook rows behind `game.events.vehicle_seat`, which stays off until B8's in-game check.
- **`game.entities`:** `query_radius` (B3) and `watch`/`unwatch` (B9). Their capabilities stay off until their in-game checks pass.
- **`game.actors`:** `health` (B4), added to the 1.0 table merged in #62 as a minor version. `state` after B4's check.
- **Not built:** camera FOV (B2, BLOCKED).

Every new row is written in `src/game/*_sigs.cpp` with exactly the locator and checks listed above, and gets its own capability. The sigcheck report for the PR is saved under `~/sc-baseline/4.10.196.36804/`.
