#pragma once
// sco::net::Core: one endpoint of a sco.net session (the host or a joiner), platform-free. It owns
// the handshake, the links to the other peers, the session channel table, reliable and unreliable
// delivery, keepalive and timeouts, all over an abstract datagram Transport and driven by
// Pump(nowMs) with the caller's clock (tests use a virtual one; nothing here sleeps or reads a
// clock). The wire is in sco/net/wire.h and docs/net-wire.md.
//
// Topology: a star. Joiners talk only to the host; the host delivers what it registered and relays
// the rest to the other joiners that registered the channel (TO_HOST channels are not relayed).
// Each link has its own key, derived from the session key and both handshake nonces.
//
// Threads: none. A Core is used from one thread at a time; plan PR 4b's network thread owns it and
// serializes send_channel calls from other threads into it. Callbacks run inside Pump, on the
// calling thread; they may call Send and RegisterChannel, and a Leave from inside one takes effect
// when Pump returns. Ids are opaque uint64_t; nothing hands out pointers to internal state.
#include "sco/net/transport.h"
#include "sco/net/wire.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sco::net {

using PeerId = uint64_t;
inline constexpr PeerId kHostPeer = 1;   // joiners get 2, 3, ... (never reused in a session)

enum class State : uint8_t { Idle, Hosting, Joining, Joined };

enum class SendResult : uint8_t {
    Ok,
    NoSession,        // not hosting or joined
    UnknownChannel,   // not registered here, or (joiner) not in the session table yet
    TooLarge,         // over the channel's max_len or the reliability's limit
    WrongDirection,   // a FROM_HOST channel sent by a joiner, or a TO_HOST channel by the host
    NotRemote,        // no other peer registered the channel: a no-op, counted as refused
    QueueFull,        // a reliable queue or the unreliable queue is full; dropped
};
const char* SendResultName(SendResult r);

// Why a join failed or a session ended (also as text in LastReason()).
enum class Refusal : uint8_t {
    None = 0, Version = SC_NET_REFUSE_VERSION, Passphrase = SC_NET_REFUSE_PASSPHRASE, Full = SC_NET_REFUSE_FULL,
    NotAdmitted = SC_NET_REFUSE_NOT_ADMITTED, BadHello = SC_NET_REFUSE_BAD_HELLO,
};

enum class PeerEvent : uint8_t { Joined, Left };

struct Message {
    std::string_view channel;   // the full name
    PeerId           from;      // the originating peer (the host relays joiners' messages)
    const uint8_t*   data;      // valid for the call only
    uint32_t         len;
};

struct PeerInfo {
    PeerId      id = 0;
    std::string name;
};

struct Callbacks {
    std::function<void(const Message&)> message;
    // Joined: text is the peer's name. Left: text is the reason.
    std::function<void(PeerEvent, PeerId, std::string_view text)> peer;
    // The session came up (active) or ended or failed (inactive, with the reason).
    std::function<void(bool active, std::string_view reason)> state;
    // Host only: the product's admission decision after the passphrase proof checked out. Unset
    // admits everyone (up to maxPeers).
    std::function<bool(std::string_view name, const Endpoint& from)> admit;
};

struct Options {
    std::string passphrase;            // never sent; stretched with PBKDF2
    std::string playerName;            // ValidName
    uint32_t maxPeers = 8;             // host: session size, self included; 2..kMaxPeers
    // Both sides must agree (it is a protocol constant); tests lower it to keep runs short.
    uint32_t pbkdf2Iters = kPbkdf2Iters;
    uint32_t retryMs = 500;            // joiner: HELLO / PROOF resend
    uint32_t joinTimeoutMs = 10000;    // joiner: give up; host: drop a half-open join
    uint32_t resendMs = 200;           // reliable: first retransmission timeout
    uint32_t maxResendMs = 2000;       // reliable: the doubling stops here
    uint32_t keepaliveMs = 1000;       // a PING after this long with nothing sent on a link
    uint32_t peerTimeoutMs = 30000;    // a link silent this long is dropped
    // Cryptographic randomness for nonces and the salt. Unset: std::random_device (rand_s on
    // MSVC, the OS's entropy source on libstdc++ / libc++). 4b may pass BCryptGenRandom.
    std::function<void(uint8_t*, size_t)> random;
};

// Counters for tests and logs. Every datagram dropped lands in exactly one of the drop counters.
struct Stats {
    uint64_t datagramsIn = 0, datagramsOut = 0;
    uint64_t malformed = 0;        // framing (Parse) or a body that doesn't decode
    uint64_t badVersion = 0;
    uint64_t badTag = 0;
    uint64_t replayed = 0;         // duplicate or too-old seq
    uint64_t spoofed = 0;          // no link at the source, or a sender id not its link's
    uint64_t unknownChannel = 0;   // an index not in the table, or not registered here
    uint64_t refused = 0;          // over max_len, wrong direction, or not wanted by any peer
    uint64_t handshakeDropped = 0; // handshake packets that matched nothing or failed a check
    uint64_t joinsRefused = 0;     // host: REFUSEs sent
    uint64_t retransmits = 0;
};

class Core {
public:
    explicit Core(Transport& transport, Callbacks callbacks = Callbacks());
    ~Core();
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    // Registers a channel on this endpoint. flags: kReliable / kFromHost / kToHost; maxLen 1..the
    // reliability's limit. false for a bad or reserved name ("sco.net"), a name already registered
    // here, bad flags or a bad maxLen. Works before and during a session (a joiner announces it to
    // the host; Send works once the host's table update arrives).
    bool RegisterChannel(std::string_view fqn, uint32_t flags, uint32_t maxLen);
    // Removes a local registration: nothing more is delivered on it here. The session table keeps
    // its index. false when it wasn't registered.
    bool UnregisterChannel(std::string_view fqn);

    // Starts a session with this endpoint as host: derives the session key (PBKDF2, once) from a
    // fresh salt. false (and nothing changes) unless Idle with valid options.
    bool Host(const Options& opts, uint64_t nowMs);
    // Starts joining the host at `host`; the outcome arrives through Callbacks::state from Pump.
    bool Join(const Options& opts, const Endpoint& host, uint64_t nowMs);
    // Ends the session (a BYE to each link) or abandons a join. No-op when Idle.
    void Leave(std::string_view reason);

    SendResult Send(std::string_view fqn, const void* data, uint32_t len);

    // Receives every waiting datagram, then runs timers: join retries, retransmissions,
    // acknowledgements, keepalive, timeouts.
    void Pump(uint64_t nowMs);

    State GetState() const;
    bool Active() const;           // Hosting, or Joined with the table received
    PeerId Self() const;           // 0 when not in a session
    std::vector<PeerInfo> Peers() const;   // self first, then by id; empty when not active
    bool PeerName(PeerId id, std::string* out) const;
    const Stats& GetStats() const;
    std::string LastReason() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace sco::net
