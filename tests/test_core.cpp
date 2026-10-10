// Unit tests for sco-core's scanners and signature registry. Host build, no game needed.
//   tools/test.sh
#ifndef SCO_KERNEL_ONLY   // the Star Citizen rows (game pack, SCO_GAME_SC)
#include "sco/game/asop.h"
#include "sco/game/features.h"
#include "sco/game/pak.h"
#include "sco/game/signatures.h"
#include "sco/game/system.h"
#endif
#include "sco/log.h"
#include "sco/scan.h"
#include "sco/signatures.h"
#include "sco/status.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::vector<std::string> g_lines;
static void Capture(const char* line) { g_lines.emplace_back(line); }
static bool Logged(const char* needle) {
    for (const auto& l : g_lines) if (l.find(needle) != std::string::npos) return true;
    return false;
}

// A fake image: .text with known bytes, .rdata with strings.
static uint8_t g_mem[0x400];
static sco::Image FakeImage() {
    memset(g_mem, 0xCC, sizeof(g_mem));
    sco::Image img;
    img.base = g_mem;
    img.text = { g_mem + 0x100, 0x100 };
    img.rdata = { g_mem + 0x300, 0x100 };
    memset(img.rdata.base, 0, img.rdata.size);
    strcpy(reinterpret_cast<char*>(img.rdata.base + 0x10), "HelloAnchor");
    // unique: 48 8B 05 <rel32 to .rdata+0x40> at text+0x10 (mov rax, [rip+x])
    uint8_t* p = img.text.base + 0x10;
    p[0] = 0x48; p[1] = 0x8B; p[2] = 0x05;
    const int32_t rel = static_cast<int32_t>((img.rdata.base + 0x40) - (p + 7));
    memcpy(p + 3, &rel, 4);
    p[7] = 0xC3; p[8] = 0x90;
    // twice: AA BB CC DD
    const uint8_t twice[] = { 0xAA, 0xBB, 0x11, 0xDD };
    memcpy(img.text.base + 0x40, twice, 4);
    memcpy(img.text.base + 0x80, twice, 4);
    // lea rdx, [rip+"HelloAnchor"] at text+0xC0
    uint8_t* lea = img.text.base + 0xC0;
    lea[0] = 0x48; lea[1] = 0x8D; lea[2] = 0x15;
    const int32_t rel2 = static_cast<int32_t>((img.rdata.base + 0x10) - (lea + 7));
    memcpy(lea + 3, &rel2, 4);
    return img;
}

static void TestScan() {
    const sco::Image img = FakeImage();
    int n = 0;
    CHECK(sco::FindUniquePattern(img.text, "48 8B 05 ?? ?? ?? ?? C3", n) == img.text.base + 0x10 && n == 1);
    CHECK(sco::FindUniquePattern(img.text, "AA BB ? DD", n) == nullptr && n == 2);
    CHECK(sco::FindUniquePattern(img.text, "01 02 03 04 05", n) == nullptr && n == 0);
    uint8_t* hits[4] = {};
    CHECK(sco::FindPattern(img.text, "AA BB 11 DD", hits, 4) == 2 && hits[0] == img.text.base + 0x40 && hits[1] == img.text.base + 0x80);
    CHECK(sco::FindPattern(img.text, "", hits, 4) == 0);
    std::string tooLong;
    for (size_t i = 0; i <= sco::kMaxPatternBytes; ++i) tooLong += "CC ";
    CHECK(sco::FindPattern(img.text, tooLong.c_str(), hits, 4) == 0);   // over the limit: no match, not a truncated match
    CHECK(sco::BytesMatch(img.text.base + 0x10, "48 8B ?? ?? ?? ?? ?? C3"));
    CHECK(!sco::BytesMatch(img.text.base + 0x10, "48 8C"));
    CHECK(sco::RipTarget(img.text.base + 0x10, 3, 7) == img.rdata.base + 0x40);
    const uint8_t* s = sco::FindCString(img.rdata, "HelloAnchor");
    CHECK(s == img.rdata.base + 0x10);
    CHECK(sco::FindCString(img.rdata, "Anchor") == nullptr);   // must start at a string boundary
    CHECK(sco::FindRipLea(img.text, 0x48, 0x8D, 0x15, s) == img.text.base + 0xC0);
    sco::Section empty;
    CHECK(sco::FindPattern(empty, "48", hits, 4) == 0);
    CHECK(sco::FindCString(empty, "x") == nullptr);
}

static sco::SigResult ResolveFromAnchor(const sco::Image& img) {
    const uint8_t* s = sco::FindCString(img.rdata, "HelloAnchor");
    const uint8_t* lea = s ? sco::FindRipLea(img.text, 0x48, 0x8D, 0x15, s) : nullptr;
    if (!lea) return sco::SigFail("anchor string not referenced");
    return sco::SigOk(lea);
}
static sco::SigResult ResolveNeedsGlobal(const sco::Image&) {
    uint8_t* g = sco::Sig("t.global");
    return g ? sco::SigOk(g + 8) : sco::SigFail("t.global missing");
}

static const sco::SigDef kRows[] = {
    { "t.dependent", nullptr, 0, 0, ResolveNeedsGlobal, { "t.global" } },   // listed before its need on purpose
    { "t.global",    "48 8B 05 ?? ?? ?? ?? C3", 3, 7, nullptr, {} },
    { "t.code",      "48 8B 05 ?? ?? ?? ?? C3", 0, 0, nullptr, {} },
    { "t.anchor",    nullptr, 0, 0, ResolveFromAnchor, {} },
    { "t.ambig",     "AA BB ?? DD", 0, 0, nullptr, {} },
    { "t.missing",   "01 02 03 04", 0, 0, nullptr, {} },
    { "t.blocked",   nullptr, 0, 0, ResolveNeedsGlobal, { "t.missing" } },
    { "t.unknown",   nullptr, 0, 0, ResolveNeedsGlobal, { "t.nope" } },
    { "t.loop_a",    nullptr, 0, 0, ResolveNeedsGlobal, { "t.loop_b" } },
    { "t.loop_b",    nullptr, 0, 0, ResolveNeedsGlobal, { "t.loop_a" } },
    { "t.empty",     nullptr, 0, 0, nullptr, {} },
};
static const sco::SigDef kDup[] = { { "t.code", "90", 0, 0, nullptr, {} } };

static void TestRegistry() {
    const sco::Image img = FakeImage();
    CHECK(sco::RegisterSignatures(kRows, sizeof(kRows) / sizeof(kRows[0])));
    CHECK(!sco::RegisterSignatures(kDup, 1));
    CHECK(Logged("duplicate signature id t.code"));
    CHECK(sco::SignatureCount() == sizeof(kRows) / sizeof(kRows[0]));
    CHECK(sco::Sig("t.global") == nullptr);   // not resolved yet
    sco::ResolveAll(img);
    using S = sco::SigState;
    auto st = [](const char* id) { const sco::SigResult* r = sco::SigLookup(id); return r ? r->state : S::NotRun; };
    CHECK(sco::Sig("t.global") == img.rdata.base + 0x40);
    CHECK(sco::Sig("t.code") == img.text.base + 0x10);
    CHECK(sco::Sig("t.dependent") == img.rdata.base + 0x48);
    CHECK(sco::Sig("t.anchor") == img.text.base + 0xC0);
    CHECK(st("t.ambig") == S::Ambiguous && sco::SigLookup("t.ambig")->matches == 2 && !sco::Sig("t.ambig"));
    CHECK(st("t.missing") == S::Missing);
    CHECK(st("t.blocked") == S::Blocked && strcmp(sco::SigLookup("t.blocked")->why, "t.missing") == 0);
    CHECK(st("t.unknown") == S::Failed);
    CHECK(st("t.loop_a") != S::Ok && st("t.loop_b") != S::Ok);
    CHECK(st("t.empty") == S::Failed);
    CHECK(sco::SigLookup("no.such") == nullptr && !sco::SigReady("no.such"));
    g_lines.clear();
    sco::LogSignatureReport(false);
    CHECK(Logged("[core] signatures: 4/11 OK"));
    CHECK(Logged("[core] AMBIG    t.ambig (pattern: 2 matches)"));
    CHECK(Logged("[core] MISSING  t.missing"));
    CHECK(Logged("[core] BLOCKED  t.blocked (needs t.missing)"));
    CHECK(!Logged("OK       t.code"));
    g_lines.clear();
    sco::LogSignatureReport(true);
    CHECK(Logged("[core] OK       t.code"));
    sco::ResolveAll(img);   // re-resolving gives the same answer
    CHECK(sco::Sig("t.dependent") == img.rdata.base + 0x48);
}

#ifndef SCO_KERNEL_ONLY   // the Star Citizen game pack (SCO_GAME_SC)
// A fake image with CSystem::Quit's log string and a function laid out like build 4.10.193.11644:
// the bytes system.quit checks, the string's lea r9 at +0xA3 and +0x176, 0xCC everywhere else.
// `flip` changes one byte of the function (offset from its start), < 0 for none.
static uint8_t g_quit[0x600];
static uint8_t* QuitImage(sco::Image& img, bool withString, int flip) {
    memset(g_quit, 0xCC, sizeof(g_quit));
    img = {};
    img.base = g_quit;
    img.text = { g_quit + 0x100, 0x300 };
    img.rdata = { g_quit + 0x400, 0x200 };
    memset(img.rdata.base, 0, img.rdata.size);
    const char* fmt = "CSystem::Quit invoked with - cause=$$, reason=$$, exitCode=$$, thread id=$$, main thread id=$$";
    uint8_t* s = img.rdata.base + 0x20;
    if (withString) strcpy(reinterpret_cast<char*>(s), fmt);
    uint8_t* f = img.text.base + 0x20;
    static const struct { size_t off; std::vector<uint8_t> bytes; } kBytes[] = {
        { 0x000, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20 } },
        { 0x00F, { 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 } },
        { 0x018, { 0x48, 0x8D, 0xAC, 0x24, 0x00, 0xFF, 0xFF, 0xFF } },
        { 0x020, { 0x48, 0x81, 0xEC, 0x00, 0x02, 0x00, 0x00 } },
        { 0x027, { 0x45, 0x8B, 0xF8, 0x48, 0x8B, 0xF2, 0x4C, 0x8B, 0xF1 } },
        { 0x035, { 0x41, 0x83, 0xF8, 0x4E } },
        { 0x0BB, { 0x4C, 0x8D, 0x05, 0, 0, 0, 0 } },
        { 0x0DD, { 0xE8, 0, 0, 0, 0 } },
    };
    for (const auto& b : kBytes) memcpy(f + b.off, b.bytes.data(), b.bytes.size());
    for (size_t off : { size_t{ 0xA3 }, size_t{ 0x176 } }) {
        uint8_t* lea = f + off;
        lea[0] = 0x4C; lea[1] = 0x8D; lea[2] = 0x0D;
        const int32_t rel = static_cast<int32_t>(s - (lea + 7));
        memcpy(lea + 3, &rel, 4);
    }
    if (flip >= 0) f[flip] = static_cast<uint8_t>(f[flip] ^ 0x01);
    return f;
}

static void TestSystemQuit() {
    using S = sco::SigState;
    CHECK(sco::game::RegisterGameSignatures());
    sco::Image img;
    uint8_t* f = QuitImage(img, true, -1);
    sco::ResolveAll(img);
    CHECK(sco::SigLookup("system.quit")->state == S::Ok && sco::Sig("system.quit") == f);
    sco::game::QuitHook h;
    CHECK(sco::game::QuitFunction(h) && h.fn == f && h.stolenBytes == 15);
    CHECK(sco::game::kQuitStolenBytes == 15);
    CHECK(!sco::Sig("teleport.to_camera"));   // the other game rows don't find this image

    QuitImage(img, true, 0x0B);   // one prologue byte, inside the stolen bytes
    sco::ResolveAll(img);
    const sco::SigResult* r = sco::SigLookup("system.quit");
    CHECK(r->state == S::Failed && strcmp(r->why, "layout changed at +0x000") == 0);
    sco::game::QuitHook untouched;
    CHECK(!sco::game::QuitFunction(untouched) && untouched.fn == nullptr && untouched.stolenBytes == 0);
    g_lines.clear();
    sco::LogSignatureReport(false);
    CHECK(Logged("[core] FAILED   system.quit (layout changed at +0x000)"));

    QuitImage(img, true, 0x179);   // the second reference no longer reads the log string
    sco::ResolveAll(img);
    r = sco::SigLookup("system.quit");
    CHECK(r->state == S::Failed && strcmp(r->why, "second Quit log line not at +0x176") == 0);

    QuitImage(img, false, -1);
    sco::ResolveAll(img);
    CHECK(sco::SigLookup("system.quit")->state == S::Missing && !sco::Sig("system.quit"));
    g_lines.clear();
    sco::LogSignatureReport(false);
    CHECK(Logged("[core] MISSING  system.quit"));
}

// A fake image for the pak.* rows: the DataCore loader laid out like build 4.10.193 (prologue,
// the CryPak FOpen load at +0x40, read/seek/close calls), with the "DCB file is smaller than
// expected" lea r9 in a chained funclet (.pdata entry 1, chained to entry 0, the loader).
static uint8_t g_pakImg[0x4000];
struct PakLayout { uint8_t* loader; uint8_t* global; uint8_t* readCall; };
static PakLayout PakImage(sco::Image& img, bool withString) {
    memset(g_pakImg, 0xCC, sizeof(g_pakImg));
    img = {};
    img.base = g_pakImg;
    img.size = sizeof(g_pakImg);
    img.text = { g_pakImg + 0x1000, 0x2800 };
    img.rdata = { g_pakImg + 0x3800, 0x400 };
    img.pdata = { g_pakImg + 0x3C00, 3 * 12 };
    memset(img.rdata.base, 0, img.rdata.size);
    uint8_t* msg = img.rdata.base + 0x20;
    if (withString) strcpy(reinterpret_cast<char*>(msg), "DCB file is smaller than expected");
    PakLayout l{ img.text.base + 0x100, img.rdata.base + 0x200, nullptr };
    static const uint8_t kPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18,
                                         0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
    memcpy(l.loader, kPrologue, sizeof kPrologue);
    static const uint8_t kOpen[] = { 0x48, 0x8B, 0x0D, 0, 0, 0, 0, 0x4C, 0x8D, 0x05, 0, 0, 0, 0, 0x48, 0x8B, 0x55, 0x10,
                                     0x45, 0x33, 0xC9, 0x48, 0x8B, 0x01, 0xFF, 0x90, 0x48, 0x01, 0x00, 0x00 };
    uint8_t* open = l.loader + 0x40;
    memcpy(open, kOpen, sizeof kOpen);
    const int32_t g = static_cast<int32_t>(l.global - (open + 7));
    memcpy(open + 3, &g, 4);
    const auto call = [](uint8_t* at, uint32_t slot) { at[0] = 0xFF; at[1] = 0x90; memcpy(at + 2, &slot, 4); };
    l.readCall = l.loader + 0x200;
    call(l.readCall, 0x160);
    call(l.loader + 0x300, 0x1D0);
    call(l.loader + 0x2300, 0x1E0);
    uint8_t* lea = l.loader + 0x1000;   // in the funclet
    lea[0] = 0x4C; lea[1] = 0x8D; lea[2] = 0x0D;
    const int32_t m = static_cast<int32_t>(msg - (lea + 7));
    memcpy(lea + 3, &m, 4);
    // .pdata: [0x1100, 0x1800) the loader, [0x1800, 0x3600) its funclet (chained), [0x3600, 0x3700) another.
    // Unwind info at rdata+0x300 (primary) and rdata+0x310 (UNW_FLAG_CHAININFO, then the parent entry).
    const uint32_t u0 = 0x3B00, u1 = 0x3B10;
    const uint32_t pdata[9] = { 0x1100, 0x1800, u0, 0x1800, 0x3600, u1, 0x3600, 0x3700, u0 };
    memcpy(img.pdata.base, pdata, sizeof pdata);
    g_pakImg[u0] = 0x01;
    g_pakImg[u1] = 0x01 | (4 << 3);
    memcpy(g_pakImg + u1 + 4, pdata, 12);
    return l;
}

static void TestPakRows() {
    using S = sco::SigState;
    CHECK(sco::game::RegisterGameSignatures());
    sco::Image img;
    PakLayout l = PakImage(img, true);
    // FunctionStart: the primary entry, through the chain; nothing outside .pdata's ranges.
    CHECK(sco::FunctionStart(img, l.loader + 0x10) == l.loader);
    CHECK(sco::FunctionStart(img, l.loader + 0x1000) == l.loader);         // chained funclet
    CHECK(sco::FunctionStart(img, g_pakImg + 0x3650) == g_pakImg + 0x3600);
    CHECK(sco::FunctionStart(img, g_pakImg + 0x1000) == nullptr);           // before the first entry
    CHECK(sco::FunctionStart(img, g_pakImg + sizeof(g_pakImg)) == nullptr); // outside the image
    sco::Image noPdata = img;
    noPdata.pdata = {};
    CHECK(sco::FunctionStart(noPdata, l.loader) == nullptr);

    sco::ResolveAll(img);
    CHECK(sco::SigLookup("pak.datacore_loader")->state == S::Ok && sco::Sig("pak.datacore_loader") == l.loader);
    CHECK(sco::SigLookup("pak.crypak")->state == S::Ok && sco::Sig("pak.crypak") == l.global);
    CHECK(sco::SigLookup("pak.slots")->state == S::Ok && sco::Sig("pak.slots") == l.readCall);
    sco::game::pak::Targets t;
    CHECK(sco::game::pak::Resolve(t) && t.loader == l.loader && reinterpret_cast<uint8_t*>(t.cryPak) == l.global);
    static_assert(sco::game::pak::kOpenSlot == 0x148 && sco::game::pak::kReadSlot == 0x160);
    static_assert(sco::game::pak::kSeekSlot == 0x1D0 && sco::game::pak::kCloseSlot == 0x1E0);

    l.loader[3] ^= 1;   // the prologue
    sco::ResolveAll(img);
    const sco::SigResult* r = sco::SigLookup("pak.datacore_loader");
    CHECK(r->state == S::Failed && strcmp(r->why, "loader prologue changed") == 0);
    CHECK(sco::SigLookup("pak.crypak")->state == S::Blocked && sco::SigLookup("pak.slots")->state == S::Blocked);
    sco::game::pak::Targets untouched;
    CHECK(!sco::game::pak::Resolve(untouched) && !untouched.loader && !untouched.cryPak);

    l = PakImage(img, true);
    img.pdata = {};
    sco::ResolveAll(img);
    r = sco::SigLookup("pak.datacore_loader");
    CHECK(r->state == S::Failed && strcmp(r->why, "no .pdata entry for the DCB size message's function") == 0);

    l = PakImage(img, true);
    l.loader[0x2300] = 0xCC;   // no close call
    sco::ResolveAll(img);
    r = sco::SigLookup("pak.slots");
    CHECK(r->state == S::Failed && strcmp(r->why, "no call [rax+0x1e0] (CryPak close) in the loader") == 0);
    CHECK(sco::SigLookup("pak.crypak")->state == S::Ok && !sco::game::pak::Resolve(untouched));

    l = PakImage(img, true);
    l.loader[0x41] = 0x8A;   // the CryPak load
    sco::ResolveAll(img);
    r = sco::SigLookup("pak.crypak");
    CHECK(r->state == S::Failed && strcmp(r->why, "CryPak FOpen call not in the loader's first 0x400 bytes") == 0);

    l = PakImage(img, true);   // a second lea r9 of the message, in another function
    uint8_t* lea2 = g_pakImg + 0x3650;
    memcpy(lea2, l.loader + 0x1000, 3);
    const int32_t m2 = static_cast<int32_t>((img.rdata.base + 0x20) - (lea2 + 7));
    memcpy(lea2 + 3, &m2, 4);
    sco::ResolveAll(img);
    CHECK(sco::SigLookup("pak.datacore_loader")->state == S::Ambiguous && sco::SigLookup("pak.datacore_loader")->matches == 2);

    PakImage(img, false);
    sco::ResolveAll(img);
    CHECK(sco::SigLookup("pak.datacore_loader")->state == S::Missing);
    g_lines.clear();
    sco::LogSignatureReport(false);
    CHECK(Logged("[core] MISSING  pak.datacore_loader") && Logged("[core] BLOCKED  pak.crypak (needs pak.datacore_loader)"));
}

// A fake image for the asop.on_request_open rows: the prologue, shard gate site 1 at +0x21 and the
// guards of build 4.10.196 (+0x189, +0x616, +0x958, the jmp at +0x95E to +0xDC0, the client gate
// at +0x963) with the two globals in .rdata.
static uint8_t g_asopImg[0x1800];
struct AsopLayout { uint8_t* open; uint8_t* shardPersisted; uint8_t* clientGate; };
static AsopLayout AsopImage(sco::Image& img) {
    memset(g_asopImg, 0xCC, sizeof(g_asopImg));
    img = {};
    img.base = g_asopImg;
    img.size = sizeof(g_asopImg);
    img.text = { g_asopImg + 0x100, 0x1000 };
    img.rdata = { g_asopImg + 0x1100, 0x700 };
    memset(img.rdata.base, 0, img.rdata.size);
    AsopLayout l{ img.text.base + 0x20, img.rdata.base + 0x40, img.rdata.base + 0x48 };
    const auto put = [&](size_t off, std::vector<uint8_t> b) { memcpy(l.open + off, b.data(), b.size()); };
    const auto rel = [&](size_t insn, size_t disp, size_t len, const uint8_t* to) {
        const int32_t r = static_cast<int32_t>(to - (l.open + insn + len));
        memcpy(l.open + insn + disp, &r, 4);
    };
    put(0x000, { 0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0x80 });
    put(0x021, { 0x44, 0x89, 0xAD, 0xD0, 0x00, 0x00, 0x00, 0x44, 0x38, 0x2D, 0, 0, 0, 0, 0x0F, 0x85, 0x10, 0x00, 0x00, 0x00,
                 0x48, 0x8B, 0x51, 0x08, 0x48, 0x8D, 0x8D, 0xD0, 0x00, 0x00, 0x00, 0xE8 });
    rel(0x028, 3, 7, l.shardPersisted);
    put(0x189, { 0x48, 0x8B, 0x9E, 0xF8, 0x09, 0x00, 0x00 });
    put(0x616, { 0x48, 0x8B, 0xBD, 0xD8, 0x00, 0x00, 0x00 });
    put(0x958, { 0x89, 0xBE, 0xE8, 0x09, 0x00, 0x00 });
    put(0x95E, { 0xE9, 0, 0, 0, 0 });
    rel(0x95E, 1, 5, l.open + 0xDC0);
    put(0x963, { 0x80, 0x3D, 0, 0, 0, 0, 0x00 });
    rel(0x963, 2, 7, l.clientGate);
    return l;
}

static void TestAsopRows() {
    using S = sco::SigState;
    CHECK(sco::game::RegisterGameSignatures());
    sco::Image img;
    AsopLayout l = AsopImage(img);
    sco::ResolveAll(img);
    CHECK(sco::SigLookup("asop.on_request_open")->state == S::Ok && sco::Sig("asop.on_request_open") == l.open);
    CHECK(sco::Sig("asop.shard_persisted") == l.shardPersisted && sco::Sig("asop.client_gate") == l.clientGate);
    CHECK(sco::Sig("asop.shard_gate_open") == l.open + 0x21);
    CHECK(sco::SigLookup("asop.shard_gate_validation")->state == S::Blocked);   // no Deliver handler here
    CHECK(sco::SigLookup("asop.validate_caller")->state == S::Blocked);
    CHECK(sco::SigLookup("asop.rm_request_deliver")->state == S::Missing);

    l.open[0x95F] ^= 1;   // the server half's jmp no longer reaches the exit
    sco::ResolveAll(img);
    const sco::SigResult* r = sco::SigLookup("asop.on_request_open");
    CHECK(r->state == S::Failed && strcmp(r->why, "jmp at +0x95e doesn't reach +0xdc0") == 0);
    CHECK(sco::SigLookup("asop.client_gate")->state == S::Blocked && sco::SigLookup("asop.shard_gate_open")->state == S::Blocked);

    l = AsopImage(img);
    l.open[0x618] = 0xB5;   // mov rsi instead of mov rdi
    sco::ResolveAll(img);
    r = sco::SigLookup("asop.on_request_open");
    CHECK(r->state == S::Failed && strcmp(r->why, "layout changed at +0x616") == 0);

    l = AsopImage(img);
    memcpy(l.open + 0x41, l.open + 0x21, 32);   // a second shard gate site 1, not at +0x21
    sco::ResolveAll(img);
    CHECK(sco::SigLookup("asop.on_request_open")->state == S::Ok);
    CHECK(sco::SigLookup("asop.shard_gate_open")->state == S::Ambiguous && sco::SigLookup("asop.shard_gate_open")->matches == 2);

    // Every capability names registered rows only, and every asop/atc/hangar row is in one.
    size_t n = 0;
    const sco::game::asop::Capability* caps = sco::game::asop::Capabilities(n);
    CHECK(n == 11);
    for (size_t i = 0; i < n; ++i) {
        CHECK(caps[i].count > 0);
        for (size_t j = 0; j < caps[i].count; ++j) CHECK(sco::SigLookup(caps[i].rows[j]) != nullptr);
        for (size_t j = 0; j < i; ++j) CHECK(strcmp(caps[i].name, caps[j].name) != 0);
    }
    size_t feature = 0;
    for (size_t i = 0; i < sco::SignatureCount(); ++i) {
        const char* id = sco::SignatureDef(i)->id;
        bool ours = false;
        for (const char* p : { "asop.", "atc.", "hangar.", "lift.", "landing.", "respawn.", "insurance." })
            ours |= strncmp(id, p, strlen(p)) == 0;
        if (!ours) continue;
        ++feature;
        bool listed = false;
        for (size_t c = 0; c < n && !listed; ++c)
            for (size_t j = 0; j < caps[c].count && !listed; ++j) listed = strcmp(caps[c].rows[j], id) == 0;
        CHECK(listed);
    }
    CHECK(feature == 62);
}

// sc-offline's own features (sco/game/features.h): every capability row is registered, and every
// row of the table is in a capability, with the site counts the header promises.
static void TestFeatureRows() {
    CHECK(sco::game::RegisterGameSignatures());
    size_t n = 0;
    const sco::game::features::Capability* caps = sco::game::features::Capabilities(n);
    CHECK(caps && n == 5);
    for (size_t c = 0; c < n; ++c)
        for (size_t j = 0; j < caps[c].count; ++j) CHECK(sco::SigLookup(caps[c].rows[j]) != nullptr);
    size_t rows = 0, reputation = 0, orLoop = 0;
    for (size_t i = 0; i < sco::SignatureCount(); ++i) {
        const char* id = sco::SignatureDef(i)->id;
        bool ours = false;
        for (const char* p : { "spawn.", "npc.", "quantum.", "contracts.", "offline." }) ours |= strncmp(id, p, strlen(p)) == 0;
        if (!ours) continue;
        ++rows;
        reputation += strncmp(id, "contracts.reputation_check.", 27) == 0;
        orLoop += strncmp(id, "offline.or_loop_bound.", 22) == 0;
        bool listed = false;
        for (size_t c = 0; c < n && !listed; ++c)
            for (size_t j = 0; j < caps[c].count && !listed; ++j) listed = strcmp(caps[c].rows[j], id) == 0;
        CHECK(listed);
    }
    CHECK(rows == 18);
    CHECK(reputation == static_cast<size_t>(sco::game::features::kReputationChecks));
    CHECK(orLoop == static_cast<size_t>(sco::game::features::kOrLoopSites));
}
#endif   // SCO_KERNEL_ONLY

static void TestStatus() {
    char buf[64] = "junk";
    CHECK(!sco::GetStatus(buf, sizeof(buf)) && buf[0] == 0);
    g_lines.clear();
    sco::Status("Teleported to %s.", "Daymar");
    CHECK(sco::GetStatus(buf, sizeof(buf)) && strcmp(buf, "Teleported to Daymar.") == 0);
    CHECK(Logged("[status] Teleported to Daymar."));
    char small[8];
    CHECK(sco::GetStatus(small, sizeof(small)) && strcmp(small, "Telepor") == 0);
    CHECK(!sco::GetStatus(nullptr, 4));
}

int main() {
    sco::SetLogSink(Capture);
    TestStatus();   // first: checks the "never set" state
    TestScan();
    TestRegistry();
#ifndef SCO_KERNEL_ONLY
    TestSystemQuit();
    TestPakRows();
    TestAsopRows();
    TestFeatureRows();
#endif
    std::printf("sco-core tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
