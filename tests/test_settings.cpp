// Unit tests for the sco.settings service (sco/settings.h, sco_settings.h, scosdk/settings.hpp):
// the declared defaults through the real host table, typed sets with their checks, the event
// "settings.changed" (once per change), values kept in sco.storage across a restart of the
// services, kept values that no longer fit a declaration, release and stop, readers on threads
// beside the game thread's sets. Host build, no game; this test's main thread is the game thread.
//   test_settings <out dir>                      (tools/test.sh, CTest test_settings)
#include "sco/host.h"
#include "sco/log.h"
#include "sco/plugins.h"
#include "sco/runtime.h"
#include "sco/settings.h"
#include "sco/storage.h"
#include "sco/ui.h"
#include "sco_api.h"
#include "sco_settings.h"
#include "sco_storage.h"
#include "sco_ui.h"
#include "scosdk/settings.hpp"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace P = sco::plugins;
using sco::Result;

static std::atomic<int> g_fail{ 0 }, g_pass{ 0 };
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const sco_api* g_api = nullptr;
static fs::path       g_root;

static std::vector<std::string> g_log;
static void Sink(const char* line) { g_log.emplace_back(line); }
static bool Logged(const char* needle) {
    for (const auto& l : g_log) if (l.find(needle) != std::string::npos) return true;
    return false;
}

// A plugin that declares one setting of each type.
static const char* const kIni =
    "id = pilot\nname = Pilot\nversion = 1.0.0\napi = 1.0\nkind = native\nentry = pilot.dll\n"
    "[settings]\n"
    "god_mode = bool   default false label \"God mode\" help \"No damage\"\n"
    "speed    = int    default 5 min 1 max 10\n"
    "fov      = float  default 90 min 60 max 120\n"
    "nick     = string default \"Pilot\"\n"
    "mode     = enum(easy,normal,hard) default normal\n";

static P::Manifest Parse(const char* ini) {
    P::Manifest m;
    std::string err;
    if (!P::ParseManifest(ini, m, err)) std::printf("test bug: %s\n", err.c_str());
    return m;
}

static sco::storage::Options StorageOpts() {
    sco::storage::Options o;
    o.dataRoot = g_root;
    return o;
}

static const sco_settings_v1* Query() {
    const void* t = nullptr;
    return g_api->query_service(SCO_SETTINGS_NAME, SCO_SETTINGS_VERSION_1_0, &t) == SCO_OK
               ? static_cast<const sco_settings_v1*>(t) : nullptr;
}

static std::string LastError(const sco_settings_v1* st, sco_plugin* p) {
    char buf[256];
    uint32_t n = sizeof(buf);
    return st->last_error(p, buf, &n) == SCO_OK ? std::string(buf) : "<none>";
}

// The kept text of a setting in the plugin's storage namespace; "<none>" when there is none.
static std::string Kept(sco_plugin* p, const char* name) {
    char buf[256];
    uint32_t n = sizeof(buf);
    const std::string key = sco::settings::StorageKey(name);
    return sco::storage::Table()->get(p, key.c_str(), buf, &n) == SCO_OK ? std::string(buf, n) : "<none>";
}

// "settings.changed" events seen by a watcher plugin: "<plugin>.<name>".
static std::vector<std::string> g_events;
static void OnChanged(const char*, const void* data, void*) {
    const sco_settings_changed* c = sco::sdk::AsSettingsChanged(data);
    if (c) g_events.push_back(std::string(c->plugin) + "." + c->name);
    else g_events.push_back("<bad event data>");
}

// Starts the services over a fresh view of the storage folder, as a host start does.
static void StartServices(bool withStorage = true) {
    if (withStorage) CHECK(sco::storage::Start(StorageOpts()) == Result::Ok);
    CHECK(sco::ui::Start() == Result::Ok);
    CHECK(sco::settings::Start() == Result::Ok);
}
static void StopServices() {
    sco::settings::Stop();
    sco::ui::Stop();
    sco::storage::Stop();
}

// ---- defaults and reads ------------------------------------------------------------------------

static void TestDefaults() {
    const sco_settings_v1* st = Query();
    CHECK(st && st->size == sizeof(sco_settings_v1));
    if (!st) return;
    const P::Manifest m = Parse(kIni);
    CHECK(m.settings.size() == 5);
    sco_plugin* p = sco::host::NewPlugin("pilot");
    CHECK(sco::settings::Declare(p, m.settings) == Result::Ok);

    int32_t b = -1;
    int64_t i = -1;
    double f = -1;
    char s[64];
    uint32_t n = sizeof(s);
    CHECK(st->get_bool(p, "god_mode", &b) == SCO_OK && b == 0);
    CHECK(st->get_int(p, "speed", &i) == SCO_OK && i == 5);
    CHECK(st->get_float(p, "fov", &f) == SCO_OK && f == 90.0);
    CHECK(st->get_string(p, "nick", s, &n) == SCO_OK && std::strcmp(s, "Pilot") == 0 && n == 6);
    n = sizeof(s);
    CHECK(st->get_string(p, "mode", s, &n) == SCO_OK && std::strcmp(s, "normal") == 0);

    // The size handshake: ask, then fit; too small says how much.
    n = 0;
    CHECK(st->get_string(p, "nick", nullptr, &n) == SCO_TOO_MANY && n == 6);
    n = 3;
    CHECK(st->get_string(p, "nick", s, &n) == SCO_TOO_MANY && n == 6);

    // Refusals leave *out alone and say why.
    i = 99;
    CHECK(st->get_int(p, "god_mode", &i) == SCO_BAD_ARG && i == 99);
    CHECK(LastError(st, p) == "setting 'god_mode' is bool, not an int");
    CHECK(st->get_int(p, "mode", &i) == SCO_BAD_ARG && st->get_float(p, "speed", &f) == SCO_BAD_ARG);
    CHECK(st->get_bool(p, "speed", &b) == SCO_BAD_ARG);
    n = sizeof(s);
    CHECK(st->get_string(p, "speed", s, &n) == SCO_BAD_ARG && n == 0);
    CHECK(st->get_int(p, "nope", &i) == SCO_NOT_FOUND);
    CHECK(LastError(st, p) == "this plugin declares no setting 'nope'");
    CHECK(st->get_int(p, nullptr, &i) == SCO_BAD_ARG && st->get_int(p, "speed", nullptr) == SCO_BAD_ARG);
    CHECK(st->get_int(p, "a_name_longer_than_thirty_one_bytes", &i) == SCO_NOT_FOUND);
    CHECK(st->get_int(reinterpret_cast<sco_plugin*>(&b), "speed", &i) == SCO_BAD_ARG);   // not a handle NewPlugin made

    // A plugin that declared nothing finds nothing, and cannot see another plugin's settings.
    sco_plugin* other = sco::host::NewPlugin("other");
    CHECK(st->get_int(other, "speed", &i) == SCO_NOT_FOUND);

    // The C++ wrapper.
    sco::sdk::Settings w;
    CHECK(w.Open(g_api, p) == SCO_OK && w);
    bool wb = true;
    int64_t wi = 0;
    double wf = 0;
    std::string ws;
    CHECK(w.GetBool("god_mode", wb) == SCO_OK && !wb);
    CHECK(w.GetInt("speed", wi) == SCO_OK && wi == 5);
    CHECK(w.GetFloat("fov", wf) == SCO_OK && wf == 90.0);
    CHECK(w.GetString("mode", ws) == SCO_OK && ws == "normal");
    CHECK(w.Int("speed", 77) == 5 && w.Int("nope", 77) == 77 && w.Int("god_mode", 77) == 77);
    CHECK(w.String("nick", "x") == "Pilot" && w.String("nope", "x") == "x" && w.Bool("nope", true));
    CHECK(w.GetInt("god_mode", wi) == SCO_BAD_ARG && w.LastError() == "setting 'god_mode' is bool, not an int");
    sco::sdk::Settings none;
    CHECK(!none && none.GetInt("speed", wi) == SCO_UNAVAILABLE && none.Int("speed", 3) == 3);
    CHECK(sco::sdk::AsSettingsChanged(nullptr) == nullptr);
    const sco_settings_changed tiny{ 8, 0, nullptr, nullptr };
    CHECK(sco::sdk::AsSettingsChanged(&tiny) == nullptr);

    // Declare refuses what it can't keep.
    CHECK(sco::settings::Declare(p, m.settings) == Result::BadArg);                 // twice
    CHECK(sco::settings::Declare(other, {}) == Result::BadArg);                     // nothing to declare
    P::Setting twice = m.settings[1];
    CHECK(sco::settings::Declare(other, { twice, twice }) == Result::BadArg);       // a name twice
    P::Setting bad = m.settings[1];
    bad.def.i = 11;                                                                 // outside its own range
    CHECK(sco::settings::Declare(other, { bad }) == Result::BadArg);
    CHECK(sco::settings::Declare(reinterpret_cast<sco_plugin*>(&b), m.settings) == Result::BadArg);
    CHECK(sco::Release(other) == Result::Ok);
    CHECK(sco::settings::Declare(other, m.settings) == Result::BadArg);             // released
    CHECK(sco::Release(p) == Result::Ok);
}

// ---- pages, for the product's draw path ----------------------------------------------------------

static void TestPages() {
    const P::Manifest m = Parse(kIni);
    sco_plugin* p = sco::host::NewPlugin("pilot");
    CHECK(sco::settings::Declare(p, m.settings) == Result::Ok);
    auto pages = sco::settings::Pages();
    CHECK(pages.size() == 1 && pages[0].plugin == "pilot" && pages[0].tab.empty() && pages[0].entries.size() == 5);
    CHECK(pages[0].entries[0].decl.name == "god_mode" && pages[0].entries[0].decl.label == "God mode" &&
          pages[0].entries[0].decl.help == "No damage");
    CHECK(pages[0].entries[1].decl.type == P::SettingType::Int && pages[0].entries[1].value.i == 5 &&
          pages[0].entries[1].decl.hasMin && pages[0].entries[1].decl.maxI == 10 && pages[0].entries[1].decl.label == "speed");
    CHECK((pages[0].entries[4].decl.choices == std::vector<std::string>{ "easy", "normal", "hard" }));
    CHECK(sco::settings::Entries("pilot").size() == 5 && sco::settings::Entries("nobody").empty() && sco::settings::Entries(nullptr).empty());

    // The plugin's first sco.ui tab (by order) is the page's tab.
    const void* t = nullptr;
    CHECK(g_api->query_service(SCO_UI_NAME, SCO_UI_VERSION_1_0, &t) == SCO_OK);
    const sco_ui_v1* ui = static_cast<const sco_ui_v1*>(t);
    auto draw = [](void*, void*) {};
    CHECK(ui->register_tab(p, "pilot.later", "Later", 20, draw, nullptr) == SCO_OK);
    CHECK(ui->register_tab(p, "pilot.main", "Main", 10, draw, nullptr) == SCO_OK);
    pages = sco::settings::Pages();
    CHECK(pages.size() == 1 && pages[0].tab == "pilot.main");
    CHECK(sco::Release(p) == Result::Ok);
    CHECK(sco::settings::Pages().empty());
}

// ---- sets, the event, persistence --------------------------------------------------------------------

static void TestSetAndRestart() {
    const P::Manifest m = Parse(kIni);
    sco_plugin* watcher = sco::host::NewPlugin("watcher");
    CHECK(g_api->subscribe(watcher, SCO_SETTINGS_CHANGED_EVENT, OnChanged, nullptr) == SCO_OK);
    sco_plugin* p = sco::host::NewPlugin("pilot");
    CHECK(sco::settings::Declare(p, m.settings) == Result::Ok);
    const sco_settings_v1* st = Query();
    sco::sdk::Settings w;
    CHECK(w.Open(g_api, p) == SCO_OK);
    g_events.clear();

    // One event per change, none for a set to the current value or a refused set.
    CHECK(sco::settings::SetInt("pilot", "speed", 7) == Result::Ok);
    CHECK(w.Int("speed") == 7 && g_events == std::vector<std::string>{ "pilot.speed" });
    CHECK(sco::settings::SetInt("pilot", "speed", 7) == Result::Ok && g_events.size() == 1);
    CHECK(sco::settings::SetInt("pilot", "speed", 11) == Result::BadArg && w.Int("speed") == 7 && g_events.size() == 1);
    CHECK(sco::settings::SetInt("pilot", "speed", 0) == Result::BadArg && g_events.size() == 1);
    CHECK(sco::settings::SetBool("pilot", "speed", true) == Result::BadArg);            // another type
    CHECK(sco::settings::SetFloat("pilot", "speed", 5.0) == Result::BadArg);
    CHECK(sco::settings::SetInt("pilot", "nope", 1) == Result::NotFound);
    CHECK(sco::settings::SetInt("nobody", "speed", 1) == Result::NotFound);
    CHECK(sco::settings::SetInt(nullptr, "speed", 1) == Result::BadArg && sco::settings::SetInt("pilot", nullptr, 1) == Result::BadArg);
    CHECK(sco::settings::SetBool("pilot", "god_mode", true) == Result::Ok && w.Bool("god_mode") && g_events.size() == 2);
    CHECK(sco::settings::SetFloat("pilot", "fov", 100.5) == Result::Ok && w.Float("fov") == 100.5 && g_events.size() == 3);
    CHECK(sco::settings::SetFloat("pilot", "fov", 120.5) == Result::BadArg && sco::settings::SetFloat("pilot", "fov", 59.9) == Result::BadArg);
    CHECK(sco::settings::SetFloat("pilot", "fov", std::nan("")) == Result::BadArg);
    CHECK(sco::settings::SetString("pilot", "nick", "Ace of Spades") == Result::Ok && w.String("nick") == "Ace of Spades");
    CHECK(sco::settings::SetString("pilot", "nick", "bad\ttext") == Result::BadArg);
    CHECK(sco::settings::SetString("pilot", "nick", std::string(256, 'x').c_str()) == Result::BadArg);
    CHECK(sco::settings::SetString("pilot", "nick", nullptr) == Result::BadArg);
    CHECK(sco::settings::SetString("pilot", "mode", "hard") == Result::Ok && w.String("mode") == "hard");
    CHECK(sco::settings::SetString("pilot", "mode", "brutal") == Result::BadArg && w.String("mode") == "hard");
    CHECK(sco::settings::SetString("pilot", "speed", "7") == Result::BadArg);
    CHECK((g_events == std::vector<std::string>{ "pilot.speed", "pilot.god_mode", "pilot.fov", "pilot.nick", "pilot.mode" }));
    CHECK(sco::settings::Entries("pilot")[1].value.i == 7);

    // The values are in the plugin's own storage namespace, typed.
    CHECK(Kept(p, "speed") == "int:7" && Kept(p, "god_mode") == "bool:true" && Kept(p, "fov") == "float:100.5");
    CHECK(Kept(p, "nick") == "string:Ace of Spades" && Kept(p, "mode") == "enum:hard");

    // A set from another thread is refused (events run on the game thread).
    std::atomic<int> fromThread{ -1 };
    std::thread([&] { fromThread = static_cast<int>(sco::settings::SetInt("pilot", "speed", 3)); }).join();
    CHECK(fromThread == static_cast<int>(Result::WrongThread) && w.Int("speed") == 7);

    // Restart the host kit: unload, stop, start over the same folder, load again.
    CHECK(sco::Release(p) == Result::Ok);
    int64_t gone = 0;
    CHECK(st->get_int(p, "speed", &gone) == SCO_BAD_ARG);
    CHECK(sco::settings::Entries("pilot").empty());
    StopServices();
    StartServices();
    sco_plugin* again = sco::host::NewPlugin("pilot");
    CHECK(sco::settings::Declare(again, m.settings) == Result::Ok);
    sco::sdk::Settings w2;
    CHECK(w2.Open(g_api, again) == SCO_OK);
    CHECK(w2.Int("speed") == 7 && w2.Bool("god_mode") && w2.Float("fov") == 100.5);
    CHECK(w2.String("nick") == "Ace of Spades" && w2.String("mode") == "hard");
    CHECK(!Logged("kept value dropped"));
    g_events.clear();
    CHECK(sco::settings::SetInt("pilot", "speed", 8) == Result::Ok && g_events.size() == 1);   // the watcher kept listening
    CHECK(sco::Release(again) == Result::Ok);
    CHECK(sco::Release(watcher) == Result::Ok);
}

// ---- kept values that no longer fit ---------------------------------------------------------------------

static void TestStale() {
    // The previous test left speed = 8 and the rest as set. An update of the plugin changes
    // speed to a string, narrows fov, drops the choice "hard" and re-types god_mode.
    const char* const kNew =
        "id = pilot\nname = Pilot\nversion = 2.0.0\napi = 1.0\nkind = native\nentry = pilot.dll\n"
        "[settings]\n"
        "god_mode = int    default 1\n"
        "speed    = string default \"fast\"\n"
        "fov      = float  default 70 min 60 max 80\n"
        "nick     = string default \"Pilot\"\n"
        "mode     = enum(easy,normal) default easy\n";
    g_log.clear();
    sco_plugin* p = sco::host::NewPlugin("pilot");
    CHECK(sco::settings::Declare(p, Parse(kNew).settings) == Result::Ok);
    sco::sdk::Settings w;
    CHECK(w.Open(g_api, p) == SCO_OK);
    CHECK(w.Int("god_mode", -1) == 1);                        // was bool:true
    CHECK(w.String("speed") == "fast");                       // was int:8
    CHECK(w.Float("fov") == 70.0);                            // was float:100.5, above the new max
    CHECK(w.String("mode") == "easy");                        // was enum:hard
    CHECK(w.String("nick") == "Ace of Spades");               // unchanged declaration: kept
    CHECK(Logged("[settings] pilot.god_mode: kept value dropped (it was saved as bool, the setting is int now)"));
    CHECK(Logged("[settings] pilot.speed: kept value dropped (it was saved as int, the setting is string now)"));
    CHECK(Logged("[settings] pilot.fov: kept value dropped (100.5 is above max 80)"));
    CHECK(Logged("[settings] pilot.mode: kept value dropped ('hard' is not one of easy, normal)"));
    CHECK(!Logged("pilot.nick: kept value dropped"));
    // Dropped values are deleted, so the next start doesn't repeat the line.
    CHECK(Kept(p, "god_mode") == "<none>" && Kept(p, "speed") == "<none>" && Kept(p, "fov") == "<none>" && Kept(p, "mode") == "<none>");
    CHECK(Kept(p, "nick") == "string:Ace of Spades");

    // A value somebody wrote by hand: no type, junk, over-long.
    const sco_storage_v1* store = sco::storage::Table();
    const std::string huge(600, '7');
    CHECK(store->put(p, "sco.settings.speed", "banana", 6) == SCO_OK);
    CHECK(store->put(p, "sco.settings.fov", huge.data(), static_cast<uint32_t>(huge.size())) == SCO_OK);
    CHECK(store->put(p, "sco.settings.mode", "enum:normal", 11) == SCO_OK);
    CHECK(sco::Release(p) == Result::Ok);
    g_log.clear();
    p = sco::host::NewPlugin("pilot");
    CHECK(sco::settings::Declare(p, Parse(kNew).settings) == Result::Ok);
    CHECK(w.Open(g_api, p) == SCO_OK);
    CHECK(w.String("speed") == "fast" && w.Float("fov") == 70.0 && w.String("mode") == "normal");
    CHECK(Logged("[settings] pilot.speed: kept value dropped (it is not <type>:<value>)"));
    CHECK(Logged("[settings] pilot.fov: kept value dropped (it is over 511 bytes)"));
    CHECK(!Logged("pilot.mode: kept value dropped"));
    CHECK(sco::Release(p) == Result::Ok);
}

// ---- no storage: values live in memory -----------------------------------------------------------------

static void TestMemoryOnly() {
    StopServices();
    StartServices(false);   // no sco.storage
    CHECK(!sco::storage::Started());
    sco_plugin* p = sco::host::NewPlugin("pilot");
    const P::Manifest m = Parse(kIni);
    CHECK(sco::settings::Declare(p, m.settings) == Result::Ok);
    sco::sdk::Settings w;
    CHECK(w.Open(g_api, p) == SCO_OK && w.Int("speed") == 5);       // defaults: the old files are not read
    g_events.clear();
    sco_plugin* watcher = sco::host::NewPlugin("watcher");
    CHECK(g_api->subscribe(watcher, SCO_SETTINGS_CHANGED_EVENT, OnChanged, nullptr) == SCO_OK);
    CHECK(sco::settings::SetInt("pilot", "speed", 9) == Result::Ok && w.Int("speed") == 9 && g_events.size() == 1);
    CHECK(sco::Release(p) == Result::Ok && sco::Release(watcher) == Result::Ok);
    // The same plugin loaded again in memory-only mode starts from its defaults again.
    p = sco::host::NewPlugin("pilot");
    CHECK(sco::settings::Declare(p, m.settings) == Result::Ok);
    CHECK(w.Open(g_api, p) == SCO_OK && w.Int("speed") == 5);
    CHECK(sco::Release(p) == Result::Ok);
    StopServices();
    StartServices();
}

// ---- readers beside sets --------------------------------------------------------------------------------

static void TestThreads() {
    const P::Manifest m = Parse(kIni);
    sco_plugin* p = sco::host::NewPlugin("pilot");
    CHECK(sco::settings::Declare(p, m.settings) == Result::Ok);
    // Kept values from the earlier tests are still in storage: start from known ones.
    CHECK(sco::settings::SetString("pilot", "nick", "Pilot") == Result::Ok && sco::settings::SetInt("pilot", "speed", 1) == Result::Ok);
    std::atomic<bool> stop{ false };
    std::atomic<int> bad{ 0 };
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t)
        readers.emplace_back([&] {
            sco::sdk::Settings w;
            if (w.Open(g_api, p) != SCO_OK) { ++bad; return; }
            while (!stop) {
                int64_t v = 0;
                std::string s;
                if (w.GetInt("speed", v) != SCO_OK || v < 1 || v > 10) ++bad;
                if (w.GetString("nick", s) != SCO_OK || (s != "Pilot" && s != "Ace")) ++bad;
            }
        });
    for (int k = 0; k < 60; ++k) {
        CHECK(sco::settings::SetInt("pilot", "speed", 1 + k % 10) == Result::Ok);
        CHECK(sco::settings::SetString("pilot", "nick", k % 2 ? "Ace" : "Pilot") == Result::Ok);
    }
    stop = true;
    for (auto& t : readers) t.join();
    CHECK(bad == 0);
    CHECK(sco::Release(p) == Result::Ok);
}

// ---- stop -----------------------------------------------------------------------------------------------

static void TestStop() {
    const P::Manifest m = Parse(kIni);
    sco_plugin* p = sco::host::NewPlugin("pilot");
    CHECK(sco::settings::Declare(p, m.settings) == Result::Ok);
    const sco_settings_v1* st = Query();
    sco::settings::Stop();
    CHECK(!sco::settings::Started());
    const void* t = nullptr;
    CHECK(g_api->query_service(SCO_SETTINGS_NAME, SCO_SETTINGS_VERSION_1_0, &t) == SCO_NOT_FOUND);
    int64_t i = 0;
    CHECK(st->get_int(p, "speed", &i) == SCO_UNAVAILABLE);
    CHECK(sco::settings::SetInt("pilot", "speed", 2) == Result::Unavailable);
    CHECK(sco::settings::Declare(p, m.settings) == Result::Unavailable);
    CHECK(sco::settings::Entries("pilot").empty() && sco::settings::Pages().empty());
    sco::settings::Stop();   // no-op
    CHECK(sco::Release(p) == Result::Ok);
    // A fresh start: empty.
    CHECK(sco::settings::Start() == Result::Ok && sco::settings::Entries("pilot").empty());
    CHECK(sco::settings::Start() == Result::BadArg);   // already started
    sco::settings::Stop();
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: test_settings <out dir>\n");
        return 2;
    }
    g_root = fs::path(argv[1]) / "settings_root";
    std::error_code ec;
    fs::remove_all(g_root, ec);
    fs::create_directories(g_root, ec);
    sco::SetLogSink(Sink);
    sco::SetGameThread();
    g_api = sco::host::BuildApi({ "test_settings" });

    CHECK(Query() == nullptr);   // not started yet
    StartServices();
    TestDefaults();
    TestPages();
    TestSetAndRestart();
    TestStale();
    TestMemoryOnly();
    TestThreads();
    StopServices();
    StartServices();
    TestStop();
    sco::ui::Stop();
    sco::storage::Stop();
    std::printf("sco-core settings tests: %d passed, %d failed\n", g_pass.load(), g_fail.load());
    return g_fail.load() ? 1 : 0;
}
