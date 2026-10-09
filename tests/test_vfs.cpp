// Unit tests for sco::vfs (sco/vfs.h): Compose, the Reader's read/seek arithmetic against a
// materialized reference, expected-old-bytes refusal, limits, the mount table's merge, and readers
// beside mount-table swaps (run under ThreadSanitizer). No game, no engine: fake BaseIo objects.
//   tools/test.sh
#include "sco/vfs.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <latch>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

using sco::vfs::Bytes;
using sco::vfs::Composed;
using sco::vfs::Limits;
using sco::vfs::Mount;
using sco::vfs::MountInfo;
using sco::vfs::MountState;
using sco::vfs::MountTable;
using sco::vfs::Reader;
using sco::vfs::Result;
using sco::vfs::Segment;
using sco::vfs::Splice;
using sco::vfs::SpliceList;
using sco::vfs::Table;
using sco::vfs::Whence;

// ---- fakes and the reference model -----------------------------------------------------------

// A base file in memory. `readable` cuts reads short (a truncated or failing file); `failSeek`
// makes every seek fail. Counts calls so tests can check the reader seeks only when it must.
struct MemBase : sco::vfs::BaseIo {
    const Bytes* data = nullptr;
    uint64_t pos = 0;
    uint64_t readable = ~0ull;
    bool failSeek = false;
    int seeks = 0, reads = 0;

    explicit MemBase(const Bytes& d) : data(&d) {}
    bool Seek(uint64_t p) override {
        ++seeks;
        if (failSeek) return false;
        pos = p;
        return true;
    }
    size_t Read(void* dst, size_t n) override {
        ++reads;
        const uint64_t end = std::min<uint64_t>(data->size(), readable);
        if (pos >= end) return 0;
        const size_t k = static_cast<size_t>(std::min<uint64_t>(n, end - pos));
        std::memcpy(dst, data->data() + pos, k);
        pos += k;
        return k;
    }
};

// A huge base whose byte at p is a function of p (high bits included), so >2 GiB and >4 GiB
// positions are checked without allocating the file.
static uint8_t Gen(uint64_t p) { return static_cast<uint8_t>(p ^ (p >> 8) ^ (p >> 31) ^ ((p >> 32) * 7)); }

struct GenBase : sco::vfs::BaseIo {
    uint64_t size = 0, pos = 0;
    explicit GenBase(uint64_t s) : size(s) {}
    bool Seek(uint64_t p) override {
        pos = p;
        return true;
    }
    size_t Read(void* dst, size_t n) override {
        if (pos >= size) return 0;
        const size_t k = static_cast<size_t>(std::min<uint64_t>(n, size - pos));
        uint8_t* out = static_cast<uint8_t*>(dst);
        for (size_t i = 0; i < k; ++i) out[i] = Gen(pos + i);
        pos += k;
        return k;
    }
};

static std::shared_ptr<const Bytes> B(std::initializer_list<uint8_t> v) { return std::make_shared<const Bytes>(v); }
static std::shared_ptr<const Bytes> B(const std::string& s) { return std::make_shared<const Bytes>(s.begin(), s.end()); }

static Splice S(uint64_t at, uint64_t removed, std::shared_ptr<const Bytes> bytes = nullptr,
                std::shared_ptr<const Bytes> old = nullptr) {
    return Splice{ at, removed, std::move(bytes), std::move(old) };
}

// The patched file, built the obvious way.
static Bytes Materialize(const Bytes& base, const std::vector<Splice>& splices) {
    Bytes out;
    uint64_t pos = 0;
    for (const Splice& s : splices) {
        out.insert(out.end(), base.begin() + static_cast<ptrdiff_t>(pos), base.begin() + static_cast<ptrdiff_t>(s.at));
        if (s.bytes) out.insert(out.end(), s.bytes->begin(), s.bytes->end());
        pos = s.at + s.removed;
    }
    out.insert(out.end(), base.begin() + static_cast<ptrdiff_t>(pos), base.end());
    return out;
}

// The byte at virtual offset v over a Gen base, walking the splices linearly.
static uint8_t ModelAt(const std::vector<Splice>& splices, uint64_t v) {
    uint64_t basePos = 0, vpos = 0;
    for (const Splice& s : splices) {
        const uint64_t gap = s.at - basePos;
        if (v < vpos + gap) return Gen(basePos + (v - vpos));
        vpos += gap;
        const uint64_t add = s.bytes ? s.bytes->size() : 0;
        if (v < vpos + add) return (*s.bytes)[static_cast<size_t>(v - vpos)];
        vpos += add;
        basePos = s.at + s.removed;
    }
    return Gen(basePos + (v - vpos));
}

static std::shared_ptr<const Composed> MustCompose(uint64_t baseSize, const std::vector<Splice>& splices,
                                                   uint64_t maxSize = sco::vfs::kMaxFileSize) {
    auto c = std::make_shared<Composed>();
    Result r = Compose(baseSize, splices, *c, maxSize);
    if (!r) std::printf("compose: %s\n", r.error.c_str());
    CHECK(r.ok());
    return c;
}

static bool Tiles(const Composed& c) {
    uint64_t at = 0;
    for (const Segment& s : c.segments) {
        if (s.start != at || s.len == 0) return false;
        at += s.len;
    }
    return at == c.size;
}

static Bytes Base(size_t n, uint32_t seed) {
    Bytes b(n);
    uint32_t x = seed * 2654435761u + 1;
    for (auto& v : b) {
        x = x * 1664525u + 1013904223u;
        v = static_cast<uint8_t>(x >> 24);
    }
    return b;
}

// ---- Compose ---------------------------------------------------------------------------------

static void TestCompose() {
    // Insert at 0, replace 2 -> 3 at 10, delete 5 at 50, insert at the end.
    const std::vector<Splice> sp = { S(0, 0, B({ 1, 2 })), S(10, 2, B({ 7, 8, 9 })), S(50, 5), S(100, 0, B({ 4, 4, 4, 4 })) };
    auto c = MustCompose(100, sp);
    CHECK(c->baseSize == 100 && c->size == 100 + 2 + 1 - 5 + 4);
    CHECK(Tiles(*c));
    CHECK(c->segments.size() == 6);   // buf, base[0,10), buf, base[12,50), base[55,100), buf
    CHECK(c->segments[0].kind == Segment::Buffer && c->segments[0].len == 2);
    CHECK(c->segments[1].kind == Segment::Base && c->segments[1].from == 0 && c->segments[1].len == 10);
    CHECK(c->segments[2].kind == Segment::Buffer && c->segments[2].len == 3);
    CHECK(c->segments[3].kind == Segment::Base && c->segments[3].from == 12 && c->segments[3].len == 38);
    CHECK(c->segments[4].kind == Segment::Base && c->segments[4].from == 55 && c->segments[4].len == 45);
    CHECK(c->segments[5].kind == Segment::Buffer && c->segments[5].len == 4);
    CHECK(c->buffers.size() == 3);

    // No splices: one base segment. Empty base: nothing, or only buffers.
    auto plain = MustCompose(10, {});
    CHECK(plain->size == 10 && plain->segments.size() == 1 && Tiles(*plain));
    auto empty = MustCompose(0, {});
    CHECK(empty->size == 0 && empty->segments.empty());
    auto fromEmpty = MustCompose(0, { S(0, 0, B("abc")) });
    CHECK(fromEmpty->size == 3 && fromEmpty->segments.size() == 1 && fromEmpty->segments[0].kind == Segment::Buffer);
    // Delete everything; adjacent splices; a whole-file replacement.
    auto gone = MustCompose(10, { S(0, 10) });
    CHECK(gone->size == 0 && gone->segments.empty());
    auto adj = MustCompose(10, { S(2, 3, B({ 1 })), S(5, 0, B({ 2 })), S(5 + 0 + 1, 1) });
    CHECK(adj->size == 10 - 3 + 1 + 1 - 1 && Tiles(*adj));
    auto whole = MustCompose(4, { S(0, 4, B("new contents")) });
    CHECK(whole->size == 12 && whole->segments.size() == 1);
}

static void TestComposeRejects() {
    auto rejects = [](uint64_t base, std::vector<Splice> sp, const char* text, uint64_t maxSize = sco::vfs::kMaxFileSize) {
        Composed out;
        out.size = 1234;
        Result r = Compose(base, sp, out, maxSize);
        const bool named = r.error.find(text) != std::string::npos;
        if (!named) std::printf("  got \"%s\", wanted \"%s\"\n", r.error.c_str(), text);
        return !r && named && out.size == 1234 && out.segments.empty();   // out untouched
    };
    CHECK(rejects(100, { S(10, 1, B({ 1 })), S(5, 1) }, "splice 1 (at 5): comes before splice 0"));
    CHECK(rejects(100, { S(10, 5), S(12, 1) }, "splice 1 (at 12): overlaps splice 0 (at 10, removes 5)"));
    CHECK(rejects(100, { S(10, 0, B({ 1 })), S(10, 1) }, "splice 1 (at 10): overlaps splice 0"));   // same offset
    CHECK(rejects(100, { S(10, 1), S(10, 0, B({ 1 })) }, "splice 1 (at 10): overlaps"));
    CHECK(rejects(100, { S(101, 0, B({ 1 })) }, "splice 0 (at 101): reaches past the end of the base (100 bytes)"));
    CHECK(rejects(100, { S(1, 0, B({ 1 })), S(90, 11) }, "splice 1 (at 90): reaches past the end"));
    CHECK(rejects(100, { S(0, ~0ull) }, "splice 0 (at 0): reaches past the end"));   // no overflow
    CHECK(rejects(100, { S(~0ull, 1) }, "reaches past the end"));
    CHECK(rejects(100, { S(3, 0) }, "splice 0 (at 3): removes nothing and adds nothing"));
    CHECK(rejects(100, { S(3, 0, std::make_shared<const Bytes>()) }, "removes nothing and adds nothing"));
    CHECK(rejects(100, { S(3, 2, nullptr, B({ 1 })) }, "splice 0 (at 3): expects 1 old bytes but removes 2"));
    CHECK(rejects(100, { S(0, 0, B({ 1 })) }, "over the limit of 100", 100));
    // At the limit is fine.
    Composed ok;
    CHECK(Compose(100, std::vector<Splice>{ S(0, 1, B({ 1 })) }, ok, 100).ok() && ok.size == 100);
}

// ---- Reader ----------------------------------------------------------------------------------

// Every (start, length) pair over a small file holding every segment kind, each from a fresh
// reader and from one reader that seeks: reads spanning base ranges and buffers, starting and
// ending on each boundary, zero-length, and reaching or starting past the end.
static void TestReadEveryBoundary() {
    const Bytes base = Base(40, 1);
    const std::vector<Splice> sp = { S(0, 0, B({ 0xA0, 0xA1 })), S(5, 3, B({ 0xB0 })), S(8, 0, B({ 0xC0, 0xC1, 0xC2 })),
                                     S(20, 4), S(30, 1, B({ 0xD0, 0xD1 })), S(40, 0, B({ 0xE0 })) };
    const Bytes want = Materialize(base, sp);
    auto file = MustCompose(base.size(), sp);
    CHECK(file->size == want.size());
    MemBase mb(base);
    Reader shared(file);
    int bad = 0;
    const uint64_t n = want.size();
    for (uint64_t start = 0; start <= n + 2; ++start) {
        for (uint64_t len = 0; len <= n + 3; ++len) {
            Bytes got(static_cast<size_t>(len) + 1, 0x55);
            Reader r(file);
            if (!r.Seek(static_cast<int64_t>(start), Whence::Set)) { ++bad; continue; }
            const size_t k = r.Read(got.data(), static_cast<size_t>(len), mb);
            const uint64_t expect = start >= n ? 0 : std::min(len, n - start);
            if (k != expect || r.Tell() != start + expect) ++bad;
            else if (k && std::memcmp(got.data(), want.data() + start, k) != 0) ++bad;
            else if (got[k] != 0x55) ++bad;   // nothing written past what was read
            // The same through a reader that has been elsewhere (its base position is stale).
            Bytes again(static_cast<size_t>(len));
            shared.Seek(static_cast<int64_t>(start), Whence::Set);
            if (shared.Read(again.data(), again.size(), mb) != expect ||
                (expect && std::memcmp(again.data(), want.data() + start, static_cast<size_t>(expect)) != 0))
                ++bad;
        }
    }
    CHECK(bad == 0);
    // Zero-length and null reads change nothing.
    Reader r(file);
    r.Seek(7, Whence::Set);
    uint8_t one = 0;
    CHECK(r.Read(&one, 0, mb) == 0 && r.Tell() == 7);
    CHECK(r.Read(nullptr, 5, mb) == 0 && r.Tell() == 7);
    // A default reader is an empty file.
    Reader none;
    CHECK(none.Size() == 0 && none.Eof() && none.Read(&one, 1, mb) == 0);
}

static void TestSeek() {
    const Bytes base = Base(10, 2);
    auto file = MustCompose(10, { S(4, 2, B({ 1, 2, 3 })) });   // size 11
    Reader r(file);
    CHECK(r.Size() == 11 && r.Tell() == 0 && !r.Eof());
    CHECK(r.Seek(5, Whence::Set) && r.Tell() == 5);
    CHECK(r.Seek(-2, Whence::Cur) && r.Tell() == 3);
    CHECK(r.Seek(3, Whence::Cur) && r.Tell() == 6);
    CHECK(r.Seek(0, Whence::End) && r.Tell() == 11 && r.Eof());
    CHECK(r.Seek(-11, Whence::End) && r.Tell() == 0);
    CHECK(r.Seek(-1, Whence::End) && r.Tell() == 10 && !r.Eof());
    // Negative results fail and leave the position alone.
    CHECK(!r.Seek(-12, Whence::End) && r.Tell() == 10);
    CHECK(!r.Seek(-1, Whence::Set) && r.Tell() == 10);
    CHECK(!r.Seek(-11, Whence::Cur) && r.Tell() == 10);
    CHECK(!r.Seek(std::numeric_limits<int64_t>::min(), Whence::Cur) && r.Tell() == 10);
    CHECK(!r.Seek(5, static_cast<Whence>(9)) && r.Tell() == 10);
    // Past the end is allowed; reads there return 0 and don't move.
    MemBase mb(base);
    uint8_t buf[4];
    CHECK(r.Seek(100, Whence::End) && r.Tell() == 111 && r.Eof());
    CHECK(r.Read(buf, 4, mb) == 0 && r.Tell() == 111);
    CHECK(r.Seek(-100, Whence::Cur) && r.Tell() == 11 && r.Read(buf, 4, mb) == 0);
    // 64-bit extremes: the largest position works, one more overflows.
    const int64_t kMax = std::numeric_limits<int64_t>::max();
    CHECK(r.Seek(kMax, Whence::Set) && r.Tell() == static_cast<uint64_t>(kMax));
    CHECK(!r.Seek(1, Whence::Cur) && r.Tell() == static_cast<uint64_t>(kMax));
    CHECK(!r.Seek(kMax, Whence::End));
    CHECK(r.Read(buf, 4, mb) == 0);
}

// The base is seeked only when its position isn't already the one needed.
static void TestBaseSeeks() {
    const Bytes base = Base(100, 3);
    auto file = MustCompose(100, { S(10, 2, B({ 1, 2, 3 })) });
    MemBase mb(base);
    Reader r(file);
    uint8_t buf[200];
    CHECK(r.Read(buf, 5, mb) == 5 && mb.seeks == 1);    // unknown base position: one seek to 0
    CHECK(r.Read(buf, 5, mb) == 5 && mb.seeks == 1);    // sequential: none
    CHECK(r.Read(buf, 3, mb) == 3 && mb.seeks == 1);    // the buffer
    CHECK(r.Read(buf, 10, mb) == 10 && mb.seeks == 2);  // skips the 2 removed bytes: one seek to 12
    CHECK(r.Read(buf, 200, mb) == 101 - 23 && mb.seeks == 2);
    CHECK(r.Seek(0, Whence::Set) && r.Read(buf, 1, mb) == 1 && mb.seeks == 3);
    // Someone else moved the base (the adapter's other calls): a new reader re-seeks.
    Reader r2(file);
    mb.pos = 77;
    CHECK(r2.Read(buf, 4, mb) == 4 && mb.seeks == 4 && std::memcmp(buf, base.data(), 4) == 0);
}

// A short or failed base read ends the call with what was read; never garbage.
static void TestShortBase() {
    const Bytes base = Base(50, 4);
    const std::vector<Splice> sp = { S(10, 2, B({ 1, 2, 3 })) };
    const Bytes want = Materialize(base, sp);
    auto file = MustCompose(50, sp);
    MemBase mb(base);
    mb.readable = 30;   // the real file ends early: base bytes 30.. never arrive
    Reader r(file);
    Bytes buf(100, 0xEE);
    // Virtual 0..13 = base 0..10 + buffer; base 12..30 = virtual 13..31.
    const size_t k = r.Read(buf.data(), 100, mb);
    CHECK(k == 31 && r.Tell() == 31 && std::memcmp(buf.data(), want.data(), k) == 0 && buf[k] == 0xEE);
    CHECK(r.Read(buf.data(), 100, mb) == 0 && r.Tell() == 31);
    // The buffer is still served when the base fails before it.
    Reader r2(file);
    mb.readable = 4;
    CHECK(r2.Read(buf.data(), 20, mb) == 4 && r2.Tell() == 4);
    CHECK(r2.Seek(10, Whence::Set) && r2.Read(buf.data(), 3, mb) == 3 && std::memcmp(buf.data(), want.data() + 10, 3) == 0);
    // A failing seek: nothing from the base, and the next read seeks again.
    mb.readable = ~0ull;
    mb.failSeek = true;
    Reader r3(file);
    CHECK(r3.Read(buf.data(), 20, mb) == 0 && r3.Tell() == 0);
    mb.failSeek = false;
    CHECK(r3.Read(buf.data(), 20, mb) == 20 && std::memcmp(buf.data(), want.data(), 20) == 0);
}

// Random bases, random valid splice lists and random Seek/Read/Tell sequences, each result
// against the materialized reference. Fixed seed; plain modulo so every platform runs the same.
static void TestRandom() {
    std::mt19937_64 rng(0x5C0F5ull);
    auto rnd = [&](uint64_t n) { return n ? rng() % n : 0; };
    int bad = 0, ops = 0;
    for (int iter = 0; iter < 400; ++iter) {
        const Bytes base = Base(static_cast<size_t>(rnd(3000)), static_cast<uint32_t>(iter));
        std::vector<Splice> sp;
        uint64_t at = 0;
        while (at <= base.size() && sp.size() < 40) {
            at += rnd(200);
            if (at > base.size()) break;
            const uint64_t removed = std::min<uint64_t>(rnd(4) ? rnd(50) : 0, base.size() - at);
            Bytes add(static_cast<size_t>(rnd(3) ? rnd(60) : 0));
            for (auto& b : add) b = static_cast<uint8_t>(rng());
            if (removed == 0 && add.empty()) add.push_back(0x42);
            sp.push_back(S(at, removed, std::make_shared<const Bytes>(std::move(add))));
            at += removed + 1;   // strictly after: never the same offset
        }
        const Bytes want = Materialize(base, sp);
        auto file = MustCompose(base.size(), sp);
        if (file->size != want.size() || !Tiles(*file)) { ++bad; continue; }
        MemBase mb(base);
        Reader r(file);
        uint64_t model = 0;
        for (int op = 0; op < 60; ++op, ++ops) {
            const int kind = static_cast<int>(rnd(4));
            if (kind == 0) {
                const int64_t off = static_cast<int64_t>(rnd(want.size() + 40)) - 20;
                const Whence w = static_cast<Whence>(rnd(3));
                const int64_t origin = w == Whence::Set ? 0 : w == Whence::Cur ? static_cast<int64_t>(model)
                                                                                 : static_cast<int64_t>(want.size());
                const bool ok = r.Seek(off, w);
                if (ok != (origin + off >= 0)) ++bad;
                if (ok) model = static_cast<uint64_t>(origin + off);
            } else if (kind == 3) {
                if (r.Tell() != model || r.Eof() != (model >= want.size())) ++bad;
            } else {
                const size_t len = static_cast<size_t>(rnd(400));
                Bytes got(len);
                const size_t k = r.Read(got.data(), len, mb);
                const uint64_t expect = model >= want.size() ? 0 : std::min<uint64_t>(len, want.size() - model);
                if (k != expect || (k && std::memcmp(got.data(), want.data() + model, k) != 0)) ++bad;
                model += k;
            }
        }
    }
    CHECK(bad == 0);
    CHECK(ops == 400 * 60);
}

// Positions above 2 GiB and 4 GiB, through a generated base; and the 4 GiB default limit.
static void TestLargeFiles() {
    const uint64_t G2 = 1ull << 31, G4 = 1ull << 32;
    const uint64_t baseSize = 3 * (1ull << 30) + 123;   // 3 GiB + 123
    const std::vector<Splice> sp = { S(G2 - 3, 6, B({ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 })), S(G2 + 100, 1000),
                                     S(baseSize, 0, B("tail")) };
    auto file = MustCompose(baseSize, sp);
    CHECK(file->size == baseSize + 4 - 1000 + 4 && Tiles(*file));
    GenBase gb(baseSize);
    Reader r(file);
    auto check = [&](uint64_t v, size_t len) {
        std::vector<uint8_t> got(len);
        if (!r.Seek(static_cast<int64_t>(v), Whence::Set)) return false;
        const size_t k = r.Read(got.data(), len, gb);
        const uint64_t expect = v >= file->size ? 0 : std::min<uint64_t>(len, file->size - v);
        if (k != expect || r.Tell() != v + k) return false;
        for (size_t i = 0; i < k; ++i)
            if (got[i] != ModelAt(sp, v + i)) return false;
        return true;
    };
    CHECK(check(0, 64));
    CHECK(check(G2 - 40, 100));         // across the 2 GiB line and the replacement
    CHECK(check(G2 + 90, 64));          // across the deletion
    CHECK(check(G2 + 2000, 4096));      // a plain base range above 2 GiB
    CHECK(check(file->size - 10, 64));  // the end, the appended tail, a short read
    CHECK(check(file->size + 5, 8));    // past the end
    CHECK(r.Seek(-4, Whence::End) && r.Tell() == file->size - 4 && r.Tell() > G2);
    uint8_t tail[8] = {};
    CHECK(r.Read(tail, 8, gb) == 4 && std::memcmp(tail, "tail", 4) == 0 && r.Eof());
    CHECK(r.Seek(-static_cast<int64_t>(G2), Whence::Cur) && r.Tell() == file->size - G2);

    // The default limit: exactly 4 GiB composes, one byte more is refused.
    Composed c;
    CHECK(Compose(G4, {}, c).ok() && c.size == G4);
    Composed big;
    Result over = Compose(G4, std::vector<Splice>{ S(G4, 0, B({ 1 })) }, big);
    CHECK(!over && over.error.find("over the limit") != std::string::npos);
    // With a larger limit, a 5 GiB file reads correctly above 4 GiB.
    const uint64_t huge = 5 * (1ull << 30);
    const std::vector<Splice> sp2 = { S(G4 + 7, 2, B({ 0xAB, 0xCD, 0xEF })) };
    auto file2 = MustCompose(huge, sp2, ~0ull);
    GenBase gb2(huge);
    Reader r2(file2);
    std::vector<uint8_t> got(32);
    CHECK(r2.Seek(static_cast<int64_t>(G4), Whence::Set) && r2.Read(got.data(), 32, gb2) == 32);
    bool same = true;
    for (size_t i = 0; i < 32; ++i) same = same && got[i] == ModelAt(sp2, G4 + i);
    CHECK(same && got[7] == 0xAB && got[9] == 0xEF);
}

// ---- paths and the mount table ---------------------------------------------------------------

static void TestPaths() {
    CHECK(sco::vfs::NormalizePath("\\Data\\Game2.DCB") == "data/game2.dcb");
    CHECK(sco::vfs::NormalizePath("//Data//Sub\\\\x.TXT") == "data/sub/x.txt");
    CHECK(sco::vfs::NormalizePath("data/game2.dcb") == "data/game2.dcb");
    CHECK(sco::vfs::NormalizePath("").empty() && sco::vfs::NormalizePath("///").empty());
    CHECK(sco::vfs::NormalizePath(std::string(sco::vfs::kMaxPathLength, 'a')).size() == sco::vfs::kMaxPathLength);
    CHECK(sco::vfs::NormalizePath(std::string(sco::vfs::kMaxPathLength + 1, 'a')).empty());
    CHECK(sco::vfs::NormalizePath("/" + std::string(sco::vfs::kMaxPathLength, 'a')).size() == sco::vfs::kMaxPathLength);
}

static Mount SpliceMount(std::string path, int prio, std::string source, std::vector<Splice> sp,
                         std::shared_ptr<const Bytes> header = nullptr) {
    Mount m;
    m.path = std::move(path);
    m.priority = prio;
    m.source = std::move(source);
    m.producer = SpliceList{ std::move(sp), std::move(header) };
    return m;
}

static Mount TransformMount(std::string path, int prio, std::string source, sco::vfs::Transform fn) {
    Mount m;
    m.path = std::move(path);
    m.priority = prio;
    m.source = std::move(source);
    m.producer = std::move(fn);
    return m;
}

static const MountInfo* Info(const std::vector<MountInfo>& all, const std::string& source) {
    for (const MountInfo& m : all)
        if (m.source == source) return &m;
    return nullptr;
}

static bool ReadsAs(const std::shared_ptr<const Composed>& file, sco::vfs::BaseIo& base, const Bytes& want) {
    if (!file) return false;
    Reader r(file);
    Bytes got(want.size() + 8);
    return r.Read(got.data(), got.size(), base) == want.size() && std::memcmp(got.data(), want.data(), want.size()) == 0;
}

// Expected old bytes and the expected header: a match applies, any mismatch makes the mount inert.
static void TestExpectedOldBytes() {
    const Bytes base = Base(200, 5);
    auto old = [&](uint64_t at, uint64_t n) {
        return std::make_shared<const Bytes>(base.begin() + static_cast<ptrdiff_t>(at), base.begin() + static_cast<ptrdiff_t>(at + n));
    };
    const auto header = old(0, 16);
    const std::vector<Splice> good = { S(20, 4, B({ 1, 2, 3, 4 }), old(20, 4)), S(100, 0, B("new")), S(150, 10, nullptr, old(150, 10)) };
    Bytes wrongOld = *old(150, 10);
    wrongOld[9] ^= 1;
    std::vector<Splice> bad = good;
    bad[2].old = std::make_shared<const Bytes>(wrongOld);
    Bytes wrongHeader = *header;
    wrongHeader[0] ^= 0xFF;

    auto t = Table::Build({ SpliceMount("Data/Game2.dcb", 0, "good", good, header),
                            SpliceMount("data/bad.dcb", 0, "badold", bad, header),
                            SpliceMount("data/hdr.dcb", 0, "badheader", good, std::make_shared<const Bytes>(wrongHeader)) });
    CHECK(t->Mounted("data\\GAME2.dcb") && t->Mounted("data/bad.dcb") && t->Mounted("data/hdr.dcb"));
    CHECK(Info(t->Mounts(), "good")->state == MountState::Pending);
    MemBase mb(base);
    auto file = t->Open("data/game2.dcb", mb, base.size());
    CHECK(ReadsAs(file, mb, Materialize(base, good)));
    CHECK(Info(t->Mounts(), "good")->state == MountState::Applied && Info(t->Mounts(), "good")->reason.empty());

    CHECK(t->Open("data/bad.dcb", mb, base.size()) == nullptr);   // passes through
    const auto infos = t->Mounts();
    const MountInfo* b = Info(infos, "badold");
    CHECK(b->state == MountState::Inert && b->reason == "expected bytes differ at 150 (splice 2; game updated?)");
    CHECK(t->Open("data/hdr.dcb", mb, base.size()) == nullptr);
    CHECK(Info(t->Mounts(), "badheader")->state == MountState::Inert &&
          Info(t->Mounts(), "badheader")->reason == "base header differs (game updated?)");

    // A base too short for the expected bytes is a mismatch, not a crash.
    MemBase cut(base);
    cut.readable = 152;
    auto t2 = Table::Build({ SpliceMount("x", 0, "good", good, header) });
    CHECK(t2->Open("x", cut, base.size()) == nullptr && Info(t2->Mounts(), "good")->state == MountState::Inert);

    // An expected-bytes mount that fails leaves a second mount of the same path applying alone.
    auto t3 = Table::Build({ SpliceMount("p", 5, "badold", bad), SpliceMount("p", 1, "other", { S(60, 1, B({ 9 })) }) });
    auto f3 = t3->Open("p", mb, base.size());
    CHECK(ReadsAs(f3, mb, Materialize(base, { S(60, 1, B({ 9 })) })));
    CHECK(Info(t3->Mounts(), "badold")->state == MountState::Inert && Info(t3->Mounts(), "other")->state == MountState::Applied);
}

// One composition per path and base identity; a different base composes again.
static void TestCache() {
    const Bytes base = Base(300, 6);
    int calls = 0;
    auto t = Table::Build({ TransformMount("f", 0, "tf", [&](sco::vfs::BaseIo&, uint64_t, std::vector<Splice>& out) {
        ++calls;
        out.push_back(S(10, 1, B({ 0 })));
        return Result{};
    }) });
    MemBase mb(base);
    auto a = t->Open("f", mb, base.size());
    auto b = t->Open("F", mb, base.size());
    CHECK(a && a == b && calls == 1);
    Bytes other = base;
    other[5] ^= 1;   // first 4 KiB differ: another build of the file
    MemBase mo(other);
    auto c = t->Open("f", mo, other.size());
    CHECK(c && c != a && calls == 2 && ReadsAs(c, mo, Materialize(other, { S(10, 1, B({ 0 })) })));
    Bytes longer = base;
    longer.push_back(1);   // same head, other size
    MemBase ml(longer);
    CHECK(t->Open("f", ml, longer.size()) != a && calls == 3);
    CHECK(t->Open("f", mb, base.size()) == a && calls == 3);
    // Unmounted paths: no base access at all.
    MemBase untouched(base);
    CHECK(!t->Mounted("g") && t->Open("g", untouched, base.size()) == nullptr);
    CHECK(untouched.seeks == 0 && untouched.reads == 0);
    auto none = Table::Build({});
    CHECK(!none->Mounted("f") && none->Open("f", untouched, 1) == nullptr && untouched.seeks == 0);
    CHECK(!t->Mounted(std::string(sco::vfs::kMaxPathLength + 1, 'f')));
}

// Merge across mounts: priority wins overlaps, the loser names the winner, transforms that fail
// or throw are inert.
static void TestMerge() {
    const Bytes base = Base(400, 7);
    MemBase mb(base);
    auto t = Table::Build({
        SpliceMount("p", 1, "low", { S(10, 5, B("LOW")), S(100, 1, B("l")), S(300, 0, B("ins")) }),
        SpliceMount("p", 9, "high", { S(12, 2, B("HIGH")), S(200, 1, B("h")) }),
        SpliceMount("p", 9, "high2", { S(200, 1, B("H")), S(250, 0, B("x")) }),   // equal priority, later: wins 200
        TransformMount("p", 20, "broken", [](sco::vfs::BaseIo&, uint64_t, std::vector<Splice>& out) {
            out.push_back(S(0, 1, B("!")));
            return Result{ "record not found" };
        }),
        TransformMount("p", 20, "throws", [](sco::vfs::BaseIo&, uint64_t, std::vector<Splice>&) -> Result {
            throw std::runtime_error("bad file");
        }),
        TransformMount("p", 20, "unsorted", [](sco::vfs::BaseIo&, uint64_t, std::vector<Splice>& out) {
            out = { S(50, 1), S(40, 1) };
            return Result{};
        }),
        TransformMount("p", 20, "pastend", [](sco::vfs::BaseIo&, uint64_t size, std::vector<Splice>& out) {
            out = { S(size, 1) };
            return Result{};
        }),
    });
    auto file = t->Open("p", mb, base.size());
    const std::vector<Splice> expect = { S(12, 2, B("HIGH")), S(100, 1, B("l")), S(200, 1, B("H")), S(250, 0, B("x")),
                                         S(300, 0, B("ins")) };
    CHECK(ReadsAs(file, mb, Materialize(base, expect)));
    const auto all = t->Mounts();
    CHECK(all.size() == 7 && all[0].source == "low" && all[6].source == "pastend");   // Build order
    CHECK(Info(all, "low")->state == MountState::Applied &&
          Info(all, "low")->reason == "1 splice(s) dropped; first: splice at 10 overlaps one from high (higher priority)");
    CHECK(Info(all, "high")->state == MountState::Applied &&
          Info(all, "high")->reason.find("splice at 200 overlaps one from high2") != std::string::npos);
    CHECK(Info(all, "high2")->state == MountState::Applied && Info(all, "high2")->reason.empty());
    CHECK(Info(all, "broken")->state == MountState::Inert && Info(all, "broken")->reason == "transform failed: record not found");
    CHECK(Info(all, "throws")->state == MountState::Inert && Info(all, "throws")->reason == "transform failed: threw: bad file");
    CHECK(Info(all, "unsorted")->state == MountState::Inert &&
          Info(all, "unsorted")->reason.find("splice 1 (at 40): comes before splice 0") != std::string::npos);
    CHECK(Info(all, "pastend")->state == MountState::Inert &&
          Info(all, "pastend")->reason.find("reaches past the end") != std::string::npos);

    // Insertions at the same offset from two sources conflict; inserting right after a removed
    // range doesn't.
    auto t2 = Table::Build({ SpliceMount("q", 2, "a", { S(10, 5) }), SpliceMount("q", 1, "b", { S(10, 0, B("z")), S(15, 0, B("y")) }) });
    CHECK(ReadsAs(t2->Open("q", mb, base.size()), mb, Materialize(base, { S(10, 5), S(15, 0, B("y")) })));
    CHECK(Info(t2->Mounts(), "b")->reason.find("1 splice(s) dropped; first: splice at 10") == 0);

    // Every mount inert: the file passes through.
    auto t3 = Table::Build({ TransformMount("r", 0, "broken", [](sco::vfs::BaseIo&, uint64_t, std::vector<Splice>&) {
        return Result{ "nope" };
    }) });
    CHECK(t3->Mounted("r") && t3->Open("r", mb, base.size()) == nullptr);
}

static void TestLimits() {
    const Bytes base = Base(1000, 8);
    MemBase mb(base);
    // Build refusals: bad path, unsorted splices, an empty transform, too many splices in one mount.
    Limits small;
    small.splicesPerPath = 3;
    std::vector<Splice> four = { S(1, 1), S(3, 1), S(5, 1), S(7, 1) };
    auto t = Table::Build({ SpliceMount("", 0, "nopath", { S(1, 1) }), SpliceMount("a", 0, "unsorted", { S(5, 1), S(1, 1) }),
                            TransformMount("a", 0, "empty", nullptr), SpliceMount("b", 0, "four", four),
                            SpliceMount("c", 0, "three", { S(1, 1), S(3, 1), S(5, 1) }) },
                          small);
    auto all = t->Mounts();
    CHECK(Info(all, "nopath")->state == MountState::Refused && Info(all, "nopath")->reason.find("bad path") == 0);
    CHECK(Info(all, "unsorted")->state == MountState::Refused &&
          Info(all, "unsorted")->reason.find("splice 1 (at 1): comes before") == 0);
    CHECK(Info(all, "empty")->state == MountState::Refused && Info(all, "empty")->reason == "no producer");
    CHECK(Info(all, "four")->state == MountState::Refused && Info(all, "four")->reason == "4 splices, over the limit of 3");
    CHECK(!t->Mounted("a") && !t->Mounted("b") && t->Mounted("c"));   // refused mounts don't mount the path
    CHECK(t->Open("c", mb, base.size()) != nullptr);

    // Splices per path after the merge: two mounts of 2 each over a limit of 3 make the path inert.
    auto t2 = Table::Build({ SpliceMount("p", 1, "x", { S(1, 1), S(3, 1) }), SpliceMount("p", 0, "y", { S(5, 1), S(7, 1) }) }, small);
    CHECK(t2->Open("p", mb, base.size()) == nullptr);
    CHECK(Info(t2->Mounts(), "x")->state == MountState::Inert &&
          Info(t2->Mounts(), "x")->reason == "path has 4 splices, over the limit of 3");
    // A transform returning too many splices is inert.
    auto t2b = Table::Build({ TransformMount("p", 0, "many", [&](sco::vfs::BaseIo&, uint64_t, std::vector<Splice>& out) {
        out = four;
        return Result{};
    }) }, small);
    CHECK(t2b->Open("p", mb, base.size()) == nullptr && Info(t2b->Mounts(), "many")->reason == "4 splices, over the limit of 3");

    // Replacement-byte budget: highest priority first, what doesn't fit is refused.
    Limits budget;
    budget.bufferBytes = 100;
    auto bytes = [](size_t n) { return std::make_shared<const Bytes>(n, uint8_t{ 1 }); };
    auto t3 = Table::Build({ SpliceMount("a", 1, "low40", { S(0, 0, bytes(40)) }), SpliceMount("b", 3, "high60", { S(0, 0, bytes(60)) }),
                             SpliceMount("c", 2, "mid30", { S(0, 0, bytes(30)) }), SpliceMount("d", 0, "lowest0", { S(0, 1) }) },
                           budget);
    all = t3->Mounts();
    CHECK(Info(all, "high60")->state == MountState::Pending && Info(all, "mid30")->state == MountState::Pending);
    CHECK(Info(all, "low40")->state == MountState::Refused &&
          Info(all, "low40")->reason == "replacement bytes over the budget of 100");
    CHECK(Info(all, "lowest0")->state == MountState::Pending);   // a deletion costs nothing
    CHECK(t3->BufferBytes() == 90);
    // Transform output counts against what is left at composition time.
    std::atomic<size_t> ask{ 20 };
    auto tf = [&](sco::vfs::BaseIo&, uint64_t, std::vector<Splice>& out) {
        out = { S(0, 0, bytes(ask.load())) };
        return Result{};
    };
    auto t4 = Table::Build({ SpliceMount("a", 0, "fixed", { S(0, 0, bytes(60)) }), TransformMount("t1", 0, "t1", tf),
                             TransformMount("t2", 0, "t2", tf) },
                           budget);
    CHECK(t4->Open("t1", mb, base.size()) != nullptr && t4->BufferBytes() == 80);
    ask.store(21);   // 20 are left
    CHECK(t4->Open("t2", mb, base.size()) == nullptr && Info(t4->Mounts(), "t2")->reason == "replacement bytes over the budget of 100");
    CHECK(t4->BufferBytes() == 80);

    // Virtual file size: over Limits::fileSize the path is inert.
    Limits tiny;
    tiny.fileSize = 1000;
    auto t5 = Table::Build({ SpliceMount("f", 0, "grow", { S(0, 0, B({ 1 })) }) }, tiny);
    CHECK(t5->Open("f", mb, base.size()) == nullptr);
    CHECK(Info(t5->Mounts(), "grow")->state == MountState::Inert &&
          Info(t5->Mounts(), "grow")->reason == "virtual size is over the limit of 1000 bytes");
    auto t6 = Table::Build({ SpliceMount("f", 0, "same", { S(0, 1, B({ 1 })) }) }, tiny);
    CHECK(t6->Open("f", mb, base.size()) != nullptr);
}

// ---- threads -----------------------------------------------------------------------------------

// Eight threads open one path of one table at the same moment: one composition, one file.
static void TestOneComposition() {
    const Bytes base = Base(64 * 1024, 9);
    const std::vector<Splice> sp = { S(100, 10, B("first")), S(5000, 0, B("second")), S(40000, 300) };
    const Bytes want = Materialize(base, sp);
    std::atomic<int> calls{ 0 };
    auto t = Table::Build({ TransformMount("data/game2.dcb", 0, "dc", [&](sco::vfs::BaseIo&, uint64_t, std::vector<Splice>& out) {
        calls.fetch_add(1);
        out = sp;
        return Result{};
    }) });
    constexpr int kThreads = 8;
    std::latch go(kThreads);
    std::vector<std::shared_ptr<const Composed>> files(kThreads);
    std::atomic<int> bad{ 0 };
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            MemBase mb(base);   // each thread its own handle
            go.arrive_and_wait();
            files[static_cast<size_t>(i)] = t->Open("Data\\Game2.dcb", mb, base.size());
            if (!ReadsAs(files[static_cast<size_t>(i)], mb, want)) bad.fetch_add(1);
        });
    }
    for (auto& th : threads) th.join();
    CHECK(calls.load() == 1);
    CHECK(bad.load() == 0);
    bool same = true;
    for (auto& f : files) same = same && f && f == files[0];
    CHECK(same);
}

// Readers open, read and keep handles while a writer publishes new tables. Each table composes
// its path at most once; handles opened on an old table keep reading their file after the swap;
// unmounted paths pass through.
static void TestReadersBesideSwaps() {
    const Bytes base = Base(16 * 1024, 10);
    const Bytes other = Base(4 * 1024, 11);
    const std::vector<Splice> spA = { S(0, 0, B("head")), S(777, 3, B("mid")), S(9000, 1000), S(base.size(), 0, B("end")) };
    const std::vector<Splice> spB = { S(10, 10, B("other")) };
    const Bytes wantA = Materialize(base, spA), wantB = Materialize(other, spB);
    constexpr int kSwaps = 100, kReaders = 4;
    std::vector<std::unique_ptr<std::atomic<int>>> counters;
    for (int i = 0; i <= kSwaps; ++i) counters.push_back(std::make_unique<std::atomic<int>>(0));
    auto build = [&](std::atomic<int>* calls) {
        return Table::Build({ TransformMount("data/game2.dcb", 0, "dc", [&spA, calls](sco::vfs::BaseIo&, uint64_t, std::vector<Splice>& out) {
                                  calls->fetch_add(1);
                                  out = spA;
                                  return Result{};
                              }),
                              SpliceMount("data/other.bin", 0, "pack", spB) });
    };
    MountTable mounts;
    CHECK(mounts.Current() == nullptr);
    mounts.Publish(build(counters[0].get()));
    std::atomic<bool> stop{ false };
    std::atomic<int> bad{ 0 }, rounds{ 0 };
    std::latch go(kReaders + 1);
    std::vector<std::thread> readers;
    for (int i = 0; i < kReaders; ++i) {
        readers.emplace_back([&, i] {
            std::mt19937_64 rng(static_cast<uint64_t>(i) + 1);
            MemBase mbA(base), mbB(other), mbC(base);
            Reader held;   // opened on some earlier table, read across swaps
            uint64_t heldAt = 0;
            go.arrive_and_wait();
            do {
                auto snap = mounts.Current();
                if (!snap || !snap->Mounted("DATA/GAME2.DCB") || !snap->Mounted("data\\other.bin") ||
                    snap->Mounted("data/unmounted.dat")) {
                    bad.fetch_add(1);
                    continue;
                }
                auto fa = snap->Open("data/game2.dcb", mbA, base.size());
                auto fb = snap->Open("data/other.bin", mbB, other.size());
                if (!ReadsAs(fa, mbA, wantA) || !ReadsAs(fb, mbB, wantB)) bad.fetch_add(1);
                if (!held.File() || held.Eof()) {
                    held = Reader(fa);
                    heldAt = 0;
                }
                uint8_t chunk[512];
                const size_t k = held.Read(chunk, 1 + static_cast<size_t>(rng() % sizeof chunk), mbC);
                if (k == 0 || std::memcmp(chunk, wantA.data() + heldAt, k) != 0) bad.fetch_add(1);
                heldAt += k;
                rounds.fetch_add(1);
            } while (!stop.load());
        });
    }
    go.arrive_and_wait();
    for (int i = 1; i <= kSwaps; ++i) mounts.Publish(build(counters[static_cast<size_t>(i)].get()));
    stop.store(true);
    for (auto& th : readers) th.join();
    CHECK(bad.load() == 0);
    CHECK(rounds.load() >= kReaders);
    bool once = true;
    for (auto& c : counters) once = once && c->load() <= 1;
    CHECK(once);
    // The final table composes once more for a late opener, then never again.
    MemBase late(base);
    auto last = mounts.Current();
    CHECK(ReadsAs(last->Open("data/game2.dcb", late, base.size()), late, wantA));
    CHECK(ReadsAs(last->Open("data/game2.dcb", late, base.size()), late, wantA));
    CHECK(counters[static_cast<size_t>(kSwaps)]->load() == 1);
}

int main() {
    TestCompose();
    TestComposeRejects();
    TestReadEveryBoundary();
    TestSeek();
    TestBaseSeeks();
    TestShortBase();
    TestRandom();
    TestLargeFiles();
    TestPaths();
    TestExpectedOldBytes();
    TestCache();
    TestMerge();
    TestLimits();
    TestOneComposition();
    TestReadersBesideSwaps();
    std::printf("sco-core vfs tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
