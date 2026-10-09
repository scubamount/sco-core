#pragma once
// Plugin discovery, plugin.ini parsing, the native loader and the data-pack content index.
// C++ and internal: sc-offline (the host) calls this; plugins only see sco_api.h.
//
// A plugin is a folder data/plugins/<id>/ holding plugin.ini (format: docs/plugins.md):
//
//   id = hello              ; [a-z0-9_], 1-31 chars, equals the folder name, also the command prefix
//   name = Hello
//   version = 1.0.0
//   author = you            ; optional
//   api = 1.0               ; sco_api major.minor it needs
//   kind = native           ; native | lua | data
//   entry = hello.dll       ; native: the DLL; lua: the main script; data: absent
//   requires = teleport, spawn.ship   ; optional capabilities
//
// Flow on the game thread, after game.ready:
//   auto list = sco::plugins::Discover(root, opts);       // parse + check every folder
//   sco::plugins::ContainCallouts(&list);                 // guard every plugin callback
//   for (auto& p : list) if (p.state == State::Ready && p.manifest.kind == Kind::Native)
//       sco::plugins::LoadNative(p, api, selfFor(p));     // query -> checks -> load, all guarded
//   index.Build(list);                                    // data packs -> content index
//   sco::plugins::LogReport(list);                        // one line per plugin in mod.log
//   ...
//   sco::plugins::UnloadAll(list);                        // game.exit, reverse load order
//
// Nothing here runs plugin code unless the host calls LoadNative. Discover() never loads code;
// with opts.enabled false it still lists every folder (state Off) so `status` can show them.
#include "sco_api.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace sco::plugins {

namespace fs = std::filesystem;

// ---- manifest -------------------------------------------------------------------------------

enum class Kind : uint32_t { Native = 0, Lua = 1, Data = 2 };
const char* KindName(Kind k);                     // "native", "lua", "data"

constexpr size_t kMaxIdLen = 31, kMaxNameLen = 63, kMaxVersionLen = 31, kMaxAuthorLen = 63;
constexpr size_t kMaxCapabilityLen = 63, kMaxRequires = 16, kMaxEntryLen = 63;
constexpr size_t kMaxManifestBytes = 16 * 1024;

struct Manifest {
    std::string id, name, version, author;        // author may be empty
    uint16_t    apiMajor = 0, apiMinor = 0;
    Kind        kind = Kind::Data;
    std::string entry;                            // empty for data packs
    std::vector<std::string> requires_;           // capability names, file order, no duplicates
};

// Parses plugin.ini text. Lines are `key = value`; `;` or `#` starts a comment at the start of a
// line or after whitespace; blank lines are skipped; CRLF and a UTF-8 BOM are accepted. Unknown
// keys are ignored (a later minor may add keys). False with `error` set ("line 3: api must be
// <major>.<minor>") for: a line without '=', a duplicate key, a missing required key (id, name,
// version, api, kind), a value that breaks its rule (see the constants above), entry on a data
// pack or missing on native/lua, entry not a bare file name, text over kMaxManifestBytes ("too
// big"), a reserved id (sco, host, menu, game). On failure `out` is left empty.
bool ParseManifest(std::string_view text, Manifest& out, std::string& error);

// ---- discovery ------------------------------------------------------------------------------

enum class State : uint32_t {
    Off,        // plugins = off in sc-offline.ini; listed, never parsed past the manifest
    Disabled,   // data/plugins/<id>/disabled exists (or the menu switched it off)
    Refused,    // a check failed; `reason` says which
    Ready,      // passed discovery; LoadNative (native), LoadScript (lua) or Build (data) takes it
    Loaded,     // native: sco_plugin_load returned OK; lua: the entry script ran; data: indexed
    Crashed,    // faulted in plugin code, or couldn't be released; never called again, DLL kept mapped
    Unloaded,   // unloaded cleanly; DLL closed
};
const char* StateName(State s);                   // "off", "disabled", "refused", ...

// The three exports, resolved by LoadNative.
struct NativeExports {
    sco_plugin_query_fn  query  = nullptr;
    sco_plugin_load_fn   load   = nullptr;
    sco_plugin_unload_fn unload = nullptr;
};

struct Plugin {
    fs::path    dir;                              // data/plugins/<folder>
    std::string folder;                           // the folder name (== manifest.id when valid)
    Manifest    manifest;                         // valid only when manifestOk
    bool        manifestOk = false;               // plugin.ini parsed (state may still be Refused)
    State       state = State::Refused;
    std::string reason;                           // why Refused / Disabled / Crashed
    // native only
    void*         module = nullptr;               // module handle while mapped
    sco_plugin*   self = nullptr;                 // the owner handle the host passed to load (native, lua)
    NativeExports exports;
    uint32_t      loadOrder = 0;                  // 1-based among loaded natives and scripts; 0 = never
};

using CapabilityCheck = int (*)(const char* capability);

struct Options {
    bool     enabled = false;                     // `plugins = on` in sc-offline.ini
    uint16_t hostMajor = SCO_API_MAJOR, hostMinor = SCO_API_MINOR;
    CapabilityCheck has = nullptr;                // nullptr: every `requires` is missing
};

constexpr size_t kMaxPlugins = 128;               // folders beyond this are listed as Refused

// Lists every subfolder of root (sorted by name, byte order) that holds a plugin.ini; folders
// without one and plain files are skipped. root missing or not a folder: empty list.
// Symlinked folders are skipped. Each entry ends Off, Disabled, Refused or Ready. Refused
// reasons, in check order: "too many plugins" (past kMaxPlugins), "plugin.ini: unreadable",
// "plugin.ini: <parse error>" (incl. "plugin.ini: too big"), "id 'x' does not match folder 'y'"
// (so ids are unique), "built for api M.m" (major differs or minor newer than the host),
// "entry 'x' not found", "missing capability 'x'".
// Discover never runs plugin code and never opens the entry file.
std::vector<Plugin> Discover(const fs::path& root, const Options& opts);

// ---- native loader --------------------------------------------------------------------------

// How the loader maps a module. PlatformModuleOps(): Windows LoadLibraryExW(absolute path,
// LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32) / GetProcAddress /
// FreeLibrary; elsewhere dlopen(RTLD_NOW | RTLD_LOCAL) / dlsym / dlclose (host tests).
struct ModuleOps {
    void* (*open)(const fs::path& file, std::string& error);
    void* (*symbol)(void* module, const char* name);
    void  (*close)(void* module);
};
ModuleOps PlatformModuleOps();

// Crash guard: runs thunk(ctx); returns 0, or a nonzero fault code if plugin code faulted
// (Windows: the SEH exception code, e.g. 0xC0000005). Default on Windows: __try/__except.
// Elsewhere the default calls straight through (no containment); tests install their own.
using CallGuard = uint32_t (*)(void (*thunk)(void* ctx), void* ctx);
void      SetCallGuard(CallGuard guard);         // nullptr restores the default
uint32_t  Guarded(void (*thunk)(void* ctx), void* ctx);

// Loads one Ready native plugin. Game thread only (sco::Release runs on failure).
//   1. open <dir>/<entry>; resolve sco_plugin_query, sco_plugin_load, sco_plugin_unload
//   2. query() (guarded) -> info: non-null, size covers author, api_major == hostMajor,
//      api_minor <= hostMinor, name == manifest id
//   3. load(api, self) (guarded)
// Result: Loaded (true). Refused (module closed, everything self registered released) when the
// module won't open, an export is missing, info fails a check or load returns non-OK.
// Crashed (released, module kept mapped, never called again) when query or load faults, or when
// load returned non-OK and sco::Release(self) failed ("release failed: TOO_MANY"): the runtime
// may still hold the plugin's callbacks, so the module is never closed under them.
// False for anything but Loaded; reason set; one [plugin] log line either way.
bool LoadNative(Plugin& p, const sco_api* api, sco_plugin* self, const Options& opts,
                const ModuleOps& ops = PlatformModuleOps());

// Calls into a loaded plugin's code (an event callback, a command, a task, a done callback):
// the runtime guard ContainCallouts installs routes every callout of a Loaded plugin through
// this. Skips the call and returns false unless p is Loaded; runs thunk(ctx) under Guarded(); on
// a fault calls MarkCrashed(p, where, code) and returns false. Game thread only.
bool CallPlugin(Plugin& p, const char* where, void (*thunk)(void* ctx), void* ctx);

// Marks a loaded plugin crashed after a fault caught in one of its callbacks (CallPlugin calls
// it for tick, a command, a task, a done callback): sco::Release(self), state Crashed, reason
// "crashed in <where> (0x<code>)", one log line and a status message. The module stays mapped
// (its code may still be on a stack). Game thread only. No-op unless state is Loaded.
void MarkCrashed(Plugin& p, const char* where, uint32_t code);

// Installs a runtime callout guard (sco::SetCalloutGuard) that finds the Plugin in *list whose
// `self` is the callout's owner and runs the call as CallPlugin(p, where, ...), so a fault in any
// plugin callback marks that plugin Crashed and a faulting command answers SCO_CRASHED. Owners
// not in the list (host features) and plugins still Ready (calls made synchronously during their
// load or script run, already inside the load guard) are called straight through; a Crashed,
// Refused or Unloaded plugin is never called. nullptr uninstalls. Game thread only; *list must
// outlive the installation and must not be resized while installed.
void ContainCallouts(std::vector<Plugin>* list);

// Unloads one loaded native plugin: unload() (guarded; a fault marks it Crashed instead, and so
// does a fault nested inside it, such as a crashing command it invoked), sco::Release(self),
// close the module, state Unloaded. If Release fails (TooMany, or WrongThread when called off
// the game thread) the module stays mapped and the plugin is Crashed with reason
// "release failed: <RESULT>". Game thread only. No-op unless Loaded.
void UnloadNative(Plugin& p, const ModuleOps& ops = PlatformModuleOps());

// ---- script loader (kind = lua) --------------------------------------------------------------

// The script runtime the host links in (sc-offline: sco-lua, plugins/lua/sco_lua.h). The loader
// only reads the entry file and hands it over; the runtime owns the sandbox.
struct ScriptRuntime {
    // Runs the script once. SCO_OK = loaded; else err says why (NUL-terminated).
    sco_result (*load)(const sco_api* api, sco_plugin* self, const char* chunkname,
                       const char* source, size_t size, char* err, size_t errSize);
    // Frees the script. Called after sco::Release(self).
    void (*unload)(sco_plugin* self);
};

constexpr size_t kMaxScriptBytes = 1024 * 1024;   // entry script size

// Loads one Ready lua plugin. Game thread only.
//   1. read <dir>/<entry> (at most kMaxScriptBytes)
//   2. runtime.load(api, self, entry, text) (guarded like native calls)
// Loaded (true), or Refused with everything self registered released ("cannot read main.lua",
// "script too big", "main.lua:3: ..." from the runtime). A fault in the runtime: Crashed.
bool LoadScript(Plugin& p, const sco_api* api, sco_plugin* self, const ScriptRuntime& runtime);

// Unloads every Loaded plugin, last loaded first: natives as UnloadNative; scripts by
// sco::Release(self) then runtime.unload(self) (state Unloaded; if Release fails the script is
// kept and the plugin Crashed, as for natives). runtime may be null when no script was loaded.
void UnloadAll(std::vector<Plugin>& list, const ModuleOps& ops = PlatformModuleOps(),
               const ScriptRuntime* runtime = nullptr);

// ---- status ---------------------------------------------------------------------------------

// "hello 1.0.0 native loaded", "pack 1.0.0 data refused: built for api 2.0",
// "broken ? ? refused: plugin.ini: line 2: ..." (unknown fields shown as '?').
std::string Describe(const Plugin& p);
// "[plugin] <Describe>" per plugin, in list order, plus a first line
// "[plugin] N found, L loaded (plugins = on|off)".
void LogReport(const std::vector<Plugin>& list, bool enabled);

// ---- data-pack content index ----------------------------------------------------------------

enum class ContentKind : uint32_t { Mission = 0, Rules = 1, Script = 2, List = 3 };
const char* ContentKindName(ContentKind k);       // "mission", "rules", "script", "list"

struct ContentItem {
    ContentKind kind;
    std::string plugin;                           // the owning plugin id
    std::string name;                             // relative to the pack, '/' separated: "missions/a.cwmission"
    fs::path    path;                             // absolute-or-as-given file path
};

constexpr size_t kMaxPackFiles = 4096;            // per pack; more refuses the pack

// Content a data pack may carry (anything else in the folder is ignored):
//   missions/*.cwmission   rules/*.rules   scripts/**.xml (any depth)   lists/*.txt
// Extensions match case-insensitively. Symlinks are skipped (a pack can't point outside itself).
class ContentIndex {
public:
    // Indexes every Ready data pack in list order and sets it Loaded; a pack over kMaxPackFiles
    // is set Refused ("too many files") and contributes nothing. Rebuilding replaces the index.
    // Returns the number of items indexed.
    size_t Build(std::vector<Plugin>& list);
    void   Clear();

    // Every item of `kind`, in plugin order, then name order.
    std::vector<const ContentItem*> Items(ContentKind kind) const;
    // Items of `kind` whose name equals `name` (several packs may ship the same name), plugin order.
    std::vector<const ContentItem*> Find(ContentKind kind, std::string_view name) const;
    // Every item from one plugin.
    std::vector<const ContentItem*> FromPlugin(std::string_view plugin) const;
    size_t Size() const { return items_.size(); }

private:
    std::vector<ContentItem> items_;
};

}  // namespace sco::plugins
