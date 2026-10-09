// Unit tests for the sco.ui service (sco/ui.h, sco_ui.h, scosdk/ui.hpp) through the real host
// table, with built-in plugins under the real loader's crash guard for the draw-crash cases.
// Host build, no game, no renderer: the "frame" is a pointer the test passes through. The "game
// thread" is this test's main thread. Off Windows the crash guard is a signal handler installed
// here (as in test_app.cpp); on Windows the real __try/__except guard.
//   test_ui                                   (tools/test.sh under ASan+UBSan and TSan, CTest test_ui)
#include "sco/host.h"
#include "sco/log.h"
#include "sco/plugins.h"
#include "sco/runtime.h"
#include "sco/ui.h"
#include "sco_api.h"
#include "sco_ui.h"
#include "scosdk/ui.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <csetjmp>
#include <csignal>
#endif

namespace P = sco::plugins;
using sco::Result;

static std::atomic<int> g_fail{ 0 }, g_pass{ 0 };
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::vector<std::string> g_log;   // game thread only
static void Sink(const char* line) { g_log.emplace_back(line); }
static bool Logged(const char* needle) {
    for (const auto& l : g_log) if (l.find(needle) != std::string::npos) return true;
    return false;
}

static const sco_api*   g_api = nullptr;
static const sco_ui_v1* g_ui = nullptr;

// ---- crash guard (POSIX) --------------------------------------------------------------------

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
    if (sigsetjmp(g_jump, 1)) {
        code = 0xC0000005u;   // report like an access violation on Windows
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

// A real fault in plugin code. Not instrumented, so no sanitizer reports the store before the
// guard sees the signal.
#if defined(__clang__)
__attribute__((noinline, no_sanitize("address", "undefined", "thread")))
#elif defined(__GNUC__)
__attribute__((noinline, no_sanitize("address", "undefined"), no_sanitize_thread))
#endif
static void Fault() {
    volatile int* volatile p = nullptr;
    *p = 1;
}

// ---- helpers --------------------------------------------------------------------------------

static std::string LastError(sco_plugin* p) {
    char buf[256];
    uint32_t n = sizeof(buf);
    const sco_result r = g_ui->last_error(p, buf, &n);
    return r == SCO_OK ? std::string(buf) : "<" + std::to_string(r) + ">";
}

static std::string Norm(const char* chord) {
    std::string out;
    return sco::ui::NormalizeChord(chord, out) ? out : "<bad>";
}

static sco_arg Str(const char* s) { sco_arg a{}; a.type = SCO_ARG_STRING; a.v.s = s; return a; }
static sco_arg Int(int64_t i) { sco_arg a{}; a.type = SCO_ARG_INT; a.v.i = i; return a; }

// Draws append "<name>@<frame>" to g_drawn (game thread only).
static std::vector<std::string> g_drawn;
static void Draw(void* frame, void* ctx) {
    g_drawn.push_back(std::string(static_cast<const char*>(ctx)) + "@" + std::to_string(reinterpret_cast<uintptr_t>(frame)));
}
static void* Frame(uintptr_t n) { return reinterpret_cast<void*>(n); }

static std::vector<std::string> TabIds() {
    std::vector<std::string> v;
    for (const auto& t : sco::ui::Tabs()) v.push_back(t.id);
    return v;
}

// beta.go(name: string, n: int): records the call; replies "went <name> <n>".
static std::atomic<int> g_went{ 0 };
static std::string      g_wentArgs;   // game thread only
static sco_result Go(const sco_arg* args, uint32_t nargs, void*, char* reply, uint32_t size) {
    ++g_went;
    g_wentArgs = std::string(args[0].v.s) + " " + std::to_string(args[1].v.i);
    std::snprintf(reply, size, "went %s", g_wentArgs.c_str());
    return nargs == 2 ? SCO_OK : SCO_FAILED;
}
static void RegisterGo(sco_plugin* beta) {
    static const sco_arg_def defs[] = { { "name", SCO_ARG_STRING, 0, "who" }, { "n", SCO_ARG_INT, 0, "how many" } };
    sco_command c{};
    c.size = sizeof(sco_command);
    c.name = "beta.go";
    c.title = "Go";
    c.args = defs;
    c.nargs = 2;
    c.arg_def_size = sizeof(sco_arg_def);
    c.fn = Go;
    CHECK(g_api->register_command(beta, &c) == SCO_OK);
}

// ---- the service ----------------------------------------------------------------------------

static void TestStart() {
    const void* t = nullptr;
    CHECK(g_api->query_service(SCO_UI_NAME, SCO_UI_VERSION_1_0, &t) == SCO_NOT_FOUND);   // not started
    CHECK(sco::ui::Start() == Result::Ok);
    CHECK(sco::ui::Start() == Result::BadArg);   // already
    CHECK(sco::ui::Started());
    CHECK(g_api->query_service(SCO_UI_NAME, SCO_UI_VERSION_1_0, &t) == SCO_OK && t == sco::ui::Table());
    g_ui = static_cast<const sco_ui_v1*>(t);
    CHECK(g_ui->size == sizeof(sco_ui_v1));
    CHECK(g_api->query_service(SCO_UI_NAME, 0x00010001, &t) == SCO_UNAVAILABLE);   // 1.0 is older
    CHECK(g_api->query_service(SCO_UI_NAME, 0x00020000, &t) == SCO_UNAVAILABLE);
}

static void TestChords() {
    CHECK(Norm("f6") == "f6");
    CHECK(Norm("F6") == "f6");
    CHECK(Norm("Ctrl+Alt+4") == "ctrl+alt+4");
    CHECK(Norm("alt + ctrl + esc") == "ctrl+alt+escape");
    CHECK(Norm("shift+ctrl+A") == "ctrl+shift+a");
    CHECK(Norm("shift+alt+ctrl+f12") == "ctrl+alt+shift+f12");
    CHECK(Norm("control+del") == "ctrl+delete");
    CHECK(Norm(" f1 ") == "f1");
    CHECK(Norm("F24") == "f24");
    CHECK(Norm("numpad5") == "num5" && Norm("Num0") == "num0");
    CHECK(Norm("ctrl+-") == "ctrl+minus" && Norm("ctrl+minus") == "ctrl+minus");
    CHECK(Norm("alt+\\") == "alt+backslash" && Norm("`") == "grave" && Norm("shift+[") == "shift+lbracket");
    CHECK(Norm("pgup") == "pageup" && Norm("Return") == "enter" && Norm("ins") == "insert" && Norm("SPACE") == "space");
    for (const char* bad : { "", " ", "ctrl", "ctrl+", "+a", "ctrl++a", "ctrl+ctrl+a", "a+b", "f0", "f25", "f01",
                             "hyper+a", "win+a", "page up", "ctrl+alt+shift", "num10", "+", "\xc3\xa9",
                             "ctrl+alt+shift+aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" }) {
        std::string out = "untouched";
        CHECK(!sco::ui::NormalizeChord(bad, out) && out == "untouched");
    }
    std::string out;
    CHECK(!sco::ui::NormalizeChord(nullptr, out));

    // Through the table, with the size handshake.
    char buf[32];
    uint32_t n = sizeof(buf);
    CHECK(g_ui->normalize_chord("Alt+Ctrl+Esc", buf, &n) == SCO_OK && n == 16 && std::strcmp(buf, "ctrl+alt+escape") == 0);
    n = 4;
    CHECK(g_ui->normalize_chord("Alt+Ctrl+Esc", buf, &n) == SCO_TOO_MANY && n == 16);
    n = 0;
    CHECK(g_ui->normalize_chord("f6", nullptr, &n) == SCO_TOO_MANY && n == 3);
    n = sizeof(buf);
    CHECK(g_ui->normalize_chord("ctrl++", buf, &n) == SCO_BAD_ARG && n == 0);
    CHECK(g_ui->normalize_chord("f6", buf, nullptr) == SCO_BAD_ARG);
    n = 4;
    CHECK(g_ui->normalize_chord("f6", nullptr, &n) == SCO_BAD_ARG);   // no buffer with a capacity
}

static sco_plugin* g_alpha = nullptr;
static sco_plugin* g_beta = nullptr;

static void TestTabs() {
    static const char kA[] = "a", kB[] = "b", kZ[] = "z", kBeta[] = "beta";
    // Order: ascending order, ties in registration order.
    CHECK(g_ui->register_tab(g_alpha, "alpha.b", "Alpha B", 10, Draw, const_cast<char*>(kB)) == SCO_OK);
    CHECK(g_ui->register_tab(g_beta, "beta.a", "Beta", 5, Draw, const_cast<char*>(kBeta)) == SCO_OK);
    CHECK(g_ui->register_tab(g_alpha, "alpha.a", "Alpha A", 10, Draw, const_cast<char*>(kA)) == SCO_OK);
    CHECK(g_ui->register_tab(g_alpha, "alpha.z", "Alpha Z", -1, Draw, const_cast<char*>(kZ)) == SCO_OK);
    CHECK((TabIds() == std::vector<std::string>{ "alpha.z", "beta.a", "alpha.b", "alpha.a" }));
    const auto tabs = sco::ui::Tabs();
    CHECK(tabs.size() == 4 && tabs[1].title == "Beta" && tabs[1].owner == "beta" && tabs[1].order == 5 && tabs[1].badge.empty());

    // Ids outside the plugin's own id, bad names, bad titles, a null draw, a taken id.
    for (const char* bad : { "beta.x", "alpha", "alphax.y", "alpha.", "alpha..x", "Alpha.x", "alpha.x y", ".alpha.x", "" }) {
        CHECK(g_ui->register_tab(g_alpha, bad, "T", 0, Draw, nullptr) == SCO_BAD_ARG);
    }
    CHECK(g_ui->register_tab(g_alpha, "beta.x", "T", 0, Draw, nullptr) == SCO_BAD_ARG);
    CHECK(LastError(g_alpha) == "tab id 'beta.x' is not 'alpha.<name>' ([a-z0-9_.], at most 63 bytes)");
    CHECK(g_ui->register_tab(g_alpha, nullptr, "T", 0, Draw, nullptr) == SCO_BAD_ARG);
    const std::string longId = "alpha." + std::string(58, 'x');   // 64 bytes
    CHECK(g_ui->register_tab(g_alpha, longId.c_str(), "T", 0, Draw, nullptr) == SCO_BAD_ARG);
    CHECK(g_ui->register_tab(g_alpha, longId.substr(0, 63).c_str(), "T", 0, Draw, nullptr) == SCO_OK);
    CHECK(g_ui->unregister_tab(g_alpha, longId.substr(0, 63).c_str()) == SCO_OK);
    CHECK(g_ui->register_tab(g_alpha, "alpha.t", nullptr, 0, Draw, nullptr) == SCO_BAD_ARG);
    CHECK(g_ui->register_tab(g_alpha, "alpha.t", "", 0, Draw, nullptr) == SCO_BAD_ARG);
    CHECK(g_ui->register_tab(g_alpha, "alpha.t", std::string(64, 't').c_str(), 0, Draw, nullptr) == SCO_BAD_ARG);
    CHECK(g_ui->register_tab(g_alpha, "alpha.t", "T", 0, nullptr, nullptr) == SCO_BAD_ARG);
    CHECK(LastError(g_alpha) == "tab alpha.t: draw is NULL");
    CHECK(g_ui->register_tab(g_alpha, "alpha.a", "Again", 0, Draw, nullptr) == SCO_BAD_ARG);
    CHECK(LastError(g_alpha) == "tab id 'alpha.a' is taken");
    CHECK(g_ui->register_tab(nullptr, "alpha.t", "T", 0, Draw, nullptr) == SCO_BAD_ARG);
    int notAPlugin = 0;
    CHECK(g_ui->register_tab(reinterpret_cast<sco_plugin*>(&notAPlugin), "alpha.t", "T", 0, Draw, nullptr) == SCO_BAD_ARG);
    CHECK(sco::ui::Tabs().size() == 4);

    // Badges: the owner only, at most 15 bytes, NULL or "" clears.
    CHECK(g_ui->set_badge(g_alpha, "alpha.a", "3") == SCO_OK && sco::ui::Badge("alpha.a") == "3");
    CHECK(sco::ui::Tabs()[3].badge == "3");
    CHECK(g_ui->set_badge(g_beta, "alpha.a", "x") == SCO_NOT_FOUND && sco::ui::Badge("alpha.a") == "3");
    CHECK(g_ui->set_badge(g_alpha, "alpha.a", "0123456789abcdef") == SCO_BAD_ARG);
    CHECK(g_ui->set_badge(g_alpha, "alpha.a", "0123456789abcde") == SCO_OK);
    CHECK(g_ui->set_badge(g_alpha, "alpha.a", "") == SCO_OK && sco::ui::Badge("alpha.a").empty());
    CHECK(g_ui->set_badge(g_alpha, "alpha.a", "new") == SCO_OK);
    CHECK(g_ui->set_badge(g_alpha, "alpha.a", nullptr) == SCO_OK && sco::ui::Badge("alpha.a").empty());
    CHECK(g_ui->set_badge(g_alpha, "alpha.nope", "1") == SCO_NOT_FOUND);
    CHECK(sco::ui::Badge("nope.x").empty() && sco::ui::Badge(nullptr).empty());

    // Drawing passes the frame and the plugin's ctx through, on the game thread only.
    g_drawn.clear();
    CHECK(sco::ui::DrawTab("alpha.a", Frame(7)) == Result::Ok);
    CHECK(sco::ui::DrawTab("beta.a", Frame(8)) == Result::Ok);
    CHECK((g_drawn == std::vector<std::string>{ "a@7", "beta@8" }));
    CHECK(sco::ui::DrawTab("alpha.nope", Frame(1)) == Result::NotFound && sco::ui::DrawTab(nullptr, Frame(1)) == Result::NotFound);
    Result off = Result::Ok;
    std::thread([&] { off = sco::ui::DrawTab("alpha.a", Frame(1)); }).join();
    CHECK(off == Result::WrongThread && g_drawn.size() == 2);

    // Unregister: the owner only; the badge goes with the tab.
    CHECK(g_ui->set_badge(g_alpha, "alpha.b", "2") == SCO_OK);
    CHECK(g_ui->unregister_tab(g_beta, "alpha.b") == SCO_NOT_FOUND);
    CHECK(g_ui->unregister_tab(g_alpha, "alpha.b") == SCO_OK);
    CHECK(g_ui->unregister_tab(g_alpha, "alpha.b") == SCO_NOT_FOUND);
    CHECK(sco::ui::Badge("alpha.b").empty() && sco::ui::DrawTab("alpha.b", Frame(1)) == Result::NotFound);
    CHECK((TabIds() == std::vector<std::string>{ "alpha.z", "beta.a", "alpha.a" }));
    CHECK(g_ui->unregister_tab(g_alpha, "alpha.z") == SCO_OK && g_ui->unregister_tab(g_alpha, "alpha.a") == SCO_OK);
    CHECK(g_ui->unregister_tab(g_beta, "beta.a") == SCO_OK && sco::ui::Tabs().empty());
}

// An overlay that removes the next one and adds a new one while the pass runs.
static void Meddle(void* frame, void* ctx) {
    Draw(frame, ctx);
    CHECK(g_ui->unregister_overlay(g_alpha, "alpha.o3") == SCO_OK);
    CHECK(g_ui->register_overlay(g_alpha, "alpha.o4", Draw, const_cast<char*>("o4")) == SCO_OK);
}
// A tab that removes itself while it draws.
static void SelfRemove(void* frame, void* ctx) {
    Draw(frame, ctx);
    CHECK(g_ui->unregister_tab(g_alpha, "alpha.once") == SCO_OK);
}

static void TestOverlays() {
    // Overlays and tabs have their own id spaces; overlays draw in registration order.
    CHECK(g_ui->register_tab(g_alpha, "alpha.o1", "Tab", 0, Draw, const_cast<char*>("tab")) == SCO_OK);
    CHECK(g_ui->register_overlay(g_alpha, "alpha.o1", Draw, const_cast<char*>("o1")) == SCO_OK);
    CHECK(g_ui->register_overlay(g_beta, "beta.o2", Meddle, const_cast<char*>("o2")) == SCO_OK);
    CHECK(g_ui->register_overlay(g_alpha, "alpha.o3", Draw, const_cast<char*>("o3")) == SCO_OK);
    CHECK(g_ui->register_overlay(g_alpha, "alpha.o1", Draw, nullptr) == SCO_BAD_ARG);
    CHECK(g_ui->register_overlay(g_alpha, "beta.o9", Draw, nullptr) == SCO_BAD_ARG);
    CHECK(g_ui->register_overlay(g_alpha, "alpha.o9", nullptr, nullptr) == SCO_BAD_ARG);
    const auto list = sco::ui::Overlays();
    CHECK(list.size() == 3 && list[0].id == "alpha.o1" && list[1].id == "beta.o2" && list[1].owner == "beta");

    g_drawn.clear();
    CHECK(sco::ui::DrawOverlays(Frame(5)) == 2);   // o3 removed by o2 during the pass, o4 waits
    CHECK((g_drawn == std::vector<std::string>{ "o1@5", "o2@5" }));
    CHECK(g_ui->unregister_overlay(g_beta, "beta.o2") == SCO_OK);
    g_drawn.clear();
    CHECK(sco::ui::DrawOverlays(Frame(6)) == 2);
    CHECK((g_drawn == std::vector<std::string>{ "o1@6", "o4@6" }));
    size_t off = 9;
    std::thread([&] { off = sco::ui::DrawOverlays(Frame(1)); }).join();
    CHECK(off == 0);
    CHECK(g_ui->unregister_overlay(g_beta, "alpha.o1") == SCO_NOT_FOUND);
    CHECK(g_ui->unregister_overlay(g_alpha, "alpha.o1") == SCO_OK && g_ui->unregister_overlay(g_alpha, "alpha.o4") == SCO_OK);
    CHECK(g_ui->unregister_tab(g_alpha, "alpha.o1") == SCO_OK);
    CHECK(sco::ui::Overlays().empty() && sco::ui::DrawOverlays(Frame(1)) == 0);

    // A draw may remove its own tab.
    CHECK(g_ui->register_tab(g_alpha, "alpha.once", "Once", 0, SelfRemove, const_cast<char*>("once")) == SCO_OK);
    g_drawn.clear();
    CHECK(sco::ui::DrawTab("alpha.once", Frame(2)) == Result::Ok && g_drawn.size() == 1);
    CHECK(sco::ui::DrawTab("alpha.once", Frame(2)) == Result::NotFound);
}

static void TestHotkeys() {
    RegisterGo(g_beta);
    const sco_arg args[] = { Str("x"), Int(3) };
    // Any plugin's command; the chord is stored normalized with copies of the args.
    std::string text = "x";   // a string argument the binding must copy
    sco_arg copied[] = { Str(text.c_str()), Int(3) };
    CHECK(g_ui->bind_hotkey(g_alpha, "Ctrl + Alt + 4", "beta.go", copied, 2) == SCO_OK);
    text = "y";
    auto keys = sco::ui::Hotkeys();
    CHECK(keys.size() == 1 && keys[0].chord == "ctrl+alt+4" && keys[0].command == "beta.go" && keys[0].owner == "alpha" && keys[0].nargs == 2);

    // Conflicts: one binding per chord, whoever asks; the owner is named.
    CHECK(g_ui->bind_hotkey(g_beta, "alt+ctrl+4", "beta.go", args, 2) == SCO_BAD_ARG);
    CHECK(LastError(g_beta) == "ctrl+alt+4 is bound by 'alpha' to beta.go");
    CHECK(g_ui->bind_hotkey(g_alpha, "ctrl+alt+4", "beta.go", args, 2) == SCO_BAD_ARG);

    // Reserved by the product: refused to plugins, never dispatched.
    CHECK(sco::ui::ReserveChord("F6") == Result::Ok && sco::ui::ReserveChord("f6") == Result::Ok);
    CHECK(sco::ui::ReserveChord("ctrl+alt+4") == Result::BadArg);   // a plugin holds it
    CHECK(sco::ui::ReserveChord("ctrl++") == Result::BadArg && sco::ui::ReserveChord(nullptr) == Result::BadArg);
    CHECK(g_ui->bind_hotkey(g_beta, "f6", "beta.go", args, 2) == SCO_BAD_ARG);
    CHECK(LastError(g_beta) == "f6 is reserved by the host");
    CHECK(sco::ui::Dispatch("f6") == Result::NotFound);
    CHECK((sco::ui::ReservedChords() == std::vector<std::string>{ "f6" }));

    // Dispatch: the command runs through the registry with the stored args (copied at bind).
    std::string reply;
    g_went = 0;
    CHECK(sco::ui::Dispatch("alt+ctrl+4", &reply) == Result::Ok);
    CHECK(g_went == 1 && g_wentArgs == "x 3" && reply == "went x 3");
    CHECK(sco::ui::Dispatch("ctrl+alt+5", &reply) == Result::NotFound && reply.empty());
    CHECK(sco::ui::Dispatch("not a chord") == Result::NotFound && sco::ui::Dispatch(nullptr) == Result::NotFound);
    // From another thread the invoke is queued and runs on the next tick.
    Result off = Result::Failed;
    std::thread([&] { off = sco::ui::Dispatch("ctrl+alt+4"); }).join();
    CHECK(off == Result::Ok && g_went == 1);
    CHECK(sco::GameThreadTick(0) == Result::Ok && g_went == 2);

    // The registry checks the command when the key is pressed, not when it is bound.
    CHECK(g_ui->bind_hotkey(g_alpha, "f9", "nobody.cmd", nullptr, 0) == SCO_OK);
    CHECK(sco::ui::Dispatch("f9") == Result::NotFound);
    CHECK(g_ui->bind_hotkey(g_alpha, "f10", "beta.go", args, 1) == SCO_OK);   // one arg short
    CHECK(sco::ui::Dispatch("f10") == Result::BadArg && g_went == 2);

    // Refusals at bind time.
    sco_arg bad[17] = {};
    CHECK(g_ui->bind_hotkey(g_alpha, "f11", "beta.go", bad, 17) == SCO_BAD_ARG);
    CHECK(g_ui->bind_hotkey(g_alpha, "f11", "beta.go", nullptr, 1) == SCO_BAD_ARG);
    bad[0].type = SCO_ARG_BOOL; bad[0].v.i = 2;
    CHECK(g_ui->bind_hotkey(g_alpha, "f11", "beta.go", bad, 1) == SCO_BAD_ARG);
    bad[0].type = SCO_ARG_STRING; bad[0].v.s = nullptr;
    CHECK(g_ui->bind_hotkey(g_alpha, "f11", "beta.go", bad, 1) == SCO_BAD_ARG);
    const std::string longArg(256, 's');
    bad[0].v.s = longArg.c_str();
    CHECK(g_ui->bind_hotkey(g_alpha, "f11", "beta.go", bad, 1) == SCO_BAD_ARG);
    bad[0].type = 9;
    CHECK(g_ui->bind_hotkey(g_alpha, "f11", "beta.go", bad, 1) == SCO_BAD_ARG);
    CHECK(LastError(g_alpha) == "f11: argument 1: unknown type");
    CHECK(g_ui->bind_hotkey(g_alpha, "f11", "nodot", nullptr, 0) == SCO_BAD_ARG);
    CHECK(g_ui->bind_hotkey(g_alpha, "f11", nullptr, nullptr, 0) == SCO_BAD_ARG);
    CHECK(g_ui->bind_hotkey(g_alpha, "ctrl++", "beta.go", nullptr, 0) == SCO_BAD_ARG);
    CHECK(LastError(g_alpha) == "not a chord: 'ctrl++'");
    CHECK(g_ui->bind_hotkey(g_alpha, nullptr, "beta.go", nullptr, 0) == SCO_BAD_ARG);
    CHECK(sco::ui::Hotkeys().size() == 3);

    // Unbind: the owner only.
    CHECK(g_ui->unbind_hotkey(g_beta, "ctrl+alt+4") == SCO_NOT_FOUND);
    CHECK(g_ui->unbind_hotkey(g_alpha, "Alt+Ctrl+4") == SCO_OK);
    CHECK(g_ui->unbind_hotkey(g_alpha, "ctrl+alt+4") == SCO_NOT_FOUND && g_ui->unbind_hotkey(g_alpha, "++") == SCO_NOT_FOUND);
    CHECK(sco::ui::Dispatch("ctrl+alt+4") == Result::NotFound);
    CHECK(g_ui->bind_hotkey(g_beta, "ctrl+alt+4", "beta.go", args, 2) == SCO_OK);   // free again
    CHECK(sco::ui::Dispatch("ctrl+alt+4", &reply) == Result::Ok && reply == "went x 3");
    keys = sco::ui::Hotkeys();   // sorted by chord
    CHECK(keys.size() == 3 && keys[0].chord == "ctrl+alt+4" && keys[1].chord == "f10" && keys[2].chord == "f9");
    CHECK(g_ui->unbind_hotkey(g_beta, "ctrl+alt+4") == SCO_OK && g_ui->unbind_hotkey(g_alpha, "f9") == SCO_OK);
    CHECK(g_ui->unbind_hotkey(g_alpha, "f10") == SCO_OK && sco::ui::Hotkeys().empty());
    CHECK(LastError(g_alpha) == "not a chord");   // the last refusal ("++"); a success leaves it
}

// ---- the C++ SDK wrapper ----------------------------------------------------------------------

struct Panel {
    int draws = 0;
    void* frame = nullptr;
    void Draw(void* f) { ++draws; frame = f; }
};
struct Thrower {
    void Draw(void*) { throw 42; }
};

static void TestSdk() {
    sco::sdk::Ui ui;
    CHECK(!ui && ui.AddTab("alpha.x", "X", 0, Draw, nullptr) == SCO_UNAVAILABLE);
    CHECK(ui.Open(g_api, nullptr) == SCO_BAD_ARG);
    CHECK(ui.Open(g_api, g_alpha) == SCO_OK && ui && ui.Table() == sco::ui::Table());
    static Panel panel;
    static Thrower thrower;
    CHECK(ui.AddTab("alpha.panel", "Panel", 0, panel) == SCO_OK);
    CHECK(ui.SetBadge("alpha.panel", "1") == SCO_OK && sco::ui::Badge("alpha.panel") == "1");
    CHECK(sco::ui::DrawTab("alpha.panel", Frame(3)) == Result::Ok && panel.draws == 1 && panel.frame == Frame(3));
    CHECK(ui.AddOverlay("alpha.throws", thrower) == SCO_OK);
    CHECK(sco::ui::DrawOverlays(Frame(1)) == 1);   // the exception stays in the SDK thunk
    CHECK(ui.BindHotkey("ctrl+shift+g", "beta.go", { sco::sdk::MakeArg("sdk"), sco::sdk::MakeArg(int64_t{ 7 }) }) == SCO_OK);
    std::string reply;
    CHECK(sco::ui::Dispatch("ctrl+shift+g", &reply) == Result::Ok && reply == "went sdk 7");
    CHECK(ui.BindHotkey("ctrl+shift+g", "beta.go") == SCO_BAD_ARG);
    CHECK(ui.LastError() == "ctrl+shift+g is bound by 'alpha' to beta.go");
    std::string n;
    CHECK(ui.NormalizeChord("Shift + Ctrl + G", n) == SCO_OK && n == "ctrl+shift+g");
    CHECK(ui.NormalizeChord("ctrl+", n) == SCO_BAD_ARG && n == "ctrl+shift+g");
    CHECK(ui.UnbindHotkey("ctrl+shift+g") == SCO_OK && ui.RemoveOverlay("alpha.throws") == SCO_OK);
    CHECK(ui.RemoveTab("alpha.panel") == SCO_OK && ui.RemoveTab("alpha.panel") == SCO_NOT_FOUND);
}

// ---- threads: registration beside the frame (TSan) ----------------------------------------------

static std::atomic<int> g_threadDraws{ 0 };
static void ThreadDraw(void*, void*) { ++g_threadDraws; }

static void TestThreads() {
    constexpr int kThreads = 4, kRounds = 300;
    sco_plugin* owners[kThreads];
    for (int t = 0; t < kThreads; ++t) {
        owners[t] = sco::host::NewPlugin(("th" + std::to_string(t)).c_str());
        CHECK(owners[t] != nullptr);
    }
    const sco_arg args[] = { Str("thread"), Int(1) };
    std::atomic<int> done{ 0 }, refused{ 0 };
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            sco_plugin* self = owners[t];
            const std::string id = "th" + std::to_string(t);
            const std::string tab = id + ".tab", overlay = id + ".hud", chord = "ctrl+f" + std::to_string(t + 1);
            for (int k = 0; k < kRounds; ++k) {
                if (g_ui->register_tab(self, tab.c_str(), "Thread", t, ThreadDraw, nullptr) != SCO_OK) ++refused;
                if (g_ui->set_badge(self, tab.c_str(), std::to_string(k % 100).c_str()) != SCO_OK) ++refused;
                if (g_ui->register_overlay(self, overlay.c_str(), ThreadDraw, nullptr) != SCO_OK) ++refused;
                if (g_ui->bind_hotkey(self, chord.c_str(), "beta.go", args, 2) != SCO_OK) ++refused;
                if (t == 0) sco::ui::Dispatch(chord.c_str());   // queued for the game thread
                if (g_ui->unbind_hotkey(self, chord.c_str()) != SCO_OK) ++refused;
                if (g_ui->unregister_overlay(self, overlay.c_str()) != SCO_OK) ++refused;
                if (g_ui->unregister_tab(self, tab.c_str()) != SCO_OK) ++refused;
            }
            ++done;
        });
    }
    // The game thread runs frames meanwhile: tab strip, each tab, overlays, a key, a tick.
    size_t frames = 0;
    std::string reply;
    while (done.load() < kThreads) {
        for (const auto& t : sco::ui::Tabs()) sco::ui::DrawTab(t.id.c_str(), Frame(frames));
        sco::ui::DrawOverlays(Frame(frames));
        sco::ui::Dispatch("ctrl+f2", &reply);
        sco::ui::Hotkeys();
        sco::GameThreadTick(0);
        ++frames;
    }
    for (auto& th : threads) th.join();
    sco::GameThreadTick(0);
    CHECK(refused == 0 && frames > 0);
    CHECK(sco::ui::Tabs().empty() && sco::ui::Overlays().empty() && sco::ui::Hotkeys().empty());
    for (sco_plugin* p : owners) CHECK(sco::Release(p) == Result::Ok);
}

// ---- unload withdraws everything --------------------------------------------------------------

static void TestRelease() {
    sco_plugin* gamma = sco::host::NewPlugin("gamma");
    CHECK(gamma != nullptr);
    const sco_arg args[] = { Str("g"), Int(1) };
    CHECK(g_ui->register_tab(gamma, "gamma.main", "Gamma", 0, Draw, nullptr) == SCO_OK);
    CHECK(g_ui->set_badge(gamma, "gamma.main", "9") == SCO_OK);
    CHECK(g_ui->register_overlay(gamma, "gamma.hud", Draw, nullptr) == SCO_OK);
    CHECK(g_ui->bind_hotkey(gamma, "ctrl+g", "beta.go", args, 2) == SCO_OK);
    CHECK(g_ui->register_tab(g_alpha, "alpha.stays", "Stays", 0, Draw, nullptr) == SCO_OK);
    CHECK(g_ui->register_tab(gamma, "gamma.main", "Again", 0, Draw, nullptr) == SCO_BAD_ARG);   // leaves an error
    CHECK(sco::Release(gamma) == Result::Ok);
    CHECK((TabIds() == std::vector<std::string>{ "alpha.stays" }));
    CHECK(sco::ui::Overlays().empty() && sco::ui::Hotkeys().empty() && sco::ui::Badge("gamma.main").empty());
    CHECK(sco::ui::Dispatch("ctrl+g") == Result::NotFound);
    // A released plugin can't register again, or read its error.
    CHECK(g_ui->register_tab(gamma, "gamma.main", "Gamma", 0, Draw, nullptr) == SCO_BAD_ARG);
    CHECK(g_ui->bind_hotkey(gamma, "ctrl+g", "beta.go", args, 2) == SCO_BAD_ARG);
    char buf[64];
    uint32_t n = sizeof(buf);
    CHECK(g_ui->last_error(gamma, buf, &n) == SCO_BAD_ARG && n == 0);
    // The chord is free for someone else now.
    CHECK(g_ui->bind_hotkey(g_alpha, "ctrl+g", "beta.go", args, 2) == SCO_OK);
    CHECK(g_ui->unbind_hotkey(g_alpha, "ctrl+g") == SCO_OK && g_ui->unregister_tab(g_alpha, "alpha.stays") == SCO_OK);
}

// ---- a crash in a draw disables only that plugin -------------------------------------------------

static void CrashDraw(void*, void*) { Fault(); }
static std::atomic<int> g_steadyDraws{ 0 };
static void SteadyDraw(void*, void*) { ++g_steadyDraws; }

static const sco_ui_v1* UiOf(const sco_api* api) {
    const void* t = nullptr;
    return api->query_service(SCO_UI_NAME, SCO_UI_VERSION_1_0, &t) == SCO_OK ? static_cast<const sco_ui_v1*>(t) : nullptr;
}

#define BUILTIN_INFO(fn, id)                                                                          \
    static const sco_plugin_info* fn() {                                                             \
        static const sco_plugin_info info{ sizeof(sco_plugin_info), SCO_API_MAJOR, SCO_API_MINOR, id, "1.0.0", "test" }; \
        return &info;                                                                                \
    }
BUILTIN_INFO(TabCrashQuery, "tabcrash")
BUILTIN_INFO(HudCrashQuery, "hudcrash")
BUILTIN_INFO(SteadyQuery, "steady")

// tabcrash: its tab faults; it also has an overlay and a hotkey, which must go with it.
static sco_result TabCrashLoad(const sco_api* api, sco_plugin* self) {
    const sco_ui_v1* ui = UiOf(api);
    if (!ui) return SCO_UNAVAILABLE;
    if (ui->register_tab(self, "tabcrash.main", "Crashes", 0, CrashDraw, nullptr) != SCO_OK) return SCO_FAILED;
    if (ui->register_overlay(self, "tabcrash.hud", SteadyDraw, nullptr) != SCO_OK) return SCO_FAILED;
    return ui->bind_hotkey(self, "ctrl+alt+t", "beta.go", nullptr, 0);
}
// hudcrash: its overlay faults, registered before steady's.
static sco_result HudCrashLoad(const sco_api* api, sco_plugin* self) {
    const sco_ui_v1* ui = UiOf(api);
    if (!ui) return SCO_UNAVAILABLE;
    return ui->register_overlay(self, "hudcrash.hud", CrashDraw, nullptr);
}
static sco_result SteadyLoad(const sco_api* api, sco_plugin* self) {
    const sco_ui_v1* ui = UiOf(api);
    if (!ui) return SCO_UNAVAILABLE;
    if (ui->register_tab(self, "steady.main", "Steady", 1, SteadyDraw, nullptr) != SCO_OK) return SCO_FAILED;
    if (ui->register_overlay(self, "steady.hud", SteadyDraw, nullptr) != SCO_OK) return SCO_FAILED;
    return ui->bind_hotkey(self, "ctrl+alt+s", "beta.go", nullptr, 0);
}
static void NoUnload() {}

static const P::Plugin* Find(const std::vector<P::Plugin>& list, const char* id) {
    for (const auto& p : list) if (p.manifest.id == id) return &p;
    return nullptr;
}

static void TestCrash() {
    static const P::Builtin kBuiltins[] = {
        { "tabcrash", TabCrashQuery, TabCrashLoad, NoUnload },
        { "hudcrash", HudCrashQuery, HudCrashLoad, NoUnload },
        { "steady", SteadyQuery, SteadyLoad, NoUnload },
    };
    std::vector<P::Plugin> list;
    for (const auto& b : kBuiltins) list.push_back(P::FromBuiltin(b));
    P::ContainCallouts(&list);
    P::Options opts;
    for (auto& p : list) CHECK(P::LoadBuiltin(p, g_api, sco::host::NewPlugin(p.manifest.id.c_str()), opts));
    CHECK((TabIds() == std::vector<std::string>{ "tabcrash.main", "steady.main" }));
    CHECK(sco::ui::Overlays().size() == 3 && sco::ui::Hotkeys().size() == 2);

    // An overlay that faults: its plugin is disabled and released; the next overlay still draws.
    g_steadyDraws = 0;
    CHECK(sco::ui::DrawOverlays(Frame(1)) == 2);   // tabcrash.hud and steady.hud
    CHECK(g_steadyDraws == 2);
    CHECK(Find(list, "hudcrash")->state == P::State::Crashed);
    CHECK(Find(list, "hudcrash")->reason.find("crashed in hudcrash.hud") == 0);
    CHECK(Logged("[plugin] hudcrash crashed in hudcrash.hud"));
    CHECK(sco::ui::Overlays().size() == 2);

    // A tab that faults: Crashed, its plugin disabled, everything it registered withdrawn.
    CHECK(sco::ui::DrawTab("tabcrash.main", Frame(2)) == Result::Crashed);
    CHECK(Find(list, "tabcrash")->state == P::State::Crashed);
    CHECK(Find(list, "tabcrash")->reason.find("crashed in tabcrash.main") == 0);
    CHECK((TabIds() == std::vector<std::string>{ "steady.main" }));
    const auto overlays = sco::ui::Overlays();
    CHECK(overlays.size() == 1 && overlays[0].id == "steady.hud");
    const auto keys = sco::ui::Hotkeys();
    CHECK(keys.size() == 1 && keys[0].chord == "ctrl+alt+s");
    CHECK(sco::ui::DrawTab("tabcrash.main", Frame(2)) == Result::NotFound);

    // The other plugin is untouched.
    g_steadyDraws = 0;
    CHECK(Find(list, "steady")->state == P::State::Loaded);
    CHECK(sco::ui::DrawTab("steady.main", Frame(3)) == Result::Ok && sco::ui::DrawOverlays(Frame(3)) == 1 && g_steadyDraws == 2);

    P::UnloadAll(list, P::PlatformModuleOps(), nullptr);
    P::ContainCallouts(nullptr);
    CHECK(Find(list, "steady")->state == P::State::Unloaded);
    CHECK(sco::ui::Tabs().empty() && sco::ui::Overlays().empty() && sco::ui::Hotkeys().empty());
}

// ---- stop ---------------------------------------------------------------------------------------

static void TestStop() {
    CHECK(g_ui->register_tab(g_alpha, "alpha.left", "Left", 0, Draw, nullptr) == SCO_OK);
    sco::ui::Stop();
    CHECK(!sco::ui::Started());
    const void* t = nullptr;
    CHECK(g_api->query_service(SCO_UI_NAME, SCO_UI_VERSION_1_0, &t) == SCO_NOT_FOUND);
    CHECK(g_ui->register_tab(g_alpha, "alpha.late", "Late", 0, Draw, nullptr) == SCO_UNAVAILABLE);
    CHECK(g_ui->bind_hotkey(g_alpha, "f12", "beta.go", nullptr, 0) == SCO_UNAVAILABLE);
    CHECK(sco::ui::Tabs().empty() && sco::ui::DrawTab("alpha.left", Frame(1)) == Result::NotFound);
    CHECK(sco::ui::ReserveChord("f7") == Result::Unavailable);
    sco::ui::Stop();   // no-op
    // A fresh start: empty, and reservations gone.
    CHECK(sco::ui::Start() == Result::Ok && sco::ui::Tabs().empty() && sco::ui::ReservedChords().empty());
    CHECK(g_ui->bind_hotkey(g_alpha, "f6", "beta.go", nullptr, 0) == SCO_OK);
    sco::ui::Stop();
}

int main() {
    sco::SetLogSink(Sink);
    sco::SetGameThread();
#ifndef _WIN32
    P::SetCallGuard(SignalGuard);
#endif
    g_api = sco::host::BuildApi({ "test-ui 1.0" });
    g_alpha = sco::host::NewPlugin("alpha");
    g_beta = sco::host::NewPlugin("beta");
    TestStart();
    if (g_ui) {
        TestChords();
        TestTabs();
        TestOverlays();
        TestHotkeys();
        TestSdk();
        TestThreads();
        TestRelease();
        TestCrash();
        TestStop();
    }
    P::SetCallGuard(nullptr);
    std::printf("sco-core ui tests: %d passed, %d failed\n", g_pass.load(), g_fail.load());
    return g_fail ? 1 : 0;
}
