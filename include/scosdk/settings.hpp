// scosdk/settings.hpp: the host service "sco.settings" (sco_settings.h) for C++ plugins.
// Header-only, over sco_api.h and sco_settings.h.
//
//   ; plugin.ini
//   [settings]
//   speed = int default 5 min 1 max 10 label "Walk speed"
//   mode  = enum(easy,normal,hard) default normal
//
//   sco::sdk::Settings settings;
//   if (settings.Open(*this) == SCO_OK) {
//       int64_t speed = settings.Int("speed", 5);        // the declared default until the player changes it
//       std::string mode = settings.String("mode", "normal");
//   }
//   // and to hear about a change (event "settings.changed"):
//   //   if (const sco_settings_changed* c = sco::sdk::AsSettingsChanged(data)) { /* c->plugin, c->name */ }
//
// Every call answers sco_result and is noexcept. The service is host-owned, so the table stays
// valid while the plugin is loaded and a Settings may be kept for the plugin's life. Reading is
// all a plugin can do: the player changes values in the product's menu. Reference: docs/api-v1.md
// (sco.settings), docs/plugins.md, docs/sdk-cpp.md. GPL-3.0, like sco-core.
#ifndef SCOSDK_SETTINGS_HPP
#define SCOSDK_SETTINGS_HPP

#include "plugin.hpp"
#include "sco_settings.h"

#include <string>

namespace sco::sdk {

// The data of a "settings.changed" event, or nullptr when data isn't one (null, or a size this
// header doesn't cover). Valid during the callback.
inline const sco_settings_changed* AsSettingsChanged(const void* data) noexcept {
    const auto* c = static_cast<const sco_settings_changed*>(data);
    return c && Covers(c->size, offsetof(sco_settings_changed, name)) ? c : nullptr;
}

class Settings {
public:
    // Finds sco.settings 1.x. SCO_UNAVAILABLE on a 1.0 host; SCO_NOT_FOUND from a host without
    // the service. Both leave the Settings empty.
    sco_result Open(const Plugin& plugin) noexcept { return Open(plugin.Api(), plugin.Self()); }
    // The same for code that holds the C handles (a C-style plugin, a test).
    sco_result Open(const sco_api* api, sco_plugin* self) noexcept {
        t_ = nullptr;
        self_ = self;
        if (!api || !self_) return SCO_BAD_ARG;
        if (!Covers(api->size, offsetof(sco_api, query_service))) return SCO_UNAVAILABLE;
        const void* table = nullptr;
        const sco_result r = api->query_service(SCO_SETTINGS_NAME, SCO_SETTINGS_VERSION_1_0, &table);
        if (r != SCO_OK) return r;
        t_ = static_cast<const sco_settings_v1*>(table);
        return SCO_OK;
    }
    explicit operator bool() const noexcept { return t_ != nullptr; }
    const sco_settings_v1* Table() const noexcept { return t_; }

    // ---- reads: SCO_NOT_FOUND (not declared), SCO_BAD_ARG (another type); out is untouched on failure ----

    sco_result GetBool(const char* name, bool& out) const noexcept {
        if (!t_) return SCO_UNAVAILABLE;
        int32_t v = 0;
        const sco_result r = t_->get_bool(self_, name, &v);
        if (r == SCO_OK) out = v != 0;
        return r;
    }
    sco_result GetInt(const char* name, int64_t& out) const noexcept {
        return t_ ? t_->get_int(self_, name, &out) : SCO_UNAVAILABLE;
    }
    sco_result GetFloat(const char* name, double& out) const noexcept {
        return t_ ? t_->get_float(self_, name, &out) : SCO_UNAVAILABLE;
    }
    // A string setting's text or an enum setting's choice.
    sco_result GetString(const char* name, std::string& out) const noexcept {
        if (!t_) return SCO_UNAVAILABLE;
        try {
            std::string s(SCO_SETTINGS_MAX_STRING + 1, '\0');
            uint32_t size = static_cast<uint32_t>(s.size());
            const sco_result r = t_->get_string(self_, name, s.data(), &size);
            if (r != SCO_OK) return r;
            s.resize(size - 1);
            out = std::move(s);
            return SCO_OK;
        } catch (...) {
            return SCO_TOO_MANY;
        }
    }

    // The value, or fallback when the call fails for any reason (the service missing included).
    bool Bool(const char* name, bool fallback = false) const noexcept {
        bool v = fallback;
        return GetBool(name, v) == SCO_OK ? v : fallback;
    }
    int64_t Int(const char* name, int64_t fallback = 0) const noexcept {
        int64_t v = fallback;
        return GetInt(name, v) == SCO_OK ? v : fallback;
    }
    double Float(const char* name, double fallback = 0) const noexcept {
        double v = fallback;
        return GetFloat(name, v) == SCO_OK ? v : fallback;
    }
    std::string String(const char* name, const std::string& fallback = {}) const noexcept {
        std::string v;
        try {
            return GetString(name, v) == SCO_OK ? v : fallback;
        } catch (...) {
            return {};
        }
    }

    // The message of the plugin's last failed call ("not a bool").
    std::string LastError() const noexcept {
        if (!t_) return {};
        try {
            std::string s(256, '\0');
            uint32_t size = static_cast<uint32_t>(s.size());
            sco_result r = t_->last_error(self_, s.data(), &size);
            if (r == SCO_TOO_MANY) {
                s.resize(size);
                r = t_->last_error(self_, s.data(), &size);
            }
            if (r != SCO_OK || size == 0) return {};
            s.resize(size - 1);
            return s;
        } catch (...) {
            return {};
        }
    }

private:
    const sco_settings_v1* t_ = nullptr;
    sco_plugin*            self_ = nullptr;
};

}  // namespace sco::sdk

#endif  // SCOSDK_SETTINGS_HPP
