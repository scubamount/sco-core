// The mount table (sco/vfs.h): mounts per path, their merge, and one composition per base.
#include "internal.h"
#include <algorithm>
#include <cstring>
#include <exception>
#include <iterator>
#include <map>
#include <mutex>
#include <numeric>
#include <unordered_map>

namespace sco::vfs {

namespace {

constexpr size_t kTooLong = ~size_t{ 0 };
constexpr size_t kIdentityBytes = 4096;

// Writes path's normalized form into out; its length, or kTooLong when over cap.
size_t NormalizeInto(std::string_view path, char* out, size_t cap) {
    size_t n = 0;
    for (char c : path) {
        if (c == '\\') c = '/';
        if (c == '/' && (n == 0 || out[n - 1] == '/')) continue;   // leading and repeated separators
        if (n == cap) return kTooLong;
        out[n++] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return n;
}

struct PathHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};

uint64_t Fnv1a(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

// Reads n bytes at the base's current position, looping over short reads that still make progress.
size_t ReadFull(BaseIo& base, uint8_t* dst, size_t n) {
    size_t done = 0;
    while (done < n) {
        const size_t got = base.Read(dst + done, n - done);
        if (got == 0 || got > n - done) break;
        done += got;
    }
    return done;
}

// True when the base holds exactly `expected` at `at`.
bool Matches(BaseIo& base, uint64_t at, const Bytes& expected) {
    if (expected.empty()) return true;
    if (!base.Seek(at)) return false;
    uint8_t chunk[4096];
    for (size_t done = 0; done < expected.size();) {
        const size_t want = std::min(sizeof chunk, expected.size() - done);
        if (ReadFull(base, chunk, want) != want || std::memcmp(chunk, expected.data() + done, want) != 0)
            return false;
        done += want;
    }
    return true;
}

}  // namespace

std::string NormalizePath(std::string_view path) {
    std::string out(std::min(path.size(), kMaxPathLength), '\0');
    const size_t n = NormalizeInto(path, out.data(), out.size());
    if (n == kTooLong) return {};
    out.resize(n);
    return out;
}

struct Table::Impl {
    struct Rec {
        MountInfo info;   // state and reason guarded by infoMu after Build
        std::variant<SpliceList, Transform> producer;
        uint64_t reserved = 0;   // Transform: bytes counted against the budget at composition
    };
    struct Cached {
        uint64_t size, hash;
        std::shared_ptr<const Composed> file;   // null: passes through
    };
    struct Entry {
        std::vector<size_t> recs;   // highest priority first
        std::mutex mu;              // the per-path once-guard; guards cache
        std::vector<Cached> cache;
    };

    Limits limits;
    std::vector<Rec> recs;
    std::unordered_map<std::string, std::unique_ptr<Entry>, PathHash, std::equal_to<>> entries;
    std::atomic<uint64_t> bufferBytes{ 0 };
    mutable std::mutex infoMu;

    void SetState(size_t i, MountState state, std::string reason) {
        std::lock_guard lock(infoMu);
        recs[i].info.state = state;
        recs[i].info.reason = std::move(reason);
    }

    bool Reserve(uint64_t n) {
        uint64_t cur = bufferBytes.load();
        do {
            if (n > limits.bufferBytes - cur) return false;
        } while (!bufferBytes.compare_exchange_weak(cur, cur + n));
        return true;
    }

    void Unreserve(Rec& r) {
        bufferBytes.fetch_sub(r.reserved);
        r.reserved = 0;
    }

    // Under entry.mu. Never leaves a partial result: null (pass through) or the whole file.
    std::shared_ptr<const Composed> ComposeEntry(Entry& e, BaseIo& base, uint64_t baseSize) {
        struct Pick { Splice s; size_t rec; };
        std::map<uint64_t, Pick> taken;
        std::vector<size_t> applied;
        for (size_t idx : e.recs) {
            Rec& r = recs[idx];
            std::vector<Splice> made;
            const std::vector<Splice>* list = nullptr;
            std::string why;
            if (const SpliceList* sl = std::get_if<SpliceList>(&r.producer)) {
                list = &sl->splices;
                if (sl->header && !Matches(base, 0, *sl->header)) why = "base header differs (game updated?)";
            } else {
                list = &made;
                Result res;
                try {
                    res = std::get<Transform>(r.producer)(base, baseSize, made);
                } catch (const std::exception& ex) {
                    res.error = std::string("threw: ") + ex.what();
                } catch (...) {
                    res.error = "threw";
                }
                if (!res) why = "transform failed: " + res.error;
                else if (made.size() > limits.splicesPerPath)
                    why = std::to_string(made.size()) + " splices, over the limit of " + std::to_string(limits.splicesPerPath);
            }
            if (why.empty())
                if (Result c = detail::CheckSplices(*list, baseSize); !c) why = c.error;
            if (why.empty()) {
                for (size_t i = 0; i < list->size(); ++i) {
                    const Splice& s = (*list)[i];
                    if (s.old && !Matches(base, s.at, *s.old)) {
                        why = "expected bytes differ at " + std::to_string(s.at) + " (splice " + std::to_string(i) +
                              "; game updated?)";
                        break;
                    }
                }
            }
            if (why.empty() && list == &made) {
                const uint64_t bytes = detail::AddedBytes(made);
                if (Reserve(bytes)) r.reserved = bytes;
                else why = "replacement bytes over the budget of " + std::to_string(limits.bufferBytes);
            }
            if (!why.empty()) {
                SetState(idx, MountState::Inert, std::move(why));
                continue;
            }
            // Merge under the higher-priority splices already taken (those never overlap each other).
            size_t dropped = 0;
            std::string first;
            for (const Splice& s : *list) {
                const Pick* clash = nullptr;
                auto next = taken.lower_bound(s.at);
                if (next != taken.end() && (next->first == s.at || next->first < s.at + s.removed)) clash = &next->second;
                if (!clash && next != taken.begin()) {
                    auto prev = std::prev(next);
                    if (prev->first + prev->second.s.removed > s.at) clash = &prev->second;
                }
                if (clash) {
                    if (dropped++ == 0)
                        first = "splice at " + std::to_string(s.at) + " overlaps one from " +
                                recs[clash->rec].info.source + " (higher priority)";
                    continue;
                }
                taken.emplace(s.at, Pick{ s, idx });
            }
            SetState(idx, MountState::Applied,
                     dropped ? std::to_string(dropped) + " splice(s) dropped; first: " + first : std::string());
            applied.push_back(idx);
        }
        if (taken.empty()) return nullptr;

        std::vector<Splice> merged;
        merged.reserve(taken.size());
        for (auto& kv : taken) merged.push_back(std::move(kv.second.s));
        std::string why;
        if (merged.size() > limits.splicesPerPath) {
            why = "path has " + std::to_string(merged.size()) + " splices, over the limit of " +
                  std::to_string(limits.splicesPerPath);
        } else {
            auto file = std::make_shared<Composed>();
            Result r = Compose(baseSize, merged, *file, limits.fileSize);
            if (r) return file;
            why = r.error;
        }
        for (size_t idx : applied) {
            Unreserve(recs[idx]);
            SetState(idx, MountState::Inert, why);
        }
        return nullptr;
    }
};

Table::Table() : impl_(std::make_unique<Impl>()) {}
Table::~Table() = default;

std::shared_ptr<const Table> Table::Build(std::vector<Mount> mounts, const Limits& limits) {
    std::shared_ptr<Table> t(new Table());
    Impl& im = *t->impl_;
    im.limits = limits;
    im.recs.reserve(mounts.size());
    for (Mount& m : mounts) {
        Impl::Rec r;
        r.info.path = NormalizePath(m.path);
        r.info.source = std::move(m.source);
        r.info.priority = m.priority;
        r.producer = std::move(m.producer);
        std::string why;
        if (r.info.path.empty()) {
            why = "bad path (empty or over " + std::to_string(kMaxPathLength) + " characters)";
        } else if (const SpliceList* sl = std::get_if<SpliceList>(&r.producer)) {
            if (sl->splices.size() > limits.splicesPerPath)
                why = std::to_string(sl->splices.size()) + " splices, over the limit of " +
                      std::to_string(limits.splicesPerPath);
            else if (Result c = detail::CheckSplices(sl->splices, ~0ull); !c)
                why = c.error;
        } else if (!std::get<Transform>(r.producer)) {
            why = "no producer";
        }
        if (!why.empty()) {
            r.info.state = MountState::Refused;
            r.info.reason = std::move(why);
        }
        im.recs.push_back(std::move(r));
    }

    // Highest priority first; among equals, later in the list first.
    std::vector<size_t> order(im.recs.size());
    std::iota(order.begin(), order.end(), size_t{ 0 });
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (im.recs[a].info.priority != im.recs[b].info.priority) return im.recs[a].info.priority > im.recs[b].info.priority;
        return a > b;
    });

    // The replacement-byte budget, taken in priority order: what doesn't fit is refused, so the
    // lowest priorities go first.
    uint64_t used = 0;
    for (size_t idx : order) {
        Impl::Rec& r = im.recs[idx];
        const SpliceList* sl = std::get_if<SpliceList>(&r.producer);
        if (r.info.state == MountState::Refused || !sl) continue;
        const uint64_t bytes = detail::AddedBytes(sl->splices);
        if (bytes > limits.bufferBytes - used) {
            r.info.state = MountState::Refused;
            r.info.reason = "replacement bytes over the budget of " + std::to_string(limits.bufferBytes);
            continue;
        }
        used += bytes;
    }
    im.bufferBytes.store(used);

    for (size_t idx : order) {
        Impl::Rec& r = im.recs[idx];
        if (r.info.state == MountState::Refused) continue;
        auto& entry = im.entries[r.info.path];
        if (!entry) entry = std::make_unique<Impl::Entry>();
        entry->recs.push_back(idx);
    }
    return t;
}

bool Table::Mounted(std::string_view path) const noexcept {
    if (impl_->entries.empty()) return false;
    char buf[kMaxPathLength];
    const size_t n = NormalizeInto(path, buf, sizeof buf);
    return n != kTooLong && impl_->entries.find(std::string_view(buf, n)) != impl_->entries.end();
}

std::shared_ptr<const Composed> Table::Open(std::string_view path, BaseIo& base, uint64_t baseSize) const noexcept {
    if (impl_->entries.empty()) return nullptr;
    char buf[kMaxPathLength];
    const size_t n = NormalizeInto(path, buf, sizeof buf);
    if (n == kTooLong) return nullptr;
    auto it = impl_->entries.find(std::string_view(buf, n));
    if (it == impl_->entries.end()) return nullptr;
    Impl::Entry& e = *it->second;
    std::lock_guard lock(e.mu);
    try {
        uint8_t head[kIdentityBytes];
        const size_t want = static_cast<size_t>(std::min<uint64_t>(baseSize, sizeof head));
        const size_t got = (want && base.Seek(0)) ? ReadFull(base, head, want) : 0;
        const uint64_t hash = Fnv1a(head, got);
        for (const Impl::Cached& c : e.cache)
            if (c.size == baseSize && c.hash == hash) return c.file;
        std::shared_ptr<const Composed> file = impl_->ComposeEntry(e, base, baseSize);
        e.cache.push_back({ baseSize, hash, file });
        return file;
    } catch (const std::exception& ex) {
        for (size_t idx : e.recs) impl_->SetState(idx, MountState::Inert, std::string("composition failed: ") + ex.what());
    } catch (...) {
        for (size_t idx : e.recs) impl_->SetState(idx, MountState::Inert, "composition failed");
    }
    return nullptr;
}

std::vector<MountInfo> Table::Mounts() const {
    std::lock_guard lock(impl_->infoMu);
    std::vector<MountInfo> out;
    out.reserve(impl_->recs.size());
    for (const Impl::Rec& r : impl_->recs) out.push_back(r.info);
    return out;
}

uint64_t Table::BufferBytes() const { return impl_->bufferBytes.load(); }

}  // namespace sco::vfs
