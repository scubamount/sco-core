# Multiplayer messages: `sco.net`

`sco.net` 1.0 lets a plugin exchange typed messages with the same plugin in the other players' games, when the player is in a private session. Sessions are between players who all run sc-offline, one of them hosting on their own PC; nothing here talks to Cloud Imperium Games' servers, and nothing finds or lists sessions: players share an address. Design: [multiplayer.md § 3.1](design/multiplayer.md#31-sconet-10-a-session-and-typed-channels-game-agnostic). The packets: [sco.net wire format](net-wire.md).

| Header | For |
|---|---|
| [`include/sco_net.h`](../include/sco_net.h) | The service table `sco_net_v1`, plain C, pinned by `tests/abi_net.c` |
| [`include/scosdk/net.hpp`](../include/scosdk/net.hpp) | The C++20 wrapper: `Net`, `NetChannel` |
| `include/sco/net/session.h` | The **product's** side (host, join, leave). Not for plugins |

## What a plugin can and can't do

- It **can** see whether a session is up (`is_active`), who is in it (`get_peers`, `get_peer_name`, `self_peer`), register channels under its own id, send on them, and get the other players' messages.
- It **can't** host, join or leave a session, pick a port or an address, or see one. Those are the product's decisions (sc-offline's launcher and menu). A plugin can never make a player's PC listen.
- With no session, `is_active()` is 0, `send_channel` answers `SCO_UNAVAILABLE`, and no callback runs. A plugin must work exactly as before offline: send only "in addition" (see the design's [announce, don't centralize](design/multiplayer.md#42-domain-features-announce-dont-centralize)).

Declare the dependency so a host without the service refuses your plugin with a clear reason instead of loading it to find a NULL table:

```ini
requires = sco.net
```

The host sets the capability `sco.net` whenever it publishes the service.

## Channels

A channel is `"<plugin id>.<name>"`: `myplugin.pose`, `myplugin.spawn`. `<name>` is one or more dot-separated parts of `[A-Za-z0-9_-]`; the whole is at most 64 bytes. **A plugin registers and sends only under its own id**; anything else is `SCO_BAD_ARG`. At most 32 channels per plugin.

| Flag | Meaning |
|---|---|
| (none) | Unreliable: at most 1,200 bytes, may be lost or arrive out of order, never twice. For state that is resent anyway (poses) |
| `SCO_NET_RELIABLE` | Exactly once and in order per sender, up to 256 KiB. For announcements (a ghost spawned, a ghost removed) |
| `SCO_NET_FROM_HOST` | Only the session host may send; joiners only receive |
| `SCO_NET_TO_HOST` | Joiners send to the host only (not relayed to the other joiners); the host never sends |

`max_len` is the largest message this side sends or accepts; a longer incoming one is dropped before it is reassembled. Give it the size your message really has.

Every player's copy of the plugin registers the same channels. A message on a channel no other player has registered goes nowhere (the session drops it and counts it); that is how two versions of a plugin, or a player without it, coexist.

## Sending and receiving

```c
static void OnPose(uint64_t from, const void* buf, uint32_t len, void* ctx) {
    if (len != sizeof(struct Pose)) return;            /* never trust the sender: check sizes */
    struct Pose p; memcpy(&p, buf, sizeof p);           /* ... move from's ghost ... */
}

net->register_channel(self, "myplugin.pose", 0, sizeof(struct Pose), OnPose, NULL);
...
if (net->is_active()) net->send_channel(self, "myplugin.pose", &pose, sizeof pose);
```

```cpp
sco::sdk::Net net;
if (net.Open(*this) == SCO_OK) {
    pose_ = net.Channel("myplugin.pose", 0, sizeof(Pose), [this](uint64_t from, std::span<const std::byte> b) {
        if (b.size() == sizeof(Pose)) MoveGhost(from, b);
    });
}
...
if (net.Active()) pose_.Send(pose);   // a trivially copyable struct
```

- **Threads.** Every table function may be called from any thread and never blocks on the network. **Callbacks run on the game thread**, from the host tick, one message per call, in arrival order; `buf` is valid for the call only. A callback may call anything in the table. A callback that faults disables only your plugin.
- **Results.** `send_channel` answers at once what it can know: `SCO_BAD_ARG` (not your channel, over `max_len`, the wrong direction), `SCO_NOT_FOUND` (you didn't register it), `SCO_UNAVAILABLE` (no session), `SCO_TOO_MANY` (over your quota). `SCO_OK` means queued; delivery is the channel's (reliable or not).
- **What you receive is untrusted.** Another player's game (or someone with the passphrase) wrote it. Check every size and field before using it; never send or accept pointers, code addresses or raw game objects. Send semantic fields (a class name, a session-scoped id, a zone name and a position) and build your own objects from them.
- **Plaintext.** Messages are authenticated (only players with the passphrase can send, and nothing can be altered on the way) but **not encrypted**. Never send secrets or personal data. Players who want confidentiality run the session over a VPN.

## Peers and events

`get_peers` lists the session's players, yourself first; `self_peer` is your own id (the host is 1, joiners 2, 3, ... in join order, never reused in a session). `entity_id` is the session-scoped id of the ghost the product spawned for that player, 0 when there is none. Names come from `get_peer_name` (UTF-8, cut to your buffer, never in the middle of a character); they are what each player chose to show.

Two events on the bus, on the game thread, each starting with its size:

| Event | Data |
|---|---|
| `net.state` | `sco_net_state_event { size, active, reason[128] }`: the session came up, or ended (the reason: "wrong passphrase", "the host timed out", ...) |
| `net.peer` | `sco_net_peer_event { size, what, peer }`: `SCO_NET_PEER_JOINED`, `SCO_NET_PEER_LEFT` (with the entity id it had, so you can remove its ghost) or `SCO_NET_PEER_ENTITY` (the product set its ghost) |

## Limits

| Limit | Value |
|---|---|
| Players in a session | 16 at most (the host picks; default 8) |
| Unreliable message | 1,200 bytes |
| Reliable message | 256 KiB |
| Channels per plugin | 32 |
| Send quota per plugin | 256 messages and 512 KiB per second (a bucket holding one second's worth; the product may change both). Over it: `SCO_TOO_MANY`, the message is dropped |
| Waiting to go out | 1 MiB per plugin (then `SCO_TOO_MANY`) |
| Waiting for your next tick | 1,024 messages or 4 MiB per plugin (more are dropped) |

When your plugin unloads or crashes, the host removes its channels and every message still queued for it or by it.

## Who can connect (the product's side)

For product authors (sc-offline); plugins never see this. `sco::net::Host(HostOptions)` binds UDP 64091 (by default) and accepts players who know the passphrase; `sco::net::Join(JoinOptions)` joins one by numeric IPv4 address; `sco::net::Leave(reason)` ends it. Nothing listens or connects until one of them is called.

**LAN only by default.** A session exchanges datagrams only with loopback (`127.0.0.0/8`), private (`10.0.0.0/8`, `172.16.0.0/12`, `192.168.0.0/16`) and link-local (`169.254.0.0/16`) IPv4 addresses (and `::1`, `fe80::/10`, `fc00::/7` for IPv6 once it is supported). Datagrams from anywhere else are dropped before they are parsed, nothing is sent there, and `Join` refuses such an address up front. A product may add ranges for the VPN the player chose through `Scope::allow`, a list of CIDRs:

| VPN | Range to allow |
|---|---|
| Tailscale | `100.64.0.0/10` (its CGNAT range) |
| ZeroTier | the network's managed range, when it isn't already private (ZeroTier's defaults are in `10.0.0.0/8` or `172.16.0.0/12`, which need nothing) |

Allow the narrowest range the VPN really uses: every address in it can reach the session's port.

`Scope::any` lifts the rule entirely. It exists as an explicit opt-in; sc-offline doesn't offer it by default. The firewall rule sc-offline adds should match (`remoteip=localsubnet`, plus the VPN range when the player picks one).

**Threads.** One network thread per session owns the socket and the session (handshake, keys, resends, timeouts) and never calls plugin code. Plugin calls copy their data and queue it for that thread. What it receives is queued for the game thread and delivered by `sco::net::Tick()` (the host kit calls it from `sco::app::Tick`). `Host`, `Join`, `Leave` and `Stop` are for the game thread or one product thread; `Leave` waits for the network thread to finish.

## Not in 1.0

The C# (`Sco.Sdk`) and Lua layers come in a follow-up (the Lua one gives a sandboxed plugin `is_active`, `peers`, `register` and `send` when its manifest `requires = sco.net`). IPv6 sockets, and a test with two `sco-host-sim` processes, follow too.
