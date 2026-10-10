#pragma once
// sco::settings: the host side of the "sco.settings" service (include/sco_settings.h): the typed
// [settings] a plugin declares in plugin.ini, their values, and what the product's menu needs to
// show and change them. C++ and internal: sco::app::Start declares each plugin's settings before
// its code runs; the product draws and edits; plugins only see the C table (read-only).
//
//   sco::settings::Start();                       // publishes sco.settings 1.0 (sco::app::Start does)
//   sco::settings::Declare(self, manifest.settings);   // before the plugin loads (sco::app::Start does)
//   ... each frame, on the game thread, inside the plugin's menu page (see docs/ui.md):
//   for (const sco::settings::Page& page : sco::settings::Pages()) {
//       if (page.tab.empty()) { /* no sco.ui tab: the product lists the plugin under "Settings" */ }
//       for (const sco::settings::Entry& e : page.entries) {
//           switch (e.decl.type) {
//               case sco::plugins::SettingType::Bool: {
//                   bool v = e.value.b;
//                   if (ImGui::Checkbox(e.decl.label.c_str(), &v)) sco::settings::SetBool(page.plugin.c_str(), e.decl.name.c_str(), v);
//                   break;
//               }
//               ...
//           }
//       }
//   }
//   sco::settings::Stop();                        // after every plugin unloaded
//
// sco-core has no renderer: Pages() and Entries() are snapshots the product turns into widgets,
// and SetBool / SetInt / SetFloat / SetString are what the widgets call. Every set is checked
// against the declaration, stored (the plugin's sco.storage namespace, when storage runs) and
// then announced with the event "settings.changed". Reference: docs/ui.md, docs/api-v1.md.
//
// Values are read from storage once, in Declare: a kept value of another type, outside the
// range or not among the choices is dropped (the default applies, mod.log says so). A set that
// storage refuses changes nothing and returns its error.
#include "sco_settings.h"
#include "sco/plugins.h"
#include "sco/runtime.h"
#include <string>
#include <vector>

namespace sco::settings {

// Publishes "sco.settings" 1.0 under the host's id and installs the release hook. Any thread.
// BadArg: already started, or the name already published. TooMany: no release hook slot, out of
// memory. Nothing changes unless Ok. Start sco::storage first to keep values across runs.
Result Start();

// Withdraws "sco.settings" and forgets every declaration; later calls through the table answer
// SCO_UNAVAILABLE. No-op unless started. Call before sco::storage::Stop and after every plugin
// has unloaded (sco::app::Stop does). Any thread.
void Stop();

bool Started();

// The service table (what query_service hands out). Valid for the life of the process.
const sco_settings_v1* Table();

// Declares self's settings (self: a handle from sco::host::NewPlugin, not yet released) and
// reads their kept values: a value the storage holds is used when it fits the declaration, else
// the default (with a mod.log line). Call before the plugin's code runs. Release(self) withdraws
// them. BadArg: an unknown or released self, empty or more than kMaxSettings decls, a plugin
// already declared, or decls that don't validate (duplicate name, a default outside its own
// range). Unavailable: not started. TooMany: out of memory. Any thread.
Result Declare(sco_plugin* self, const std::vector<plugins::Setting>& decls);

// ---- what the product reads (any thread; snapshots) --------------------------------------------

struct Entry {
    std::string           plugin;   // the plugin id
    plugins::Setting      decl;     // name, type, label, help, default, range, choices
    plugins::SettingValue value;    // the current value
};

// The settings of one plugin, in declaration order; empty when it declares none.
std::vector<Entry> Entries(const char* plugin);

// One page per plugin that declares settings, in the order the plugins were declared. tab is the
// id of the plugin's first sco.ui tab (ui::Tabs() order), where the product draws the entries
// below the tab's own content; "" when the plugin has no tab.
struct Page {
    std::string        plugin;
    std::string        tab;
    std::vector<Entry> entries;
};
std::vector<Page> Pages();

// ---- what the product's widgets call (game thread) -----------------------------------------------

// Sets one value. Ok: it is stored (and "settings.changed" dispatched) when it changed; setting the
// current value is Ok and silent. NotFound: no such plugin or setting. BadArg: the setting has
// another type (SetString sets a string or an enum), or the value breaks the declaration (below
// min, above max, not a choice, not a printable string of at most 255 bytes, NaN). WrongThread: not the game
// thread. Unavailable: not started. TooMany / Failed: storage refused the write (nothing changed).
Result SetBool(const char* plugin, const char* name, bool value);
Result SetInt(const char* plugin, const char* name, int64_t value);
Result SetFloat(const char* plugin, const char* name, double value);
Result SetString(const char* plugin, const char* name, const char* value);

// The key a setting's value is kept under in the plugin's storage namespace: "sco.settings.<name>".
// The value is "<type>:<text>" ("int:7", "enum:hard"); the type prefix is how a value written
// for an older declaration is recognized.
std::string StorageKey(const char* name);

}  // namespace sco::settings
