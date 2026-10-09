// scosdk/ui.hpp: the host service "sco.ui" (sco_ui.h) for C++ plugins. Header-only, over
// sco_api.h and sco_ui.h.
//
//   struct Panel { int clicks = 0; void Draw(void* frame) { /* the product's ImGui context */ } };
//   static Panel g_panel;
//
//   sco::sdk::Ui ui;
//   if (ui.Open(*this) == SCO_OK) {
//       ui.AddTab("hello.main", "Hello", 100, g_panel);              // calls g_panel.Draw(frame)
//       ui.SetBadge("hello.main", "new");
//       if (ui.BindHotkey("ctrl+alt+h", "hello.wave", { sco::sdk::MakeArg("Pilot") }) != SCO_OK)
//           Warn("hotkey: %s", ui.LastError().c_str());               // "ctrl+alt+h is bound by ..."
//   }
//
// Every call answers sco_result and is noexcept. The service is host-owned, so the table stays
// valid while the plugin is loaded and a Ui may be kept for the plugin's life. Everything the
// plugin registers is withdrawn when it unloads; the object passed to AddTab / AddOverlay must
// stay alive until then (or until RemoveTab / RemoveOverlay, freed as sco_ui.h says). The frame
// pointer a draw gets is valid only during the call. Draw functions run on the game thread; an
// exception from Draw is caught and dropped (it never crosses the C ABI). Reference: docs/ui.md,
// docs/sdk-cpp.md. GPL-3.0, like sco-core.
#ifndef SCOSDK_UI_HPP
#define SCOSDK_UI_HPP

#include "plugin.hpp"
#include "sco_ui.h"

#include <initializer_list>
#include <string>

namespace sco::sdk {

namespace detail {
template <class T>
void UiDraw(void* frame, void* ctx) noexcept {
    try {
        static_cast<T*>(ctx)->Draw(frame);
    } catch (...) {
    }
}
}  // namespace detail

class Ui {
public:
    // Finds sco.ui 1.x. SCO_UNAVAILABLE on a 1.0 host; SCO_NOT_FOUND from a host without UI. Both
    // leave the Ui empty.
    sco_result Open(const Plugin& plugin) noexcept { return Open(plugin.Api(), plugin.Self()); }
    // The same for code that holds the C handles (a C-style plugin, a test).
    sco_result Open(const sco_api* api, sco_plugin* self) noexcept {
        t_ = nullptr;
        self_ = self;
        if (!api || !self_) return SCO_BAD_ARG;
        if (!Covers(api->size, offsetof(sco_api, query_service))) return SCO_UNAVAILABLE;
        const void* table = nullptr;
        const sco_result r = api->query_service(SCO_UI_NAME, SCO_UI_VERSION_1_0, &table);
        if (r != SCO_OK) return r;
        t_ = static_cast<const sco_ui_v1*>(table);
        return SCO_OK;
    }
    explicit operator bool() const noexcept { return t_ != nullptr; }
    const sco_ui_v1* Table() const noexcept { return t_; }

    // ---- tabs and overlays ----

    sco_result AddTab(const char* id, const char* title, int32_t order, sco_ui_draw_fn draw, void* ctx) noexcept {
        return t_ ? t_->register_tab(self_, id, title, order, draw, ctx) : SCO_UNAVAILABLE;
    }
    // obj.Draw(void* frame) draws the tab.
    template <class T>
    sco_result AddTab(const char* id, const char* title, int32_t order, T& obj) noexcept {
        return AddTab(id, title, order, &detail::UiDraw<T>, &obj);
    }
    sco_result RemoveTab(const char* id) noexcept { return t_ ? t_->unregister_tab(self_, id) : SCO_UNAVAILABLE; }
    // nullptr or "" clears it.
    sco_result SetBadge(const char* tabId, const char* text) noexcept {
        return t_ ? t_->set_badge(self_, tabId, text) : SCO_UNAVAILABLE;
    }

    sco_result AddOverlay(const char* id, sco_ui_draw_fn draw, void* ctx) noexcept {
        return t_ ? t_->register_overlay(self_, id, draw, ctx) : SCO_UNAVAILABLE;
    }
    template <class T>
    sco_result AddOverlay(const char* id, T& obj) noexcept {
        return AddOverlay(id, &detail::UiDraw<T>, &obj);
    }
    sco_result RemoveOverlay(const char* id) noexcept { return t_ ? t_->unregister_overlay(self_, id) : SCO_UNAVAILABLE; }

    // ---- hotkeys ----

    // Binds chord to command with args (MakeArg(...)); copied by the host.
    sco_result BindHotkey(const char* chord, const char* command, std::initializer_list<sco_arg> args = {}) noexcept {
        if (!t_) return SCO_UNAVAILABLE;
        return t_->bind_hotkey(self_, chord, command, args.size() ? args.begin() : nullptr, static_cast<uint32_t>(args.size()));
    }
    sco_result UnbindHotkey(const char* chord) noexcept { return t_ ? t_->unbind_hotkey(self_, chord) : SCO_UNAVAILABLE; }

    // The normalized chord ("Alt + Ctrl + Esc" -> "ctrl+alt+escape"); SCO_BAD_ARG when it isn't one.
    sco_result NormalizeChord(const char* chord, std::string& out) const noexcept {
        if (!t_) return SCO_UNAVAILABLE;
        char buf[SCO_UI_MAX_CHORD + 1];
        uint32_t size = sizeof(buf);
        const sco_result r = t_->normalize_chord(chord, buf, &size);
        if (r != SCO_OK) return r;
        try {
            out.assign(buf, size - 1);
        } catch (...) {
            return SCO_TOO_MANY;
        }
        return SCO_OK;
    }

    // The message of the plugin's last refused UI call.
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
    const sco_ui_v1* t_ = nullptr;
    sco_plugin*      self_ = nullptr;
};

}  // namespace sco::sdk

#endif  // SCOSDK_UI_HPP
