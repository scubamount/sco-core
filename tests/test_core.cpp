// Unit tests for sco-core's scanners and signature registry. Host build, no game needed.
//   tools/test.sh
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
    std::printf("sco-core tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
