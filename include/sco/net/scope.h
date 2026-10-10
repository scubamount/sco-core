#pragma once
// sco::net scope: which addresses a session may talk to. Platform-free.
//
// The rule (docs/net.md § Who can connect): by default a session exchanges datagrams only with
// LAN addresses:
//   IPv4  127.0.0.0/8 (loopback), 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16 (RFC 1918),
//         169.254.0.0/16 (link-local)
//   IPv6  ::1, fe80::/10 (link-local), fc00::/7 (unique local)
// A product may add extra ranges with an allow-list of CIDRs, for the VPN a player chose:
// Tailscale's 100.64.0.0/10 (CGNAT), or a ZeroTier network's managed range when it is outside
// the private blocks above. Scope::any lifts the rule entirely; it is an explicit opt-in that
// sc-offline does not offer by default. Anything else is dropped before it is parsed (receive)
// and never sent to (send, and Join refuses the address up front).
// In-memory endpoints (Endpoint::kMemory, the tests' network) are always in scope.
#include "sco/net/transport.h"
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace sco::net {

struct Cidr {
    uint8_t family = Endpoint::kNone;   // kIpv4 or kIpv6
    uint8_t bits = 0;                   // prefix length: 0..32 or 0..128
    uint8_t addr[16] = {};              // network order; bits past the prefix are zero
};

struct Scope {
    bool any = false;          // opt-in: every address (the LAN rule off)
    std::vector<Cidr> allow;   // extra ranges beside the LAN ones
};

// "100.64.0.0/10", "fd00::/8"; an address without "/n" is a single host. false for anything
// else (bad syntax, a prefix too long, bits set past the prefix).
bool ParseCidr(std::string_view text, Cidr* out);

// A numeric IPv4 address ("192.168.1.20") with port into an Endpoint. false for anything else
// (no name resolution: a session is found by its address).
bool ParseAddress(std::string_view text, uint16_t port, Endpoint* out);

bool IsLan(const Endpoint& e);
bool InCidr(const Cidr& c, const Endpoint& e);
bool InScope(const Scope& s, const Endpoint& e);

// A Transport that keeps a session inside a Scope: Receive drops (and counts) datagrams from
// outside it, Send refuses destinations outside it. Used by the sco.net service around every
// transport it runs a session on.
class ScopedTransport final : public Transport {
public:
    ScopedTransport(Transport& inner, Scope scope) : inner_(inner), scope_(std::move(scope)) {}
    bool Send(const Endpoint& to, const uint8_t* data, size_t len) override;
    bool Receive(Endpoint* from, uint8_t* buf, size_t cap, size_t* len) override;
    uint64_t Dropped() const { return dropped_; }   // datagrams received from outside the scope
    uint64_t Refused() const { return refused_; }   // sends to outside the scope

private:
    Transport& inner_;
    Scope      scope_;
    uint64_t   dropped_ = 0;
    uint64_t   refused_ = 0;
};

}  // namespace sco::net
