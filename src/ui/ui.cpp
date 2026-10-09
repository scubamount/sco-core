// sco.ui: the registry of plugin tabs, overlays and badges and the hotkey table behind the C
// table of sco_ui.h (sco/ui.h). sco-core draws nothing; the product calls DrawTab / DrawOverlays
// during its frame and Dispatch on a key.
//
// One lock, g_lock, guards everything below. Order: g_lock, then the runtime's owner lock
// (detail::Released). It is never held while plugin code runs: plugin strings are copied before
// it is taken, and draw callbacks and dispatched commands run after it is released, so a draw
// may call back into the table and a crash guard's Release(owner) can reach the release hook.
#include "sco/ui.h"
#include "sco/host.h"
#include "../api/internal.h"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace sco::ui {

namespace {

struct Entry {   // a tab or an overlay
    std::string    id, title, badge;
    const void*    owner = nullptr;
    std::string    ownerId;
    int32_t        order = 0;
    uint64_t       seq = 0;
    sco_ui_draw_fn draw = nullptr;
    void*          ctx = nullptr;
};

struct StoredArg {
    ArgType     type = ArgType::Int;
    int64_t     i = 0;
    double      f = 0;
    std::string s;
};

struct Binding {
    std::string            chord, command;
    const void*            owner = nullptr;
    std::string            ownerId;
    std::vector<StoredArg> args;
};

std::mutex                                       g_lock;
bool                                             g_started = false;
std::vector<Entry>                               g_tabs;       // registration order
std::vector<Entry>                               g_overlays;   // registration order
std::vector<Binding>                             g_keys;
std::vector<std::string>                         g_reserved;   // sorted
std::vector<std::pair<const void*, std::string>> g_errors;     // owner -> last refused call
uint64_t                                         g_seq = 0;

sco_result C(Result r) { return static_cast<sco_result>(static_cast<uint32_t>(r)); }

// Runs a table function: nothing throws across the C ABI (out of memory is SCO_TOO_MANY).
template <class F>
sco_result Guard(F&& f) noexcept {
    try {
        return C(f());
    } catch (...) {
        return SCO_TOO_MANY;
    }
}

// ---- names and chords ---------------------------------------------------------------------------

// "<x>.<y>": [a-z0-9_.], 1-max bytes, at least one '.', none leading, trailing or doubled.
bool ValidDotted(const std::string& s, size_t max) {
    if (s.empty() || s.size() > max || s.front() == '.' || s.back() == '.') return false;
    bool dot = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '.') {
            if (s[i - 1] == '.') return false;
            dot = true;
        } else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return dot;
}

// Copies a plugin string of at most max bytes; false for null or longer.
bool CopyString(const char* s, size_t max, std::string& out) {
    if (!s) return false;
    const size_t n = strnlen(s, max + 1);
    if (n > max) return false;
    out.assign(s, n);
    return true;
}

// Named keys: the first name is the normalized one, the rest are aliases.
struct KeyName { const char* names[3]; };
const KeyName kKeys[] = {
    { { "escape", "esc", nullptr } },       { { "enter", "return", nullptr } },
    { { "tab", nullptr, nullptr } },        { { "space", nullptr, nullptr } },
    { { "backspace", nullptr, nullptr } },  { { "insert", "ins", nullptr } },
    { { "delete", "del", nullptr } },       { { "home", nullptr, nullptr } },
    { { "end", nullptr, nullptr } },        { { "pageup", "pgup", nullptr } },
    { { "pagedown", "pgdn", nullptr } },    { { "up", nullptr, nullptr } },
    { { "down", nullptr, nullptr } },       { { "left", nullptr, nullptr } },
    { { "right", nullptr, nullptr } },      { { "minus", "-", nullptr } },
    { { "equals", "=", nullptr } },         { { "comma", ",", nullptr } },
    { { "period", ".", nullptr } },         { { "slash", "/", nullptr } },
    { { "backslash", "\\", nullptr } },     { { "semicolon", ";", nullptr } },
    { { "apostrophe", "'", nullptr } },     { { "grave", "`", nullptr } },
    { { "lbracket", "[", nullptr } },       { { "rbracket", "]", nullptr } },
};

// A key token (lower case, trimmed) to its normalized name; "" when it is none.
std::string KeyOf(const std::string& t) {
    if (t.size() == 1 && ((t[0] >= 'a' && t[0] <= 'z') || (t[0] >= '0' && t[0] <= '9'))) return t;
    auto number = [](const std::string& digits, int lo, int hi) {   // no sign, no leading zero
        if (digits.empty() || digits.size() > 2 || digits[0] == '0') return -1;
        int n = 0;
        for (char c : digits) {
            if (c < '0' || c > '9') return -1;
            n = n * 10 + (c - '0');
        }
        return n >= lo && n <= hi ? n : -1;
    };
    if (t.size() >= 2 && t[0] == 'f') {
        const int n = number(t.substr(1), 1, 24);
        if (n > 0) return "f" + std::to_string(n);
    }
    for (const char* prefix : { "numpad", "num" }) {
        const size_t n = std::strlen(prefix);
        if (t.size() == n + 1 && t.compare(0, n, prefix) == 0 && t[n] >= '0' && t[n] <= '9')
            return std::string("num") + t[n];
    }
    for (const KeyName& k : kKeys)
        for (const char* name : k.names)
            if (name && t == name) return k.names[0];
    return {};
}

bool Normalize(const char* text, std::string& out) {
    std::string s;
    if (!CopyString(text, 63, s) || s.empty()) return false;
    enum : unsigned { kCtrl = 1, kAlt = 2, kShift = 4 };
    unsigned mods = 0;
    std::string key;
    size_t start = 0;
    for (;;) {
        const size_t plus = s.find('+', start);
        std::string t = s.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        const size_t a = t.find_first_not_of(" \t");
        if (a == std::string::npos) return false;   // an empty part: "ctrl+", "+a", "ctrl++a"
        t = t.substr(a, t.find_last_not_of(" \t") - a + 1);
        for (char& c : t) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c == ' ' || c == '\t') return false;   // "page up"
        }
        unsigned m = 0;
        if (t == "ctrl" || t == "control") m = kCtrl;
        else if (t == "alt") m = kAlt;
        else if (t == "shift") m = kShift;
        if (m) {
            if (mods & m) return false;   // a modifier twice
            mods |= m;
        } else {
            if (!key.empty()) return false;   // two keys
            key = KeyOf(t);
            if (key.empty()) return false;
        }
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    if (key.empty()) return false;   // modifiers only
    std::string n;
    if (mods & kCtrl) n += "ctrl+";
    if (mods & kAlt) n += "alt+";
    if (mods & kShift) n += "shift+";
    n += key;
    out = std::move(n);
    return true;
}

// ---- entering a call ------------------------------------------------------------------------------

// Records owner's last refused call (g_lock held).
Result Refuse(const void* owner, Result r, std::string why) {
    for (auto& e : g_errors)
        if (e.first == owner) { e.second = std::move(why); return r; }
    g_errors.emplace_back(owner, std::move(why));
    return r;
}

// The caller's plugin id; null for a pointer NewPlugin didn't return.
const char* IdOf(sco_plugin* self) { return host::PluginId(self); }

// Under g_lock: Ok when the service runs and self may still call.
Result Live(sco_plugin* self) {
    if (!g_started) return Result::Unavailable;
    if (detail::Released(self)) return Result::BadArg;
    return Result::Ok;
}

// An id under the caller's own id.
bool OwnId(const std::string& id, const char* pluginId) {
    const size_t n = std::strlen(pluginId);
    return ValidDotted(id, SCO_UI_MAX_ID) && id.size() > n + 1 && id.compare(0, n, pluginId) == 0 && id[n] == '.';
}

Entry* FindEntry(std::vector<Entry>& list, const std::string& id) {
    for (auto& e : list)
        if (e.id == id) return &e;
    return nullptr;
}

Binding* FindBinding(const std::string& chord) {
    for (auto& b : g_keys)
        if (b.chord == chord) return &b;
    return nullptr;
}

bool IsReserved(const std::string& chord) {
    return std::binary_search(g_reserved.begin(), g_reserved.end(), chord);
}

// ---- the table ------------------------------------------------------------------------------------

Result AddEntry(std::vector<Entry>& list, size_t max, const char* what, sco_plugin* self, const char* id,
                const char* title, int32_t order, sco_ui_draw_fn draw, void* ctx) {
    const char* pid = IdOf(self);
    if (!pid) return Result::BadArg;
    std::string i, t;
    const bool idOk = CopyString(id, SCO_UI_MAX_ID, i);
    const bool titleOk = !title || (CopyString(title, SCO_UI_MAX_TITLE, t) && !t.empty());
    std::lock_guard<std::mutex> hold(g_lock);
    if (const Result r = Live(self); r != Result::Ok) return r;
    if (!idOk || !OwnId(i, pid))
        return Refuse(self, Result::BadArg, std::string(what) + " id '" + (idOk ? i : std::string("?")) +
                                                "' is not '" + pid + ".<name>' ([a-z0-9_.], at most 63 bytes)");
    if (!titleOk) return Refuse(self, Result::BadArg, std::string(what) + " " + i + ": the title must be 1-63 bytes");
    if (!draw) return Refuse(self, Result::BadArg, std::string(what) + " " + i + ": draw is NULL");
    if (FindEntry(list, i)) return Refuse(self, Result::BadArg, std::string(what) + " id '" + i + "' is taken");
    if (list.size() >= max)
        return Refuse(self, Result::TooMany, std::string("too many ") + what + "s (" + std::to_string(max) + ")");
    Entry e;
    e.id = std::move(i);
    e.title = std::move(t);
    e.owner = self;
    e.ownerId = pid;
    e.order = order;
    e.seq = ++g_seq;
    e.draw = draw;
    e.ctx = ctx;
    list.push_back(std::move(e));
    return Result::Ok;
}

Result RemoveEntry(std::vector<Entry>& list, const char* what, sco_plugin* self, const char* id) {
    if (!IdOf(self)) return Result::BadArg;
    std::string i;
    const bool idOk = CopyString(id, SCO_UI_MAX_ID, i);
    std::lock_guard<std::mutex> hold(g_lock);
    if (const Result r = Live(self); r != Result::Ok) return r;
    for (size_t k = 0; idOk && k < list.size(); ++k)
        if (list[k].id == i && list[k].owner == self) {
            list.erase(list.begin() + static_cast<std::ptrdiff_t>(k));
            return Result::Ok;
        }
    return Refuse(self, Result::NotFound, std::string("no ") + what + " '" + (idOk ? i : std::string("?")) + "' of this plugin");
}

sco_result RegisterTab(sco_plugin* self, const char* id, const char* title, int32_t order, sco_ui_draw_fn draw, void* ctx) {
    return Guard([&] {
        if (!title) {   // AddEntry treats null as "no title" for overlays
            std::lock_guard<std::mutex> hold(g_lock);
            if (!IdOf(self)) return Result::BadArg;
            if (const Result r = Live(self); r != Result::Ok) return r;
            return Refuse(self, Result::BadArg, "tab: the title must be 1-63 bytes");
        }
        return AddEntry(g_tabs, kMaxTabs, "tab", self, id, title, order, draw, ctx);
    });
}

sco_result UnregisterTab(sco_plugin* self, const char* id) {
    return Guard([&] { return RemoveEntry(g_tabs, "tab", self, id); });
}

sco_result SetBadge(sco_plugin* self, const char* tabId, const char* text) {
    return Guard([&] {
        if (!IdOf(self)) return Result::BadArg;
        std::string i, b;
        const bool idOk = CopyString(tabId, SCO_UI_MAX_ID, i);
        const bool badgeOk = !text || CopyString(text, SCO_UI_MAX_BADGE, b);
        std::lock_guard<std::mutex> hold(g_lock);
        if (const Result r = Live(self); r != Result::Ok) return r;
        Entry* e = idOk ? FindEntry(g_tabs, i) : nullptr;
        if (!e || e->owner != self)
            return Refuse(self, Result::NotFound, "no tab '" + (idOk ? i : std::string("?")) + "' of this plugin");
        if (!badgeOk) return Refuse(self, Result::BadArg, "tab " + i + ": a badge is at most 15 bytes");
        e->badge = std::move(b);
        return Result::Ok;
    });
}

sco_result RegisterOverlay(sco_plugin* self, const char* id, sco_ui_draw_fn draw, void* ctx) {
    return Guard([&] { return AddEntry(g_overlays, kMaxOverlays, "overlay", self, id, nullptr, 0, draw, ctx); });
}

sco_result UnregisterOverlay(sco_plugin* self, const char* id) {
    return Guard([&] { return RemoveEntry(g_overlays, "overlay", self, id); });
}

// Copies a binding's arguments; false (with why) for a bad one.
bool CopyArgs(const sco_arg* args, uint32_t n, std::vector<StoredArg>& out, std::string& why) {
    if (n > SCO_UI_MAX_HOTKEY_ARGS) { why = "at most 16 arguments"; return false; }
    if (n && !args) { why = "args is NULL"; return false; }
    out.resize(n);
    for (uint32_t k = 0; k < n; ++k) {
        const sco_arg& a = args[k];
        StoredArg& s = out[k];
        why = "argument " + std::to_string(k + 1) + ": ";
        switch (a.type) {
            case SCO_ARG_INT:    s.type = ArgType::Int; s.i = a.v.i; break;
            case SCO_ARG_FLOAT:  s.type = ArgType::Float; s.f = a.v.f; break;
            case SCO_ARG_BOOL:
                if (a.v.i != 0 && a.v.i != 1) { why += "a bool is 0 or 1"; return false; }
                s.type = ArgType::Bool; s.i = a.v.i;
                break;
            case SCO_ARG_STRING:
                if (!CopyString(a.v.s, SCO_UI_MAX_ARG_STRING, s.s)) { why += "a string is non-NULL and at most 255 bytes"; return false; }
                s.type = ArgType::String;
                break;
            default: why += "unknown type"; return false;
        }
    }
    why.clear();
    return true;
}

std::string Holder(const Binding& b) { return b.chord + " is bound by '" + b.ownerId + "' to " + b.command; }

sco_result BindHotkey(sco_plugin* self, const char* chord, const char* command, const sco_arg* args, uint32_t nargs) {
    return Guard([&] {
        const char* pid = IdOf(self);
        if (!pid) return Result::BadArg;
        std::string c, cmd, why;
        std::vector<StoredArg> stored;
        const bool chordOk = Normalize(chord, c);
        std::string shown;   // the chord as given, for a refusal
        if (!chordOk && chord) shown.assign(chord, strnlen(chord, SCO_UI_MAX_CHORD));
        const bool cmdOk = CopyString(command, kMaxNameLen, cmd) && ValidDotted(cmd, kMaxNameLen);
        const bool argsOk = CopyArgs(args, nargs, stored, why);
        std::lock_guard<std::mutex> hold(g_lock);
        if (const Result r = Live(self); r != Result::Ok) return r;
        if (!chordOk) return Refuse(self, Result::BadArg, "not a chord: '" + shown + "'");
        if (!cmdOk) return Refuse(self, Result::BadArg, c + ": the command name is not '<x>.<y>'");
        if (!argsOk) return Refuse(self, Result::BadArg, c + ": " + why);
        if (IsReserved(c)) return Refuse(self, Result::BadArg, c + " is reserved by the host");
        if (const Binding* b = FindBinding(c)) return Refuse(self, Result::BadArg, Holder(*b));
        if (g_keys.size() >= kMaxHotkeys) return Refuse(self, Result::TooMany, "too many hotkeys (" + std::to_string(kMaxHotkeys) + ")");
        Binding b;
        b.chord = std::move(c);
        b.command = std::move(cmd);
        b.owner = self;
        b.ownerId = pid;
        b.args = std::move(stored);
        g_keys.push_back(std::move(b));
        return Result::Ok;
    });
}

sco_result UnbindHotkey(sco_plugin* self, const char* chord) {
    return Guard([&] {
        if (!IdOf(self)) return Result::BadArg;
        std::string c;
        const bool chordOk = Normalize(chord, c);
        std::lock_guard<std::mutex> hold(g_lock);
        if (const Result r = Live(self); r != Result::Ok) return r;
        for (size_t k = 0; chordOk && k < g_keys.size(); ++k)
            if (g_keys[k].chord == c && g_keys[k].owner == self) {
                g_keys.erase(g_keys.begin() + static_cast<std::ptrdiff_t>(k));
                return Result::Ok;
            }
        return Refuse(self, Result::NotFound, chordOk ? "no binding of this plugin on " + c : "not a chord");
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

sco_result NormalizeChordC(const char* chord, char* out, uint32_t* io) {
    return Guard([&] {
        if (!io) return Result::BadArg;
        const uint32_t capacity = *io;
        *io = 0;
        if (!out && capacity) return Result::BadArg;
        std::string c;
        if (!Normalize(chord, c)) return Result::BadArg;
        return CopyOut(c, out, io, capacity);
    });
}

sco_result LastError(sco_plugin* self, char* out, uint32_t* io) {
    return Guard([&] {
        if (!io) return Result::BadArg;
        const uint32_t capacity = *io;
        *io = 0;
        if ((!out && capacity) || !IdOf(self)) return Result::BadArg;
        std::string e;
        {
            std::lock_guard<std::mutex> hold(g_lock);
            if (const Result r = Live(self); r != Result::Ok) return r;
            for (const auto& x : g_errors)
                if (x.first == self) e = x.second;
        }
        return CopyOut(e, out, io, capacity);
    });
}

const sco_ui_v1 kTable = {
    sizeof(sco_ui_v1), 0,
    RegisterTab, UnregisterTab, SetBadge,
    RegisterOverlay, UnregisterOverlay,
    BindHotkey, UnbindHotkey, NormalizeChordC,
    LastError,
};

// ---- lifetime -------------------------------------------------------------------------------------

template <class T>
void EraseOwned(std::vector<T>& v, const void* owner) {
    v.erase(std::remove_if(v.begin(), v.end(), [owner](const T& x) { return x.owner == owner; }), v.end());
}

// The runtime calls this once Release(owner) has removed the owner's items.
void OnRelease(const void* owner) {
    std::lock_guard<std::mutex> hold(g_lock);
    EraseOwned(g_tabs, owner);
    EraseOwned(g_overlays, owner);
    EraseOwned(g_keys, owner);
    g_errors.erase(std::remove_if(g_errors.begin(), g_errors.end(), [owner](const auto& e) { return e.first == owner; }),
                   g_errors.end());
}

// ---- drawing --------------------------------------------------------------------------------------

struct DrawCall { sco_ui_draw_fn fn; void* frame; void* ctx; };

void DrawThunk(void* p) {
    const DrawCall* c = static_cast<const DrawCall*>(p);
    c->fn(c->frame, c->ctx);
}

// Runs one draw as a callout of owner, with where = the entry's id. False when it didn't complete.
bool RunDraw(const Entry& e, void* frame) {
    if (detail::Released(e.owner)) return false;
    DrawCall call{ e.draw, frame, e.ctx };
    return detail::Callout(e.owner, e.id.c_str(), DrawThunk, &call);
}

}  // namespace

Result Start() {
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (g_started) return Result::BadArg;
        g_started = true;
    }
    Result r = AddReleaseHook(OnRelease);
    if (r == Result::Ok) {
        r = host::ProvideHostService(SCO_UI_NAME, SCO_UI_VERSION_1_0, &kTable);
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
        g_tabs.clear();
        g_overlays.clear();
        g_keys.clear();
        g_reserved.clear();
        g_errors.clear();
    }
    host::WithdrawHostService(SCO_UI_NAME);
    RemoveReleaseHook(OnRelease);
}

bool Started() {
    std::lock_guard<std::mutex> hold(g_lock);
    return g_started;
}

const sco_ui_v1* Table() { return &kTable; }

bool NormalizeChord(const char* text, std::string& out) {
    try {
        return Normalize(text, out);
    } catch (...) {
        return false;
    }
}

Result ReserveChord(const char* chord) {
    std::string c;
    if (!NormalizeChord(chord, c)) return Result::BadArg;
    std::lock_guard<std::mutex> hold(g_lock);
    if (!g_started) return Result::Unavailable;
    if (IsReserved(c)) return Result::Ok;
    if (FindBinding(c)) return Result::BadArg;
    if (g_reserved.size() >= kMaxReserved) return Result::TooMany;
    g_reserved.insert(std::upper_bound(g_reserved.begin(), g_reserved.end(), c), c);
    return Result::Ok;
}

std::vector<TabInfo> Tabs() {
    std::vector<std::pair<uint64_t, TabInfo>> v;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        v.reserve(g_tabs.size());
        for (const Entry& e : g_tabs) v.push_back({ e.seq, TabInfo{ e.id, e.title, e.badge, e.ownerId, e.order } });
    }
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
        return a.second.order != b.second.order ? a.second.order < b.second.order : a.first < b.first;
    });
    std::vector<TabInfo> out;
    out.reserve(v.size());
    for (auto& x : v) out.push_back(std::move(x.second));
    return out;
}

std::vector<OverlayInfo> Overlays() {
    std::lock_guard<std::mutex> hold(g_lock);
    std::vector<OverlayInfo> out;
    out.reserve(g_overlays.size());
    for (const Entry& e : g_overlays) out.push_back({ e.id, e.ownerId });
    return out;
}

std::string Badge(const char* tabId) {
    if (!tabId) return {};
    std::lock_guard<std::mutex> hold(g_lock);
    const Entry* e = FindEntry(g_tabs, tabId);
    return e ? e->badge : std::string();
}

std::vector<HotkeyInfo> Hotkeys() {
    std::vector<HotkeyInfo> out;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        out.reserve(g_keys.size());
        for (const Binding& b : g_keys)
            out.push_back({ b.chord, b.command, b.ownerId, static_cast<uint32_t>(b.args.size()) });
    }
    std::sort(out.begin(), out.end(), [](const HotkeyInfo& a, const HotkeyInfo& b) { return a.chord < b.chord; });
    return out;
}

std::vector<std::string> ReservedChords() {
    std::lock_guard<std::mutex> hold(g_lock);
    return g_reserved;
}

Result DrawTab(const char* id, void* frame) {
    if (!OnGameThread()) return Result::WrongThread;
    if (!id) return Result::NotFound;
    Entry e;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        const Entry* t = g_started ? FindEntry(g_tabs, id) : nullptr;
        if (!t) return Result::NotFound;
        e = *t;
    }
    return RunDraw(e, frame) ? Result::Ok : Result::Crashed;
}

size_t DrawOverlays(void* frame) {
    if (!OnGameThread()) return 0;
    std::vector<uint64_t> pass;   // the overlays live when the pass starts, by sequence number
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (!g_started) return 0;
        for (const Entry& e : g_overlays) pass.push_back(e.seq);
    }
    size_t ran = 0;
    for (const uint64_t seq : pass) {
        Entry e;
        {
            std::lock_guard<std::mutex> hold(g_lock);
            const auto it = std::find_if(g_overlays.begin(), g_overlays.end(), [seq](const Entry& x) { return x.seq == seq; });
            if (it == g_overlays.end()) continue;   // removed during the pass
            e = *it;
        }
        if (RunDraw(e, frame)) ++ran;
    }
    return ran;
}

namespace {
void KeepReply(Result, const char* reply, void* ctx) {
    std::string* out = static_cast<std::string*>(ctx);
    if (out) *out = reply ? reply : "";
}
}  // namespace

Result Dispatch(const char* chord, std::string* reply) {
    if (reply) reply->clear();
    std::string c;
    if (!NormalizeChord(chord, c)) return Result::NotFound;
    Binding b;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        const Binding* found = g_started ? FindBinding(c) : nullptr;
        if (!found) return Result::NotFound;
        b = *found;
    }
    Arg args[kMaxCommandArgs] = {};
    const uint32_t n = static_cast<uint32_t>(b.args.size());
    for (uint32_t k = 0; k < n; ++k) {
        const StoredArg& s = b.args[k];
        args[k].type = s.type;
        if (s.type == ArgType::Float) args[k].v.f = s.f;
        else if (s.type == ArgType::String) args[k].v.s = s.s.c_str();
        else args[k].v.i = s.i;
    }
    if (OnGameThread()) return Invoke(b.command.c_str(), n ? args : nullptr, n, KeepReply, reply, b.owner);
    return Invoke(b.command.c_str(), n ? args : nullptr, n, nullptr, nullptr, b.owner);
}

}  // namespace sco::ui
