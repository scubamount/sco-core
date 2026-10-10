# Actor status and game event rows

The rows in [`src/game/events_sigs.cpp`](../../src/game/events_sigs.cpp) come from the spikes in [game world spikes](../design/game-world-spikes.md) (B4, B5, B6, B8), each locator and byte check as written there and proven unique in the .text of 4.10.196.36804. Accessor: [`sco/game/events.h`](../../include/sco/game/events.h).

| Capability | Rows |
|---|---|
| `game.actors.health` | `actor.health_site`, `actor.status_accessor`, `actor.get_stat` |
| `game.actors.state` | `actor.state_site`, `actor.status_accessor`, `actor.is_dead_confirmed` |
| `game.events.player_spawned` | `event.player_spawn` |
| `game.events.player_died` | `event.player_death` |
| `game.events.vehicle_seat` | `event.seat_enter`, `event.seat_exit` |

| Row | What it finds and checks |
|---|---|
| `actor.health_site` | One unique pattern in the code that logs "...HealthPool: %.4f, Stun: %.4f": `+0x00` calls the status accessor, `+0x0D` `GetStat(status, 6)`, `+0x1D` and `+0x2A` the accessor and `GetStat(status, 0x0B)` again. Checks the accessor is `mov rax,[rcx+208h]; add rax,74A0h; ret` and that both pairs of calls reach the same functions |
| `actor.status_accessor` / `actor.get_stat` | The call targets at `actor.health_site +0x00` and `+0x0D` |
| `actor.state_site` | One unique pattern in the "not fully alive" test, `P2(status) \|\| P1(status)`: the accessor call at `+0x00`, `P2` called at `+0x0B`, `mov rdx,[rcx+60h]` (`P1`, status vtable slot `0x60`) at `+0x17` |
| `actor.is_dead_confirmed` | `P2`: checked to be the function that labels itself `CSCActorStatus::IsDeadConfirmed` (one label load, its .pdata function). Which of `P1` and `P2` means dead and which incapacitated is not pinned by any byte |
| `event.player_spawn` | `SCigEventDispatcher::QueueEvent<SPI_Player_OnSpawn>`: one unique prologue; `+0x2C` calls entity slot `0x7A8`, `+0x60` loads its label (and the label's function is the match) |
| `event.player_death` | `QueueEvent<SPI_Player_OnDeath>`: its label (one load, at `+0x414`) and .pdata; prologue, `+0x22` (`rbx = rdx; r15 = rcx`) and `+0x69` (the actor's entity handle) checked |
| `event.seat_enter` / `event.seat_exit` | `CSCActorResultStateLinked::Enter` / `::Exit`: one label load each and .pdata; prologues checked |
