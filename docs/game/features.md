# Rows for sc-offline's own features

Rows that sc-offline's features used to find by scanning the game themselves, moved into sco-core (`src/game/features_sigs.cpp`, `include/sco/game/features.h`) so `sco-sigcheck` checks them against a new `StarCitizen.exe` before anyone plays. Each resolver is the old scan, moved byte for byte. Where the old scan took "the last match" or "every match", the row now insists on the count found in 4.10.196.36804, so a patch that adds or loses a site shows as a failed row.

Rows only find addresses. sc-offline still writes the patches and installs the hooks, after `ResolveAll` (rows match the game's original bytes).

| Row | Found by | Checks | Capability |
|---|---|---|---|
| `spawn.request_fly_mode` | every `89 54 24 10 48 83 EC 28 48 8D 54 24 38 E8` wrapper whose call target loads (`lea` at +0xAE) the `CSCActorActionHandler::Request<SFlyMode,...>` label | exactly one such wrapper | `spawn.fly_mode` |
| `npc.remove_entity_call` | the first `mov rcx,[entity system]; ...; call [rax+slot]` loop site | every site on `teleport.entity_system` agrees on the slot (read at +15), slot in (0, 0x1000) | `npc.clear` |
| `quantum.send_effect_tag` | every tag sender pattern whose `lea rdx` at +0x25 loads the `EntityEffectSystem.h` path | exactly one | `quantum.effect_tag` |
| `contracts.reputation_services` | the `mov rcx,[services]; ...` load within 0x120 bytes before the "Couldn't access reputation service internal" `lea` | RIP target inside the image | `contracts.reputation` |
| `contracts.reputation_check.1`..`.10` | the two check forms on that global, form A in address order then form B | exactly 10 sites (9 + 1 in 4.10.196.36804); tail at +16, patch at +13 | `contracts.reputation` |
| `offline.or_loop_bound.1`..`.4` | the OR-loop pattern | exactly 4 sites; bound check at +27 | `offline.or_loop_bound` |

Checked locally with `sco-sigcheck` on 4.10.196.36804: 88/88 OK, every address equal to what sc-offline's old scan found.
