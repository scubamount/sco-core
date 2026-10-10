// sco::net wire framing, MAC and replay window (sco/net/wire.h). Every length read from a
// datagram is checked against the datagram before anything is read through it.
#include "sco/net/wire.h"
#include <cstring>

namespace sco::net {

namespace {

uint16_t Le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t Le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t Le64(const uint8_t* p) { return static_cast<uint64_t>(Le32(p)) | (static_cast<uint64_t>(Le32(p + 4)) << 32); }
void Put16(uint8_t* p, uint16_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }
void Put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
void Put64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

bool KnownKind(uint8_t k) { return k >= static_cast<uint8_t>(Kind::Hello) && k <= static_cast<uint8_t>(Kind::Bye); }

void FullTag(const MacKey& key, std::string_view fqn, const Header& h, const uint8_t* body, uint8_t out[32]) {
    uint8_t pre[2 + 2];
    pre[0] = h.version;
    pre[1] = static_cast<uint8_t>(h.kind);
    Put16(pre + 2, static_cast<uint16_t>(fqn.size()));
    uint8_t post[8 + 8 + 4];
    Put64(post, h.sender);
    Put64(post + 8, h.seq);
    Put32(post + 16, h.bodyLen);
    sco_hmac_sha256 c = key.keyed;
    sco_hmac_sha256_update(&c, pre, sizeof(pre));
    sco_hmac_sha256_update(&c, fqn.data(), fqn.size());
    sco_hmac_sha256_update(&c, post, sizeof(post));
    if (h.bodyLen) sco_hmac_sha256_update(&c, body, h.bodyLen);
    sco_hmac_sha256_final(&c, out);
}

bool IdChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

}  // namespace

bool KindHasTag(Kind k) { return static_cast<uint8_t>(k) >= static_cast<uint8_t>(Kind::Data); }

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
    if (!data || len < kHeaderBytes) return ParseError::Short;
    if (len > kMaxDatagram) return ParseError::Oversize;
    if (Le32(data) != kMagic) return ParseError::BadMagic;
    Header h;
    h.version = data[4];
    h.kind = static_cast<Kind>(data[5]);
    h.channel = Le16(data + 6);
    h.sender = Le64(data + 8);
    h.seq = Le64(data + 16);
    h.bodyLen = Le32(data + 24);
    out->h = h;
    out->body = nullptr;
    out->tag = nullptr;
    if (h.version != kProtocolVersion) return ParseError::BadVersion;
    if (!KnownKind(data[5])) return ParseError::BadKind;
    if (h.bodyLen > kMaxBody) return ParseError::Oversize;
    // 64-bit sums: bodyLen is at most kMaxBody here, but keep the arithmetic overflow-free anyway.
    const uint64_t need = uint64_t{ kHeaderBytes } + h.bodyLen + (KindHasTag(h.kind) ? kTagBytes : 0u);
    if (len < need) return ParseError::Truncated;
    if (len > need) return ParseError::Trailing;
    out->body = data + kHeaderBytes;
    if (KindHasTag(h.kind)) out->tag = data + kHeaderBytes + h.bodyLen;
    return ParseError::Ok;
}

void MacKey::Set(const uint8_t key[kKeyBytes]) { sco_hmac_sha256_init(&keyed, key, kKeyBytes); }

void ComputeTag(const MacKey& key, std::string_view fqn, const Header& h, const uint8_t* body, uint8_t tag[kTagBytes]) {
    uint8_t full[32];
    FullTag(key, fqn, h, body, full);
    std::memcpy(tag, full, kTagBytes);
    sco_wipe(full, sizeof(full));
}

bool VerifyTag(const MacKey& key, std::string_view fqn, const Packet& p) {
    if (!p.tag || fqn.size() > kMaxFqn) return false;
    uint8_t want[kTagBytes];
    ComputeTag(key, fqn, p.h, p.body, want);
    return sco_ct_equal(want, p.tag, kTagBytes) == 1;
}

size_t Encode(const Header& h, const uint8_t* body, uint32_t bodyLen, const MacKey* key, std::string_view fqn,
              uint8_t* out, size_t cap) {
    const bool tagged = KindHasTag(h.kind);
    if (tagged != (key != nullptr) || bodyLen > kMaxBody || fqn.size() > kMaxFqn) return 0;
    const size_t total = kHeaderBytes + size_t{ bodyLen } + (tagged ? kTagBytes : 0u);
    if (total > cap || total > kMaxDatagram) return 0;
    Header w = h;
    w.bodyLen = bodyLen;
    Put32(out, kMagic);
    out[4] = w.version;
    out[5] = static_cast<uint8_t>(w.kind);
    Put16(out + 6, w.channel);
    Put64(out + 8, w.sender);
    Put64(out + 16, w.seq);
    Put32(out + 24, w.bodyLen);
    if (bodyLen) std::memcpy(out + kHeaderBytes, body, bodyLen);
    if (tagged) ComputeTag(*key, fqn, w, out + kHeaderBytes, out + kHeaderBytes + bodyLen);
    return total;
}

ReplayWindow::Verdict ReplayWindow::Check(uint64_t seq) const {
    if (seq == 0) return Verdict::TooOld;
    if (seq > top_) return Verdict::New;
    const uint64_t d = top_ - seq;
    if (d >= kReplayWindow) return Verdict::TooOld;
    return (bits_[d / 64] >> (d % 64)) & 1u ? Verdict::Duplicate : Verdict::New;
}

void ReplayWindow::Accept(uint64_t seq) {
    if (seq == 0) return;
    if (seq > top_) {
        const uint64_t shift = seq - top_;
        if (shift >= kReplayWindow) {
            std::memset(bits_, 0, sizeof(bits_));
        } else {
            const uint32_t ws = static_cast<uint32_t>(shift / 64), bs = static_cast<uint32_t>(shift % 64);
            for (uint32_t i = kWords; i-- > 0;) {
                uint64_t v = 0;
                if (i >= ws) {
                    v = bits_[i - ws] << bs;
                    if (bs && i >= ws + 1) v |= bits_[i - ws - 1] >> (64 - bs);
                }
                bits_[i] = v;
            }
        }
        top_ = seq;
        bits_[0] |= 1u;
        return;
    }
    const uint64_t d = top_ - seq;
    if (d < kReplayWindow) bits_[d / 64] |= uint64_t{ 1 } << (d % 64);
}

uint8_t Reader::U8() {
    const uint8_t* b = Bytes(1);
    return b ? b[0] : 0;
}
uint16_t Reader::U16() {
    const uint8_t* b = Bytes(2);
    return b ? Le16(b) : 0;
}
uint32_t Reader::U32() {
    const uint8_t* b = Bytes(4);
    return b ? Le32(b) : 0;
}
uint64_t Reader::U64() {
    const uint8_t* b = Bytes(8);
    return b ? Le64(b) : 0;
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
    Put16(b, x);
    Bytes(b, 2);
}
void Writer::U32(uint32_t x) {
    uint8_t b[4];
    Put32(b, x);
    Bytes(b, 4);
}
void Writer::U64(uint64_t x) {
    uint8_t b[8];
    Put64(b, x);
    Bytes(b, 8);
}
void Writer::Bytes(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    v_.insert(v_.end(), b, b + n);
}

bool ValidFqn(std::string_view fqn) {
    if (fqn.size() < 3 || fqn.size() > kMaxFqn) return false;
    bool dot = false;
    char prev = '.';
    for (char c : fqn) {
        if (c == '.') {
            if (prev == '.') return false;
            dot = true;
        } else if (!IdChar(c)) {
            return false;
        }
        prev = c;
    }
    return dot && prev != '.';
}

bool ValidName(std::string_view name) {
    if (name.empty() || name.size() > kMaxName) return false;
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20u || u == 0x7Fu) return false;
    }
    return true;
}

}  // namespace sco::net
