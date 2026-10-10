#pragma once
// sco::net service and session control: the host side of "sco.net" (include/sco_net.h). C++ and
// internal: the host kit starts the service; the product (sc-offline's launcher and menu, through
// its multiplayer built-in) opens and closes sessions. Plugins only see the C table, which has no
// session control: a plugin can never make a player's PC listen or connect.
//
//   sco::net::Start();                       // publishes sco.net 1.0 and the capability "sco.net"
//   ... plugins query_service("sco.net", 0x00010000, ...) and register channels ...
//   sco::net::HostOptions h;                 // only when the player asks for it:
//   h.passphrase = "..."; h.playerName = "Pilot";
//   sco::net::Host(h);                       // binds UDP 64091, accepts LAN peers that know it
//   sco::net::Tick();                        // every host tick, game thread: delivers messages
//   sco::net::Leave("player left");
//   sco::net::Stop();                        // after every plugin unloaded
//
// Nothing opens a socket until Host or Join; with no session the service is inert.
//
// Threading rule:
//   - One network thread per session owns the sco::net::Core and its socket. It is started by
//     Host / Join and ends with the session. It never calls plugin code.
//   - Table calls (any thread) and Leave / SetPeerEntity copy what they need and queue it for
//     the network thread; they never block on the network. Results a call can know up front
//     (ownership, sizes, no session, quotas) are answered at once; a message refused later by
//     the session (no other peer has the channel) is dropped and counted.
//   - Everything the network thread receives (messages, peer and session events) is queued for
//     the game thread and delivered by Tick: message callbacks as guarded callouts of the plugin
//     that registered the channel (a fault disables only that plugin), "net.state" and
//     "net.peer" through the event bus. Nothing reaches a plugin between ticks.
//   - Host, Join, Leave and Stop are serialized among themselves; call them from the game thread
//     (or one product thread). Leave and Stop wait for the network thread to finish.
//
// Quotas (ServiceOptions): each plugin may send quotaMsgs messages and quotaBytes bytes per
// second (token buckets holding one second's worth; defaults SCO_NET_QUOTA_MSGS = 256 and
// SCO_NET_QUOTA_BYTES = 512 KiB), with at most kMaxQueuedBytesPerPlugin waiting for the network
// thread. Inbound, at most kMaxInboxPerPlugin messages (kMaxInboxBytesPerPlugin bytes) wait for a
// plugin's next tick; more are dropped and counted.
//
// The LAN rule: sco/net/scope.h. Reference: docs/net.md.
#include "sco_net.h"
#include "sco/net/core.h"
#include "sco/net/scope.h"
#include "sco/runtime.h"
#include <cstdint>
#include <functional>
#include <string>

namespace sco::net {

inline constexpr uint16_t kDefaultPort = 64091;
inline constexpr size_t   kMaxQueuedBytesPerPlugin = 1u << 20;   // waiting for the network thread
inline constexpr size_t   kMaxInboxPerPlugin = 1024;              // messages waiting for Tick
inline constexpr size_t   kMaxInboxBytesPerPlugin = 4u << 20;

struct ServiceOptions {
    uint32_t quotaMsgs = SCO_NET_QUOTA_MSGS;     // per plugin, per second; 0 is BadArg
    uint32_t quotaBytes = SCO_NET_QUOTA_BYTES;   // per plugin, per second; at least SCO_NET_MAX_RELIABLE
    // The network thread runs a session step at least this often (and at once when a call queues
    // work). Products leave it alone.
    uint32_t pollMs = 5;
    // false: no network thread; the caller runs each step with PumpNetwork(). For deterministic
    // tests over an in-memory transport. Products leave it true.
    bool thread = true;
    // Milliseconds, for the session's timers and the quotas. Unset: a steady clock. Tests pass a
    // virtual one.
    std::function<uint64_t()> clock;
    // PBKDF2 iterations; both peers must agree (a protocol constant). Tests lower it.
    uint32_t pbkdf2Iters = kPbkdf2Iters;
};

// Publishes "sco.net" 1.0 under the host's id, sets the capability "sco.net" and installs the
// release hook. No socket, no thread. BadArg: already started, bad options, or the name taken.
// TooMany: no release hook slot or out of memory. Nothing changes unless Ok.
Result Start(const ServiceOptions& opts = ServiceOptions());

// Leaves any session (waiting for the network thread), withdraws "sco.net", sets the capability
// not ready and drops every channel and queued message; later table calls answer SCO_UNAVAILABLE.
// No-op unless started. Call after every plugin has unloaded.
void Stop();

bool Started();

// Game thread, not inside a callout (the host kit calls it from Tick): delivers everything the
// network thread queued, in order. No-op elsewhere.
void Tick();

// Any thread: true as soon as something is queued for Tick, false after timeoutMs with nothing.
// For tools and tests that wait for an event with a bound (never a fixed sleep).
bool WaitForDelivery(uint32_t timeoutMs);

// ServiceOptions::thread false only: runs one network step now, on the caller (queued calls,
// then receive and timers). No-op otherwise or with no session.
void PumpNetwork();

// ---- session control (the product's) ----------------------------------------------------------

struct HostOptions {
    uint16_t    port = kDefaultPort;   // 0: the OS picks one (BoundPort tells)
    bool        loopbackOnly = false;  // bind 127.0.0.1 only (local testing)
    std::string passphrase;            // never sent or logged; stretched with PBKDF2
    std::string playerName;            // shown to the other players; 1-64 bytes, no control chars
    uint32_t    maxPeers = 8;          // the session's size, self included: 2..SCO_NET_MAX_PEERS
    Scope       scope;                 // the LAN rule plus the product's allow-list
    // Tests: run on this transport (not owned; must outlive the session) instead of a socket.
    Transport*  transport = nullptr;
    // Host only: the product's admission decision after the passphrase checked out. Runs on the
    // network thread; must not block. Unset admits everyone up to maxPeers.
    std::function<bool(std::string_view name, const Endpoint& from)> admit;
};

struct JoinOptions {
    std::string address;               // numeric IPv4 ("192.168.1.20"); must be in scope
    uint16_t    port = kDefaultPort;   // the host's port
    bool        loopbackOnly = false;  // bind this side on 127.0.0.1 only
    std::string passphrase;
    std::string playerName;
    Scope       scope;
    Transport*  transport = nullptr;   // tests; then endpoint is the host and address is unused
    Endpoint    endpoint;
};

// Starts hosting: binds the port and starts the network thread, which derives the session key
// and comes up ("net.state" active). Unavailable: the service isn't started. BadArg: bad options
// or a session already running (Leave first). Failed: the port couldn't be bound (logged).
Result Host(const HostOptions& opts);
// Starts joining the host at address:port; the outcome arrives as "net.state" (active, or
// inactive with the reason: wrong passphrase, full, refused, no answer). Unavailable, BadArg (as
// Host; also an address outside the scope) or Failed (no socket).
Result Join(const JoinOptions& opts);
// Ends the session (each peer gets a BYE) and waits for the network thread. "net.state" inactive
// follows at the next Tick. No-op with no session.
void Leave(const char* reason);

State GetState();          // Idle, Hosting, Joining, Joined (as the network thread last saw it)
std::string LastReason();  // why the last session ended or failed
uint16_t BoundPort();      // the session's UDP port, 0 with no socket

// The ghost entity the product spawned for a peer (session-scoped id; 0 clears it). It shows in
// get_peers and as a "net.peer" ENTITY event. NotFound: no such peer in the active session.
Result SetPeerEntity(PeerId peer, uint64_t entityId);

struct ServiceStats {
    Stats    core;                 // the session's counters (the last session's once it ended)
    uint64_t outOfScope = 0;       // datagrams dropped by the LAN rule
    uint64_t quotaRefused = 0;     // send_channel answered SCO_TOO_MANY
    uint64_t sendDropped = 0;      // queued sends the session refused (no peer has the channel...)
    uint64_t inboxDropped = 0;     // messages dropped: no such channel here, or a plugin's inbox full
};
ServiceStats GetServiceStats();

// The service table (what query_service hands out). Valid for the life of the process.
const sco_net_v1* Table();

}  // namespace sco::net
