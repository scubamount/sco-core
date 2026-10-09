// scosdk/command.hpp: commands for C++ plugins. Header-only, over sco_api.h.
//
//   sco::sdk::CommandBuilder(*this, "hello.wave")
//       .Title("Wave")
//       .Help("Says hello on the status line")
//       .Arg<const char*>("name", "Who to wave at")
//       .Handle([](const sco::sdk::Args& args, sco::sdk::Reply& reply) {
//           reply.Printf("Hello, %s", args.String(0));
//           return SCO_OK;
//       })
//       .Register();
//
// The handler runs on the game thread. An exception it throws is caught, logged and answered
// with SCO_FAILED and "<name> failed: <what>" as the reply. Reference: docs/sdk-cpp.md.
// GPL-3.0, like sco-core.
#ifndef SCOSDK_COMMAND_HPP
#define SCOSDK_COMMAND_HPP

#include "plugin.hpp"

namespace sco::sdk {

// A typed view of a command's arguments, valid during the handler. The host has already checked
// the count and types against the command's Arg<T> list; an index out of range or of another
// type gives the fallback.
class Args {
public:
    Args(const sco_arg* args, uint32_t count) noexcept : args_(args), count_(args ? count : 0) {}

    uint32_t Count() const noexcept { return count_; }
    // The argument's sco_arg_type, or SCO_ARG_FORCE32 out of range.
    sco_arg_type Type(uint32_t i) const noexcept {
        return i < count_ ? static_cast<sco_arg_type>(args_[i].type) : SCO_ARG_FORCE32;
    }
    int64_t Int(uint32_t i, int64_t fallback = 0) const noexcept {
        return Type(i) == SCO_ARG_INT ? args_[i].v.i : fallback;
    }
    double Float(uint32_t i, double fallback = 0.0) const noexcept {
        return Type(i) == SCO_ARG_FLOAT ? args_[i].v.f : fallback;
    }
    const char* String(uint32_t i, const char* fallback = "") const noexcept {
        return Type(i) == SCO_ARG_STRING && args_[i].v.s ? args_[i].v.s : fallback;
    }
    bool Bool(uint32_t i, bool fallback = false) const noexcept {
        return Type(i) == SCO_ARG_BOOL ? args_[i].v.i != 0 : fallback;
    }
    const sco_arg* Raw() const noexcept { return args_; }

private:
    const sco_arg* args_;
    uint32_t       count_;
};

// The command's reply: a short message for the player, at most the host's buffer (256 bytes,
// NUL included); longer is cut.
class Reply {
public:
    Reply(char* buffer, uint32_t size) noexcept : buf_(size ? buffer : nullptr), size_(buffer ? size : 0) {
        if (buf_) buf_[0] = '\0';
    }
    void Set(const char* text) noexcept {
        if (!buf_) return;
        std::snprintf(buf_, size_, "%s", text ? text : "");
    }
    void Printf(const char* fmt, ...) noexcept SCOSDK_PRINTF(2, 3);
    const char* Text() const noexcept { return buf_ ? buf_ : ""; }

private:
    char*    buf_;
    uint32_t size_;
};

inline void Reply::Printf(const char* fmt, ...) noexcept {
    if (!buf_ || !fmt) return;
    va_list ap;
    va_start(ap, fmt);
    if (std::vsnprintf(buf_, size_, fmt, ap) < 0) buf_[0] = '\0';
    va_end(ap);
    buf_[size_ - 1] = '\0';
}

using CommandFn = std::function<sco_result(const Args&, Reply&)>;

namespace detail {

template <class T>
struct ArgTypeOf {
    static_assert(sizeof(T) == 0, "Arg<T>: T must be int64_t, double, const char* or bool");
};
template <>
struct ArgTypeOf<int64_t> { static constexpr sco_arg_type value = SCO_ARG_INT; };
template <>
struct ArgTypeOf<double> { static constexpr sco_arg_type value = SCO_ARG_FLOAT; };
template <>
struct ArgTypeOf<const char*> { static constexpr sco_arg_type value = SCO_ARG_STRING; };
template <>
struct ArgTypeOf<bool> { static constexpr sco_arg_type value = SCO_ARG_BOOL; };

struct ArgSpec {
    std::string  name;
    std::string  help;
    bool         hasHelp = false;
    sco_arg_type type = SCO_ARG_INT;
};

// What a registered command keeps for the plugin's life: its strings, arg defs and handler.
struct CommandNode : Node {
    std::string              name, title, help, capability;
    bool                     hasHelp = false, hasCapability = false;
    std::vector<ArgSpec>     specs;
    std::vector<sco_arg_def> defs;
    CommandFn                fn;
};

inline sco_result CommandTrampoline(const sco_arg* args, uint32_t nargs, void* ctx, char* reply,
                                    uint32_t replySize) noexcept {
    const std::shared_ptr<Node> n = Nodes().Find(FromCtx(ctx));
    Reply out(reply, replySize);
    if (!n) return SCO_NOT_FOUND;   // the plugin is unloading
    CommandNode* c = static_cast<CommandNode*>(n.get());
    const Args in(args, nargs);
    try {
        return c->fn(in, out);
    } catch (...) {
        const std::exception_ptr e = std::current_exception();
        const char* what = "unknown exception";
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& x) {
            out.Printf("%s failed: %s", c->name.c_str(), x.what());
            ReportException(c->plugin, c->name.c_str(), e);
            return SCO_FAILED;
        } catch (...) {
        }
        out.Printf("%s failed: %s", c->name.c_str(), what);
        ReportException(c->plugin, c->name.c_str(), e);
        return SCO_FAILED;
    }
}

}  // namespace detail

// Builds and registers one command for plugin. Methods chain; an allocation failure along the
// way makes Register answer SCO_TOO_MANY. name is "<plugin id>.<action>". The title defaults to
// the name; help and capability default to none.
class CommandBuilder {
public:
    CommandBuilder(Plugin& plugin, const char* name) noexcept : plugin_(plugin) {
        try {
            node_ = std::make_shared<detail::CommandNode>();
            if (name) {
                node_->name = name;
                hasName_ = true;
            }
        } catch (...) {
            failed_ = true;
        }
    }

    CommandBuilder& Title(const char* title) noexcept { return SetText(&detail::CommandNode::title, nullptr, title); }
    CommandBuilder& Help(const char* help) noexcept {
        return SetText(&detail::CommandNode::help, &detail::CommandNode::hasHelp, help);
    }
    // The has() name the command needs; the host answers SCO_UNAVAILABLE without calling the
    // handler while it is missing.
    CommandBuilder& Capability(const char* capability) noexcept {
        return SetText(&detail::CommandNode::capability, &detail::CommandNode::hasCapability, capability);
    }
    // Adds an argument: T is int64_t, double, const char* or bool. At most 16.
    template <class T>
    CommandBuilder& Arg(const char* name, const char* help = nullptr) noexcept {
        if (failed_) return *this;
        if (!name) {
            badArg_ = true;
            return *this;
        }
        try {
            detail::ArgSpec spec;
            spec.name = name;
            if (help) {
                spec.help = help;
                spec.hasHelp = true;
            }
            spec.type = detail::ArgTypeOf<T>::value;
            node_->specs.push_back(std::move(spec));
        } catch (...) {
            failed_ = true;
        }
        return *this;
    }
    CommandBuilder& Handle(CommandFn fn) noexcept {
        if (!failed_) node_->fn = std::move(fn);
        return *this;
    }

    // Registers the command. The strings and the handler stay alive until the plugin unloads.
    // SCO_BAD_ARG: no name or handler, or the host refused it (a bad or taken name, a string too
    // long; see docs/api-v1.md). Call once.
    sco_result Register() noexcept {
        if (failed_) return SCO_TOO_MANY;
        if (!hasName_ || badArg_ || !node_->fn || !plugin_.Api()) return SCO_BAD_ARG;
        detail::CommandNode& c = *node_;
        try {
            c.defs.reserve(c.specs.size());
            for (const detail::ArgSpec& s : c.specs) {
                sco_arg_def d{};
                d.name = s.name.c_str();
                d.type = s.type;
                d.help = s.hasHelp ? s.help.c_str() : nullptr;
                c.defs.push_back(d);
            }
        } catch (...) {
            return SCO_TOO_MANY;
        }
        sco_command cmd{};
        cmd.size = sizeof(sco_command);
        cmd.name = c.name.c_str();
        cmd.title = c.title.empty() ? c.name.c_str() : c.title.c_str();
        cmd.help = c.hasHelp ? c.help.c_str() : nullptr;
        cmd.capability = c.hasCapability ? c.capability.c_str() : nullptr;
        cmd.args = c.defs.empty() ? nullptr : c.defs.data();
        cmd.nargs = static_cast<uint32_t>(c.defs.size());
        cmd.arg_def_size = sizeof(sco_arg_def);
        cmd.fn = detail::CommandTrampoline;
        uint64_t id = 0;
        if (const sco_result added = detail::AddNode(detail::Access::StateOf(plugin_), node_, id); added != SCO_OK)
            return added;
        cmd.ctx = detail::ToCtx(id);
        const sco_result r = plugin_.Api()->register_command(plugin_.Self(), &cmd);
        if (r != SCO_OK) detail::Nodes().Remove(id);
        return r;
    }

private:
    CommandBuilder& SetText(std::string detail::CommandNode::*field, bool detail::CommandNode::*has,
                            const char* text) noexcept {
        if (failed_ || !text) return *this;
        try {
            (*node_).*field = text;
            if (has) (*node_).*has = true;
        } catch (...) {
            failed_ = true;
        }
        return *this;
    }

    Plugin&                              plugin_;
    std::shared_ptr<detail::CommandNode> node_;
    bool                                 hasName_ = false, badArg_ = false, failed_ = false;
};

}  // namespace sco::sdk

#endif  // SCOSDK_COMMAND_HPP
