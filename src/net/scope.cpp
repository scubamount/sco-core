// sco::net scope: the LAN rule and the allow-list (sco/net/scope.h).
#include "sco/net/scope.h"
#include <cstring>

namespace sco::net {

namespace {

bool ParseU32(std::string_view s, uint32_t max, uint32_t* out) {
    if (s.empty() || s.size() > 10) return false;
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<uint64_t>(c - '0');
        if (v > max) return false;
    }
    *out = static_cast<uint32_t>(v);
    return true;
}

bool ParseIpv4(std::string_view s, uint8_t out[4]) {
    for (int i = 0; i < 4; ++i) {
        const size_t dot = i < 3 ? s.find('.') : s.size();
        if (dot == std::string_view::npos) return false;
        const std::string_view part = s.substr(0, dot);
        if (part.size() > 1 && part[0] == '0') return false;   // no octal-looking parts
        uint32_t v = 0;
        if (!ParseU32(part, 255, &v)) return false;
        out[i] = static_cast<uint8_t>(v);
        s = i < 3 ? s.substr(dot + 1) : std::string_view();
    }
    return true;
}

int Hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Plain IPv6 text: hex groups with at most one "::"; no embedded IPv4, no zone.
bool ParseIpv6(std::string_view s, uint8_t out[16]) {
    uint16_t head[8] = {}, tail[8] = {};
    int nHead = 0, nTail = 0;
    bool gap = false;
    if (s.size() < 2 || s.size() > 39) return false;
    size_t i = 0;
    if (s.substr(0, 2) == "::") {
        gap = true;
        i = 2;
    }
    while (i < s.size()) {
        size_t j = i;
        uint32_t v = 0;
        while (j < s.size() && j - i < 4 && Hex(s[j]) >= 0) v = v * 16 + static_cast<uint32_t>(Hex(s[j++]));
        if (j == i) return false;
        if (nHead + nTail >= 8) return false;
        if (gap) tail[nTail++] = static_cast<uint16_t>(v);
        else head[nHead++] = static_cast<uint16_t>(v);
        if (j == s.size()) break;
        if (s[j] != ':') return false;
        if (j + 1 < s.size() && s[j + 1] == ':') {
            if (gap) return false;
            gap = true;
            i = j + 2;
        } else {
            i = j + 1;
            if (i == s.size()) return false;   // a trailing single ':'
        }
    }
    if (gap ? nHead + nTail > 7 : nHead + nTail != 8) return false;
    uint16_t g[8] = {};
    for (int k = 0; k < nHead; ++k) g[k] = head[k];
    for (int k = 0; k < nTail; ++k) g[8 - nTail + k] = tail[k];
    for (int k = 0; k < 8; ++k) {
        out[2 * k] = static_cast<uint8_t>(g[k] >> 8);
        out[2 * k + 1] = static_cast<uint8_t>(g[k]);
    }
    return true;
}

bool Match(const uint8_t* addr, const uint8_t* net, unsigned bits) {
    const unsigned whole = bits / 8, rest = bits % 8;
    if (std::memcmp(addr, net, whole) != 0) return false;
    if (!rest) return true;
    const uint8_t mask = static_cast<uint8_t>(0xFF << (8 - rest));
    return (addr[whole] & mask) == (net[whole] & mask);
}

bool In4(const Endpoint& e, uint8_t a, uint8_t b, unsigned bits) {
    const uint8_t net[4] = { a, b, 0, 0 };
    return Match(e.addr, net, bits);
}

}  // namespace

bool ParseCidr(std::string_view text, Cidr* out) {
    if (!out) return false;
    Cidr c;
    const size_t slash = text.find('/');
    const std::string_view host = text.substr(0, slash);
    if (ParseIpv4(host, c.addr)) {
        c.family = Endpoint::kIpv4;
    } else if (ParseIpv6(host, c.addr)) {
        c.family = Endpoint::kIpv6;
    } else {
        return false;
    }
    const uint32_t maxBits = c.family == Endpoint::kIpv4 ? 32u : 128u;
    uint32_t bits = maxBits;
    if (slash != std::string_view::npos && !ParseU32(text.substr(slash + 1), maxBits, &bits)) return false;
    c.bits = static_cast<uint8_t>(bits);
    // Bits past the prefix must be zero: "10.1.2.3/8" is a typo for something, so refuse it.
    for (uint32_t b = bits; b < maxBits; ++b)
        if (c.addr[b / 8] & (0x80 >> (b % 8))) return false;
    *out = c;
    return true;
}

bool ParseAddress(std::string_view text, uint16_t port, Endpoint* out) {
    if (!out) return false;
    Endpoint e;
    if (!ParseIpv4(text, e.addr)) return false;
    e.family = Endpoint::kIpv4;
    e.port = port;
    *out = e;
    return true;
}

bool IsLan(const Endpoint& e) {
    if (e.family == Endpoint::kIpv4) {
        return In4(e, 127, 0, 8) || In4(e, 10, 0, 8) || In4(e, 172, 16, 12) || In4(e, 192, 168, 16) ||
               In4(e, 169, 254, 16);
    }
    if (e.family == Endpoint::kIpv6) {
        static const uint8_t kLoop[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
        static const uint8_t kLink[16] = { 0xFE, 0x80 };
        static const uint8_t kUla[16] = { 0xFC };
        return Match(e.addr, kLoop, 128) || Match(e.addr, kLink, 10) || Match(e.addr, kUla, 7);
    }
    return false;
}

bool InCidr(const Cidr& c, const Endpoint& e) {
    return c.family != Endpoint::kNone && c.family == e.family && Match(e.addr, c.addr, c.bits);
}

bool InScope(const Scope& s, const Endpoint& e) {
    if (e.family == Endpoint::kMemory) return true;
    if (e.family != Endpoint::kIpv4 && e.family != Endpoint::kIpv6) return false;
    if (s.any || IsLan(e)) return true;
    for (const Cidr& c : s.allow)
        if (InCidr(c, e)) return true;
    return false;
}

bool ScopedTransport::Send(const Endpoint& to, const uint8_t* data, size_t len) {
    if (!InScope(scope_, to)) {
        ++refused_;
        return false;
    }
    return inner_.Send(to, data, len);
}

bool ScopedTransport::Receive(Endpoint* from, uint8_t* buf, size_t cap, size_t* len) {
    for (;;) {
        if (!inner_.Receive(from, buf, cap, len)) return false;
        if (InScope(scope_, *from)) return true;
        ++dropped_;
    }
}

}  // namespace sco::net
