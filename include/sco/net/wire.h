#pragma once
// sco::net wire: the packet framing of sco.net as built (docs/net-wire.md), its hostile-input
// parser, the per-packet MAC and the replay window. Platform-free and internal to sco-core: the
// session (sco/net/core.h) and the 4b socket layer use it; plugins never see it.
//
// The wire itself (layout, MAC input order, replay rules, the constants) is defined once, in the
// MIT header include/sc_net.h; everything here is a C++ view of it. docs/net-wire.md describes it.
#include "sc_net.h"
#include "sco/net/sha2.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sco::net {

inline constexpr uint32_t kMagic           = SC_NET_MAGIC;
inline constexpr uint8_t  kProtocolVersion = SC_NET_PROTOCOL_VERSION;
// PBKDF2-HMAC-SHA256 iterations for the session key (SC_NET_PBKDF2_ITERS). Raising it is a protocol
// change: bump the protocol version with it, since both sides must use the same count.
inline constexpr uint32_t kPbkdf2Iters     = SC_NET_PBKDF2_ITERS;

inline constexpr uint32_t kMaxPeers        = SC_NET_MAX_PEERS;
inline constexpr uint32_t kMaxUnreliable   = SC_NET_MAX_UNREL;
inline constexpr uint32_t kMaxReliable     = SC_NET_MAX_RELIABLE;
inline constexpr uint32_t kMaxDatagram     = SC_NET_MAX_DATAGRAM;
inline constexpr uint32_t kHeaderBytes     = SC_NET_HEADER_BYTES;
inline constexpr uint32_t kTagBytes        = SC_NET_TAG_BYTES;
inline constexpr uint32_t kMaxBody         = SC_NET_MAX_BODY;
inline constexpr uint32_t kMaxFqn          = SC_NET_MAX_FQN;
inline constexpr uint32_t kMaxName         = SC_NET_MAX_NAME;
inline constexpr uint32_t kMaxChannels     = SC_NET_MAX_CHANNELS;
inline constexpr uint32_t kReplayWindow    = SC_NET_REPLAY_WINDOW;
inline constexpr uint32_t kKeyBytes        = SC_NET_KEY_BYTES;
inline constexpr uint32_t kSaltBytes       = SC_NET_SALT_BYTES;
inline constexpr uint32_t kNonceBytes      = SC_NET_NONCE_BYTES;
inline constexpr const char* kControlFqn   = SC_NET_CONTROL_FQN;   // channel index 0

// Channel flags (SCO_NET_RELIABLE / FROM_HOST / TO_HOST in the design).
inline constexpr uint32_t kReliable = SC_NET_RELIABLE;
inline constexpr uint32_t kFromHost = SC_NET_FROM_HOST;
inline constexpr uint32_t kToHost   = SC_NET_TO_HOST;
inline constexpr uint32_t kChannelFlags = kReliable | kFromHost | kToHost;

enum class Kind : uint8_t {
    Hello = SC_NET_HELLO, Challenge = SC_NET_CHALLENGE, Proof = SC_NET_PROOF, Welcome = SC_NET_WELCOME,
    Refuse = SC_NET_REFUSE,                                              // handshake: no tag
    Data = SC_NET_DATA, Ack = SC_NET_ACK, Ping = SC_NET_PING, Bye = SC_NET_BYE,   // session: tagged
};
// True for the kinds that carry a tag (and need an established link).
bool KindHasTag(Kind k);

struct Header {
    uint8_t  version = kProtocolVersion;
    Kind     kind = Kind::Data;
    uint16_t channel = 0;
    uint64_t sender = 0;
    uint64_t seq = 0;
    uint32_t bodyLen = 0;
};

enum class ParseError : uint8_t {
    Ok,
    Short,        // under kHeaderBytes
    Oversize,     // over kMaxDatagram, or body_len over kMaxBody
    BadMagic,
    BadVersion,   // header still filled in (a HELLO from another version gets a REFUSE)
    BadKind,
    Truncated,    // shorter than header + body_len (+ tag)
    Trailing,     // longer than that
};
const char* ParseErrorName(ParseError e);

// A parsed datagram: views into the caller's buffer, valid while it is.
struct Packet {
    Header         h;
    const uint8_t* body = nullptr;
    const uint8_t* tag = nullptr;   // kTagBytes, or nullptr for handshake kinds
};

// Checks the framing of len bytes at data; reads nothing outside them whatever the bytes say.
// Fills out->h before returning BadVersion; out is otherwise valid only for Ok.
ParseError Parse(const uint8_t* data, size_t len, Packet* out);

// The keyed MAC of one link (the HMAC's inner and outer states, computed once).
struct MacKey {
    sco_hmac_sha256 keyed{};
    void Set(const uint8_t key[kKeyBytes]);
};

// Writes the tag for h/body under key, fqn being the full name of h.channel.
void ComputeTag(const MacKey& key, std::string_view fqn, const Header& h, const uint8_t* body,
                uint8_t tag[kTagBytes]);
// Constant-time check of p.tag; false for a packet with no tag.
bool VerifyTag(const MacKey& key, std::string_view fqn, const Packet& p);

// Frames h and body into out (body_len taken from bodyLen) and, when key is given, appends the tag.
// Returns the datagram length, or 0 when it would pass kMaxDatagram / cap or the kind and key
// disagree (a tagged kind needs a key; a handshake kind takes none).
size_t Encode(const Header& h, const uint8_t* body, uint32_t bodyLen, const MacKey* key,
              std::string_view fqn, uint8_t* out, size_t cap);

// Sliding replay window over one sender's seq: 0 is never valid; anything at or below
// highest - kReplayWindow is too old; inside the window each seq is accepted once.
class ReplayWindow {
public:
    enum class Verdict : uint8_t { New, Duplicate, TooOld };
    Verdict Check(uint64_t seq) const;
    // Records seq; call only after Check said New and the packet authenticated.
    void Accept(uint64_t seq);
    uint64_t Highest() const { return w_.top; }

private:
    sc_net_replay w_{};
};

// Bounds-checked little-endian reader. Any read past the end sets !ok() and returns zeros/null;
// ok() stays false from then on.
class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    uint8_t  U8();
    uint16_t U16();
    uint32_t U32();
    uint64_t U64();
    const uint8_t* Bytes(size_t len);   // nullptr when short
    bool Str(size_t len, std::string* out);
    size_t Left() const { return ok_ ? n_ - off_ : 0; }
    bool ok() const { return ok_; }

private:
    const uint8_t* p_;
    size_t n_;
    size_t off_ = 0;
    bool ok_ = true;
};

// Little-endian writer into a growing vector.
class Writer {
public:
    explicit Writer(std::vector<uint8_t>& v) : v_(v) {}
    void U8(uint8_t x) { v_.push_back(x); }
    void U16(uint16_t x);
    void U32(uint32_t x);
    void U64(uint64_t x);
    void Bytes(const void* p, size_t n);

private:
    std::vector<uint8_t>& v_;
};

// A channel name: 3..kMaxFqn bytes of [A-Za-z0-9_-] in two or more dot-separated parts
// ("<plugin id>.<channel>"). "sco.net" itself is valid here; the session reserves it.
bool ValidFqn(std::string_view fqn);
// A player name: 1..kMaxName bytes, none of them a control character (UTF-8 passes through).
bool ValidName(std::string_view name);

}  // namespace sco::net
