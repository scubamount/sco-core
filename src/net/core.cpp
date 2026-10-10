// sco::net::Core (sco/net/core.h): the session over a Transport. The wire is docs/net-wire.md.
//
// Handshake (design section 3.5), joiner J and host H:
//   J -> H  HELLO      client nonce cn, player name, the channel names that fit
//   H -> J  CHALLENGE  cn, host nonce hn, the session salt (picked once by the host per session)
//   J -> H  PROOF      cn, hn, HMAC(K, "sco.net proof" || cn || hn)
//                      K = PBKDF2-HMAC-SHA256(passphrase, salt, kPbkdf2Iters, 32 bytes)
//   (H checks the proof in constant time, then asks the product: Callbacks::admit)
//   H -> J  WELCOME    cn, peer id, HMAC(K, "sco.net welcome" || cn || hn || peer id)
//   H -> J  SYNC       on the reliable control channel: the channel table and the peers
// A wrong passphrase fails the PROOF check: H answers REFUSE (code Passphrase) and forgets the
// join. J checks WELCOME's MAC, so a host without the passphrase can't admit anyone either. Each
// link then uses its own key L = HMAC(K, "sco.net link" || cn || hn): fresh per join, so a packet
// from an earlier join (same peer, same seq) never verifies again, and one joiner can't forge
// packets on another's link.
#include "sco/net/core.h"
#include "sco/net/reliable.h"
#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <random>
#include <set>

namespace sco::net {

namespace {

constexpr uint32_t kControlMaxLen = SC_NET_CONTROL_MAX;
constexpr uint32_t kMaxPending = 32;        // half-open joins the host remembers
constexpr uint32_t kHelloMinBody = SC_NET_HELLO_MIN_BODY;   // so a CHALLENGE is never bigger
constexpr uint32_t kMaxPumpDatagrams = 4096;
constexpr uint32_t kMaxUnrelQueue = 256;    // per link
constexpr uint32_t kMaxReason = 128;
constexpr uint32_t kAckEntryBytes = SC_NET_ACK_ENTRY_BYTES;

// DATA body: u8 flags, u8 0, u16 0, u64 origin; reliable adds u64 unit seq, u32 msg_len, u32 offset.
constexpr uint8_t kDataReliable = SC_NET_DATA_F_RELIABLE;
constexpr uint8_t kDataToHost = SC_NET_DATA_F_TO_HOST;
static_assert(SC_NET_DATA_HEAD_UNREL == 12 && SC_NET_DATA_HEAD_REL == 28, "DATA body heads");
static_assert(SC_NET_CHALLENGE_BODY == 3 * kNonceBytes && SC_NET_PROOF_BODY == 2 * kNonceBytes + 32 &&
                  SC_NET_WELCOME_BODY == kNonceBytes + 8 + 32,
              "handshake bodies");

enum class Ctl : uint8_t {
    Sync = SC_NET_CTL_SYNC, Channel = SC_NET_CTL_CHANNEL, Register = SC_NET_CTL_REGISTER,
    PeerJoined = SC_NET_CTL_PEER_JOINED, PeerLeft = SC_NET_CTL_PEER_LEFT,
};

void Label(const uint8_t key[kKeyBytes], const char* label, const uint8_t cn[kNonceBytes],
           const uint8_t hn[kNonceBytes], const uint8_t* extra, size_t extraLen, uint8_t out[32]) {
    sco_hmac_sha256 c;
    sco_hmac_sha256_init(&c, key, kKeyBytes);
    sco_hmac_sha256_update(&c, label, std::strlen(label) + 1);   // with its NUL, as sc_net.h says
    sco_hmac_sha256_update(&c, cn, kNonceBytes);
    sco_hmac_sha256_update(&c, hn, kNonceBytes);
    if (extraLen) sco_hmac_sha256_update(&c, extra, extraLen);
    sco_hmac_sha256_final(&c, out);
}

void Le64(uint64_t v, uint8_t out[8]) {
    for (int i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(v >> (8 * i));
}

std::string Clean(std::string s) {
    if (s.size() > kMaxReason) s.resize(kMaxReason);
    for (char& c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20u || u == 0x7Fu) c = '?';
    }
    return s;
}

void WipeString(std::string& s) {
    if (!s.empty()) sco_wipe(s.data(), s.size());
    s.clear();
}

struct Pending {
    Endpoint ep;
    uint8_t  cn[kNonceBytes] = {};
    uint8_t  hn[kNonceBytes] = {};
    std::string name;
    std::vector<std::string> channels;
    uint64_t createdMs = 0;
};

struct UnrelOut {
    uint16_t channel = 0;
    std::vector<uint8_t> body;
};

struct Link {
    PeerId   id = 0;
    std::string name;
    Endpoint ep;
    uint8_t  cn[kNonceBytes] = {};
    uint8_t  hn[kNonceBytes] = {};
    MacKey   key;
    uint64_t sendSeq = 0;
    ReplayWindow replay;
    uint64_t lastHeardMs = 0;
    uint64_t lastSentMs = 0;
    std::map<uint16_t, SendStream> out;
    std::map<uint16_t, RecvStream> in;
    std::deque<UnrelOut> unrel;
    std::set<uint16_t> registered;   // host: the channels this joiner registered
};

struct LocalChannel {
    uint32_t flags = 0;
    uint32_t maxLen = 0;
};

}  // namespace

const char* SendResultName(SendResult r) {
    switch (r) {
    case SendResult::Ok: return "ok";
    case SendResult::NoSession: return "no session";
    case SendResult::UnknownChannel: return "unknown channel";
    case SendResult::TooLarge: return "too large";
    case SendResult::WrongDirection: return "wrong direction";
    case SendResult::NotRemote: return "no peer registered the channel";
    case SendResult::QueueFull: return "queue full";
    }
    return "?";
}

struct Core::Impl {
    Transport& t;
    Callbacks  cb;
    Options    opts;
    State      state = State::Idle;
    bool       synced = false;   // joiner: SYNC received
    PeerId     self = 0;
    uint64_t   now = 0;
    Stats      stats;
    std::string reason;

    uint8_t sessionKey[kKeyBytes] = {};
    bool    haveKey = false;
    uint8_t salt[kSaltBytes] = {};

    std::vector<std::string> table{ std::string(kControlFqn) };   // index -> name, "" unused
    std::map<std::string, uint16_t, std::less<>> index;
    std::map<std::string, LocalChannel, std::less<>> local;
    std::map<PeerId, std::unique_ptr<Link>> links;
    std::map<PeerId, std::string> names;   // joiner: the other peers
    PeerId nextPeer = kHostPeer + 1;
    std::vector<Pending> pending;          // host

    // Joiner handshake.
    Endpoint hostEp;
    uint8_t  cn[kNonceBytes] = {};
    uint8_t  hn[kNonceBytes] = {};
    bool     challenged = false;
    uint64_t joinStartMs = 0;
    uint64_t lastTryMs = 0;
    std::set<std::string, std::less<>> announced;

    bool inPump = false;
    bool leaveRequested = false;
    std::string leaveReason;
    uint8_t buf[kMaxDatagram + 1] = {};

    Impl(Transport& tr, Callbacks c) : t(tr), cb(std::move(c)) {}

    // ---- helpers --------------------------------------------------------------------------

    void Random(uint8_t* p, size_t n) {
        if (opts.random) {
            opts.random(p, n);
            return;
        }
        std::random_device rd;
        for (size_t i = 0; i < n; i += 4) {
            const uint32_t v = rd();
            for (size_t j = 0; j < 4 && i + j < n; ++j) p[i + j] = static_cast<uint8_t>(v >> (8 * j));
        }
    }

    bool ValidOptions(const Options& o, bool host) const {
        if (o.passphrase.empty() || !ValidName(o.playerName) || o.pbkdf2Iters == 0) return false;
        if (host && (o.maxPeers < 2 || o.maxPeers > kMaxPeers)) return false;
        return o.retryMs && o.resendMs && o.maxResendMs >= o.resendMs && o.keepaliveMs && o.peerTimeoutMs &&
               o.joinTimeoutMs;
    }

    void ResetSession() {
        links.clear();
        names.clear();
        pending.clear();
        table.assign(1, std::string(kControlFqn));
        index.clear();
        announced.clear();
        synced = false;
        challenged = false;
        self = 0;
        nextPeer = kHostPeer + 1;
        sco_wipe(sessionKey, sizeof(sessionKey));
        haveKey = false;
        WipeString(opts.passphrase);
        state = State::Idle;
    }

    void End(const std::string& why) {
        ResetSession();
        reason = why;
        if (cb.state) cb.state(false, reason);
    }

    // Adds fqn to the session table (host). Returns its index and whether it is new; -1 when full.
    int TableAdd(std::string_view fqn, bool* added) {
        *added = false;
        auto it = index.find(fqn);
        if (it != index.end()) return it->second;
        if (table.size() >= kMaxChannels) return -1;
        const uint16_t idx = static_cast<uint16_t>(table.size());
        table.emplace_back(fqn);
        index.emplace(std::string(fqn), idx);
        *added = true;
        return idx;
    }

    // Joiner: a table entry from the host.
    bool TableSet(uint16_t idx, const std::string& fqn) {
        if (idx == 0 || idx >= kMaxChannels || !ValidFqn(fqn) || fqn == kControlFqn) return false;
        auto it = index.find(fqn);
        if (it != index.end()) return it->second == idx;
        if (idx >= table.size()) table.resize(size_t{ idx } + 1);
        if (!table[idx].empty()) return false;
        table[idx] = fqn;
        index.emplace(fqn, idx);
        return true;
    }

    Link* LinkAt(const Endpoint& ep) {
        for (auto& kv : links)
            if (kv.second->ep == ep) return kv.second.get();
        return nullptr;
    }

    void SendRaw(const Endpoint& to, Kind kind, const std::vector<uint8_t>& body) {
        Header h;
        h.kind = kind;
        uint8_t out[kMaxDatagram];
        const size_t n = Encode(h, body.data(), static_cast<uint32_t>(body.size()), nullptr, {}, out, sizeof(out));
        if (!n) return;
        t.Send(to, out, n);
        ++stats.datagramsOut;
    }

    void SendTagged(Link& l, Kind kind, uint16_t channel, const std::vector<uint8_t>& body) {
        Header h;
        h.kind = kind;
        h.channel = channel;
        h.sender = self;
        h.seq = ++l.sendSeq;
        uint8_t out[kMaxDatagram];
        const size_t n = Encode(h, body.data(), static_cast<uint32_t>(body.size()), &l.key, table[channel], out,
                                sizeof(out));
        if (!n) return;
        t.Send(l.ep, out, n);
        ++stats.datagramsOut;
        l.lastSentMs = now;
    }

    void SendControl(Link& l, const std::vector<uint8_t>& msg) {
        l.out[0].Push(self, false, msg.data(), static_cast<uint32_t>(msg.size()));
    }

    void SendRefuse(const Endpoint& to, const uint8_t ncn[kNonceBytes], Refusal code, const char* text) {
        std::vector<uint8_t> b;
        Writer w(b);
        w.Bytes(ncn, kNonceBytes);
        w.U8(static_cast<uint8_t>(code));
        w.U8(kProtocolVersion);
        const size_t n = std::strlen(text);
        w.U16(static_cast<uint16_t>(n));
        w.Bytes(text, n);
        SendRaw(to, Kind::Refuse, b);
        ++stats.joinsRefused;
    }

    void DeriveLinkKey(Link& l) {
        uint8_t k[32];
        Label(sessionKey, SC_NET_LABEL_LINK, l.cn, l.hn, nullptr, 0, k);
        l.key.Set(k);
        sco_wipe(k, sizeof(k));
    }

    static std::vector<uint8_t> CtlPeer(Ctl type, PeerId id, const std::string& text) {
        std::vector<uint8_t> b;
        Writer w(b);
        w.U8(static_cast<uint8_t>(type));
        w.U64(id);
        w.U16(static_cast<uint16_t>(text.size()));
        w.Bytes(text.data(), text.size());
        return b;
    }

    std::vector<uint8_t> CtlChannel(uint16_t idx) const {
        std::vector<uint8_t> b;
        Writer w(b);
        w.U8(static_cast<uint8_t>(Ctl::Channel));
        w.U16(idx);
        w.U16(static_cast<uint16_t>(table[idx].size()));
        w.Bytes(table[idx].data(), table[idx].size());
        return b;
    }

    void BroadcastChannel(uint16_t idx) {
        const std::vector<uint8_t> msg = CtlChannel(idx);
        for (auto& kv : links) SendControl(*kv.second, msg);
    }

    // ---- joiner side ----------------------------------------------------------------------

    void SendHello() {
        std::vector<uint8_t> b;
        Writer w(b);
        w.Bytes(cn, kNonceBytes);
        w.U16(static_cast<uint16_t>(opts.playerName.size()));
        w.Bytes(opts.playerName.data(), opts.playerName.size());
        // The channel names that fit in one datagram; the rest go as REGISTER after SYNC.
        std::vector<const std::string*> fit;
        size_t used = b.size() + 2;
        for (const auto& kv : local) {
            if (used + 2 + kv.first.size() > kMaxBody) break;
            used += 2 + kv.first.size();
            fit.push_back(&kv.first);
        }
        w.U16(static_cast<uint16_t>(fit.size()));
        announced.clear();
        for (const std::string* s : fit) {
            w.U16(static_cast<uint16_t>(s->size()));
            w.Bytes(s->data(), s->size());
            announced.insert(*s);
        }
        if (b.size() < kHelloMinBody) b.resize(kHelloMinBody, 0);
        SendRaw(hostEp, Kind::Hello, b);
    }

    void SendProof() {
        uint8_t proof[32];
        Label(sessionKey, SC_NET_LABEL_PROOF, cn, hn, nullptr, 0, proof);
        std::vector<uint8_t> b;
        Writer w(b);
        w.Bytes(cn, kNonceBytes);
        w.Bytes(hn, kNonceBytes);
        w.Bytes(proof, sizeof(proof));
        SendRaw(hostEp, Kind::Proof, b);
    }

    void OnChallenge(const Endpoint& from, const Packet& p) {
        Reader r(p.body, p.h.bodyLen);
        const uint8_t* rcn = r.Bytes(kNonceBytes);
        const uint8_t* rhn = r.Bytes(kNonceBytes);
        const uint8_t* rsalt = r.Bytes(kSaltBytes);
        if (!(from == hostEp) || !r.ok() || r.Left() != 0 || std::memcmp(rcn, cn, kNonceBytes) != 0) {
            ++stats.handshakeDropped;
            return;
        }
        if (challenged && std::memcmp(rhn, hn, kNonceBytes) == 0) return;   // a resent CHALLENGE
        std::memcpy(hn, rhn, kNonceBytes);
        if (!haveKey || std::memcmp(salt, rsalt, kSaltBytes) != 0) {
            std::memcpy(salt, rsalt, kSaltBytes);
            sco_pbkdf2_hmac_sha256(opts.passphrase.data(), opts.passphrase.size(), salt, kSaltBytes,
                                   opts.pbkdf2Iters, sessionKey, kKeyBytes);
            haveKey = true;
        }
        challenged = true;
        SendProof();
        lastTryMs = now;
    }

    void OnWelcome(const Endpoint& from, const Packet& p) {
        Reader r(p.body, p.h.bodyLen);
        const uint8_t* rcn = r.Bytes(kNonceBytes);
        const uint64_t id = r.U64();
        const uint8_t* mac = r.Bytes(32);
        if (!(from == hostEp) || !challenged || !r.ok() || r.Left() != 0 ||
            std::memcmp(rcn, cn, kNonceBytes) != 0 || id <= kHostPeer) {
            ++stats.handshakeDropped;
            return;
        }
        uint8_t idb[8], want[32];
        Le64(id, idb);
        Label(sessionKey, SC_NET_LABEL_WELCOME, cn, hn, idb, sizeof(idb), want);
        if (!sco_ct_equal(want, mac, sizeof(want))) {
            ++stats.handshakeDropped;   // not from a host that knows the passphrase
            return;
        }
        auto l = std::make_unique<Link>();
        l->id = kHostPeer;
        l->ep = hostEp;
        std::memcpy(l->cn, cn, kNonceBytes);
        std::memcpy(l->hn, hn, kNonceBytes);
        DeriveLinkKey(*l);
        l->lastHeardMs = now;
        l->lastSentMs = now;
        links.emplace(kHostPeer, std::move(l));
        self = id;
        state = State::Joined;
        WipeString(opts.passphrase);
    }

    void OnRefuse(const Endpoint& from, const Packet& p) {
        Reader r(p.body, p.h.bodyLen);
        const uint8_t* rcn = r.Bytes(kNonceBytes);
        const uint8_t code = r.U8();
        const uint8_t hostVersion = r.U8();
        if (!(from == hostEp) || !r.ok() || std::memcmp(rcn, cn, kNonceBytes) != 0) {
            ++stats.handshakeDropped;
            return;
        }
        // The host isn't authenticated yet, so the text is ours, chosen by the code.
        switch (static_cast<Refusal>(code)) {
        case Refusal::Version:
            End("refused: protocol version mismatch (host v" + std::to_string(hostVersion) + ", this v" +
                std::to_string(kProtocolVersion) + ")");
            return;
        case Refusal::Passphrase: End("refused: wrong passphrase"); return;
        case Refusal::Full: End("refused: the session is full"); return;
        case Refusal::NotAdmitted: End("refused: not admitted by the host"); return;
        case Refusal::BadHello: End("refused: the host could not read the join request"); return;
        default: End("refused by the host"); return;
        }
    }

    // ---- host side ------------------------------------------------------------------------

    void SendChallenge(const Pending& pd) {
        std::vector<uint8_t> b;
        Writer w(b);
        w.Bytes(pd.cn, kNonceBytes);
        w.Bytes(pd.hn, kNonceBytes);
        w.Bytes(salt, kSaltBytes);
        SendRaw(pd.ep, Kind::Challenge, b);
    }

    void SendWelcome(const Link& l) {
        uint8_t idb[8], mac[32];
        Le64(l.id, idb);
        Label(sessionKey, SC_NET_LABEL_WELCOME, l.cn, l.hn, idb, sizeof(idb), mac);
        std::vector<uint8_t> b;
        Writer w(b);
        w.Bytes(l.cn, kNonceBytes);
        w.U64(l.id);
        w.Bytes(mac, sizeof(mac));
        SendRaw(l.ep, Kind::Welcome, b);
    }

    void OnHello(const Endpoint& from, const Packet& p) {
        if (p.h.bodyLen < kHelloMinBody) {   // never answer more bytes than were sent
            ++stats.handshakeDropped;
            return;
        }
        Reader r(p.body, p.h.bodyLen);
        const uint8_t* rcn = r.Bytes(kNonceBytes);
        std::string name;
        const bool nameOk = r.Str(r.U16(), &name) && ValidName(name);
        const uint32_t count = r.U16();
        std::vector<std::string> chans;
        bool chansOk = r.ok();
        for (uint32_t i = 0; chansOk && i < count; ++i) {
            std::string fqn;
            chansOk = r.Str(r.U16(), &fqn) && ValidFqn(fqn) && fqn != kControlFqn;
            if (chansOk) chans.push_back(std::move(fqn));
        }
        if (!rcn) {
            ++stats.handshakeDropped;
            return;
        }
        if (!nameOk || !chansOk) {
            SendRefuse(from, rcn, Refusal::BadHello, "bad hello");
            return;
        }
        if (Link* l = LinkAt(from); l && std::memcmp(l->cn, rcn, kNonceBytes) == 0) return;   // late copy
        for (auto it = pending.begin(); it != pending.end(); ++it) {
            if (!(it->ep == from)) continue;
            if (std::memcmp(it->cn, rcn, kNonceBytes) == 0) {
                SendChallenge(*it);   // the joiner didn't get it; same nonce, same answer
                return;
            }
            pending.erase(it);
            break;
        }
        if (pending.size() >= kMaxPending) {
            auto oldest = std::min_element(pending.begin(), pending.end(), [](const Pending& a, const Pending& b) {
                return a.createdMs < b.createdMs;
            });
            pending.erase(oldest);
        }
        Pending pd;
        pd.ep = from;
        std::memcpy(pd.cn, rcn, kNonceBytes);
        Random(pd.hn, kNonceBytes);
        pd.name = std::move(name);
        pd.channels = std::move(chans);
        pd.createdMs = now;
        pending.push_back(std::move(pd));
        SendChallenge(pending.back());
    }

    void OnProof(const Endpoint& from, const Packet& p) {
        Reader r(p.body, p.h.bodyLen);
        const uint8_t* rcn = r.Bytes(kNonceBytes);
        const uint8_t* rhn = r.Bytes(kNonceBytes);
        const uint8_t* proof = r.Bytes(32);
        if (!r.ok() || r.Left() != 0) {
            ++stats.handshakeDropped;
            return;
        }
        uint8_t want[32];
        Label(sessionKey, SC_NET_LABEL_PROOF, rcn, rhn, nullptr, 0, want);
        const bool good = sco_ct_equal(want, proof, sizeof(want)) == 1;
        if (Link* l = LinkAt(from);
            l && std::memcmp(l->cn, rcn, kNonceBytes) == 0 && std::memcmp(l->hn, rhn, kNonceBytes) == 0) {
            if (good) SendWelcome(*l);   // the WELCOME was lost
            return;
        }
        auto it = std::find_if(pending.begin(), pending.end(), [&](const Pending& pd) {
            return pd.ep == from && std::memcmp(pd.cn, rcn, kNonceBytes) == 0 &&
                   std::memcmp(pd.hn, rhn, kNonceBytes) == 0;
        });
        if (it == pending.end()) {
            ++stats.handshakeDropped;
            return;
        }
        Pending pd = std::move(*it);
        pending.erase(it);
        if (!good) {
            SendRefuse(from, pd.cn, Refusal::Passphrase, "wrong passphrase");
            return;
        }
        Link* old = LinkAt(from);   // the same address joining again (a restarted game)
        const size_t others = links.size() - (old ? 1u : 0u);
        if (others + 1 >= opts.maxPeers) {
            SendRefuse(from, pd.cn, Refusal::Full, "session full");
            return;
        }
        if (cb.admit && !cb.admit(pd.name, from)) {
            SendRefuse(from, pd.cn, Refusal::NotAdmitted, "not admitted");
            return;
        }
        if (old) RemoveLink(old->id, "rejoined");

        auto nl = std::make_unique<Link>();
        Link& l = *nl;
        l.id = nextPeer++;
        l.name = pd.name;
        l.ep = from;
        std::memcpy(l.cn, pd.cn, kNonceBytes);
        std::memcpy(l.hn, pd.hn, kNonceBytes);
        DeriveLinkKey(l);
        l.lastHeardMs = now;
        l.lastSentMs = now;
        for (const std::string& fqn : pd.channels) {
            bool added = false;
            const int idx = TableAdd(fqn, &added);
            if (idx < 0) continue;
            if (added) BroadcastChannel(static_cast<uint16_t>(idx));
            l.registered.insert(static_cast<uint16_t>(idx));
        }
        // Tell the others before the new link exists, so it isn't told about itself twice.
        for (auto& kv : links) SendControl(*kv.second, CtlPeer(Ctl::PeerJoined, l.id, l.name));
        const PeerId id = l.id;
        const std::string name = l.name;
        links.emplace(id, std::move(nl));
        Link& lr = *links[id];
        SendWelcome(lr);
        SendControl(lr, Sync());
        if (cb.peer) cb.peer(PeerEvent::Joined, id, name);
    }

    std::vector<uint8_t> Sync() const {
        std::vector<uint8_t> b;
        Writer w(b);
        w.U8(static_cast<uint8_t>(Ctl::Sync));
        w.U16(static_cast<uint16_t>(index.size()));
        for (const auto& kv : index) {
            w.U16(kv.second);
            w.U16(static_cast<uint16_t>(kv.first.size()));
            w.Bytes(kv.first.data(), kv.first.size());
        }
        w.U16(static_cast<uint16_t>(links.size() + 1));
        w.U64(kHostPeer);
        w.U16(static_cast<uint16_t>(opts.playerName.size()));
        w.Bytes(opts.playerName.data(), opts.playerName.size());
        for (const auto& kv : links) {
            w.U64(kv.first);
            w.U16(static_cast<uint16_t>(kv.second->name.size()));
            w.Bytes(kv.second->name.data(), kv.second->name.size());
        }
        return b;
    }

    void RemoveLink(PeerId id, const std::string& why) {
        if (!links.erase(id)) return;
        for (auto& kv : links) SendControl(*kv.second, CtlPeer(Ctl::PeerLeft, id, why));
        if (cb.peer) cb.peer(PeerEvent::Left, id, why);
    }

    // ---- authenticated packets ------------------------------------------------------------

    void OnTagged(const Endpoint& from, const Packet& p) {
        Link* l = (state == State::Hosting || state == State::Joined) ? LinkAt(from) : nullptr;
        if (!l || p.h.sender != l->id) {
            ++stats.spoofed;
            return;
        }
        const uint16_t ch = p.h.channel;
        if (p.h.kind != Kind::Data && ch != 0) {
            ++stats.malformed;
            return;
        }
        if (ch >= table.size() || table[ch].empty()) {
            ++stats.unknownChannel;   // can't name it, so can't check its tag
            return;
        }
        if (!VerifyTag(l->key, table[ch], p)) {
            ++stats.badTag;
            return;
        }
        if (l->replay.Check(p.h.seq) != ReplayWindow::Verdict::New) {
            ++stats.replayed;
            return;
        }
        l->replay.Accept(p.h.seq);
        l->lastHeardMs = now;
        switch (p.h.kind) {
        case Kind::Ping: return;
        case Kind::Ack: OnAck(*l, p); return;
        case Kind::Bye: OnBye(*l, p); return;
        case Kind::Data: OnData(*l, p); return;
        default: ++stats.malformed; return;
        }
    }

    void OnAck(Link& l, const Packet& p) {
        Reader r(p.body, p.h.bodyLen);
        const uint32_t count = r.U16();
        if (!r.ok() || r.Left() != size_t{ count } * kAckEntryBytes) {
            ++stats.malformed;
            return;
        }
        for (uint32_t i = 0; i < count; ++i) {
            const uint16_t ch = r.U16();
            r.U16();
            const uint64_t next = r.U64();
            const uint64_t mask = r.U64();
            auto it = l.out.find(ch);
            if (it != l.out.end()) it->second.OnAck(next, mask);
        }
    }

    void OnBye(Link& l, const Packet& p) {
        Reader r(p.body, p.h.bodyLen);
        std::string text;
        r.Str(r.U16(), &text);
        text = Clean(text);
        if (state == State::Hosting) {
            RemoveLink(l.id, "left: " + text);
        } else {
            End("the host ended the session: " + text);
        }
    }

    void OnData(Link& l, const Packet& p) {
        Reader r(p.body, p.h.bodyLen);
        const uint8_t flags = r.U8();
        const uint8_t z8 = r.U8();
        const uint16_t z16 = r.U16();
        const uint64_t origin = r.U64();
        if (!r.ok() || z8 || z16 || (flags & ~(kDataReliable | kDataToHost))) {
            ++stats.malformed;
            return;
        }
        if (state == State::Hosting && origin != l.id) {   // a joiner speaks only for itself
            ++stats.spoofed;
            return;
        }
        const uint16_t ch = p.h.channel;
        const bool reliable = (flags & kDataReliable) != 0;
        const bool toHost = (flags & kDataToHost) != 0;
        if (ch == 0 && !reliable) {
            ++stats.malformed;
            return;
        }
        if (!reliable) {
            const uint32_t n = static_cast<uint32_t>(r.Left());
            if (n > kMaxUnreliable) {
                ++stats.malformed;
                return;
            }
            Deliver(l.id, ch, origin, r.Bytes(n), n, toHost, false);
            return;
        }
        const uint64_t unit = r.U64();
        const uint32_t msgLen = r.U32();
        const uint32_t offset = r.U32();
        if (!r.ok()) {
            ++stats.malformed;
            return;
        }
        const uint32_t n = static_cast<uint32_t>(r.Left());
        const uint8_t* data = r.Bytes(n);
        uint32_t maxLen = kControlMaxLen;
        if (ch != 0) {
            auto lc = local.find(table[ch]);
            // The host reassembles up to the protocol limit, since it may relay; Deliver applies
            // its own max_len. A joiner discards (but acknowledges) what it didn't register.
            maxLen = state == State::Hosting ? kMaxReliable : (lc != local.end() ? lc->second.maxLen : 0u);
        }
        RecvStream& rs = l.in[ch];
        const uint64_t refusedBefore = rs.Refused();
        std::vector<RecvStream::Delivery> done;
        if (rs.OnUnit(unit, origin, msgLen, offset, data, n, maxLen, &done) == RecvStream::Verdict::Malformed)
            ++stats.malformed;
        stats.refused += rs.Refused() - refusedBefore;
        const PeerId linkId = l.id;
        for (RecvStream::Delivery& d : done) {
            // A callback may Leave (deferred) but never remove links, so linkId stays valid.
            Deliver(linkId, ch, d.origin, d.data.data(), static_cast<uint32_t>(d.data.size()), toHost, true);
        }
    }

    void Deliver(PeerId linkId, uint16_t ch, PeerId origin, const uint8_t* data, uint32_t len, bool toHost,
                 bool reliable) {
        if (ch == 0) {
            auto it = links.find(linkId);
            if (it != links.end()) OnControl(*it->second, data, len);
            return;
        }
        const std::string fqn = table[ch];   // a copy: a callback may grow the table
        bool used = false;
        auto lc = local.find(fqn);
        if (lc != local.end()) {
            used = true;
            const LocalChannel c = lc->second;
            if ((c.flags & kFromHost) && origin != kHostPeer) {
                ++stats.refused;
            } else if (len > c.maxLen) {
                ++stats.refused;
            } else if (cb.message) {
                cb.message(Message{ fqn, origin, data, len });
            }
        }
        if (state == State::Hosting && !toHost) {
            for (auto& kv : links) {
                Link& b = *kv.second;
                if (b.id == linkId || !b.registered.count(ch)) continue;
                used = true;
                if (!Queue(b, ch, origin, data, len, reliable)) ++stats.refused;
            }
        }
        if (!used) ++stats.unknownChannel;
    }

    bool Queue(Link& l, uint16_t ch, PeerId origin, const uint8_t* data, uint32_t len, bool reliable, bool toHost = false) {
        if (reliable) return l.out[ch].Push(origin, toHost, data, len);
        if (l.unrel.size() >= kMaxUnrelQueue) return false;
        UnrelOut u;
        u.channel = ch;
        Writer w(u.body);
        w.U8(toHost ? kDataToHost : 0);
        w.U8(0);
        w.U16(0);
        w.U64(origin);
        w.Bytes(data, len);
        l.unrel.push_back(std::move(u));
        return true;
    }

    void OnControl(Link& l, const uint8_t* data, uint32_t len) {
        Reader r(data, len);
        const Ctl type = static_cast<Ctl>(r.U8());
        if (state == State::Hosting) {
            if (type != Ctl::Register) {
                ++stats.malformed;
                return;
            }
            std::string fqn;
            if (!r.Str(r.U16(), &fqn) || r.Left() || !ValidFqn(fqn) || fqn == kControlFqn) {
                ++stats.malformed;
                return;
            }
            bool added = false;
            const int idx = TableAdd(fqn, &added);
            if (idx < 0) {
                ++stats.refused;   // the table is full; the joiner never gets an index
                return;
            }
            if (added) BroadcastChannel(static_cast<uint16_t>(idx));
            l.registered.insert(static_cast<uint16_t>(idx));
            return;
        }
        switch (type) {
        case Ctl::Sync: OnSync(l, r); return;
        case Ctl::Channel: {
            const uint16_t idx = r.U16();
            std::string fqn;
            if (!r.Str(r.U16(), &fqn) || r.Left() || !TableSet(idx, fqn)) ++stats.malformed;
            return;
        }
        case Ctl::PeerJoined:
        case Ctl::PeerLeft: {
            const PeerId id = r.U64();
            std::string text;
            if (!r.Str(r.U16(), &text) || r.Left() || id == self || id == kHostPeer ||
                (type == Ctl::PeerJoined && !ValidName(text))) {
                ++stats.malformed;
                return;
            }
            if (type == Ctl::PeerJoined) {
                names[id] = text;
                if (cb.peer) cb.peer(PeerEvent::Joined, id, text);
            } else if (names.erase(id)) {
                text = Clean(text);
                if (cb.peer) cb.peer(PeerEvent::Left, id, text);
            }
            return;
        }
        default: ++stats.malformed; return;
        }
    }

    void OnSync(Link& l, Reader& r) {
        if (synced) {
            ++stats.malformed;
            return;
        }
        const uint32_t nch = r.U16();
        for (uint32_t i = 0; i < nch && r.ok(); ++i) {
            const uint16_t idx = r.U16();
            std::string fqn;
            if (r.Str(r.U16(), &fqn) && !TableSet(idx, fqn)) {
                ++stats.malformed;
                return;
            }
        }
        std::map<PeerId, std::string> got;
        const uint32_t np = r.U16();
        for (uint32_t i = 0; i < np && r.ok(); ++i) {
            const PeerId id = r.U64();
            std::string name;
            if (r.Str(r.U16(), &name) && (!ValidName(name) || id == 0)) {
                ++stats.malformed;
                return;
            }
            if (id != self) got[id] = std::move(name);
        }
        if (!r.ok() || r.Left() || !got.count(kHostPeer)) {
            ++stats.malformed;
            return;
        }
        names = std::move(got);
        synced = true;
        // Channels registered after the HELLO (or that didn't fit in it).
        for (const auto& kv : local) {
            if (announced.count(kv.first)) continue;
            std::vector<uint8_t> b;
            Writer w(b);
            w.U8(static_cast<uint8_t>(Ctl::Register));
            w.U16(static_cast<uint16_t>(kv.first.size()));
            w.Bytes(kv.first.data(), kv.first.size());
            SendControl(l, b);
            announced.insert(kv.first);
        }
        reason.clear();
        if (cb.state) cb.state(true, {});
        if (cb.peer) {
            const std::map<PeerId, std::string> snapshot = names;
            for (const auto& kv : snapshot) cb.peer(PeerEvent::Joined, kv.first, kv.second);
        }
    }

    // ---- timers ---------------------------------------------------------------------------

    void Timers() {
        if (state == State::Joining) {
            if (now >= joinStartMs + opts.joinTimeoutMs) {
                End("no answer from the host");
                return;
            }
            if (now >= lastTryMs + opts.retryMs) {
                if (challenged) {
                    SendProof();
                } else {
                    SendHello();
                }
                lastTryMs = now;
            }
            return;
        }
        if (state == State::Hosting) {
            pending.erase(std::remove_if(pending.begin(), pending.end(),
                                         [&](const Pending& pd) { return now >= pd.createdMs + opts.joinTimeoutMs; }),
                          pending.end());
        }
        if (state != State::Hosting && state != State::Joined) return;
        std::vector<PeerId> silent;
        for (auto& kv : links)
            if (now >= kv.second->lastHeardMs + opts.peerTimeoutMs) silent.push_back(kv.first);
        for (PeerId id : silent) {
            if (state == State::Joined) {
                End("the host timed out");
                return;
            }
            RemoveLink(id, "timed out");
        }
        for (auto& kv : links) Flush(*kv.second);
    }

    void Flush(Link& l) {
        // Acknowledgements first: they unblock the other side's window.
        std::vector<uint8_t> ack;
        uint32_t entries = 0;
        auto sendAck = [&] {
            if (!entries) return;
            ack[0] = static_cast<uint8_t>(entries);
            ack[1] = static_cast<uint8_t>(entries >> 8);
            SendTagged(l, Kind::Ack, 0, ack);
            entries = 0;
        };
        for (auto& kv : l.in) {
            if (!kv.second.ackDue) continue;
            kv.second.ackDue = false;
            if (!entries) ack.assign(2, 0);
            Writer w(ack);
            w.U16(kv.first);
            w.U16(0);
            w.U64(kv.second.Next());
            w.U64(kv.second.Mask());
            if (++entries == (kMaxBody - 2) / kAckEntryBytes) sendAck();
        }
        sendAck();
        for (auto& kv : l.out) {
            for (const OutUnit* u : kv.second.Due(now, opts.resendMs, opts.maxResendMs)) {
                if (u->sends > 1) ++stats.retransmits;
                std::vector<uint8_t> b;
                b.reserve(28 + u->data.size());
                Writer w(b);
                w.U8(static_cast<uint8_t>(kDataReliable | (u->toHost ? kDataToHost : 0)));
                w.U8(0);
                w.U16(0);
                w.U64(u->origin);
                w.U64(u->seq);
                w.U32(u->msgLen);
                w.U32(u->offset);
                w.Bytes(u->data.data(), u->data.size());
                SendTagged(l, Kind::Data, kv.first, b);
            }
        }
        while (!l.unrel.empty()) {
            SendTagged(l, Kind::Data, l.unrel.front().channel, l.unrel.front().body);
            l.unrel.pop_front();
        }
        if (now >= l.lastSentMs + opts.keepaliveMs) SendTagged(l, Kind::Ping, 0, {});
    }

    void DoLeave(const std::string& why) {
        if (state == State::Idle) return;
        std::vector<uint8_t> b;
        Writer w(b);
        const std::string text = Clean(why);
        w.U16(static_cast<uint16_t>(text.size()));
        w.Bytes(text.data(), text.size());
        for (auto& kv : links) SendTagged(*kv.second, Kind::Bye, 0, b);
        End(text);
    }

    void Handle(const Endpoint& from, const uint8_t* data, size_t len) {
        Packet p;
        const ParseError e = Parse(data, len, &p);
        if (e == ParseError::BadVersion) {
            ++stats.badVersion;
            // A HELLO from another version gets a REFUSE naming ours. Only the first 6 bytes of
            // the header are relied on, and the client nonce if the datagram has one.
            if (state == State::Hosting && p.h.kind == Kind::Hello) {
                uint8_t ncn[kNonceBytes] = {};
                if (len >= kHeaderBytes + kNonceBytes) std::memcpy(ncn, data + kHeaderBytes, kNonceBytes);
                SendRefuse(from, ncn, Refusal::Version, "protocol version mismatch");
            } else if (state == State::Joining && p.h.kind == Kind::Refuse && from == hostEp &&
                       len >= kHeaderBytes + kNonceBytes && std::memcmp(data + kHeaderBytes, cn, kNonceBytes) == 0) {
                End("refused: protocol version mismatch (host v" + std::to_string(p.h.version) + ", this v" +
                    std::to_string(kProtocolVersion) + ")");
            }
            return;
        }
        if (e != ParseError::Ok) {
            ++stats.malformed;
            return;
        }
        switch (p.h.kind) {
        case Kind::Hello:
            if (state == State::Hosting) return OnHello(from, p);
            break;
        case Kind::Challenge:
            if (state == State::Joining) return OnChallenge(from, p);
            break;
        case Kind::Proof:
            if (state == State::Hosting) return OnProof(from, p);
            break;
        case Kind::Welcome:
            if (state == State::Joining) return OnWelcome(from, p);
            break;
        case Kind::Refuse:
            if (state == State::Joining) return OnRefuse(from, p);
            break;
        default: return OnTagged(from, p);
        }
        ++stats.handshakeDropped;
    }
};

Core::Core(Transport& transport, Callbacks callbacks) : d_(std::make_unique<Impl>(transport, std::move(callbacks))) {}

Core::~Core() = default;

bool Core::RegisterChannel(std::string_view fqn, uint32_t flags, uint32_t maxLen) {
    Impl& d = *d_;
    if (!ValidFqn(fqn) || fqn == kControlFqn || (flags & ~kChannelFlags) ||
        ((flags & kFromHost) && (flags & kToHost)))
        return false;
    const uint32_t limit = (flags & kReliable) ? kMaxReliable : kMaxUnreliable;
    if (maxLen == 0 || maxLen > limit || d.local.count(fqn)) return false;
    d.local.emplace(std::string(fqn), LocalChannel{ flags, maxLen });
    if (d.state == State::Hosting) {
        bool added = false;
        const int idx = d.TableAdd(fqn, &added);
        if (idx > 0 && added) d.BroadcastChannel(static_cast<uint16_t>(idx));
    } else if (d.state == State::Joined && d.synced) {
        std::vector<uint8_t> b;
        Writer w(b);
        w.U8(static_cast<uint8_t>(Ctl::Register));
        w.U16(static_cast<uint16_t>(fqn.size()));
        w.Bytes(fqn.data(), fqn.size());
        auto it = d.links.find(kHostPeer);
        if (it != d.links.end()) d.SendControl(*it->second, b);
        d.announced.emplace(fqn);
    }
    return true;
}

bool Core::UnregisterChannel(std::string_view fqn) {
    auto it = d_->local.find(fqn);
    if (it == d_->local.end()) return false;
    d_->local.erase(it);
    return true;
}

bool Core::Host(const Options& opts, uint64_t nowMs) {
    Impl& d = *d_;
    if (d.state != State::Idle || d.inPump || !d.ValidOptions(opts, true)) return false;
    d.ResetSession();
    d.opts = opts;
    d.now = nowMs;
    d.Random(d.salt, kSaltBytes);
    sco_pbkdf2_hmac_sha256(d.opts.passphrase.data(), d.opts.passphrase.size(), d.salt, kSaltBytes,
                           d.opts.pbkdf2Iters, d.sessionKey, kKeyBytes);
    WipeString(d.opts.passphrase);
    d.haveKey = true;
    d.self = kHostPeer;
    d.state = State::Hosting;
    for (const auto& kv : d.local) {
        bool added = false;
        d.TableAdd(kv.first, &added);
    }
    d.reason.clear();
    if (d.cb.state) d.cb.state(true, {});
    return true;
}

bool Core::Join(const Options& opts, const Endpoint& host, uint64_t nowMs) {
    Impl& d = *d_;
    if (d.state != State::Idle || d.inPump || !d.ValidOptions(opts, false)) return false;
    d.ResetSession();
    d.opts = opts;
    d.now = nowMs;
    d.hostEp = host;
    d.Random(d.cn, kNonceBytes);
    d.state = State::Joining;
    d.joinStartMs = nowMs;
    d.lastTryMs = nowMs;
    d.reason.clear();
    d.SendHello();
    return true;
}

void Core::Leave(std::string_view reason) {
    Impl& d = *d_;
    if (d.inPump) {
        d.leaveRequested = true;
        d.leaveReason = std::string(reason);
        return;
    }
    d.DoLeave(std::string(reason));
}

SendResult Core::Send(std::string_view fqn, const void* data, uint32_t len) {
    Impl& d = *d_;
    if (!Active()) return SendResult::NoSession;
    auto lc = d.local.find(fqn);
    if (lc == d.local.end()) return SendResult::UnknownChannel;
    auto ix = d.index.find(fqn);
    if (ix == d.index.end()) return SendResult::UnknownChannel;
    const LocalChannel c = lc->second;
    const uint16_t ch = ix->second;
    const bool reliable = (c.flags & kReliable) != 0;
    if (len > std::min(c.maxLen, reliable ? kMaxReliable : kMaxUnreliable)) return SendResult::TooLarge;
    if (len && !data) return SendResult::TooLarge;
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    if (d.state == State::Hosting) {
        if (c.flags & kToHost) return SendResult::WrongDirection;
        bool any = false, full = false;
        for (auto& kv : d.links) {
            if (!kv.second->registered.count(ch)) continue;
            any = true;
            if (!d.Queue(*kv.second, ch, d.self, bytes, len, reliable)) full = true;
        }
        if (!any) {
            ++d.stats.refused;
            return SendResult::NotRemote;
        }
        return full ? SendResult::QueueFull : SendResult::Ok;
    }
    if (c.flags & kFromHost) return SendResult::WrongDirection;
    auto it = d.links.find(kHostPeer);
    if (it == d.links.end()) return SendResult::NoSession;
    const bool toHost = (c.flags & kToHost) != 0;
    return d.Queue(*it->second, ch, d.self, bytes, len, reliable, toHost) ? SendResult::Ok : SendResult::QueueFull;
}

void Core::Pump(uint64_t nowMs) {
    Impl& d = *d_;
    if (d.inPump) return;
    d.inPump = true;
    d.now = nowMs;
    for (uint32_t i = 0; i < kMaxPumpDatagrams; ++i) {
        Endpoint from;
        size_t len = 0;
        if (!d.t.Receive(&from, d.buf, sizeof(d.buf), &len)) break;
        ++d.stats.datagramsIn;
        if (len > kMaxDatagram) {
            ++d.stats.malformed;
            continue;
        }
        d.Handle(from, d.buf, len);
    }
    d.Timers();
    d.inPump = false;
    if (d.leaveRequested) {
        d.leaveRequested = false;
        d.DoLeave(d.leaveReason);
    }
}

State Core::GetState() const { return d_->state; }

bool Core::Active() const {
    return d_->state == State::Hosting || (d_->state == State::Joined && d_->synced);
}

PeerId Core::Self() const { return Active() ? d_->self : 0; }

std::vector<PeerInfo> Core::Peers() const {
    std::vector<PeerInfo> out;
    if (!Active()) return out;
    out.push_back(PeerInfo{ d_->self, d_->opts.playerName });
    if (d_->state == State::Hosting) {
        for (const auto& kv : d_->links) out.push_back(PeerInfo{ kv.first, kv.second->name });
    } else {
        for (const auto& kv : d_->names) out.push_back(PeerInfo{ kv.first, kv.second });
    }
    return out;
}

bool Core::PeerName(PeerId id, std::string* out) const {
    for (const PeerInfo& p : Peers()) {
        if (p.id == id) {
            *out = p.name;
            return true;
        }
    }
    return false;
}

const Stats& Core::GetStats() const { return d_->stats; }

std::string Core::LastReason() const { return d_->reason; }

}  // namespace sco::net
