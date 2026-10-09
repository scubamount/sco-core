// sco::vfs core arithmetic (sco/vfs.h): Compose turns splices into segments, Reader serves them.
#include "internal.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace sco::vfs {

namespace detail {

static uint64_t Added(const Splice& s) { return s.bytes ? s.bytes->size() : 0; }

uint64_t AddedBytes(std::span<const Splice> splices) {
    uint64_t n = 0;
    for (const Splice& s : splices) n += Added(s);
    return n;
}

Result CheckSplices(std::span<const Splice> splices, uint64_t baseSize) {
    auto fail = [&](size_t i, const std::string& what) {
        return Result{ "splice " + std::to_string(i) + " (at " + std::to_string(splices[i].at) + "): " + what };
    };
    for (size_t i = 0; i < splices.size(); ++i) {
        const Splice& s = splices[i];
        if (s.removed == 0 && Added(s) == 0) return fail(i, "removes nothing and adds nothing");
        if (s.old && s.old->size() != s.removed)
            return fail(i, "expects " + std::to_string(s.old->size()) + " old bytes but removes " +
                               std::to_string(s.removed));
        if (s.at > baseSize || s.removed > baseSize - s.at)
            return fail(i, "reaches past the end of the base (" + std::to_string(baseSize) + " bytes)");
        if (i > 0) {
            const Splice& p = splices[i - 1];
            if (s.at < p.at)
                return fail(i, "comes before splice " + std::to_string(i - 1) + " (at " + std::to_string(p.at) +
                                   "): splices must be sorted by offset");
            if (s.at == p.at || s.at < p.at + p.removed)
                return fail(i, "overlaps splice " + std::to_string(i - 1) + " (at " + std::to_string(p.at) +
                                   ", removes " + std::to_string(p.removed) + ")");
        }
    }
    return {};
}

}  // namespace detail

Result Compose(uint64_t baseSize, std::span<const Splice> splices, Composed& out, uint64_t maxSize) {
    if (Result r = detail::CheckSplices(splices, baseSize); !r) return r;
    uint64_t removed = 0;
    for (const Splice& s : splices) removed += s.removed;   // <= baseSize: the ranges are disjoint
    const uint64_t kept = baseSize - removed;
    const uint64_t added = detail::AddedBytes(splices);
    if (added > maxSize || kept > maxSize - added)
        return { "virtual size is over the limit of " + std::to_string(maxSize) + " bytes" };

    Composed c;
    c.baseSize = baseSize;
    c.size = kept + added;
    c.segments.reserve(splices.size() * 2 + 1);
    uint64_t basePos = 0, vpos = 0;
    for (const Splice& s : splices) {
        if (s.at > basePos) {
            c.segments.push_back({ vpos, s.at - basePos, Segment::Base, basePos, nullptr });
            vpos += s.at - basePos;
        }
        if (s.bytes && !s.bytes->empty()) {
            c.segments.push_back({ vpos, s.bytes->size(), Segment::Buffer, 0, s.bytes->data() });
            c.buffers.push_back(s.bytes);
            vpos += s.bytes->size();
        }
        basePos = s.at + s.removed;
    }
    if (basePos < baseSize) c.segments.push_back({ vpos, baseSize - basePos, Segment::Base, basePos, nullptr });
    out = std::move(c);
    return {};
}

bool Reader::Seek(int64_t off, Whence whence) {
    constexpr uint64_t kMax = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    uint64_t origin = 0;
    switch (whence) {
    case Whence::Set: origin = 0; break;
    case Whence::Cur: origin = pos_; break;
    case Whence::End: origin = Size(); break;
    default: return false;
    }
    if (origin > kMax) return false;
    const int64_t o = static_cast<int64_t>(origin);
    if (off > 0 && o > std::numeric_limits<int64_t>::max() - off) return false;
    const int64_t next = o + off;   // o >= 0, so a negative off can't overflow
    if (next < 0) return false;
    pos_ = static_cast<uint64_t>(next);
    return true;
}

size_t Reader::Read(void* dst, size_t n, BaseIo& base) {
    if (!file_ || !dst || n == 0 || pos_ >= file_->size) return 0;
    const std::vector<Segment>& segs = file_->segments;
    // The last segment starting at or before pos_ (segments tile [0, size), so it contains pos_).
    auto it = std::upper_bound(segs.begin(), segs.end(), pos_,
                               [](uint64_t p, const Segment& s) { return p < s.start; });
    --it;
    uint8_t* out = static_cast<uint8_t*>(dst);
    size_t done = 0;
    for (; done < n && it != segs.end(); ++it) {
        const uint64_t off = pos_ - it->start;
        const size_t want = static_cast<size_t>(std::min<uint64_t>(it->len - off, n - done));
        if (it->kind == Segment::Buffer) {
            std::memcpy(out + done, it->src + it->from + off, want);
            done += want;
            pos_ += want;
            continue;
        }
        const uint64_t at = it->from + off;
        if (basePos_ != at) {
            if (!base.Seek(at)) {
                basePos_ = kUnknown;
                break;
            }
            basePos_ = at;
        }
        const size_t got = base.Read(out + done, want);
        if (got > want) {   // a broken BaseIo; don't trust the bytes or its position
            basePos_ = kUnknown;
            break;
        }
        basePos_ += got;
        done += got;
        pos_ += got;
        if (got < want) break;
    }
    return done;
}

}  // namespace sco::vfs
