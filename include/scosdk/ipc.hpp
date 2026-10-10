// scosdk/ipc.hpp: the host service "sco.ipc" (sco_ipc.h) for C++ plugins. Header-only, over
// sco_api.h and sco_ipc.h (which includes the MIT wire sc_ipc.h).
//
//   sco::sdk::Ipc ipc;
//   if (ipc.Open(*this) == SCO_OK) {
//       sco::sdk::IpcChannel link = ipc.Create("link", 1u << 20, kLayoutId, 1);  // Local\SCO_<id>.link
//       if (link) {
//           link.InitRing(4096, 65536, SCO_IPC_TO_PEER);
//           link.InitRing(4096 + 65536 + 192, 65536, SCO_IPC_FROM_PEER);
//           link.Push(4096, kMsgHello, hello);                     // a trivially copyable struct
//           link.Write(1 << 19, pose);                             // a seqlock snapshot block
//           std::vector<std::byte> msg; uint32_t type = 0;
//           while (link.Pop(4096 + 65536 + 192, type, msg) == SCO_OK) Handle(type, msg);
//       }
//   }                                                              // ~IpcChannel closes it
//
// Every call answers sco_result and is noexcept (out of memory is SCO_TOO_MANY). The service is
// host-owned, so an Ipc may be kept for the plugin's life. Reference: docs/ipc.md,
// docs/sdk-cpp.md. GPL-3.0, like sco-core.
#ifndef SCOSDK_IPC_HPP
#define SCOSDK_IPC_HPP

#include "plugin.hpp"
#include "sco_ipc.h"

#include <cstddef>
#include <span>
#include <type_traits>
#include <vector>

namespace sco::sdk {

class Ipc;

// One open channel. Move-only; closes the channel on destruction. Empty (Result() says why)
// when Create failed.
class IpcChannel {
public:
    IpcChannel() noexcept = default;
    IpcChannel(IpcChannel&& o) noexcept { *this = std::move(o); }
    IpcChannel& operator=(IpcChannel&& o) noexcept {
        if (this != &o) {
            Close();
            t_ = o.t_; self_ = o.self_; id_ = o.id_; r_ = o.r_;
            o.t_ = nullptr; o.id_ = 0;
        }
        return *this;
    }
    IpcChannel(const IpcChannel&) = delete;
    IpcChannel& operator=(const IpcChannel&) = delete;
    ~IpcChannel() { Close(); }

    explicit operator bool() const noexcept { return t_ != nullptr; }
    sco_result Result() const noexcept { return r_; }
    uint64_t Id() const noexcept { return id_; }

    // ---- seqlock blocks ----

    sco_result Write(uint64_t offset, std::span<const std::byte> data) noexcept {
        if (!t_) return r_;
        if (data.size() > SCO_IPC_MAX_CHANNEL_BYTES) return SCO_BAD_ARG;
        return t_->block_write(self_, id_, offset, data.data(), static_cast<uint32_t>(data.size()));
    }
    template <class T>
        requires(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>)
    sco_result Write(uint64_t offset, const T& value) noexcept {
        return Write(offset, std::span<const std::byte>(std::as_bytes(std::span<const T, 1>(&value, 1))));
    }
    // Exactly data.size() bytes. SCO_FAILED: torn by the writer; try again later.
    sco_result Read(uint64_t offset, std::span<std::byte> data) const noexcept {
        if (!t_) return r_;
        if (data.size() > SCO_IPC_MAX_CHANNEL_BYTES) return SCO_BAD_ARG;
        return t_->block_read(self_, id_, offset, data.data(), static_cast<uint32_t>(data.size()));
    }
    template <class T>
        requires(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>)
    sco_result Read(uint64_t offset, T& out) const noexcept {
        T got{};
        const sco_result r = Read(offset, std::span<std::byte>(std::as_writable_bytes(std::span<T, 1>(&got, 1))));
        if (r == SCO_OK) out = got;
        return r;
    }

    // ---- rings ----

    sco_result InitRing(uint64_t offset, uint64_t capacity, uint32_t direction) noexcept {
        return t_ ? t_->ring_init(self_, id_, offset, capacity, direction) : r_;
    }
    sco_result Push(uint64_t offset, uint32_t type, std::span<const std::byte> data) noexcept {
        if (!t_) return r_;
        if (data.size() > SCO_IPC_MAX_CHANNEL_BYTES) return SCO_BAD_ARG;
        return t_->ring_push(self_, id_, offset, type, data.data(), static_cast<uint32_t>(data.size()));
    }
    template <class T>
        requires(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>)
    sco_result Push(uint64_t offset, uint32_t type, const T& value) noexcept {
        return Push(offset, type, std::span<const std::byte>(std::as_bytes(std::span<const T, 1>(&value, 1))));
    }
    // The oldest record of a FROM_PEER ring, any size (the handshake is done here).
    // SCO_NOT_FOUND: nothing queued.
    sco_result Pop(uint64_t offset, uint32_t& type, std::vector<std::byte>& out) noexcept {
        if (!t_) return r_;
        try {
            for (int attempt = 0; attempt < 2; ++attempt) {
                uint32_t size = static_cast<uint32_t>(out.capacity() < SCO_IPC_MAX_CHANNEL_BYTES ? out.capacity()
                                                                                               : SCO_IPC_MAX_CHANNEL_BYTES);
                out.resize(size);
                const sco_result r = t_->ring_pop(self_, id_, offset, &type, size ? out.data() : nullptr, &size);
                if (r == SCO_OK) { out.resize(size); return SCO_OK; }
                if (r != SCO_TOO_MANY) { out.clear(); return r; }
                out.reserve(size);   // the record stays queued: take it with a buffer that fits
            }
        } catch (...) {
            return SCO_TOO_MANY;
        }
        return SCO_FAILED;
    }

    // ---- the rest ----

    // Milliseconds since the peer's last beat. SCO_NOT_FOUND: it hasn't beaten yet.
    sco_result PeerAgeMs(uint32_t& ms) const noexcept { return t_ ? t_->peer_age_ms(self_, id_, &ms) : r_; }
    // The plugin's own mapping (header included), for bulk regions; empty on error. Valid until
    // Close or unload.
    std::span<std::byte> View() const noexcept {
        void* base = nullptr;
        uint64_t bytes = 0;
        if (!t_ || t_->view(self_, id_, &base, &bytes) != SCO_OK) return {};
        return { static_cast<std::byte*>(base), static_cast<size_t>(bytes) };
    }
    void Close() noexcept {
        if (t_) t_->close(self_, id_);
        t_ = nullptr;
        id_ = 0;
    }

private:
    friend class Ipc;
    const sco_ipc_v1* t_ = nullptr;
    sco_plugin*       self_ = nullptr;
    uint64_t          id_ = 0;
    sco_result        r_ = SCO_UNAVAILABLE;
};

class Ipc {
public:
    // Finds sco.ipc 1.x. SCO_UNAVAILABLE on a 1.0 host; SCO_NOT_FOUND on a host without it.
    sco_result Open(const Plugin& plugin) noexcept { return Open(plugin.Api(), plugin.Self()); }
    sco_result Open(const sco_api* api, sco_plugin* self) noexcept {
        t_ = nullptr;
        self_ = self;
        if (!api || !self_) return SCO_BAD_ARG;
        if (!Covers(api->size, offsetof(sco_api, query_service))) return SCO_UNAVAILABLE;
        const void* table = nullptr;
        const sco_result r = api->query_service(SCO_IPC_NAME, SCO_IPC_VERSION_1_0, &table);
        if (r != SCO_OK) return r;
        t_ = static_cast<const sco_ipc_v1*>(table);
        return SCO_OK;
    }
    explicit operator bool() const noexcept { return t_ != nullptr; }
    const sco_ipc_v1* Table() const noexcept { return t_; }

    // Creates Local\SCO_<plugin id>.<name> (see sco_ipc.h create).
    IpcChannel Create(const char* name, uint64_t bytes, uint32_t layoutId, uint32_t layoutVersion) const noexcept {
        IpcChannel c;
        c.self_ = self_;
        if (!t_) return c;
        uint64_t id = 0;
        c.r_ = t_->create(self_, name, bytes, layoutId, layoutVersion, &id);
        if (c.r_ == SCO_OK) {
            c.t_ = t_;
            c.id_ = id;
        }
        return c;
    }

private:
    const sco_ipc_v1* t_ = nullptr;
    sco_plugin*       self_ = nullptr;
};

}  // namespace sco::sdk

#endif  // SCOSDK_IPC_HPP
