// sco.settings: the typed [settings] of every plugin, their values, and the read-only C table of
// sco_settings.h (sco/settings.h). The product edits through SetBool / SetInt / SetFloat /
// SetString and draws from Pages(); plugins read.
//
// One lock, g_lock, guards the registry. It is never held across a storage call or an event
// dispatch (storage can wait on a busy database; a "settings.changed" subscriber may call back
// into the table). Every writer runs on the game thread, as does Release, so a record read under
// the lock and used after it cannot be withdrawn meanwhile; readers (the C table) run anywhere.
#include "sco/settings.h"
#include "sco/host.h"
#include "sco/log.h"
#include "sco/storage.h"
#include "sco/ui.h"
#include "../api/internal.h"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <utility>

namespace sco::settings {

namespace {

using plugins::Setting;
using plugins::SettingType;
using plugins::SettingValue;

struct Record {   // one plugin that declared settings
    const void*          owner = nullptr;
    sco_plugin*          self = nullptr;
    std::string          id;
    std::vector<Setting> decls;
    std::vector<SettingValue> values;   // parallel to decls
};

std::mutex                                       g_lock;
bool                                             g_started = false;
std::vector<Record>                              g_records;   // declaration order
std::vector<std::pair<const void*, std::string>> g_errors;    // owner -> last failed call

sco_result C(Result r) { return static_cast<sco_result>(static_cast<uint32_t>(r)); }

template <class F>
sco_result Guard(F&& f) noexcept {
    try {
        return C(f());
    } catch (...) {
        return SCO_TOO_MANY;
    }
}

constexpr unsigned Bit(SettingType t) { return 1u << static_cast<unsigned>(t); }

// Text from a plugin-writable place, made safe for the log: printable ASCII, at most 48 bytes.
std::string Shown(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size() && i < 48; ++i)
        out += (static_cast<unsigned char>(s[i]) >= 0x20 && static_cast<unsigned char>(s[i]) < 0x7f) ? s[i] : '?';
    if (s.size() > 48) out += "...";
    return out;
}

// ---- kept values ------------------------------------------------------------------------------

std::string Encode(const Setting& d, const SettingValue& v) {
    return std::string(plugins::SettingTypeName(d.type)) + ":" + plugins::FormatSettingValue(d, v);
}

// Reads d's kept value from the plugin's storage namespace into v; v keeps the default when
// nothing usable is kept. A value that no longer fits the declaration is deleted, with a log line.
void LoadKept(sco_plugin* self, const std::string& id, const Setting& d, SettingValue& v) {
    if (!storage::Started()) return;
    const sco_storage_v1* st = storage::Table();
    const std::string key = StorageKey(d.name.c_str());
    char buf[512];
    uint32_t size = sizeof(buf);
    const sco_result r = st->get(self, key.c_str(), buf, &size);
    if (r == SCO_NOT_FOUND) return;
    std::string why;
    if (r == SCO_OK || r == SCO_TOO_MANY) {
        const std::string text = r == SCO_OK ? std::string(buf, size) : std::string();
        const size_t colon = text.find(':');
        const char* have = plugins::SettingTypeName(d.type);
        SettingValue kept;
        if (r == SCO_TOO_MANY) why = "it is over 511 bytes";
        else if (colon == std::string::npos) why = "it is not <type>:<value>";
        else if (text.compare(0, colon, have) != 0)
            why = "it was saved as " + Shown(text.substr(0, colon)) + ", the setting is " + have + " now";
        else if (plugins::ParseSettingValue(d, std::string_view(text).substr(colon + 1), kept, why)) {
            v = std::move(kept);
            return;
        }
        Log("[settings] %s.%s: kept value dropped (%s); the default applies", id.c_str(), d.name.c_str(), why.c_str());
        st->del(self, key.c_str());   // so the next start doesn't say it again
        return;
    }
    Log("[settings] %s.%s: storage read failed (%s); the default applies", id.c_str(), d.name.c_str(),
        ResultName(static_cast<Result>(r)));
}

// ---- the table --------------------------------------------------------------------------------

Result Refuse(const void* owner, Result r, std::string why) {   // g_lock held
    for (auto& e : g_errors)
        if (e.first == owner) { e.second = std::move(why); return r; }
    g_errors.emplace_back(owner, std::move(why));
    return r;
}

Record* FindOwner(const void* owner) {
    for (auto& r : g_records)
        if (r.owner == owner) return &r;
    return nullptr;
}

Record* FindId(const std::string& id) {
    for (auto& r : g_records)
        if (r.id == id) return &r;
    return nullptr;
}

size_t FindSetting(const Record& r, const std::string& name) {
    for (size_t i = 0; i < r.decls.size(); ++i)
        if (r.decls[i].name == name) return i;
    return r.decls.size();
}

// Under g_lock: finds self's setting `name`, which must have one of the types in mask.
Result Find(sco_plugin* self, const char* name, unsigned mask, const char* want, const Record*& rec, size_t& idx) {
    if (!host::PluginId(self)) return Result::BadArg;
    if (!g_started) return Result::Unavailable;
    if (detail::Released(self)) return Result::BadArg;
    if (!name) return Refuse(self, Result::BadArg, "the setting name is NULL");
    std::string n;
    const size_t len = strnlen(name, SCO_SETTINGS_MAX_NAME + 1);
    if (len <= SCO_SETTINGS_MAX_NAME) n.assign(name, len);
    const Record* r = FindOwner(self);
    const size_t i = r ? FindSetting(*r, n) : 0;   // an empty n (null or too long) matches nothing
    if (!r || i == r->decls.size())
        return Refuse(self, Result::NotFound, "this plugin declares no setting '" + (n.empty() ? std::string("?") : n) + "'");
    if (!(mask & Bit(r->decls[i].type)))
        return Refuse(self, Result::BadArg, "setting '" + n + "' is " + plugins::SettingTypeName(r->decls[i].type) + ", not " + want);
    rec = r;
    idx = i;
    return Result::Ok;
}

sco_result GetBool(sco_plugin* self, const char* name, int32_t* out) {
    return Guard([&] {
        if (!out) return Result::BadArg;
        std::lock_guard<std::mutex> hold(g_lock);
        const Record* r = nullptr;
        size_t i = 0;
        if (const Result f = Find(self, name, Bit(SettingType::Bool), "a bool", r, i); f != Result::Ok) return f;
        *out = r->values[i].b ? 1 : 0;
        return Result::Ok;
    });
}

sco_result GetInt(sco_plugin* self, const char* name, int64_t* out) {
    return Guard([&] {
        if (!out) return Result::BadArg;
        std::lock_guard<std::mutex> hold(g_lock);
        const Record* r = nullptr;
        size_t i = 0;
        if (const Result f = Find(self, name, Bit(SettingType::Int), "an int", r, i); f != Result::Ok) return f;
        *out = r->values[i].i;
        return Result::Ok;
    });
}

sco_result GetFloat(sco_plugin* self, const char* name, double* out) {
    return Guard([&] {
        if (!out) return Result::BadArg;
        std::lock_guard<std::mutex> hold(g_lock);
        const Record* r = nullptr;
        size_t i = 0;
        if (const Result f = Find(self, name, Bit(SettingType::Float), "a float", r, i); f != Result::Ok) return f;
        *out = r->values[i].f;
        return Result::Ok;
    });
}

// The size handshake into the caller's buffer, NUL included. *io holds the capacity.
Result CopyOut(const std::string& s, char* out, uint32_t* io, uint32_t capacity) {
    const size_t need = s.size() + 1;
    *io = static_cast<uint32_t>(need);
    if (capacity < need) return Result::TooMany;
    std::memcpy(out, s.c_str(), need);
    return Result::Ok;
}

sco_result GetString(sco_plugin* self, const char* name, char* out, uint32_t* io) {
    return Guard([&] {
        if (!io) return Result::BadArg;
        const uint32_t capacity = *io;
        *io = 0;
        if (!out && capacity) return Result::BadArg;
        std::string value;
        {
            std::lock_guard<std::mutex> hold(g_lock);
            const Record* r = nullptr;
            size_t i = 0;
            if (const Result f = Find(self, name, Bit(SettingType::String) | Bit(SettingType::Enum), "a string or an enum", r, i);
                f != Result::Ok)
                return f;
            value = r->values[i].s;
        }
        return CopyOut(value, out, io, capacity);
    });
}

sco_result LastError(sco_plugin* self, char* out, uint32_t* io) {
    return Guard([&] {
        if (!io) return Result::BadArg;
        const uint32_t capacity = *io;
        *io = 0;
        if ((!out && capacity) || !host::PluginId(self)) return Result::BadArg;
        std::string e;
        {
            std::lock_guard<std::mutex> hold(g_lock);
            if (!g_started) return Result::Unavailable;
            if (detail::Released(self)) return Result::BadArg;
            for (const auto& x : g_errors)
                if (x.first == self) e = x.second;
        }
        return CopyOut(e, out, io, capacity);
    });
}

const sco_settings_v1 kTable = {
    sizeof(sco_settings_v1), 0,
    GetBool, GetInt, GetFloat, GetString,
    LastError,
};

void OnRelease(const void* owner) {
    std::lock_guard<std::mutex> hold(g_lock);
    g_records.erase(std::remove_if(g_records.begin(), g_records.end(), [owner](const Record& r) { return r.owner == owner; }),
                    g_records.end());
    g_errors.erase(std::remove_if(g_errors.begin(), g_errors.end(), [owner](const auto& e) { return e.first == owner; }),
                   g_errors.end());
}

bool Same(const Setting& d, const SettingValue& a, const SettingValue& b) {
    switch (d.type) {
        case SettingType::Bool:   return a.b == b.b;
        case SettingType::Int:    return a.i == b.i;
        case SettingType::Float:  return a.f == b.f;
        case SettingType::String:
        case SettingType::Enum:   return a.s == b.s;
    }
    return false;
}

// The one set: checks, stores, then announces. Game thread.
Result Set(const char* plugin, const char* name, unsigned mask, const SettingValue& v) {
    if (!OnGameThread()) return Result::WrongThread;
    if (!plugin || !name) return Result::BadArg;
    const std::string id = plugin, n = name;
    Setting decl;
    SettingValue old;
    sco_plugin* self = nullptr;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (!g_started) return Result::Unavailable;
        const Record* r = FindId(id);
        const size_t i = r ? FindSetting(*r, n) : 0;
        if (!r || i == r->decls.size()) return Result::NotFound;
        if (!(mask & Bit(r->decls[i].type))) return Result::BadArg;
        decl = r->decls[i];
        old = r->values[i];
        self = r->self;
    }
    std::string why;
    if (!plugins::CheckSettingValue(decl, v, why)) return Result::BadArg;
    if (Same(decl, old, v)) return Result::Ok;
    if (storage::Started()) {
        const std::string key = StorageKey(name), text = Encode(decl, v);
        const sco_result r = storage::Table()->put(self, key.c_str(), text.data(), static_cast<uint32_t>(text.size()));
        if (r != SCO_OK) {
            Log("[settings] %s.%s: not changed, storage refused the write (%s)", id.c_str(), name, ResultName(static_cast<Result>(r)));
            return static_cast<Result>(r);
        }
    }
    {
        std::lock_guard<std::mutex> hold(g_lock);
        Record* r = FindOwner(self);
        if (!r) return Result::NotFound;
        r->values[FindSetting(*r, n)] = v;
    }
    sco_settings_changed ev{ sizeof(ev), 0, id.c_str(), n.c_str() };
    const Result dr = Dispatch(SCO_SETTINGS_CHANGED_EVENT, &ev);
    if (dr != Result::Ok) Log("[settings] %s.%s: settings.changed not dispatched: %s", id.c_str(), name, ResultName(dr));
    return Result::Ok;
}

}  // namespace

std::string StorageKey(const char* name) { return std::string("sco.settings.") + (name ? name : ""); }

Result Start() {
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (g_started) return Result::BadArg;
        g_started = true;
    }
    Result r = AddReleaseHook(OnRelease);
    if (r == Result::Ok) {
        r = host::ProvideHostService(SCO_SETTINGS_NAME, SCO_SETTINGS_VERSION_1_0, &kTable);
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
        g_records.clear();
        g_errors.clear();
    }
    host::WithdrawHostService(SCO_SETTINGS_NAME);
    RemoveReleaseHook(OnRelease);
}

bool Started() {
    std::lock_guard<std::mutex> hold(g_lock);
    return g_started;
}

const sco_settings_v1* Table() { return &kTable; }

Result Declare(sco_plugin* self, const std::vector<plugins::Setting>& decls) {
    try {
        const char* pid = host::PluginId(self);
        if (!pid) return Result::BadArg;
        if (decls.empty() || decls.size() > plugins::kMaxSettings) return Result::BadArg;
        Record rec;
        rec.owner = self;
        rec.self = self;
        rec.id = pid;
        for (size_t i = 0; i < decls.size(); ++i) {
            std::string why;
            if (decls[i].name.empty() || !plugins::CheckSettingValue(decls[i], decls[i].def, why)) return Result::BadArg;
            for (size_t k = 0; k < i; ++k)
                if (decls[k].name == decls[i].name) return Result::BadArg;
        }
        {
            std::lock_guard<std::mutex> hold(g_lock);
            if (!g_started) return Result::Unavailable;
            if (detail::Released(self)) return Result::BadArg;
            if (FindOwner(self) || FindId(rec.id)) return Result::BadArg;
        }
        rec.decls = decls;
        for (const Setting& d : decls) {
            SettingValue v = d.def;
            LoadKept(self, rec.id, d, v);
            rec.values.push_back(std::move(v));
        }
        std::lock_guard<std::mutex> hold(g_lock);
        if (!g_started) return Result::Unavailable;
        if (detail::Released(self) || FindOwner(self) || FindId(rec.id)) return Result::BadArg;
        g_records.push_back(std::move(rec));
        return Result::Ok;
    } catch (...) {
        return Result::TooMany;
    }
}

namespace {
std::vector<Entry> EntriesOf(const Record& r) {
    std::vector<Entry> out;
    out.reserve(r.decls.size());
    for (size_t i = 0; i < r.decls.size(); ++i) out.push_back({ r.id, r.decls[i], r.values[i] });
    return out;
}
}  // namespace

std::vector<Entry> Entries(const char* plugin) {
    if (!plugin) return {};
    std::lock_guard<std::mutex> hold(g_lock);
    const Record* r = FindId(plugin);
    return r ? EntriesOf(*r) : std::vector<Entry>();
}

std::vector<Page> Pages() {
    std::vector<Page> out;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        for (const Record& r : g_records) out.push_back({ r.id, {}, EntriesOf(r) });
    }
    for (const ui::TabInfo& t : ui::Tabs())   // by ascending order: the first of an owner is its main tab
        for (Page& p : out)
            if (p.tab.empty() && p.plugin == t.owner) p.tab = t.id;
    return out;
}

Result SetBool(const char* plugin, const char* name, bool value) {
    SettingValue v;
    v.b = value;
    return Set(plugin, name, Bit(SettingType::Bool), v);
}

Result SetInt(const char* plugin, const char* name, int64_t value) {
    SettingValue v;
    v.i = value;
    return Set(plugin, name, Bit(SettingType::Int), v);
}

Result SetFloat(const char* plugin, const char* name, double value) {
    SettingValue v;
    v.f = value;
    return Set(plugin, name, Bit(SettingType::Float), v);
}

Result SetString(const char* plugin, const char* name, const char* value) {
    if (!value) return Result::BadArg;
    SettingValue v;
    v.s = value;
    return Set(plugin, name, Bit(SettingType::String) | Bit(SettingType::Enum), v);
}

}  // namespace sco::settings
