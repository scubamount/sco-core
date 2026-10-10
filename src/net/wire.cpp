// sco::net wire framing, MAC and replay window (sco/net/wire.h). Every length read from a
// datagram is checked against the datagram before anything is read through it.
#include "sco/net/wire.h"
#include <cstring>

namespace sco::net {

namespace {

// sc_net.h's HMAC hook over a precomputed keyed state (MacKey).
void Hmac2(void*, const void* key, const uint8_t* a, size_t aLen, const uint8_t* b, size_t bLen, uint8_t out[32]) {
    sco_hmac_sha256 c = *static_cast<const sco_hmac_sha256*>(key);
    sco_hmac_sha256_update(&c, a, aLen);
    if (bLen) sco_hmac_sha256_update(&c, b, bLen);
    sco_hmac_sha256_final(&c, out);
}

sc_net_header ToC(const Header& h) {
    sc_net_header c{};
    c.magic = SC_NET_MAGIC;
    c.version = h.version;
    c.kind = static_cast<uint8_t>(h.kind);
    c.channel = h.channel;
    c.sender = h.sender;
    c.seq = h.seq;
    c.body_len = h.bodyLen;
    return c;
}

}  // namespace

bool KindHasTag(Kind k) { return sc_net_kind_tagged(static_cast<uint8_t>(k)) != 0; }

const char* ParseErrorName(ParseError e) {
    switch (e) {
    case ParseError::Ok: return "ok";
    case ParseError::Short: return "short";
    case ParseError::Oversize: return "oversize";
    case ParseError::BadMagic: return "bad magic";
    case ParseError::BadVersion: return "bad version";
    case ParseError::BadKind: return "bad kind";
    case ParseError::Truncated: return "truncated";
    case ParseError::Trailing: return "trailing bytes";
    }
    return "?";
}

ParseError Parse(const uint8_t* data, size_t len, Packet* out) {
    sc_net_header c{};
    const uint8_t* body = nullptr;
    const uint8_t* tag = nullptr;
    const int e = sc_net_parse(data, len, &c, &body, &tag);
    out->body = body;
    out->tag = tag;
    if (e == SC_NET_OK || e == SC_NET_E_BAD_VERSION) {
        out->h.version = c.version;
        out->h.kind = static_cast<Kind>(c.kind);
        out->h.channel = c.channel;
        out->h.sender = c.sender;
        out->h.seq = c.seq;
        out->h.bodyLen = c.body_len;
    }
    switch (e) {
    case SC_NET_OK: return ParseError::Ok;
    case SC_NET_E_SHORT: return ParseError::Short;
    case SC_NET_E_OVERSIZE: return ParseError::Oversize;
    case SC_NET_E_BAD_MAGIC: return ParseError::BadMagic;
    case SC_NET_E_BAD_VERSION: return ParseError::BadVersion;
    case SC_NET_E_BAD_KIND: return ParseError::BadKind;
    case SC_NET_E_TRUNCATED: return ParseError::Truncated;
    default: return ParseError::Trailing;
    }
}

void MacKey::Set(const uint8_t key[kKeyBytes]) { sco_hmac_sha256_init(&keyed, key, kKeyBytes); }

void ComputeTag(const MacKey& key, std::string_view fqn, const Header& h, const uint8_t* body, uint8_t tag[kTagBytes]) {
    const sc_net_header c = ToC(h);
    if (!sc_net_tag(Hmac2, nullptr, &key.keyed, &c, fqn.data(), fqn.size(), body, tag)) sco_wipe(tag, kTagBytes);
}

bool VerifyTag(const MacKey& key, std::string_view fqn, const Packet& p) {
    if (!p.tag || fqn.size() > kMaxFqn) return false;
    uint8_t want[kTagBytes];
    ComputeTag(key, fqn, p.h, p.body, want);
    return sc_net_tag_equal(want, p.tag) == 1;
}

size_t Encode(const Header& h, const uint8_t* body, uint32_t bodyLen, const MacKey* key, std::string_view fqn,
              uint8_t* out, size_t cap) {
    const bool tagged = KindHasTag(h.kind);
    if (tagged != (key != nullptr) || bodyLen > kMaxBody || fqn.size() > kMaxFqn) return 0;
    const size_t total = kHeaderBytes + size_t{ bodyLen } + (tagged ? kTagBytes : 0u);
    if (total > cap || total > kMaxDatagram) return 0;
    Header w = h;
    w.bodyLen = bodyLen;
    const sc_net_header c = ToC(w);
    sc_net_write_header(&c, out);
    if (bodyLen) std::memcpy(out + kHeaderBytes, body, bodyLen);
    if (tagged) ComputeTag(*key, fqn, w, out + kHeaderBytes, out + kHeaderBytes + bodyLen);
    return total;
}

ReplayWindow::Verdict ReplayWindow::Check(uint64_t seq) const {
    switch (sc_net_replay_check(&w_, seq)) {
    case SC_NET_REPLAY_NEW: return Verdict::New;
    case SC_NET_REPLAY_DUPLICATE: return Verdict::Duplicate;
    default: return Verdict::TooOld;
    }
}

void ReplayWindow::Accept(uint64_t seq) { sc_net_replay_accept(&w_, seq); }

uint8_t Reader::U8() {
    const uint8_t* b = Bytes(1);
    return b ? b[0] : 0;
}
uint16_t Reader::U16() {
    const uint8_t* b = Bytes(2);
    return b ? sc_net_get16(b) : 0;
}
uint32_t Reader::U32() {
    const uint8_t* b = Bytes(4);
    return b ? sc_net_get32(b) : 0;
}
uint64_t Reader::U64() {
    const uint8_t* b = Bytes(8);
    return b ? sc_net_get64(b) : 0;
}
const uint8_t* Reader::Bytes(size_t len) {
    if (!ok_ || len > n_ - off_) {
        ok_ = false;
        return nullptr;
    }
    const uint8_t* b = p_ + off_;
    off_ += len;
    return b;
}
bool Reader::Str(size_t len, std::string* out) {
    const uint8_t* b = Bytes(len);
    if (!b) return false;
    out->assign(reinterpret_cast<const char*>(b), len);
    return true;
}

void Writer::U16(uint16_t x) {
    uint8_t b[2];
    sc_net_put16(b, x);
    Bytes(b, 2);
}
void Writer::U32(uint32_t x) {
    uint8_t b[4];
    sc_net_put32(b, x);
    Bytes(b, 4);
}
void Writer::U64(uint64_t x) {
    uint8_t b[8];
    sc_net_put64(b, x);
    Bytes(b, 8);
}
void Writer::Bytes(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    v_.insert(v_.end(), b, b + n);
}

bool ValidFqn(std::string_view fqn) { return sc_net_valid_fqn(fqn.data(), fqn.size()) != 0; }

bool ValidName(std::string_view name) { return sc_net_valid_name(name.data(), name.size()) != 0; }

}  // namespace sco::net
