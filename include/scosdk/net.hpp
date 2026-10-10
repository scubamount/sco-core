// scosdk/net.hpp: the host service "sco.net" (sco_net.h) for C++ plugins. Header-only, over
// sco_api.h and sco_net.h.
//
//   sco::sdk::Net net;
//   if (net.Open(*this) == SCO_OK) {
//       pose_ = net.Channel("myplugin.pose", 0, sizeof(Pose),               // unreliable
//                           [this](uint64_t from, std::span<const std::byte> b) { OnPose(from, b); });
//       ...
//       if (net.Active()) net.Send("myplugin.pose", pose);                  // a trivially copyable struct
//       for (const sco_net_peer& p : net.Peers()) Info("%s", net.PeerName(p.peer_id).c_str());
//   }                                                                        // ~NetChannel unregisters
//
// Callbacks run on the game thread from the host tick. A plugin registers and sends only under
// its own id ("<plugin id>.<name>"); sends are rate-limited per plugin (SCO_TOO_MANY when over).
// Sessions are opened by the product, never by a plugin. Every call answers sco_result (or a
// plain value) and is noexcept (out of memory is SCO_TOO_MANY). The service is host-owned, so a
// Net may be kept for the plugin's life. Reference: docs/net.md, docs/sdk-cpp.md. GPL-3.0, like
// sco-core.
#ifndef SCOSDK_NET_HPP
#define SCOSDK_NET_HPP

#include "plugin.hpp"
#include "sco_net.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace sco::sdk {

class Net;

// One registered channel with a C++ callable. Move-only; unregisters on destruction. Empty
// (Result() says why) when registration failed.
class NetChannel {
public:
    using Fn = std::function<void(uint64_t from, std::span<const std::byte> data)>;

    NetChannel() noexcept = default;
    NetChannel(NetChannel&& o) noexcept { *this = std::move(o); }
    NetChannel& operator=(NetChannel&& o) noexcept {
        if (this != &o) {
            Close();
            t_ = o.t_; self_ = o.self_; name_ = std::move(o.name_); fn_ = std::move(o.fn_); r_ = o.r_;
            o.t_ = nullptr;
        }
        return *this;
    }
    NetChannel(const NetChannel&) = delete;
    NetChannel& operator=(const NetChannel&) = delete;
    ~NetChannel() { Close(); }

    explicit operator bool() const noexcept { return t_ != nullptr; }
    sco_result Result() const noexcept { return r_; }
    const std::string& Name() const noexcept { return name_; }

    sco_result Send(std::span<const std::byte> data) const noexcept {
        if (!t_) return r_;
        if (data.size() > SCO_NET_MAX_RELIABLE) return SCO_BAD_ARG;
        return t_->send_channel(self_, name_.c_str(), data.data(), static_cast<uint32_t>(data.size()));
    }
    template <class T>
        requires(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>)
    sco_result Send(const T& value) const noexcept {
        return Send(std::span<const std::byte>(std::as_bytes(std::span<const T, 1>(&value, 1))));
    }

    // Unregisters (on the game thread, no callback runs after this returns).
    void Close() noexcept {
        if (t_) t_->unregister_channel(self_, name_.c_str());
        t_ = nullptr;
        fn_.reset();
    }

private:
    friend class Net;
    static void Thunk(uint64_t from, const void* buf, uint32_t len, void* ctx) {
        (*static_cast<Fn*>(ctx))(from, std::span<const std::byte>(static_cast<const std::byte*>(buf), len));
    }
    const sco_net_v1*   t_ = nullptr;
    sco_plugin*         self_ = nullptr;
    std::string         name_;
    std::unique_ptr<Fn> fn_;   // the callback's ctx: its address never moves with the NetChannel
    sco_result          r_ = SCO_UNAVAILABLE;
};

class Net {
public:
    // Finds sco.net 1.x. SCO_UNAVAILABLE on a 1.0 host; SCO_NOT_FOUND on a host without it.
    sco_result Open(const Plugin& plugin) noexcept { return Open(plugin.Api(), plugin.Self()); }
    sco_result Open(const sco_api* api, sco_plugin* self) noexcept {
        t_ = nullptr;
        self_ = self;
        if (!api || !self_) return SCO_BAD_ARG;
        if (!Covers(api->size, offsetof(sco_api, query_service))) return SCO_UNAVAILABLE;
        const void* table = nullptr;
        const sco_result r = api->query_service(SCO_NET_NAME, SCO_NET_VERSION_1_0, &table);
        if (r != SCO_OK) return r;
        t_ = static_cast<const sco_net_v1*>(table);
        return SCO_OK;
    }
    explicit operator bool() const noexcept { return t_ != nullptr; }
    const sco_net_v1* Table() const noexcept { return t_; }

    bool Active() const noexcept { return t_ && t_->is_active(); }
    uint64_t SelfPeer() const noexcept { return t_ ? t_->self_peer() : 0; }

    // Self first; empty with no session.
    std::vector<sco_net_peer> Peers() const noexcept {
        std::vector<sco_net_peer> out;
        if (!t_) return out;
        try {
            for (int attempt = 0; attempt < 4; ++attempt) {
                uint32_t n = static_cast<uint32_t>(out.size());
                const sco_result r = t_->get_peers(n ? out.data() : nullptr, &n);
                if (r == SCO_OK) { out.resize(n); return out; }
                if (r != SCO_TOO_MANY) break;
                out.resize(n);   // someone joined since: ask again with room for them
            }
        } catch (...) {
        }
        out.clear();
        return out;
    }
    // The peer's chosen name; empty for an unknown peer.
    std::string PeerName(uint64_t peer) const noexcept {
        char buf[SCO_NET_MAX_NAME + 1];
        if (!t_ || t_->get_peer_name(peer, buf, sizeof buf) != SCO_OK) return {};
        try {
            return buf;
        } catch (...) {
            return {};
        }
    }

    // Registers name ("<plugin id>.<channel>") with flags (SCO_NET_*) and max_len; fn runs on the
    // game thread for each message.
    NetChannel Channel(const char* name, uint32_t flags, uint32_t maxLen, NetChannel::Fn fn) const noexcept {
        NetChannel c;
        c.self_ = self_;
        if (!t_) return c;
        if (!name || !fn) { c.r_ = SCO_BAD_ARG; return c; }
        try {
            c.name_ = name;
            c.fn_ = std::make_unique<NetChannel::Fn>(std::move(fn));
        } catch (...) {
            c.r_ = SCO_TOO_MANY;
            return c;
        }
        c.r_ = t_->register_channel(self_, name, flags, maxLen, &NetChannel::Thunk, c.fn_.get());
        if (c.r_ == SCO_OK) c.t_ = t_;
        else c.fn_.reset();
        return c;
    }

    // Sends on one of this plugin's channels by name.
    sco_result Send(const char* name, std::span<const std::byte> data) const noexcept {
        if (!t_) return SCO_UNAVAILABLE;
        if (data.size() > SCO_NET_MAX_RELIABLE) return SCO_BAD_ARG;
        return t_->send_channel(self_, name, data.data(), static_cast<uint32_t>(data.size()));
    }
    template <class T>
        requires(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>)
    sco_result Send(const char* name, const T& value) const noexcept {
        return Send(name, std::span<const std::byte>(std::as_bytes(std::span<const T, 1>(&value, 1))));
    }

private:
    const sco_net_v1* t_ = nullptr;
    sco_plugin*       self_ = nullptr;
};

}  // namespace sco::sdk

#endif  // SCOSDK_NET_HPP
