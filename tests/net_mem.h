#pragma once
// An in-memory datagram network for sco::net tests: seeded loss, duplication and reordering (each
// copy gets its own random delay), a virtual clock the test advances, a tap that sees (and may
// change or drop) every datagram, and injection from any address. No threads, sockets or sleeps:
// a run is a pure function of its seed.
#include "sco/net/transport.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <random>
#include <vector>

namespace nettest {

using sco::net::Endpoint;

inline Endpoint MemEndpoint(uint16_t id) {
    Endpoint e;
    e.family = Endpoint::kMemory;
    e.port = id;
    return e;
}

class MemNet {
public:
    struct Datagram {
        uint64_t at = 0;      // deliverable from this virtual time
        uint64_t order = 0;   // ties keep send order
        Endpoint from, to;
        std::vector<uint8_t> bytes;
    };

    explicit MemNet(uint64_t seed) : rng_(seed) {}

    double   loss = 0.0;      // probability a send is dropped
    double   dup = 0.0;       // probability a send is delivered twice
    uint32_t minDelayMs = 1;
    uint32_t maxDelayMs = 1;  // > minDelayMs reorders
    uint64_t now = 0;
    // Sees every datagram as it is sent; false drops it. May change the bytes.
    std::function<bool(Datagram&)> tap;

    void Send(const Endpoint& from, const Endpoint& to, const uint8_t* data, size_t len) {
        Datagram d;
        d.from = from;
        d.to = to;
        d.bytes.assign(data, data + len);
        if (tap && !tap(d)) return;
        if (Chance(loss)) return;
        const int copies = Chance(dup) ? 2 : 1;
        for (int i = 0; i < copies; ++i) {
            Datagram c = d;
            c.at = now + Delay();
            c.order = order_++;
            q_.push_back(std::move(c));
        }
    }

    // Puts a datagram on the wire as if `from` sent it now (spoofing included); no tap, no loss.
    void Inject(const Endpoint& from, const Endpoint& to, std::vector<uint8_t> bytes) {
        Datagram d;
        d.from = from;
        d.to = to;
        d.bytes = std::move(bytes);
        d.at = now;
        d.order = order_++;
        q_.push_back(std::move(d));
    }

    bool Take(const Endpoint& to, Datagram* out) {
        size_t best = q_.size();
        for (size_t i = 0; i < q_.size(); ++i) {
            const Datagram& d = q_[i];
            if (!(d.to == to) || d.at > now) continue;
            if (best == q_.size() || d.at < q_[best].at || (d.at == q_[best].at && d.order < q_[best].order)) best = i;
        }
        if (best == q_.size()) return false;
        *out = std::move(q_[best]);
        q_.erase(q_.begin() + static_cast<std::ptrdiff_t>(best));
        return true;
    }

    size_t InFlight() const { return q_.size(); }

private:
    bool Chance(double p) { return p > 0.0 && std::uniform_real_distribution<double>(0.0, 1.0)(rng_) < p; }
    uint32_t Delay() {
        if (maxDelayMs <= minDelayMs) return minDelayMs;
        return std::uniform_int_distribution<uint32_t>(minDelayMs, maxDelayMs)(rng_);
    }

    std::mt19937_64 rng_;
    uint64_t order_ = 0;
    std::vector<Datagram> q_;
};

class MemTransport : public sco::net::Transport {
public:
    MemTransport(MemNet& net, uint16_t id) : net_(net), self_(MemEndpoint(id)) {}
    const Endpoint& Self() const { return self_; }

    bool Send(const Endpoint& to, const uint8_t* data, size_t len) override {
        net_.Send(self_, to, data, len);
        return true;
    }
    bool Receive(Endpoint* from, uint8_t* buf, size_t cap, size_t* len) override {
        MemNet::Datagram d;
        if (!net_.Take(self_, &d)) return false;
        *from = d.from;
        *len = d.bytes.size();
        const size_t n = d.bytes.size() < cap ? d.bytes.size() : cap;
        for (size_t i = 0; i < n; ++i) buf[i] = d.bytes[i];
        return true;
    }

private:
    MemNet&  net_;
    Endpoint self_;
};

}  // namespace nettest
