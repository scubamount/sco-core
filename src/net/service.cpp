// sco.net: the host service behind the C table of sco_net.h and the product's session control
// (sco/net/session.h). The session itself (handshake, links, delivery) is sco::net::Core; this
// file adds the network thread, channel ownership, quotas, the LAN rule, delivery on the game
// thread and lifetime.
//
// Locks: g_control serializes Host / Join / Leave / Stop / PumpNetwork (it is never taken by a
// table call or by the network thread). g_lock guards everything else below: the registry, the
// per-plugin state, the command queue for the network thread, the inbox for the game thread and
// the snapshot of the session that table calls read. Order: g_control, then g_lock, then the
// runtime's owner lock (detail::Released). The network thread never holds g_lock while it calls
// into the Core, so the Core's callbacks (which run inside Pump and take g_lock) can't deadlock;
// the game thread never holds it while it calls a plugin.
//
// Plugin memory (names, data, output buffers) is read and written only while no lock is held:
// inputs are copied first, outputs after the lock is released, so a bad pointer faults with
// nothing held.
#include "sco/net/session.h"
#include "sco/net/udp.h"
#include "sco/caps.h"
#include "sco/host.h"
#include "sco/log.h"
#include "../api/internal.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace sco::net {

namespace {

static_assert(SCO_NET_RELIABLE == kReliable && SCO_NET_FROM_HOST == kFromHost && SCO_NET_TO_HOST == kToHost);

constexpr const char* kCapability = "sco.net";

struct Channel {
    const void*        owner = nullptr;
    uint64_t           id = 0;
    std::string        fqn;
    uint32_t           flags = 0;
    uint32_t           maxLen = 0;
    sco_net_on_message cb = nullptr;
    void*              ctx = nullptr;
};

// Per plugin: the send quota (token buckets), bytes waiting for the network thread, messages
// waiting for Tick.
struct PluginState {
    const void* owner = nullptr;
    double      msgTokens = 0, byteTokens = 0;
    uint64_t    lastMs = 0;
    size_t      queuedBytes = 0;
    size_t      inboxItems = 0, inboxBytes = 0;
};

enum class ItemKind : uint8_t { Message, State, Peer };
struct Item {
    ItemKind             kind = ItemKind::Message;
    const void*          owner = nullptr;
    uint64_t             channel = 0;
    PeerId               from = 0;
    std::vector<uint8_t> data;
    sco_net_state_event  state{};
    sco_net_peer_event   peer{};
};

enum class CmdKind : uint8_t { Register, Unregister, Send, Leave };
struct Cmd {
    CmdKind              kind = CmdKind::Send;
    const void*          owner = nullptr;
    std::string          text;   // the channel, or Leave's reason
    uint32_t             flags = 0, maxLen = 0;
    std::vector<uint8_t> data;
};

struct Session {
    std::unique_ptr<udp::Socket>     socket;
    Transport*                       raw = nullptr;
    std::unique_ptr<ScopedTransport> scoped;
    std::unique_ptr<Core>            core;
    bool                             host = false;
    Options                          coreOpts;
    Endpoint                         hostEp;
    std::vector<Channel>             replay;   // the registry when the session began
    std::function<bool(std::string_view, const Endpoint&)> admit;
    uint64_t                         sendDropped = 0;   // network thread only
};

std::mutex                  g_control;
std::unique_ptr<Session>    g_session;   // ServiceOptions::thread false only
std::thread                 g_thread;

std::mutex                  g_lock;
std::condition_variable     g_wake;       // the network thread: commands queued
std::condition_variable     g_delivery;   // WaitForDelivery: the inbox got something
bool                        g_started = false;
ServiceOptions              g_opts;
std::vector<Channel>        g_channels;
uint64_t                    g_nextChannel = 1;
std::vector<PluginState>    g_plugins;
std::deque<Item>            g_inbox;
std::deque<Cmd>             g_cmds;
bool                        g_live = false;   // a session runs: register/unregister/send go to it
// The session as the network thread last saw it.
State                       g_state = State::Idle;
bool                        g_active = false;
PeerId                      g_self = 0;
std::vector<PeerInfo>       g_peers;
std::map<PeerId, uint64_t>  g_entities;
std::string                 g_reason;
uint16_t                    g_port = 0;
ServiceStats                g_stats;

sco_result C(Result r) { return static_cast<sco_result>(static_cast<uint32_t>(r)); }

template <class F>
sco_result Guard(F&& f) noexcept {
    try {
        return C(f());
    } catch (...) {
        return SCO_TOO_MANY;
    }
}

uint64_t Now() {
    if (g_opts.clock) return g_opts.clock();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void Wipe(std::string& s) {
    std::fill(s.begin(), s.end(), '\0');
    s.clear();
}

// Under g_lock.
PluginState& Plugin(const void* owner) {
    for (auto& p : g_plugins)
        if (p.owner == owner) return p;
    PluginState p;
    p.owner = owner;
    p.msgTokens = g_opts.quotaMsgs;
    p.byteTokens = g_opts.quotaBytes;
    p.lastMs = Now();
    g_plugins.push_back(p);
    return g_plugins.back();
}

Channel* FindChannel(std::string_view fqn) {
    for (auto& c : g_channels)
        if (c.fqn == fqn) return &c;
    return nullptr;
}

// Under g_lock.
void Push(Item&& item) {
    g_inbox.push_back(std::move(item));
    g_delivery.notify_all();
}

void PushState(bool active, std::string_view reason) {
    Item it;
    it.kind = ItemKind::State;
    it.state.size = sizeof(sco_net_state_event);
    it.state.active = active ? 1 : 0;
    const size_t n = std::min(reason.size(), sizeof(it.state.reason) - 1);
    std::memcpy(it.state.reason, reason.data(), n);
    Push(std::move(it));
}

void PushPeer(uint32_t what, PeerId id, uint64_t entity) {
    Item it;
    it.kind = ItemKind::Peer;
    it.peer.size = sizeof(sco_net_peer_event);
    it.peer.what = what;
    it.peer.peer.peer_id = id;
    it.peer.peer.entity_id = entity;
    Push(std::move(it));
}

// Under g_lock: commands are queued only while a session runs; the next session replays the
// registry instead.
void QueueCmd(Cmd&& cmd) {
    if (!g_live) return;
    g_cmds.push_back(std::move(cmd));
    g_wake.notify_all();
}

// ---- the network side (the network thread, or PumpNetwork's caller) -----------------------------

Callbacks MakeCallbacks(Session& s) {
    Callbacks cb;
    cb.message = [](const Message& m) {
        try {
            std::lock_guard<std::mutex> hold(g_lock);
            const Channel* c = FindChannel(m.channel);
            if (!c) {
                ++g_stats.inboxDropped;
                return;
            }
            PluginState& p = Plugin(c->owner);
            if (p.inboxItems >= kMaxInboxPerPlugin || p.inboxBytes + m.len > kMaxInboxBytesPerPlugin) {
                ++g_stats.inboxDropped;
                return;
            }
            Item it;
            it.kind = ItemKind::Message;
            it.owner = c->owner;
            it.channel = c->id;
            it.from = m.from;
            it.data.assign(m.data, m.data + m.len);
            ++p.inboxItems;
            p.inboxBytes += m.len;
            Push(std::move(it));
        } catch (...) {
            std::lock_guard<std::mutex> hold(g_lock);
            ++g_stats.inboxDropped;
        }
    };
    cb.peer = [](PeerEvent ev, PeerId id, std::string_view) {
        try {
            std::lock_guard<std::mutex> hold(g_lock);
            uint64_t entity = 0;
            if (ev == PeerEvent::Left) {
                auto it = g_entities.find(id);
                if (it != g_entities.end()) {
                    entity = it->second;
                    g_entities.erase(it);
                }
            }
            PushPeer(ev == PeerEvent::Joined ? SCO_NET_PEER_JOINED : SCO_NET_PEER_LEFT, id, entity);
        } catch (...) {
        }
        Log("[net] peer %llu %s", static_cast<unsigned long long>(id), ev == PeerEvent::Joined ? "joined" : "left");
    };
    cb.state = [](bool active, std::string_view reason) {
        try {
            std::lock_guard<std::mutex> hold(g_lock);
            if (!active) g_reason = std::string(reason);
            PushState(active, reason);
        } catch (...) {
        }
        if (active) Log("[net] session up");
        else Log("[net] session ended: %.*s", static_cast<int>(reason.size()), reason.data());
    };
    if (s.admit) {
        Session* sp = &s;
        cb.admit = [sp](std::string_view name, const Endpoint& from) { return sp->admit(name, from); };
    }
    return cb;
}

void Snapshot(Session& s) {
    std::vector<PeerInfo> peers = s.core->Peers();
    std::lock_guard<std::mutex> hold(g_lock);
    g_state = s.core->GetState();
    g_active = s.core->Active();
    g_self = s.core->Self();
    g_peers.swap(peers);
    g_stats.core = s.core->GetStats();
    g_stats.outOfScope = s.scoped->Dropped();
    g_stats.sendDropped = s.sendDropped;
}

// Creates the Core, registers the channels the registry had when the session began, and hosts or
// joins. False when the Core refused the options (the reason went out as "net.state").
bool Setup(Session& s) {
    s.core = std::make_unique<Core>(*s.scoped, MakeCallbacks(s));
    for (const Channel& c : s.replay) s.core->RegisterChannel(c.fqn, c.flags, c.maxLen);
    const bool ok = s.host ? s.core->Host(s.coreOpts, Now()) : s.core->Join(s.coreOpts, s.hostEp, Now());
    Wipe(s.coreOpts.passphrase);
    if (!ok) {
        std::lock_guard<std::mutex> hold(g_lock);
        g_reason = "the session options were refused";
        PushState(false, g_reason);
        return false;
    }
    Snapshot(s);
    return true;
}

// One step: the queued commands, then receive and timers. False once the session is over.
bool Step(Session& s, uint64_t now) {
    std::deque<Cmd> cmds;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        cmds.swap(g_cmds);
        for (const Cmd& c : cmds)
            if (c.kind == CmdKind::Send)
                for (auto& p : g_plugins)
                    if (p.owner == c.owner) p.queuedBytes -= std::min(p.queuedBytes, c.data.size());
    }
    for (const Cmd& c : cmds) {
        switch (c.kind) {
            case CmdKind::Register:   s.core->RegisterChannel(c.text, c.flags, c.maxLen); break;
            case CmdKind::Unregister: s.core->UnregisterChannel(c.text); break;
            case CmdKind::Send:
                if (s.core->Send(c.text, c.data.data(), static_cast<uint32_t>(c.data.size())) != SendResult::Ok)
                    ++s.sendDropped;
                break;
            case CmdKind::Leave:      s.core->Leave(c.text); break;
        }
    }
    if (s.core->GetState() != State::Idle) s.core->Pump(now);
    Snapshot(s);
    return s.core->GetState() != State::Idle;
}

// The session is over: nothing more goes to it, and table calls see no session.
void Finish(Session& s) {
    {
        std::lock_guard<std::mutex> hold(g_lock);
        g_live = false;
        g_cmds.clear();
        for (auto& p : g_plugins) p.queuedBytes = 0;
        g_state = State::Idle;
        g_active = false;
        g_self = 0;
        g_peers.clear();
        g_entities.clear();
        g_port = 0;
        if (s.core) {
            g_stats.core = s.core->GetStats();
            if (g_reason.empty()) g_reason = s.core->LastReason();
        }
        if (s.scoped) g_stats.outOfScope = s.scoped->Dropped();
        g_stats.sendDropped = s.sendDropped;
    }
    s.core.reset();
    s.scoped.reset();
    s.socket.reset();
}

// The network thread. It owns the session from here on.
void Run(Session* raw, uint32_t pollMs) {
    std::unique_ptr<Session> s(raw);
    try {
        if (Setup(*s)) {
            for (;;) {
                {
                    std::unique_lock<std::mutex> hold(g_lock);
                    g_wake.wait_for(hold, std::chrono::milliseconds(pollMs), [] { return !g_cmds.empty(); });
                }
                if (!Step(*s, Now())) break;
            }
        }
    } catch (...) {
        Log("[net] network thread stopped: out of memory");
        std::lock_guard<std::mutex> hold(g_lock);
        g_reason = "out of memory";
        PushState(false, g_reason);
    }
    Finish(*s);
}

// Under g_control. True while a session runs; reaps a network thread whose session ended.
bool Busy() {
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (g_live) return true;
    }
    if (g_thread.joinable()) g_thread.join();
    return false;
}

// Under g_control, not Busy.
Result Begin(std::unique_ptr<Session> s) {
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (!g_started) return Result::Unavailable;
        s->replay = g_channels;
        g_cmds.clear();
        for (auto& p : g_plugins) p.queuedBytes = 0;
        g_live = true;
        g_state = s->host ? State::Hosting : State::Joining;
        g_active = false;
        g_reason.clear();
        g_port = s->socket ? s->socket->Port() : 0;
        g_stats = ServiceStats();
    }
    if (g_opts.thread) {
        Session* raw = s.release();
        try {
            g_thread = std::thread(Run, raw, g_opts.pollMs);
        } catch (const std::system_error&) {
            std::unique_ptr<Session> back(raw);   // no thread took it
            Finish(*back);
            Log("[net] no network thread");
            return Result::Failed;
        }
        return Result::Ok;
    }
    g_session = std::move(s);
    if (!Setup(*g_session)) {
        Finish(*g_session);
        g_session.reset();
    }
    return Result::Ok;
}

void LeaveLocked(const char* reason) {
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (g_live) {
            Cmd c;
            c.kind = CmdKind::Leave;
            c.text = reason ? reason : "left";
            g_cmds.push_back(std::move(c));
            g_wake.notify_all();
        }
    }
    if (g_thread.joinable()) g_thread.join();
    if (g_session) {
        while (Step(*g_session, Now())) {
        }
        Finish(*g_session);
        g_session.reset();
    }
}

// ---- the table ----------------------------------------------------------------------------------

// The caller's id and a channel name it may use: "<id>.<name>", a valid sco.net name.
Result Owned(sco_plugin* self, const char* fqn, std::string* out) {
    const char* id = host::PluginId(self);
    if (!id || !fqn) return Result::BadArg;
    const size_t n = strnlen(fqn, SCO_NET_MAX_FQN + 1);
    if (n > SCO_NET_MAX_FQN) return Result::BadArg;
    const std::string_view name(fqn, n);
    const size_t idLen = std::strlen(id);
    if (!ValidFqn(name) || name.size() <= idLen + 1 || name.compare(0, idLen, id) != 0 || name[idLen] != '.')
        return Result::BadArg;
    *out = std::string(name);
    return Result::Ok;
}

int IsActive() {
    std::lock_guard<std::mutex> hold(g_lock);
    return g_started && g_active ? 1 : 0;
}

sco_result GetPeers(sco_net_peer* out, uint32_t* io) {
    return Guard([&] {
        if (!io) return Result::BadArg;
        const uint32_t cap = *io;
        if (!out && cap) return Result::BadArg;
        std::vector<sco_net_peer> peers;
        {
            std::lock_guard<std::mutex> hold(g_lock);
            if (!g_started) return Result::Unavailable;
            if (g_active) {
                for (const PeerInfo& p : g_peers) {
                    auto it = g_entities.find(p.id);
                    peers.push_back(sco_net_peer{ p.id, it == g_entities.end() ? 0 : it->second });
                }
            }
        }
        const uint32_t n = static_cast<uint32_t>(peers.size());
        for (uint32_t i = 0; i < n && i < cap; ++i) out[i] = peers[i];
        *io = n;
        return n > cap ? Result::TooMany : Result::Ok;
    });
}

sco_result GetPeerName(uint64_t peer, char* buf, uint32_t cap) {
    return Guard([&] {
        if (!buf || !cap) return Result::BadArg;
        std::string name;
        bool found = false;
        {
            std::lock_guard<std::mutex> hold(g_lock);
            if (!g_started) return Result::Unavailable;
            if (g_active) {
                for (const PeerInfo& p : g_peers)
                    if (p.id == peer) {
                        name = p.name;
                        found = true;
                        break;
                    }
            }
        }
        if (!found) return Result::NotFound;
        size_t n = std::min<size_t>(name.size(), cap - 1);
        // Never cut a UTF-8 sequence in half.
        if (n < name.size())
            while (n > 0 && (static_cast<unsigned char>(name[n]) & 0xC0) == 0x80) --n;
        std::memcpy(buf, name.data(), n);
        buf[n] = '\0';
        return Result::Ok;
    });
}

sco_result SendChannel(sco_plugin* self, const char* fqn, const void* buf, uint32_t len) {
    return Guard([&] {
        std::string name;
        if (const Result r = Owned(self, fqn, &name); r != Result::Ok) return r;
        if ((!buf && len) || len > SCO_NET_MAX_RELIABLE) return Result::BadArg;
        Cmd cmd;
        cmd.kind = CmdKind::Send;
        cmd.owner = self;
        cmd.data.resize(len);
        if (len) std::memcpy(cmd.data.data(), buf, len);
        cmd.text = std::move(name);
        std::lock_guard<std::mutex> hold(g_lock);
        if (!g_started) return Result::Unavailable;
        if (detail::Released(self)) return Result::BadArg;
        const Channel* c = FindChannel(cmd.text);
        if (!c || c->owner != self) return Result::NotFound;
        if (len > c->maxLen) return Result::BadArg;
        if (!g_active) return Result::Unavailable;
        const bool hosting = g_state == State::Hosting;
        if (((c->flags & SCO_NET_FROM_HOST) && !hosting) || ((c->flags & SCO_NET_TO_HOST) && hosting))
            return Result::BadArg;
        PluginState& p = Plugin(self);
        const uint64_t now = Now();
        if (now > p.lastMs) {
            const double dt = static_cast<double>(now - p.lastMs);
            p.msgTokens = std::min<double>(g_opts.quotaMsgs, p.msgTokens + dt * g_opts.quotaMsgs / 1000.0);
            p.byteTokens = std::min<double>(g_opts.quotaBytes, p.byteTokens + dt * g_opts.quotaBytes / 1000.0);
            p.lastMs = now;
        }
        if (p.msgTokens < 1.0 || p.byteTokens < static_cast<double>(len) ||
            p.queuedBytes + len > kMaxQueuedBytesPerPlugin) {
            ++g_stats.quotaRefused;
            return Result::TooMany;
        }
        p.msgTokens -= 1.0;
        p.byteTokens -= static_cast<double>(len);
        p.queuedBytes += len;
        QueueCmd(std::move(cmd));
        return Result::Ok;
    });
}

sco_result RegisterChannel(sco_plugin* self, const char* fqn, uint32_t flags, uint32_t maxLen, sco_net_on_message cb,
                           void* ctx) {
    return Guard([&] {
        std::string name;
        if (const Result r = Owned(self, fqn, &name); r != Result::Ok) return r;
        if (!cb || (flags & ~kChannelFlags) || ((flags & kFromHost) && (flags & kToHost))) return Result::BadArg;
        const uint32_t limit = (flags & kReliable) ? SCO_NET_MAX_RELIABLE : SCO_NET_MAX_UNREL;
        if (maxLen == 0 || maxLen > limit) return Result::BadArg;
        Channel c;
        c.owner = self;
        c.fqn = std::move(name);
        c.flags = flags;
        c.maxLen = maxLen;
        c.cb = cb;
        c.ctx = ctx;
        Cmd cmd;
        cmd.kind = CmdKind::Register;
        cmd.text = c.fqn;
        cmd.flags = flags;
        cmd.maxLen = maxLen;
        std::lock_guard<std::mutex> hold(g_lock);
        if (!g_started) return Result::Unavailable;
        if (detail::Released(self)) return Result::BadArg;
        if (FindChannel(c.fqn)) return Result::BadArg;
        const size_t mine = static_cast<size_t>(
            std::count_if(g_channels.begin(), g_channels.end(), [self](const Channel& x) { return x.owner == self; }));
        if (mine >= SCO_NET_MAX_CHANNELS) return Result::TooMany;
        c.id = g_nextChannel++;
        g_channels.push_back(std::move(c));
        QueueCmd(std::move(cmd));
        return Result::Ok;
    });
}

sco_result UnregisterChannel(sco_plugin* self, const char* fqn) {
    return Guard([&] {
        std::string name;
        if (const Result r = Owned(self, fqn, &name); r != Result::Ok) return r;
        Cmd cmd;
        cmd.kind = CmdKind::Unregister;
        cmd.text = name;
        std::lock_guard<std::mutex> hold(g_lock);
        if (!g_started) return Result::Unavailable;
        if (detail::Released(self)) return Result::BadArg;
        for (size_t i = 0; i < g_channels.size(); ++i) {
            if (g_channels[i].owner == self && g_channels[i].fqn == name) {
                g_channels.erase(g_channels.begin() + static_cast<std::ptrdiff_t>(i));
                QueueCmd(std::move(cmd));
                return Result::Ok;
            }
        }
        return Result::NotFound;
    });
}

uint64_t SelfPeer() {
    std::lock_guard<std::mutex> hold(g_lock);
    return g_started && g_active ? g_self : 0;
}

const sco_net_v1 kTable = {
    sizeof(sco_net_v1), 0,
    IsActive, GetPeers, GetPeerName, SendChannel, RegisterChannel, UnregisterChannel, SelfPeer,
};

// ---- lifetime -----------------------------------------------------------------------------------

// The runtime calls this once Release(owner) has removed the owner's items: its channels, its
// queued sends and the messages waiting for it go.
void OnRelease(const void* owner) {
    if (!owner) return;
    std::lock_guard<std::mutex> hold(g_lock);
    for (size_t i = 0; i < g_channels.size();) {
        if (g_channels[i].owner == owner) {
            Cmd cmd;
            cmd.kind = CmdKind::Unregister;
            cmd.text = g_channels[i].fqn;
            try {
                QueueCmd(std::move(cmd));
            } catch (...) {
            }
            g_channels.erase(g_channels.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    g_cmds.erase(std::remove_if(g_cmds.begin(), g_cmds.end(),
                                [owner](const Cmd& c) { return c.kind == CmdKind::Send && c.owner == owner; }),
                 g_cmds.end());
    g_inbox.erase(std::remove_if(g_inbox.begin(), g_inbox.end(), [owner](const Item& it) { return it.owner == owner; }),
                  g_inbox.end());
    g_plugins.erase(std::remove_if(g_plugins.begin(), g_plugins.end(),
                                   [owner](const PluginState& p) { return p.owner == owner; }),
                    g_plugins.end());
}

struct MessageCall {
    sco_net_on_message cb;
    PeerId             from;
    const uint8_t*     data;
    uint32_t           len;
    void*              ctx;
};

void MessageThunk(void* p) {
    const MessageCall* c = static_cast<const MessageCall*>(p);
    c->cb(c->from, c->data, c->len, c->ctx);
}

}  // namespace

Result Start(const ServiceOptions& opts) {
    if (opts.quotaMsgs == 0 || opts.quotaBytes < SCO_NET_MAX_RELIABLE || opts.pollMs == 0 || opts.pollMs > 1000 ||
        opts.pbkdf2Iters == 0)
        return Result::BadArg;
    {
        std::lock_guard<std::mutex> control(g_control);
        if (g_thread.joinable()) g_thread.join();   // a session that ended after the last Stop
        std::lock_guard<std::mutex> hold(g_lock);
        if (g_started) return Result::BadArg;
        try {
            g_opts = opts;
        } catch (...) {
            return Result::TooMany;
        }
        g_started = true;
        g_stats = ServiceStats();
        g_reason.clear();
    }
    Result r = AddReleaseHook(OnRelease);
    if (r == Result::Ok) {
        r = host::ProvideHostService(SCO_NET_NAME, SCO_NET_VERSION_1_0, &kTable);
        if (r == Result::Ok) {
            r = caps::Set(kCapability, true);
            if (r != Result::Ok) host::WithdrawHostService(SCO_NET_NAME);
        }
        if (r != Result::Ok) RemoveReleaseHook(OnRelease);
    }
    if (r != Result::Ok) {
        std::lock_guard<std::mutex> hold(g_lock);
        g_started = false;
    }
    return r;
}

void Stop() {
    std::lock_guard<std::mutex> control(g_control);
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (!g_started) return;
    }
    LeaveLocked("the host stopped");
    {
        std::lock_guard<std::mutex> hold(g_lock);
        g_started = false;
        g_channels.clear();
        g_inbox.clear();
        g_plugins.clear();
    }
    host::WithdrawHostService(SCO_NET_NAME);
    caps::Set(kCapability, false, "stopped");
    RemoveReleaseHook(OnRelease);
}

bool Started() {
    std::lock_guard<std::mutex> hold(g_lock);
    return g_started;
}

void Tick() {
    if (!OnGameThread() || detail::InCallout()) return;
    std::deque<Item> items;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (!g_started) return;
        items.swap(g_inbox);
        for (const Item& it : items) {
            if (it.kind != ItemKind::Message) continue;
            for (auto& p : g_plugins)
                if (p.owner == it.owner) {
                    p.inboxItems -= std::min<size_t>(p.inboxItems, 1);
                    p.inboxBytes -= std::min(p.inboxBytes, it.data.size());
                }
        }
    }
    for (const Item& it : items) {
        switch (it.kind) {
            case ItemKind::Message: {
                MessageCall call{ nullptr, it.from, it.data.data(), static_cast<uint32_t>(it.data.size()), nullptr };
                std::string where;
                {
                    // Looked up per message: a channel unregistered or released since is skipped.
                    std::lock_guard<std::mutex> hold(g_lock);
                    for (const Channel& c : g_channels)
                        if (c.id == it.channel) {
                            call.cb = c.cb;
                            call.ctx = c.ctx;
                            where = c.fqn;
                            break;
                        }
                }
                if (!call.cb || detail::Released(it.owner)) break;
                detail::Callout(it.owner, where.c_str(), MessageThunk, &call);
                break;
            }
            case ItemKind::State: Dispatch(SCO_NET_EVENT_STATE, &it.state); break;
            case ItemKind::Peer:  Dispatch(SCO_NET_EVENT_PEER, &it.peer); break;
        }
    }
}

bool WaitForDelivery(uint32_t timeoutMs) {
    std::unique_lock<std::mutex> hold(g_lock);
    return g_delivery.wait_for(hold, std::chrono::milliseconds(timeoutMs), [] { return !g_inbox.empty(); });
}

void PumpNetwork() {
    std::lock_guard<std::mutex> control(g_control);
    if (!g_session) return;
    if (!Step(*g_session, Now())) {
        Finish(*g_session);
        g_session.reset();
    }
}

Result Host(const HostOptions& o) {
    std::lock_guard<std::mutex> control(g_control);
    if (!Started()) return Result::Unavailable;
    if (o.passphrase.empty() || !ValidName(o.playerName) || o.maxPeers < 2 || o.maxPeers > kMaxPeers) return Result::BadArg;
    if (Busy()) return Result::BadArg;
    auto s = std::make_unique<Session>();
    s->host = true;
    if (o.transport) {
        s->raw = o.transport;
    } else {
        std::string why;
        s->socket = udp::Socket::Open(o.port, o.loopbackOnly, &why);
        if (!s->socket) {
            Log("[net] cannot host on UDP port %u: %s", static_cast<unsigned>(o.port), why.c_str());
            return Result::Failed;
        }
        s->raw = s->socket.get();
        Log("[net] hosting on UDP port %u (%s)", static_cast<unsigned>(s->socket->Port()),
            o.scope.any ? "any address" : o.scope.allow.empty() ? "LAN only" : "LAN and the allow-list");
    }
    s->scoped = std::make_unique<ScopedTransport>(*s->raw, o.scope);
    s->coreOpts.passphrase = o.passphrase;
    s->coreOpts.playerName = o.playerName;
    s->coreOpts.maxPeers = o.maxPeers;
    s->coreOpts.pbkdf2Iters = g_opts.pbkdf2Iters;
    s->admit = o.admit;
    return Begin(std::move(s));
}

Result Join(const JoinOptions& o) {
    std::lock_guard<std::mutex> control(g_control);
    if (!Started()) return Result::Unavailable;
    if (o.passphrase.empty() || !ValidName(o.playerName)) return Result::BadArg;
    Endpoint ep = o.endpoint;
    if (!o.transport && !ParseAddress(o.address, o.port, &ep)) return Result::BadArg;
    if (!InScope(o.scope, ep)) {
        Log("[net] join refused: %s is outside the allowed networks (LAN, or the allow-list)", o.address.c_str());
        return Result::BadArg;
    }
    if (Busy()) return Result::BadArg;
    auto s = std::make_unique<Session>();
    s->host = false;
    s->hostEp = ep;
    if (o.transport) {
        s->raw = o.transport;
    } else {
        std::string why;
        s->socket = udp::Socket::Open(0, o.loopbackOnly, &why);
        if (!s->socket) {
            Log("[net] cannot open a UDP socket: %s", why.c_str());
            return Result::Failed;
        }
        s->raw = s->socket.get();
        Log("[net] joining %s:%u", o.address.c_str(), static_cast<unsigned>(o.port));
    }
    s->scoped = std::make_unique<ScopedTransport>(*s->raw, o.scope);
    s->coreOpts.passphrase = o.passphrase;
    s->coreOpts.playerName = o.playerName;
    s->coreOpts.pbkdf2Iters = g_opts.pbkdf2Iters;
    return Begin(std::move(s));
}

void Leave(const char* reason) {
    std::lock_guard<std::mutex> control(g_control);
    LeaveLocked(reason);
}

State GetState() {
    std::lock_guard<std::mutex> hold(g_lock);
    return g_state;
}

std::string LastReason() {
    std::lock_guard<std::mutex> hold(g_lock);
    return g_reason;
}

uint16_t BoundPort() {
    std::lock_guard<std::mutex> hold(g_lock);
    return g_port;
}

Result SetPeerEntity(PeerId peer, uint64_t entityId) {
    std::lock_guard<std::mutex> hold(g_lock);
    if (!g_started || !g_active) return Result::NotFound;
    const bool known = std::any_of(g_peers.begin(), g_peers.end(), [peer](const PeerInfo& p) { return p.id == peer; });
    if (!known) return Result::NotFound;
    try {
        if (entityId) g_entities[peer] = entityId;
        else g_entities.erase(peer);
        PushPeer(SCO_NET_PEER_ENTITY, peer, entityId);
    } catch (...) {
        return Result::TooMany;
    }
    return Result::Ok;
}

ServiceStats GetServiceStats() {
    std::lock_guard<std::mutex> hold(g_lock);
    return g_stats;
}

const sco_net_v1* Table() { return &kTable; }

}  // namespace sco::net
