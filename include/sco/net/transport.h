#pragma once
// sco::net transport: the abstract datagram pipe the session (sco/net/core.h) runs on. Platform-
// free: the in-memory transport of the tests (tests/net_mem.h) and the UDP sockets of plan PR 4b
// (src/net/socket_win.cpp / socket_posix.cpp) implement it. Datagram semantics, as UDP: a send may
// be lost, duplicated or reordered, and nothing is ever split or merged.
#include <cstddef>
#include <cstdint>

namespace sco::net {

// Where a datagram came from or goes to. Compared byte for byte; the session never interprets it
// (the 4b socket layer fills it from a sockaddr and applies the LAN-only bind scope).
struct Endpoint {
    static constexpr uint8_t kNone = 0, kIpv4 = 4, kIpv6 = 6, kMemory = 0xFF;
    uint8_t  family = kNone;
    uint8_t  reserved = 0;
    uint16_t port = 0;
    uint8_t  addr[16] = {};   // IPv4 in the first 4 bytes
    bool operator==(const Endpoint&) const = default;
};

class Transport {
public:
    virtual ~Transport() = default;
    // Queues one datagram to `to`. false when it couldn't be queued (the session treats that as a
    // loss; reliable data is resent). Never blocks.
    virtual bool Send(const Endpoint& to, const uint8_t* data, size_t len) = 0;
    // Takes the next received datagram: copies up to cap bytes to buf, sets *len to the datagram's
    // full size (more than cap means it was cut, and the session drops it) and *from to its
    // source. false when nothing is waiting. Never blocks.
    virtual bool Receive(Endpoint* from, uint8_t* buf, size_t cap, size_t* len) = 0;
};

}  // namespace sco::net
