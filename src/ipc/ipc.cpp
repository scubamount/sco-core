// sco.ipc: named shared-memory channels behind the C table of sco_ipc.h (sco/ipc.h). The wire
// (header, rings, blocks) is sc_ipc.h; this file adds names, quotas, ownership and lifetime.
//
// Locks: g_lock guards the started flag, the options and the channel list; each channel has its
// own lock, held for one call's work on the mapping, so a plugin's calls on one channel run one at
// a time (a ring has one producer and one consumer per side) and different channels never wait
// for each other. Order: g_lock, then the runtime's owner lock (detail::Released); a channel lock
// is never taken while g_lock is held.
//
// Plugin memory (names, data, output buffers) is read and written only while no lock is held:
// inputs are copied before the channel is locked, outputs after it is unlocked. A bad pointer from
// a plugin then faults with nothing held, and the crash guard's Release(owner) can still close the
// plugin's channels.
#include "sco/ipc.h"
#include "sco/host.h"
#include "sco/log.h"
#include "shm.h"
#include "../api/internal.h"
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace sco::ipc {

namespace {

struct Ring {
    uint64_t         offset = 0;
    sc_ipc_ring_view view{};
};

struct Channel {
    std::mutex   lock;
    const void*  owner = nullptr;
    uint64_t     id = 0;
    std::string  name;           // the channel part: "link"
    std::string  label;          // "<plugin id>.<name>", for the log
    // Under lock:
    bool              closed = false;
    shm::Mapping      map;
    sc_ipc_chan       chan{};
    std::vector<Ring> rings;
    bool              loggedCorrupt = false;
};

std::mutex                            g_lock;
bool                                  g_started = false;
Options                               g_opts;
std::vector<std::shared_ptr<Channel>> g_channels;
uint64_t                              g_nextId = 1;

constexpr int kBlockRetries = 64;   // a torn block read is retried this often, then SCO_FAILED

sco_result C(Result r) { return static_cast<sco_result>(static_cast<uint32_t>(r)); }

// sc_ipc.h results as the service reports them.
Result FromWire(int rc) {
    switch (rc) {
        case SC_IPC_OK:        return Result::Ok;
        case SC_IPC_FULL:      return Result::TooMany;
        case SC_IPC_EMPTY:     return Result::NotFound;
        case SC_IPC_TOO_SMALL: return Result::TooMany;
        case SC_IPC_BAD_ARG:   return Result::BadArg;
        case SC_IPC_NOT_READY: return Result::NotFound;
        case SC_IPC_MISMATCH:  return Result::BadArg;
        default:               return Result::Failed;   // CORRUPT, EPOCH, BUSY, GONE
    }
}

bool ValidName(const char* s, size_t max) {
    if (!s) return false;
    const size_t n = strnlen(s, max + 1);
    if (n == 0 || n > max) return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

template <class F>
sco_result Guard(F&& f) noexcept {
    try {
        return C(f());
    } catch (...) {
        return SCO_TOO_MANY;
    }
}

// The caller's channel, locked for the call. NotFound: no open channel of this plugin by that id.
struct Call {
    std::shared_ptr<Channel>     ch;
    std::unique_lock<std::mutex> hold;
};

Result Enter(sco_plugin* self, uint64_t id, Call& call) {
    if (!host::PluginId(self)) return Result::BadArg;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (!g_started) return Result::Unavailable;
        if (detail::Released(self)) return Result::BadArg;
        for (const auto& c : g_channels)
            if (c->owner == self && c->id == id) { call.ch = c; break; }
    }
    if (!call.ch) return Result::NotFound;
    call.hold = std::unique_lock<std::mutex>(call.ch->lock);
    if (call.ch->closed) return Result::NotFound;
    return Result::Ok;
}

// Logs the first validation failure of a channel: the peer wrote something no correct peer writes.
Result Wire(Channel& c, int rc, const char* what, uint64_t offset) {
    if (rc == SC_IPC_CORRUPT && !c.loggedCorrupt) {
        c.loggedCorrupt = true;
        Log("[ipc] %s: %s at %llu failed validation (the peer wrote an impossible value); refused",
            c.label.c_str(), what, static_cast<unsigned long long>(offset));
    }
    return FromWire(rc);
}

Ring* FindRing(Channel& c, uint64_t offset) {
    for (auto& r : c.rings)
        if (r.offset == offset) return &r;
    return nullptr;
}

void CloseChannel(Channel& c) {
    std::lock_guard<std::mutex> hold(c.lock);
    if (c.closed) return;
    c.closed = true;
    sc_ipc_close(&c.chan);   // the peer sees SC_IPC_GONE now, not only when the heartbeat stops
    shm::Close(c.map);
    c.rings.clear();
    c.chan = sc_ipc_chan{};
}

// ---- the table ----------------------------------------------------------------------------------

sco_result Create(sco_plugin* self, const char* name, uint64_t bytes, uint32_t layoutId, uint32_t layoutVersion,
                  uint64_t* out) {
    return Guard([&] {
        if (!out) return Result::BadArg;
        *out = 0;
        const char* id = host::PluginId(self);
        if (!id) return Result::BadArg;
        if (!ValidName(name, SCO_IPC_MAX_NAME)) return Result::BadArg;
        if (bytes < SCO_IPC_MIN_CHANNEL_BYTES || bytes > SCO_IPC_MAX_CHANNEL_BYTES) return Result::BadArg;
        auto ch = std::make_shared<Channel>();
        ch->owner = self;
        ch->name = name;
        ch->label = std::string(id) + "." + ch->name;
        uint64_t newId = 0;
        {
            std::lock_guard<std::mutex> hold(g_lock);
            if (!g_started) return Result::Unavailable;
            if (detail::Released(self)) return Result::BadArg;
            size_t open = 0;
            uint64_t total = 0;
            for (const auto& c : g_channels) {
                if (c->owner != self) continue;
                if (c->name == ch->name) return Result::BadArg;
                ++open;
                total += c->map.bytes;
            }
            if (open >= SCO_IPC_MAX_CHANNELS || total + bytes > SCO_IPC_MAX_PLUGIN_BYTES) return Result::TooMany;
            std::string why;
            const std::string full = g_opts.prefix + ch->label;
            if (shm::Create(full, bytes, ch->map, why) != Result::Ok) {
                Log("[ipc] %s: cannot create %s: %s", ch->label.c_str(), full.c_str(), why.c_str());
                return Result::Failed;
            }
            const int rc = sc_ipc_init(ch->map.base, bytes, shm::Pid(), layoutId, layoutVersion, shm::NowMs(), &ch->chan);
            if (rc != SC_IPC_OK) {
                shm::Close(ch->map);
                return FromWire(rc);
            }
            ch->id = newId = g_nextId++;
            try {
                g_channels.push_back(ch);
            } catch (...) {
                shm::Close(ch->map);
                throw;
            }
        }
        *out = newId;
        return Result::Ok;
    });
}

sco_result PeerAge(sco_plugin* self, uint64_t channel, uint32_t* out) {
    return Guard([&] {
        if (!out) return Result::BadArg;
        *out = 0;
        uint32_t ms = 0;
        int rc;
        {
            Call call;
            if (const Result r = Enter(self, channel, call); r != Result::Ok) return r;
            rc = sc_ipc_peer_age_ms(&call.ch->chan, shm::NowMs(), &ms);
        }
        if (rc != SC_IPC_OK) return FromWire(rc);
        *out = ms;
        return Result::Ok;
    });
}

sco_result BlockWrite(sco_plugin* self, uint64_t channel, uint64_t offset, const void* data, uint32_t size) {
    return Guard([&] {
        if ((!data && size) || size > SCO_IPC_MAX_CHANNEL_BYTES) return Result::BadArg;
        std::vector<uint8_t> copy(size);
        if (size) std::memcpy(copy.data(), data, size);
        Call call;
        if (const Result r = Enter(self, channel, call); r != Result::Ok) return r;
        return FromWire(sc_ipc_block_write(&call.ch->chan, offset, copy.data(), size));
    });
}

sco_result BlockRead(sco_plugin* self, uint64_t channel, uint64_t offset, void* out, uint32_t size) {
    return Guard([&] {
        if ((!out && size) || size > SCO_IPC_MAX_CHANNEL_BYTES) return Result::BadArg;
        std::vector<uint8_t> copy(size);
        int rc = SC_IPC_BUSY;
        {
            Call call;
            if (const Result r = Enter(self, channel, call); r != Result::Ok) return r;
            for (int i = 0; i < kBlockRetries && rc == SC_IPC_BUSY; ++i)
                rc = sc_ipc_block_read(&call.ch->chan, offset, copy.data(), size);
        }
        if (rc != SC_IPC_OK) return FromWire(rc);
        if (size) std::memcpy(out, copy.data(), size);
        return Result::Ok;
    });
}

sco_result RingInit(sco_plugin* self, uint64_t channel, uint64_t offset, uint64_t capacity, uint32_t direction) {
    return Guard([&] {
        Call call;
        if (const Result r = Enter(self, channel, call); r != Result::Ok) return r;
        Channel& c = *call.ch;
        // [offset, end) must not overlap another ring's region (re-initializing one is fine).
        if (capacity > c.chan.bytes) return Result::BadArg;
        const uint64_t end = offset + SC_IPC_RING_BYTES + capacity;
        if (end < offset) return Result::BadArg;
        Ring* same = FindRing(c, offset);
        for (const auto& r : c.rings) {
            if (&r == same) continue;
            const uint64_t rEnd = r.offset + SC_IPC_RING_BYTES + r.view.capacity;
            if (offset < rEnd && r.offset < end) return Result::BadArg;
        }
        if (!same && c.rings.size() >= SCO_IPC_MAX_RINGS) return Result::TooMany;
        sc_ipc_ring_view view{};
        const int rc = sc_ipc_ring_init(&c.chan, offset, capacity, direction, &view);
        if (rc != SC_IPC_OK) return FromWire(rc);
        if (same) {
            same->view = view;
        } else {
            Ring ring;
            ring.offset = offset;
            ring.view = view;
            c.rings.push_back(ring);
        }
        return Result::Ok;
    });
}

sco_result RingPush(sco_plugin* self, uint64_t channel, uint64_t offset, uint32_t type, const void* data, uint32_t size) {
    return Guard([&] {
        if ((!data && size) || size > SCO_IPC_MAX_CHANNEL_BYTES || type == SC_IPC_REC_PAD) return Result::BadArg;
        std::vector<uint8_t> copy(size);
        if (size) std::memcpy(copy.data(), data, size);
        Call call;
        if (const Result r = Enter(self, channel, call); r != Result::Ok) return r;
        Ring* ring = FindRing(*call.ch, offset);
        if (!ring) return Result::NotFound;
        if (ring->view.direction != SC_IPC_TO_PEER) return Result::BadArg;
        return Wire(*call.ch, sc_ipc_ring_push(&ring->view, type, copy.data(), size), "ring", offset);
    });
}

sco_result RingPop(sco_plugin* self, uint64_t channel, uint64_t offset, uint32_t* outType, void* out, uint32_t* io) {
    return Guard([&] {
        if (!io) return Result::BadArg;
        const uint32_t capacity = *io;
        if (!out && capacity) return Result::BadArg;
        std::vector<uint8_t> copy;
        uint32_t size = 0, type = 0;
        int rc;
        {
            Call call;
            if (const Result r = Enter(self, channel, call); r != Result::Ok) return r;
            Ring* ring = FindRing(*call.ch, offset);
            if (!ring) return Result::NotFound;
            if (ring->view.direction != SC_IPC_FROM_PEER) return Result::BadArg;
            // Never more than the ring can hold, whatever capacity the caller claims.
            size = static_cast<uint32_t>(capacity < ring->view.capacity ? capacity : ring->view.capacity);
            copy.resize(size);
            rc = sc_ipc_ring_pop(&ring->view, &type, size ? copy.data() : nullptr, &size);
            if (rc != SC_IPC_OK && rc != SC_IPC_TOO_SMALL) return Wire(*call.ch, rc, "ring", offset);
        }
        *io = size;
        if (rc == SC_IPC_TOO_SMALL) return Result::TooMany;
        if (size) std::memcpy(out, copy.data(), size);
        if (outType) *outType = type;
        return Result::Ok;
    });
}

sco_result View(sco_plugin* self, uint64_t channel, void** outBase, uint64_t* outBytes) {
    return Guard([&] {
        if (!outBase || !outBytes) return Result::BadArg;
        *outBase = nullptr;
        *outBytes = 0;
        void* base = nullptr;
        uint64_t bytes = 0;
        {
            Call call;
            if (const Result r = Enter(self, channel, call); r != Result::Ok) return r;
            base = call.ch->map.base;
            bytes = call.ch->map.bytes;
        }
        *outBase = base;
        *outBytes = bytes;
        return Result::Ok;
    });
}

sco_result Close(sco_plugin* self, uint64_t channel) {
    return Guard([&] {
        if (!host::PluginId(self)) return Result::BadArg;
        std::shared_ptr<Channel> ch;
        {
            std::lock_guard<std::mutex> hold(g_lock);
            if (!g_started) return Result::Unavailable;
            if (detail::Released(self)) return Result::BadArg;
            for (size_t i = 0; i < g_channels.size(); ++i)
                if (g_channels[i]->owner == self && g_channels[i]->id == channel) {
                    ch = std::move(g_channels[i]);
                    g_channels.erase(g_channels.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
        }
        if (!ch) return Result::NotFound;
        CloseChannel(*ch);
        return Result::Ok;
    });
}

const sco_ipc_v1 kTable = {
    sizeof(sco_ipc_v1), 0,
    Create, PeerAge, BlockWrite, BlockRead, RingInit, RingPush, RingPop, View, Close,
};

// ---- lifetime -----------------------------------------------------------------------------------

std::vector<std::shared_ptr<Channel>> TakeChannels(const void* owner) {
    std::vector<std::shared_ptr<Channel>> out;
    std::lock_guard<std::mutex> hold(g_lock);
    for (size_t i = 0; i < g_channels.size();) {
        if (!owner || g_channels[i]->owner == owner) {
            out.push_back(std::move(g_channels[i]));
            g_channels.erase(g_channels.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    return out;
}

// The runtime calls this once Release(owner) has removed the owner's items.
void OnRelease(const void* owner) {
    if (!owner) return;
    for (const auto& c : TakeChannels(owner)) CloseChannel(*c);
}

}  // namespace

Result Start(const Options& opts) {
    const size_t n = std::strlen(kPrefix);
    if (opts.prefix.compare(0, n, kPrefix) != 0 || opts.prefix.size() > 64) return Result::BadArg;
    for (char c : opts.prefix.substr(n))
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return Result::BadArg;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (g_started) return Result::BadArg;
        try {
            g_opts = opts;
        } catch (...) {
            return Result::TooMany;
        }
        g_started = true;
    }
    Result r = AddReleaseHook(OnRelease);
    if (r == Result::Ok) {
        r = host::ProvideHostService(SCO_IPC_NAME, SCO_IPC_VERSION_1_0, &kTable);
        if (r != Result::Ok) RemoveReleaseHook(OnRelease);
    }
    if (r != Result::Ok) {
        std::lock_guard<std::mutex> hold(g_lock);
        g_started = false;
    }
    return r;
}

void Stop() {
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (!g_started) return;
        g_started = false;
    }
    host::WithdrawHostService(SCO_IPC_NAME);
    for (const auto& c : TakeChannels(nullptr)) CloseChannel(*c);
    RemoveReleaseHook(OnRelease);
}

bool Started() {
    std::lock_guard<std::mutex> hold(g_lock);
    return g_started;
}

void Tick() {
    std::vector<std::shared_ptr<Channel>> list;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (!g_started) return;
        list = g_channels;
    }
    const uint64_t now = shm::NowMs();
    for (const auto& c : list) {
        std::lock_guard<std::mutex> hold(c->lock);
        if (!c->closed) sc_ipc_owner_beat(&c->chan, now);
    }
}

const sco_ipc_v1* Table() { return &kTable; }

std::string MappingName(const char* pluginId, const char* channel) {
    if (!ValidName(pluginId, host::kMaxIdLen) || !ValidName(channel, SCO_IPC_MAX_NAME)) return {};
    std::lock_guard<std::mutex> hold(g_lock);
    if (!g_started) return {};
    return g_opts.prefix + pluginId + "." + channel;
}

}  // namespace sco::ipc
