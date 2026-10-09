// Unit tests for plugin discovery, plugin.ini parsing, the native loader and the content index.
// Host build, no game. The native tests load real shared libraries built from
// tests/fixtures/plugins/native/fake_plugin.c (tools/test.sh builds them into tests/out/plugins/).
// Off Windows there is no SEH, so the crash tests install a guard that turns SIGSEGV/SIGBUS in
// plugin code into a fault code with sigsetjmp/siglongjmp; the Windows run (tools/test-win.sh)
// uses the real __try guard.
//   tools/test.sh
#include "sco/plugins.h"
#include "sco/log.h"
#include "sco/runtime.h"
#include "sco/status.h"
#include "sco_api.h"
#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace P = sco::plugins;
using P::State;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::vector<std::string> g_log;
static void Sink(const char* line) { g_log.emplace_back(line); }
static bool Logged(const char* needle) {
    for (const auto& l : g_log) if (l.find(needle) != std::string::npos) return true;
    return false;
}

static fs::path g_fixtures, g_out;

// ---- manifest -------------------------------------------------------------------------------

static bool Parse(const char* text, P::Manifest& m, std::string& err) { return P::ParseManifest(text, m, err); }
static std::string ParseError(const char* text) {
    P::Manifest m; std::string err;
    return Parse(text, m, err) ? std::string("<ok>") : err;
}

static void TestManifest() {
    P::Manifest m; std::string err;
    CHECK(Parse("\xEF\xBB\xBF; header comment\r\n"
                "id = hello\r\n"
                "name = Hello World ; trailing comment\r\n"
                "version = 1.0.0\r\n"
                "\r\n"
                "author = you\r\n"
                "api = 1.0\r\n"
                "kind = native\r\n"
                "entry = hello.dll\r\n"
                "requires = teleport,  spawn.ship\r\n"
                "future_key = ignored\r\n", m, err));
    CHECK(err.empty());
    CHECK(m.id == "hello" && m.name == "Hello World" && m.version == "1.0.0" && m.author == "you");
    CHECK(m.apiMajor == 1 && m.apiMinor == 0 && m.kind == P::Kind::Native && m.entry == "hello.dll");
    CHECK((m.requires_ == std::vector<std::string>{ "teleport", "spawn.ship" }));

    // '#' and ';' inside a value (no whitespace before) are part of it.
    CHECK(Parse("id=c\nname=C#;x\nversion=1\napi=1.2\nkind=data\n", m, err) && m.name == "C#;x" && m.apiMinor == 2);
    CHECK(m.kind == P::Kind::Data && m.entry.empty() && m.author.empty() && m.requires_.empty());
    CHECK(Parse("id=l\nname=L\nversion=1\napi=1.0\nkind=lua\nentry=main.lua", m, err) && m.kind == P::Kind::Lua);

    const char* base = "name=N\nversion=1\napi=1.0\nkind=data\n";
    auto with = [&](const char* id) { return ParseError((std::string("id=") + id + "\n" + base).c_str()); };
    CHECK(with("hello") == "<ok>");
    CHECK(with("a_1") == "<ok>");
    CHECK(with("Hello").find("id must be") != std::string::npos);
    CHECK(with("he-llo").find("id must be") != std::string::npos);
    CHECK(with("he.llo").find("id must be") != std::string::npos);
    CHECK(with("abcdefghijabcdefghijabcdefghij1") == "<ok>");                       // 31
    CHECK(with("abcdefghijabcdefghijabcdefghij12").find("id must be") != std::string::npos);  // 32
    CHECK(with("") .find("id must be") != std::string::npos);
    for (const char* r : { "sco", "host", "menu", "game" }) CHECK(with(r) == "line 1: id is reserved");

    CHECK(ParseError("id=a\nname=N\nversion=1\napi=1.0\n").find("missing key 'kind'") != std::string::npos);
    CHECK(ParseError("name=N\nversion=1\napi=1.0\nkind=data\n").find("missing key 'id'") != std::string::npos);
    CHECK(ParseError("id=a\nid=b\n") == "line 2: duplicate key 'id'");
    CHECK(ParseError("id=a\njust words\n") == "line 2: expected key = value");
    CHECK(ParseError("id=a\nname=N\nversion=1\napi=1\nkind=data\n") == "line 4: api must be <major>.<minor>");
    CHECK(ParseError("id=a\nname=N\nversion=1\napi=1.x\nkind=data\n") == "line 4: api must be <major>.<minor>");
    CHECK(ParseError("id=a\nname=N\nversion=1\napi=1.65536\nkind=data\n") == "line 4: api must be <major>.<minor>");
    CHECK(ParseError("id=a\nname=N\nversion=1\napi=1.0\nkind=python\n") == "line 5: kind must be native, lua or data");
    CHECK(ParseError("id=a\nname=N\nversion=1\napi=1.0\nkind=data\nentry=x.dll\n") == "a data pack has no entry");
    CHECK(ParseError("id=a\nname=N\nversion=1\napi=1.0\nkind=native\n") == "missing key 'entry'");
    for (const char* e : { "../x.dll", "sub/x.dll", "sub\\x.dll", "C:x.dll", "..", "." })
        CHECK(ParseError((std::string("id=a\nname=N\nversion=1\napi=1.0\nkind=native\nentry=") + e + "\n").c_str())
              == "line 6: entry must be a file name in the plugin folder");
    CHECK(ParseError((std::string(base) + "id=a\nrequires=Teleport\n").c_str()).find("requires must be") != std::string::npos);
    CHECK(ParseError((std::string(base) + "id=a\nrequires=a,,b\n").c_str()).find("requires must be") != std::string::npos);
    CHECK(ParseError((std::string(base) + "id=a\nrequires=a.\n").c_str()).find("requires must be") != std::string::npos);
    CHECK(ParseError((std::string(base) + "id=a\nrequires=a, a\n").c_str()).find("twice") != std::string::npos);
    std::string many = std::string(base) + "id=a\nrequires=c0";
    for (int i = 1; i < 16; ++i) many += ",c" + std::to_string(i);
    CHECK(ParseError((many + "\n").c_str()) == "<ok>");
    CHECK(ParseError((many + ",c16\n").c_str()).find("more than 16") != std::string::npos);
    CHECK(ParseError("id=a\nname=\x01\nversion=1\napi=1.0\nkind=data\n").find("name must be") != std::string::npos);
    CHECK(ParseError("id=a\nname=\nversion=1\napi=1.0\nkind=data\n").find("name must be") != std::string::npos);

    std::string big = "id=a\nname=N\nversion=1\napi=1.0\nkind=data\n";
    big.append(P::kMaxManifestBytes - big.size(), '\n');
    CHECK(ParseError(big.c_str()) == "<ok>");
    big += "\n";
    CHECK(ParseError(big.c_str()) == "too big");
    // UTF-16 (either byte order) is refused with a clear message, not "missing key 'id'".
    CHECK(!P::ParseManifest(std::string_view("\xFF\xFEi\0d\0=\0a\0", 10), m, err) && err == "plugin.ini must be UTF-8");
    CHECK(!P::ParseManifest(std::string_view("\xFE\xFF\0i\0d", 6), m, err) && err == "plugin.ini must be UTF-8");
    // A failed parse leaves no half-filled manifest behind.
    CHECK(!Parse("id=keep\nname=", m, err) && m.id.empty());
}

// ---- discovery ------------------------------------------------------------------------------

static const P::Plugin* Find(const std::vector<P::Plugin>& list, const char* folder) {
    for (const auto& p : list) if (p.folder == folder) return &p;
    return nullptr;
}

static int HasTeleportOnly(const char* cap) { return std::strcmp(cap, "teleport") == 0; }
static int HasAll(const char*) { return 1; }

static void TestDiscover() {
    const fs::path tree = g_fixtures / "tree";
    CHECK(P::Discover(g_fixtures / "does_not_exist", {}).empty());
    CHECK(P::Discover(tree / "README.md", {}).empty());

    // Off: every folder with a plugin.ini is listed, nothing is refused or loaded.
    P::Options off;
    auto list = P::Discover(tree, off);
    CHECK(list.size() == 11);
    bool allOff = true;
    for (const auto& p : list) allOff = allOff && p.state == State::Off;
    CHECK(allOff);
    CHECK(!Find(list, "noini"));
    bool sorted = true;
    for (size_t i = 1; i < list.size(); ++i) sorted = sorted && list[i - 1].folder < list[i].folder;
    CHECK(sorted);

    P::Options on;
    on.enabled = true;
    on.has = HasTeleportOnly;
    list = P::Discover(tree, on);
    CHECK(list.size() == 11);
    auto state = [&](const char* f) { const auto* p = Find(list, f); return p ? p->state : State::Unloaded; };
    auto reason = [&](const char* f) { const auto* p = Find(list, f); return p ? p->reason : std::string("<none>"); };
    CHECK(state("hello") == State::Ready);
    CHECK(state("pack") == State::Ready && Find(list, "pack")->manifest.name == "Example pack");
    CHECK(state("zpack") == State::Ready);
    CHECK(state("luamod") == State::Ready && Find(list, "luamod")->manifest.kind == P::Kind::Lua);
    CHECK(state("offswitch") == State::Disabled);
    CHECK(reason("badapi") == "built for api 2.0");
    CHECK(reason("newminor") == "built for api 1.9");
    CHECK(reason("mismatch") == "id 'other' does not match folder 'mismatch'");
    CHECK(reason("broken") == "plugin.ini: line 3: expected key = value");
    CHECK(!Find(list, "broken")->manifestOk);
    CHECK(reason("needcap") == "missing capability 'spawn.ship'");
    CHECK(reason("noentry") == "entry 'gone.dll' not found");
    for (const char* f : { "badapi", "newminor", "mismatch", "broken", "needcap", "noentry" }) CHECK(state(f) == State::Refused);

    on.has = HasAll;
    CHECK(Find(P::Discover(tree, on), "needcap")->state == State::Ready);
    on.has = nullptr;   // no capability check installed: every requirement is missing
    CHECK(Find(P::Discover(tree, on), "needcap")->reason == "missing capability 'teleport'");

    // A host on api 1.9 takes the 1.9 plugin; api 2 hosts refuse every 1.x plugin.
    on.hostMinor = 9;
    CHECK(Find(P::Discover(tree, on), "newminor")->state == State::Ready);
    on.hostMajor = 2;
    CHECK(Find(P::Discover(tree, on), "pack")->reason == "built for api 1.0");

    // Status lines.
    CHECK(P::Describe(*Find(list, "hello")) == "hello 1.0.0 native ready");
    CHECK(P::Describe(*Find(list, "badapi")) == "badapi 1 data refused: built for api 2.0");
    CHECK(P::Describe(*Find(list, "broken")) == "broken ? ? refused: plugin.ini: line 3: expected key = value");
    CHECK(P::Describe(*Find(list, "offswitch")) == "offswitch 1 data disabled");
    g_log.clear();
    P::LogReport(list, true);
    CHECK(g_log.size() == 12 && g_log[0] == "[plugin] 11 found, 0 loaded (plugins = on)");

    // A folder whose id belongs to a built-in is refused, whatever its kind (#11); a disabled one
    // stays disabled.
    P::Options withBuiltins;
    withBuiltins.enabled = true;
    withBuiltins.has = HasAll;
    const P::Builtin builtins[] = { { "hello", nullptr, nullptr, nullptr }, { "pack", nullptr, nullptr, nullptr },
                                    { "offswitch", nullptr, nullptr, nullptr } };
    withBuiltins.builtins = builtins;
    withBuiltins.nBuiltins = 3;
    list = P::Discover(tree, withBuiltins);
    CHECK(state("hello") == State::Refused && reason("hello") == "the id belongs to a built-in plugin");
    CHECK(state("pack") == State::Refused && reason("pack") == "the id belongs to a built-in plugin");
    CHECK(state("offswitch") == State::Disabled);
    CHECK(state("needcap") == State::Ready);
}

// Duplicate ids and the plugin cap need generated trees.
static void WriteFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

static void TestDiscoverLimits() {
    const fs::path root = g_out / "limits";
    fs::remove_all(root);
    for (size_t i = 0; i < P::kMaxPlugins + 2; ++i) {
        char id[16];
        std::snprintf(id, sizeof(id), "p%03zu", i);
        WriteFile(root / id / "plugin.ini", std::string("id=") + id + "\nname=N\nversion=1\napi=1.0\nkind=data\n");
    }
    P::Options on;
    on.enabled = true;
    auto list = P::Discover(root, on);
    CHECK(list.size() == P::kMaxPlugins + 2);
    CHECK(list[P::kMaxPlugins - 1].state == State::Ready);
    CHECK(list[P::kMaxPlugins].state == State::Refused && list[P::kMaxPlugins].reason == "too many plugins");
    // With plugins off every folder is listed Off, past the cap too.
    const auto offList = P::Discover(root, P::Options{});
    CHECK(offList.size() == P::kMaxPlugins + 2 && offList.back().state == State::Off);

    // Oversized plugin.ini and a symlinked folder.
    fs::remove_all(root);
    WriteFile(root / "big" / "plugin.ini", std::string(P::kMaxManifestBytes + 100, '\n'));
    WriteFile(root / "real" / "plugin.ini", "id=real\nname=N\nversion=1\napi=1.0\nkind=data\n");
    std::error_code ec;
    fs::create_directory_symlink(root / "real", root / "link", ec);
    list = P::Discover(root, on);
    if (ec) std::printf("SKIP: symlinked plugin folder (cannot create symlink: %s)\n", ec.message().c_str());
    else CHECK(list.size() == 2);   // "link" skipped
    CHECK(Find(list, "big") && Find(list, "big")->reason == "plugin.ini: too big");
    CHECK(Find(list, "real") && Find(list, "real")->state == State::Ready);
    // Any entry named "disabled" switches a plugin off, a folder too.
    fs::create_directories(root / "real" / "disabled");
    list = P::Discover(root, on);
    CHECK(Find(list, "real") && Find(list, "real")->state == State::Disabled);

    // "sco" is the host's id (its services are sco.<name>): a folder by that name, or by another
    // reserved id, is refused whatever its plugin.ini says, a disabled one too. Off still lists it.
    WriteFile(root / "sco" / "plugin.ini", "id=sco\nname=N\nversion=1\napi=1.0\nkind=data\n");
    WriteFile(root / "sco" / "disabled", "");
    WriteFile(root / "game" / "plugin.ini", "id=other\nname=N\nversion=1\napi=1.0\nkind=data\n");
    list = P::Discover(root, on);
    CHECK(Find(list, "sco") && Find(list, "sco")->state == State::Refused &&
          Find(list, "sco")->reason == "the folder name 'sco' is reserved for the host");
    CHECK(Find(list, "game") && Find(list, "game")->reason == "the folder name 'game' is reserved for the host");
    CHECK(Find(P::Discover(root, P::Options{}), "sco")->state == State::Off);
    fs::remove_all(root);
}

// ---- content index --------------------------------------------------------------------------

static std::vector<std::string> Names(const std::vector<const P::ContentItem*>& v) {
    std::vector<std::string> out;
    for (const auto* it : v) out.push_back(it->plugin + ":" + it->name);
    return out;
}

static void TestContentIndex() {
    P::Options on;
    on.enabled = true;
    on.has = HasAll;
    auto list = P::Discover(g_fixtures / "tree", on);
    P::ContentIndex index;
    g_log.clear();
    const size_t n = index.Build(list);
    CHECK(n == 8 && index.Size() == 8);
    CHECK(Find(list, "pack")->state == State::Loaded && Find(list, "zpack")->state == State::Loaded);
    CHECK(Find(list, "needcap")->state == State::Loaded);                  // an empty pack still loads
    CHECK(Find(list, "hello")->state == State::Ready);                     // natives untouched
    CHECK(Find(list, "badapi")->state == State::Refused);                  // refused packs not indexed
    CHECK(Logged("[plugin] loaded pack 2.1 (data, 7 files)"));

    CHECK((Names(index.Items(P::ContentKind::Mission)) ==
           std::vector<std::string>{ "pack:missions/A.CWMISSION", "pack:missions/b.cwmission", "zpack:missions/b.cwmission" }));
    CHECK((Names(index.Items(P::ContentKind::Rules)) == std::vector<std::string>{ "pack:rules/arena.rules" }));
    CHECK((Names(index.Items(P::ContentKind::Script)) ==
           std::vector<std::string>{ "pack:scripts/deep/er/nested.xml", "pack:scripts/top.xml" }));
    CHECK((Names(index.Items(P::ContentKind::List)) == std::vector<std::string>{ "pack:lists/ships.txt" }));
    CHECK((Names(index.Items(P::ContentKind::DataCore)) == std::vector<std::string>{ "pack:datacore/drive.toml" }));
    CHECK(std::strcmp(P::ContentKindName(P::ContentKind::DataCore), "datacore") == 0);
    CHECK((Names(index.Find(P::ContentKind::Mission, "missions/b.cwmission")) ==
           std::vector<std::string>{ "pack:missions/b.cwmission", "zpack:missions/b.cwmission" }));
    CHECK(index.Find(P::ContentKind::Rules, "missions/b.cwmission").empty());
    CHECK(index.FromPlugin("zpack").size() == 1 && index.FromPlugin("nobody").empty());
    const auto* item = index.Items(P::ContentKind::List)[0];
    CHECK(fs::exists(item->path) && item->kind == P::ContentKind::List);
    CHECK(std::strcmp(P::ContentKindName(P::ContentKind::Script), "script") == 0);

    // Rebuilding replaces: the Loaded packs are re-read, not dropped and not indexed twice.
    const auto missions = Names(index.Items(P::ContentKind::Mission));
    CHECK(index.Build(list) == 8 && index.Size() == 8);
    CHECK(Names(index.Items(P::ContentKind::Mission)) == missions);
    CHECK(Find(list, "pack")->state == State::Loaded && Find(list, "zpack")->state == State::Loaded);
    index.Clear();
    CHECK(index.Size() == 0);

    // Symlinks inside a pack are skipped; too many files refuses the pack.
    const fs::path root = g_out / "packs";
    fs::remove_all(root);
    WriteFile(root / "big" / "plugin.ini", "id=big\nname=N\nversion=1\napi=1.0\nkind=data\n");
    for (size_t i = 0; i < P::kMaxPackFiles + 1; ++i) WriteFile(root / "big" / "lists" / (std::to_string(i) + ".txt"), "");
    WriteFile(root / "sly" / "plugin.ini", "id=sly\nname=N\nversion=1\napi=1.0\nkind=data\n");
    WriteFile(root / "outside" / "secret.xml", "<x/>");
    WriteFile(root / "sly" / "scripts" / "own.xml", "<x/>");
    std::error_code dirLink, fileLink;
    fs::create_directory_symlink(root / "outside", root / "sly" / "scripts" / "linked", dirLink);
    fs::create_symlink(root / "outside" / "secret.xml", root / "sly" / "scripts" / "file_link.xml", fileLink);
    list = P::Discover(root, on);
    const size_t built = index.Build(list);
    CHECK(Find(list, "big")->state == State::Refused && Find(list, "big")->reason == "too many files");
    CHECK(index.Find(P::ContentKind::Script, "scripts/own.xml").size() == 1);
    if (dirLink) std::printf("SKIP: symlinked pack folder (cannot create symlink: %s)\n", dirLink.message().c_str());
    if (fileLink) std::printf("SKIP: symlinked pack file (cannot create symlink: %s)\n", fileLink.message().c_str());
    if (!dirLink && !fileLink) {   // both links skipped: only the pack's own file is indexed
        CHECK(built == 1);
        CHECK((Names(index.Items(P::ContentKind::Script)) == std::vector<std::string>{ "sly:scripts/own.xml" }));
    }
    fs::remove_all(root);

    // scripts/** goes kMaxScriptDepth folders deep; deeper files are ignored.
    WriteFile(root / "deep" / "plugin.ini", "id=deep\nname=N\nversion=1\napi=1.0\nkind=data\n");
    fs::path at = root / "deep" / "scripts";
    for (int i = 0; i < P::kMaxScriptDepth; ++i) at /= "d";
    WriteFile(at / "last.xml", "<x/>");
    WriteFile(at / "d" / "too_deep.xml", "<x/>");
#ifndef _WIN32
    // A folder that can't be read refuses the pack instead of loading part of it.
    const fs::path shut = root / "locked" / "scripts" / "shut";
    WriteFile(root / "locked" / "plugin.ini", "id=locked\nname=N\nversion=1\napi=1.0\nkind=data\n");
    WriteFile(root / "locked" / "scripts" / "a.xml", "<x/>");
    WriteFile(shut / "b.xml", "<x/>");
    fs::permissions(shut, fs::perms::none);
#endif
    list = P::Discover(root, on);
    index.Build(list);
    const auto deep = index.FromPlugin("deep");
    CHECK(deep.size() == 1 && deep[0]->name.ends_with("/d/last.xml"));
#ifndef _WIN32
    if (geteuid() == 0) {
        std::printf("SKIP: unreadable pack folder (running as root reads it anyway)\n");
    } else {
        const P::Plugin* locked = Find(list, "locked");
        CHECK(locked && locked->state == State::Refused && locked->reason.rfind("cannot read scripts: ", 0) == 0);
        CHECK(index.FromPlugin("locked").empty());
    }
    fs::permissions(shut, fs::perms::owner_all);
#endif
    fs::remove_all(root);
}

// ---- native loader --------------------------------------------------------------------------

// Fault guard for the host build: SIGSEGV/SIGBUS inside thunk jumps back here.
#ifndef _WIN32
static sigjmp_buf g_jump;
static volatile sig_atomic_t g_inGuard = 0;
static void OnFault(int sig) {
    if (g_inGuard) siglongjmp(g_jump, sig);
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}
static uint32_t SignalGuard(void (*thunk)(void*), void* ctx) {
    struct sigaction sa {}, oldSegv {}, oldBus {};
    sa.sa_handler = OnFault;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, &oldSegv);
    sigaction(SIGBUS, &sa, &oldBus);
    uint32_t code = 0;
    if (const int sig = sigsetjmp(g_jump, 1)) {
        code = 0xC0000005u;   // report like an access violation on Windows
        (void)sig;
    } else {
        g_inGuard = 1;
        thunk(ctx);
    }
    g_inGuard = 0;
    sigaction(SIGSEGV, &oldSegv, nullptr);
    sigaction(SIGBUS, &oldBus, nullptr);
    return code;
}
#endif

// A tiny sco_api over the runtime, standing in for the host table (sco::host). Callbacks go to
// the runtime as they are, like the real table; ContainCallouts guards them there.
static std::vector<P::Plugin>* g_list;

static P::Plugin* PluginOf(sco_plugin* self) {
    for (auto& p : *g_list) if (p.self == self) return &p;
    return nullptr;
}

static const char* ApiHostVersion() { return "test-host 0.0"; }
static int ApiHas(const char*) { return 0; }
static sco_result ApiSubscribe(sco_plugin* self, const char* event, sco_event_fn fn, void* ctx) {
    if (!PluginOf(self)) return SCO_BAD_ARG;
    return static_cast<sco_result>(sco::Subscribe(self, event, fn, ctx));
}
static void ApiLog(sco_plugin* self, sco_log_level, const char* message) {
    const P::Plugin* p = PluginOf(self);
    sco::Log("[%s] %s", p ? p->manifest.id.c_str() : "?", message);
}

static sco_api MakeApi() {
    sco_api api{};
    api.size = sizeof(api);
    api.major = SCO_API_MAJOR;
    api.minor = SCO_API_MINOR;
    api.host_version = ApiHostVersion;
    api.has = ApiHas;
    api.subscribe = ApiSubscribe;
    api.log = ApiLog;
    return api;
}

// Module ops that record closes and read the plugin's unload counter before closing.
static std::vector<std::string> g_closed;
static int g_lastUnloadCalls = -1;
static void* RecOpen(const fs::path& f, std::string& e) { return P::PlatformModuleOps().open(f, e); }
static void* RecSymbol(void* m, const char* n) { return P::PlatformModuleOps().symbol(m, n); }
static void RecClose(void* m) {
    const int* calls = static_cast<const int*>(P::PlatformModuleOps().symbol(m, "fake_unload_calls"));
    g_lastUnloadCalls = calls ? *calls : -1;
    g_closed.emplace_back(std::to_string(g_lastUnloadCalls));
    P::PlatformModuleOps().close(m);
}
static const P::ModuleOps kRecOps{ RecOpen, RecSymbol, RecClose };

static int Ticks(const P::Plugin& p) {
    if (!p.module) return -1;
    const int* t = static_cast<const int*>(P::PlatformModuleOps().symbol(p.module, "fake_ticks"));
    return t ? *t : -1;
}

// Owner handles: distinct, never reused (Release is final per owner).
static char g_owners[64];
static int g_nextOwner = 0;
static sco_plugin* NewOwner() { return reinterpret_cast<sco_plugin*>(&g_owners[g_nextOwner++]); }

// A call guard that first marks g_victim crashed, the way a fault in a command the plugin invoked
// from inside the guarded call would (CallPlugin -> MarkCrashed), then runs the call.
static P::Plugin* g_victim;
static uint32_t NestedCrashGuard(void (*thunk)(void*), void* ctx) {
    P::MarkCrashed(*g_victim, "nested", 0xC0000005u);
    thunk(ctx);
    return 0;
}

static void TestNative() {
    const fs::path root = g_out / "plugins";
    if (!fs::is_directory(root)) { std::printf("FAIL: %s missing (tools/test.sh builds it)\n", root.string().c_str()); ++g_fail; return; }
#ifndef _WIN32
    P::SetCallGuard(SignalGuard);
#endif
    P::Options on;
    on.enabled = true;
    auto list = P::Discover(root, on);
    g_list = &list;
    P::ContainCallouts(&list);   // the real crash containment path: runtime -> CallPlugin
    const sco_api api = MakeApi();
    auto get = [&](const char* id) -> P::Plugin& { return *const_cast<P::Plugin*>(Find(list, id)); };
    CHECK(list.size() == 13);   // m0..m11 + text
    for (const auto& p : list) CHECK(p.state == State::Ready);

    g_log.clear();
    const size_t subs0 = sco::SubscriptionCount();

    // m0: clean load, its tick runs through the trampoline.
    P::Plugin& ok = get("m0");
    CHECK(P::LoadNative(ok, &api, NewOwner(), on, kRecOps));
    CHECK(ok.state == State::Loaded && ok.module && ok.loadOrder > 0);
    CHECK(Logged("[m0] hello from m0"));
    CHECK(Logged(("[plugin] loaded m0 1.0.0 (api 1." + std::to_string(SCO_API_MINOR) + ") from m0/").c_str()));
    CHECK(sco::SubscriptionCount() == subs0 + 1);
    CHECK(sco::GameThreadTick(1) == sco::Result::Ok);
    CHECK(Ticks(ok) == 1);
    CHECK(!P::LoadNative(ok, &api, NewOwner(), on, kRecOps));            // not Ready any more

    // Refusals: module closed, nothing left registered.
    struct { const char* id; const char* reason; } refused[] = {
        { "m1",  "sco_plugin_load returned UNAVAILABLE" },
        { "m2",  "sco_plugin_query returned NULL" },
        { "m3",  "DLL built for api 2." },   // the fake reports SCO_API_MINOR
        { "m4",  "DLL name 'someone_else' does not match id 'm4'" },
        { "m7",  "missing export sco_plugin_unload" },
        { "m9",  "sco_plugin_info.size too small" },
        { "text", "cannot load text." },   // .so on the host, .dll on Windows
    };
    for (const auto& r : refused) {
        P::Plugin& p = get(r.id);
        const size_t closes = g_closed.size();
        CHECK(!P::LoadNative(p, &api, NewOwner(), on, kRecOps));
        CHECK(p.state == State::Refused);
        CHECK(p.reason.rfind(r.reason, 0) == 0);
        if (p.reason.rfind(r.reason, 0) != 0) std::printf("  %s: got '%s'\n", r.id, p.reason.c_str());
        CHECK(!p.module);
        CHECK(g_closed.size() == closes + (std::strcmp(r.id, "text") == 0 ? 0 : 1));
    }
    CHECK(sco::SubscriptionCount() == subs0 + 1);   // m1 subscribed, then was released
    CHECK(Logged(("[plugin] refused m3: DLL built for api 2." + std::to_string(SCO_API_MINOR) + "").c_str()));

    // Null api / owner are refused before any plugin code runs.
    P::Plugin& m10 = get("m10");
    P::Plugin copy = m10;
    CHECK(!P::LoadNative(copy, nullptr, NewOwner(), on, kRecOps) && copy.reason == "host passed no api or owner");

    {
        // m5: faults in load after subscribing: crashed, released, module kept mapped.
        P::Plugin& c = get("m5");
        const size_t closes = g_closed.size();
        CHECK(!P::LoadNative(c, &api, NewOwner(), on, kRecOps));
        CHECK(c.state == State::Crashed && c.reason == "crashed in sco_plugin_load (0xC0000005)");
        CHECK(c.module != nullptr && g_closed.size() == closes);
        CHECK(sco::SubscriptionCount() == subs0 + 1);
        CHECK(Logged("[plugin] m5 crashed in sco_plugin_load (0xC0000005) and was disabled"));
        char status[128];
        CHECK(sco::GetStatus(status, sizeof(status)) && std::strcmp(status, "plugin m5 crashed and was disabled") == 0);

        // m6: faults in query.
        P::Plugin& q = get("m6");
        CHECK(!P::LoadNative(q, &api, NewOwner(), on, kRecOps));
        CHECK(q.state == State::Crashed && q.reason == "crashed in sco_plugin_query (0xC0000005)");

        // m10: info.name is a bad pointer; the fault happens inside the guard.
        CHECK(!P::LoadNative(m10, &api, NewOwner(), on, kRecOps));
        CHECK(m10.state == State::Crashed && m10.reason == "crashed in sco_plugin_query (0xC0000005)");

        // m8: loads, then faults in its tick: crashed mid-dispatch, never called again, the
        // other plugin keeps ticking.
        P::Plugin& t = get("m8");
        CHECK(P::LoadNative(t, &api, NewOwner(), on, kRecOps));
        CHECK(sco::SubscriptionCount() == subs0 + 2);
        CHECK(sco::GameThreadTick(2) == sco::Result::Ok);
        CHECK(t.state == State::Crashed && t.reason == "crashed in tick (0xC0000005)");
        CHECK(Ticks(t) == 1 && Ticks(ok) == 2);
        CHECK(sco::SubscriptionCount() == subs0 + 1);
        CHECK(sco::GameThreadTick(3) == sco::Result::Ok);
        CHECK(Ticks(t) == 1 && Ticks(ok) == 3);
        // CallPlugin and MarkCrashed are no-ops once crashed.
        CHECK(!P::CallPlugin(t, "x", [](void*) {}, nullptr));
        P::MarkCrashed(t, "again", 1);
        CHECK(t.reason == "crashed in tick (0xC0000005)");
        // UnloadAll skips crashed plugins; their modules stay mapped.
        CHECK(t.module != nullptr);
    }

    // CallPlugin on a healthy plugin runs the thunk.
    int ran = 0;
    CHECK(P::CallPlugin(ok, "test", [](void* c) { ++*static_cast<int*>(c); }, &ran) && ran == 1);

    // m11 loads second; UnloadAll unloads newest first, calls unload once, releases, closes.
    P::Plugin& second = get("m11");
    CHECK(P::LoadNative(second, &api, NewOwner(), on, kRecOps));
    CHECK(sco::SubscriptionCount() == subs0 + 2);
    g_closed.clear();
    g_log.clear();
    P::UnloadAll(list, kRecOps);
    CHECK(ok.state == State::Unloaded && second.state == State::Unloaded && !ok.module && !second.module);
    CHECK((g_closed == std::vector<std::string>{ "1", "1" }));        // each saw exactly one unload()
    CHECK(g_log.size() >= 2 && g_log[0] == "[plugin] unloaded m11" && g_log[1] == "[plugin] unloaded m0");
    CHECK(sco::SubscriptionCount() == subs0);
    CHECK(sco::GameThreadTick(4) == sco::Result::Ok);                 // nothing left to call
    P::UnloadNative(ok, kRecOps);                                      // no-op when not Loaded
    CHECK(ok.state == State::Unloaded);

    // Fresh entries for the same modules (each list entry loads once).
    auto again = P::Discover(root, on);
    g_list = &again;
    P::ContainCallouts(&again);
    auto get2 = [&](const char* id) -> P::Plugin& { return *const_cast<P::Plugin*>(Find(again, id)); };

    // A failed Release (UnloadNative off the game thread: WRONG_THREAD) keeps the module mapped
    // and marks the plugin Crashed; its subscription is still there but never called.
    P::Plugin& stuck = get2("m0");
    CHECK(P::LoadNative(stuck, &api, NewOwner(), on, kRecOps));
    const int ticks0 = Ticks(stuck);
    g_closed.clear();
    std::thread([&] { P::UnloadNative(stuck, kRecOps); }).join();
    CHECK(stuck.state == State::Crashed && stuck.reason == "release failed: WRONG_THREAD");
    CHECK(stuck.module != nullptr && g_closed.empty());
    CHECK(sco::SubscriptionCount() == subs0 + 1);
    CHECK(sco::GameThreadTick(5) == sco::Result::Ok && Ticks(stuck) == ticks0);
    CHECK(sco::Release(stuck.self) == sco::Result::Ok && sco::SubscriptionCount() == subs0);

    // A fault nested inside unload() is kept, not overwritten as a clean unload.
    P::Plugin& nested = get2("m11");
    CHECK(P::LoadNative(nested, &api, NewOwner(), on, kRecOps));
    g_victim = &nested;
    P::SetCallGuard(NestedCrashGuard);
    P::UnloadNative(nested, kRecOps);
    CHECK(nested.state == State::Crashed && nested.reason == "crashed in nested (0xC0000005)");
    CHECK(nested.module != nullptr && g_closed.empty());
    CHECK(sco::SubscriptionCount() == subs0);

    P::ContainCallouts(nullptr);
    g_list = nullptr;
    P::SetCallGuard(nullptr);
}

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: test_plugins <fixtures dir> <out dir>\n"); return 2; }
    g_fixtures = argv[1];
    g_out = argv[2];
    sco::SetLogSink(Sink);
    sco::SetGameThread();
    TestManifest();
    TestDiscover();
    TestDiscoverLimits();
    TestContentIndex();
    TestNative();
    std::printf("sco-core plugin tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
