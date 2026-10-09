// scosdk/raw.hpp: raw handlers (sco_api 1.1) for C++ plugins. Header-only, over sco_api.h.
//
// Typed: In and Out are trivially copyable structs whose layout is the handler's contract (put a
// size or version field first if they will grow).
//
//   struct pose_query { uint32_t entity; };
//   struct pose { double x, y, z; };
//   sco::sdk::RegisterRaw<pose_query, pose>(*this, "nav.pose", "teleport",
//       [](const pose_query& q, pose& out) { out = Lookup(q.entity); return SCO_OK; });
//
//   pose p{};                                                // in another plugin, game thread
//   sco_result r = sco::sdk::InvokeRaw(*this, "nav.pose", pose_query{ 7 }, p);
//
// The SDK does the size handshake of docs/api-v1.md (Raw handlers): input must be exactly
// sizeof(In) (else SCO_BAD_ARG, the handler isn't called), and a caller whose buffer is smaller
// than sizeof(Out) gets SCO_TOO_MANY with the size needed, so does a caller that passes no output
// buffer (the host gives the handler the same empty buffer either way: docs/api-v1.md asks the
// size that way). RegisterRaw<void, Out> takes no input.
// For variable-size data use the byte-span forms. Handlers run on the game thread; an exception
// is caught, logged and answered with SCO_FAILED and no output. Reference: docs/sdk-cpp.md.
// GPL-3.0, like sco-core.
#ifndef SCOSDK_RAW_HPP
#define SCOSDK_RAW_HPP

#include "plugin.hpp"

#include <cstring>
#include <span>

namespace sco::sdk {

// A byte handler: in is the caller's input; out is the caller's buffer (empty when it only asks
// the size or wants no output). Write at most out.size() bytes and set written to the count, or
// set written to the size you need and return SCO_TOO_MANY.
using RawBytesFn = std::function<sco_result(std::span<const std::byte> in, std::span<std::byte> out, uint32_t& written)>;

namespace detail {

struct RawNode : Node {
    std::string name;
    RawBytesFn  fn;
};

inline sco_result RawTrampoline(const void* in, uint32_t inSize, void* out, uint32_t* outSize, void* ctx) noexcept {
    const std::shared_ptr<Node> n = Nodes().Find(FromCtx(ctx));
    if (!n) {
        if (outSize) *outSize = 0;
        return SCO_NOT_FOUND;   // the plugin is unloading
    }
    RawNode* raw = static_cast<RawNode*>(n.get());
    const std::span<const std::byte> input(static_cast<const std::byte*>(in), in ? inSize : 0);
    const uint32_t capacity = outSize && out ? *outSize : 0;
    const std::span<std::byte> output(static_cast<std::byte*>(out), capacity);
    uint32_t written = 0;
    sco_result r = SCO_FAILED;
    try {
        r = raw->fn(input, output, written);
    } catch (...) {
        ReportException(raw->plugin, raw->name.c_str(), std::current_exception());
        r = SCO_FAILED;
        written = 0;
    }
    if (outSize) *outSize = written;
    return r;
}

template <class In, class Out>
struct RawHandlerOf {
    using type = std::function<sco_result(const In&, Out&)>;
};
template <class Out>
struct RawHandlerOf<void, Out> {
    using type = std::function<sco_result(Out&)>;
};

template <class T>
constexpr void CheckRawType() noexcept {
    static_assert(std::is_trivially_copyable_v<T>, "raw In/Out types must be trivially copyable");
    static_assert(std::is_default_constructible_v<T>, "raw In/Out types must be default constructible");
}

}  // namespace detail

namespace detail {

inline sco_result AddRaw(Plugin& plugin, const char* name, const char* capability, RawBytesFn fn) noexcept {
    if (!plugin.Api() || !name || !fn) return SCO_BAD_ARG;
    if (!plugin.ApiCovers(offsetof(sco_api, register_raw))) return SCO_UNAVAILABLE;
    std::shared_ptr<RawNode> node;
    try {
        node = std::make_shared<RawNode>();
        node->name = name;
        node->fn = std::move(fn);
    } catch (...) {
        return SCO_TOO_MANY;
    }
    uint64_t id = 0;
    if (const sco_result added = detail::AddNode(detail::Access::StateOf(plugin), node, id); added != SCO_OK)
        return added;
    const sco_result r =
        plugin.Api()->register_raw(plugin.Self(), name, capability, detail::RawTrampoline, detail::ToCtx(id));
    if (r != SCO_OK) detail::Nodes().Remove(id);
    return r;
}

}  // namespace detail

// Registers a byte handler under name ("<plugin id>.<name>"), gated on capability (nullptr:
// none). The handler lives until the plugin unloads. SCO_UNAVAILABLE on a 1.0 host; else the
// host's answer (SCO_BAD_ARG: a bad or taken name). Any thread.
inline sco_result RegisterRawBytes(Plugin& plugin, const char* name, const char* capability, RawBytesFn fn) noexcept {
    return detail::AddRaw(plugin, name, capability, std::move(fn));
}

// Registers a typed handler: fn(const In&, Out&) (or fn(Out&) for In = void). See the file
// comment for the size checks.
template <class In, class Out>
sco_result RegisterRaw(Plugin& plugin, const char* name, const char* capability,
                       typename detail::RawHandlerOf<In, Out>::type fn) noexcept {
    detail::CheckRawType<Out>();
    if constexpr (!std::is_void_v<In>) detail::CheckRawType<In>();
    if (!fn) return SCO_BAD_ARG;
    RawBytesFn bytes;
    try {
        bytes = [f = std::move(fn)](std::span<const std::byte> in, std::span<std::byte> out, uint32_t& written) {
            if constexpr (std::is_void_v<In>) {
                if (!in.empty()) return SCO_BAD_ARG;
            } else {
                if (in.size() != sizeof(In)) return SCO_BAD_ARG;
            }
            if (out.size() < sizeof(Out)) {
                written = static_cast<uint32_t>(sizeof(Out));
                return SCO_TOO_MANY;
            }
            Out result{};
            sco_result r = SCO_FAILED;
            if constexpr (std::is_void_v<In>) {
                r = f(result);
            } else {
                In input{};
                std::memcpy(&input, in.data(), sizeof(In));
                r = f(input, result);
            }
            if (r == SCO_OK) {
                std::memcpy(out.data(), &result, sizeof(Out));
                written = static_cast<uint32_t>(sizeof(Out));
            }
            return r;
        };
    } catch (...) {
        return SCO_TOO_MANY;
    }
    return detail::AddRaw(plugin, name, capability, std::move(bytes));
}

// Calls a raw handler now, with bytes. Game thread only (SCO_WRONG_THREAD elsewhere). written
// gets the bytes written, or with SCO_TOO_MANY the size needed (pass an empty out to ask it).
inline sco_result InvokeRawBytes(Plugin& plugin, const char* name, std::span<const std::byte> in,
                                 std::span<std::byte> out, uint32_t& written) noexcept {
    written = 0;
    if (!plugin.Api()) return SCO_BAD_ARG;
    if (!plugin.ApiCovers(offsetof(sco_api, invoke_raw))) return SCO_UNAVAILABLE;
    if (in.size() > UINT32_MAX || out.size() > UINT32_MAX) return SCO_BAD_ARG;
    uint32_t size = static_cast<uint32_t>(out.size());
    const sco_result r = plugin.Api()->invoke_raw(plugin.Self(), name, in.empty() ? nullptr : in.data(),
                                                  static_cast<uint32_t>(in.size()),
                                                  out.empty() ? nullptr : out.data(), &size);
    written = size;
    return r;
}

namespace detail {

template <class Out>
sco_result FinishTyped(sco_result r, uint32_t written, const Out& got, Out& out, uint32_t* size) noexcept {
    if (size) *size = written;
    if (r != SCO_OK) return r;
    if (written != sizeof(Out)) return SCO_BAD_ARG;   // the handler's Out isn't this Out
    out = got;
    return SCO_OK;
}

}  // namespace detail

// Calls a typed raw handler now, on the game thread. out is written only on SCO_OK. SCO_BAD_ARG
// also when the handler wrote a size other than sizeof(Out). size (optional) gets the bytes the
// handler wrote, or with SCO_TOO_MANY the size it needs.
template <class In, class Out>
sco_result InvokeRaw(Plugin& plugin, const char* name, const In& in, Out& out, uint32_t* size = nullptr) noexcept {
    detail::CheckRawType<In>();
    detail::CheckRawType<Out>();
    Out got{};
    uint32_t written = 0;
    const sco_result r =
        InvokeRawBytes(plugin, name, std::as_bytes(std::span<const In, 1>(&in, 1)),
                       std::as_writable_bytes(std::span<Out, 1>(&got, 1)), written);
    return detail::FinishTyped(r, written, got, out, size);
}

// The same with no input (a handler registered as RegisterRaw<void, Out>).
template <class Out>
sco_result InvokeRaw(Plugin& plugin, const char* name, Out& out) noexcept {
    detail::CheckRawType<Out>();
    Out got{};
    uint32_t written = 0;
    const sco_result r =
        InvokeRawBytes(plugin, name, {}, std::as_writable_bytes(std::span<Out, 1>(&got, 1)), written);
    return detail::FinishTyped(r, written, got, out, static_cast<uint32_t*>(nullptr));
}

}  // namespace sco::sdk

#endif  // SCOSDK_RAW_HPP
