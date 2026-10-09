/*
 * cpp_hello: the sco SDK's C++20 example plugin, built on include/scosdk/.
 *
 * The C++ version of hello.c, plus a service:
 *   - Info() and Status() on load,
 *   - Has() to check a capability before offering a feature,
 *   - game.ready and tick subscriptions (handles that unsubscribe themselves),
 *   - two commands: cpp_hello.wave <name> and cpp_hello.ticks,
 *   - a service, cpp_hello.greeter, published on load and looked up on game.ready the way any
 *     other plugin would look it up.
 *
 * Build: see sdk/README.md (C++ plugins). GPL-3.0, like sco-core.
 */
#include "scosdk/scosdk.hpp"

#include <cstdio>

namespace {

// The service's table: a C struct that starts with uint32_t size, so later versions can add
// functions at the end (callers check with sco::sdk::HasMember).
struct greeter_v1 {
    uint32_t size;
    int (*greet)(const char* who, char* out, uint32_t out_size);
};

int Greet(const char* who, char* out, uint32_t out_size) {
    return std::snprintf(out, out_size, "Hello, %s", who ? who : "pilot");
}

const greeter_v1 kGreeter = { sizeof(greeter_v1), Greet };

class CppHello : public sco::sdk::Plugin {
public:
    sco_result OnLoad() override {
        using namespace sco::sdk;

        // cpp_hello.wave <name>: replies "Hello, <name>". Runs on the game thread.
        sco_result r = CommandBuilder(*this, "cpp_hello.wave")
                           .Title("Wave")
                           .Help("Says hello on the status line")
                           .Arg<const char*>("name", "Who to wave at")
                           .Handle([](const Args& args, Reply& reply) {
                               reply.Printf("Hello, %s", args.String(0));
                               return SCO_OK;
                           })
                           .Register();
        if (r != SCO_OK) return r;

        // cpp_hello.ticks: no arguments, so sco-plugin-check runs it on its own.
        r = CommandBuilder(*this, "cpp_hello.ticks")
                .Title("Ticks")
                .Help("How many ticks since the plugin loaded")
                .Handle([this](const Args&, Reply& reply) {
                    reply.Printf("%u ticks", static_cast<unsigned>(ticks_));
                    return SCO_OK;
                })
                .Register();
        if (r != SCO_OK) return r;

        // Services are 1.1: on an older host Provide answers SCO_UNAVAILABLE; carry on without.
        r = Provide(*this, "cpp_hello.greeter", ServiceVersion(1, 0), &kGreeter);
        if (r != SCO_OK) Warn("greeter service not published (result %d)", static_cast<int>(r));

        ready_ = Subscribe("game.ready", [this](const void*) { OnReady(); });
        tick_ = Subscribe("tick", [this](const void* data) { OnTick(data ? *static_cast<const uint32_t*>(data) : 0); });
        if (!ready_) return ready_.Result();
        if (!tick_) return tick_.Result();

        Info("%s", Api()->host_version());
        Status("Hello from a C++ plugin");
        return SCO_OK;
    }

    // Nothing to free: the subscriptions, commands and service go with the plugin.
    void OnUnload() override { Info("%u ticks seen", static_cast<unsigned>(ticks_)); }

private:
    void OnReady() {
        // Capabilities depend on the game build. Ask before using a feature and say why it is off.
        if (Has("teleport"))
            Info("teleport is available on this game build");
        else
            Warn("teleport is not available on this game build");

        // Look the service up as another plugin would. Query when needed: a provider's table goes
        // away when it unloads.
        sco::sdk::ServiceRef<greeter_v1> greeter;
        const sco_result r = greeter.Query(*this, "cpp_hello.greeter", sco::sdk::ServiceVersion(1, 0));
        if (r == SCO_OK && sco::sdk::HasMember(greeter, &greeter_v1::greet)) {
            char line[64];
            greeter->greet("services", line, sizeof line);
            Info("%s", line);
        } else {
            Warn("greeter service: %s", r == SCO_NOT_FOUND ? "not found" : "unavailable");
        }
    }

    void OnTick(uint32_t now) {
        if (ticks_++ == 0) firstMs_ = lastReportMs_ = now;
        // About once a minute; unsigned subtraction survives the millisecond counter wrapping.
        if (now - lastReportMs_ >= 60000) {
            Info("%u ticks in %u s", static_cast<unsigned>(ticks_), static_cast<unsigned>((now - firstMs_) / 1000));
            lastReportMs_ = now;
        }
    }

    sco::sdk::Subscription ready_, tick_;
    uint32_t               ticks_ = 0, firstMs_ = 0, lastReportMs_ = 0;
};

}  // namespace

// id must equal plugin.ini's id; it is also the command and service prefix.
SCO_PLUGIN(CppHello, "cpp_hello", "1.0.0", "sco SDK example");
