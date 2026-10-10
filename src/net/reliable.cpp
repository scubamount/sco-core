// sco::net reliable streams (sco/net/reliable.h).
#include "sco/net/reliable.h"
#include "sco/net/wire.h"
#include <algorithm>

namespace sco::net {

bool SendStream::Push(uint64_t origin, bool toHost, const uint8_t* data, uint32_t len) {
    if (len > kMaxReliable) return false;
    const uint32_t units = len == 0 ? 1u : (len + kFragBytes - 1) / kFragBytes;
    if (q_.size() + units > kMaxQueuedUnits) return false;
    uint32_t off = 0;
    for (uint32_t i = 0; i < units; ++i) {
        const uint32_t n = std::min(kFragBytes, len - off);
        OutUnit u;
        u.seq = next_++;
        u.origin = origin;
        u.msgLen = len;
        u.offset = off;
        u.toHost = toHost;
        if (n) u.data.assign(data + off, data + off + n);
        q_.push_back(std::move(u));
        off += n;
    }
    return true;
}

void SendStream::OnAck(uint64_t next, uint64_t mask) {
    if (next > next_) return;
    for (OutUnit& u : q_) {
        if (u.seq < next) {
            u.acked = true;
        } else if (u.seq > next) {
            const uint64_t bit = u.seq - next - 1;
            if (bit >= 64) break;
            if ((mask >> bit) & 1u) u.acked = true;
        }
    }
    while (!q_.empty() && q_.front().acked) q_.pop_front();
}

std::vector<const OutUnit*> SendStream::Due(uint64_t nowMs, uint32_t firstRtoMs, uint32_t maxRtoMs) {
    std::vector<const OutUnit*> due;
    if (q_.empty()) return due;
    const uint64_t end = q_.front().seq + kWindow;
    for (OutUnit& u : q_) {
        if (u.seq >= end) break;
        if (u.acked) continue;
        if (u.sends != 0 && nowMs < u.lastSendMs + u.rtoMs) continue;
        u.rtoMs = u.sends == 0 ? firstRtoMs : std::min(maxRtoMs, u.rtoMs * 2);
        ++u.sends;
        u.lastSendMs = nowMs;
        due.push_back(&u);
    }
    return due;
}

RecvStream::Verdict RecvStream::OnUnit(uint64_t seq, uint64_t origin, uint32_t msgLen, uint32_t offset,
                                       const uint8_t* data, uint32_t len, uint32_t maxLen,
                                       std::vector<Delivery>* out) {
    // Fields that can't belong to any honest message; checked before anything is stored.
    if (len > kFragBytes || msgLen > kMaxReliable || offset > msgLen || len > msgLen - offset ||
        (len == 0 && msgLen != 0))
        return Verdict::Malformed;
    if (seq < next_) {
        ackDue = true;
        return Verdict::Duplicate;
    }
    if (seq - next_ >= kWindow) return Verdict::Ahead;
    if (ahead_.count(seq)) {
        ackDue = true;
        return Verdict::Duplicate;
    }
    Unit u{ origin, msgLen, offset, std::vector<uint8_t>(data, data + len) };
    ahead_.emplace(seq, std::move(u));
    ackDue = true;
    for (auto it = ahead_.find(next_); it != ahead_.end(); it = ahead_.find(next_)) {
        Consume(it->second, maxLen, out);
        ahead_.erase(it);
        ++next_;
    }
    return Verdict::Accepted;
}

void RecvStream::Consume(const Unit& u, uint32_t maxLen, std::vector<Delivery>* out) {
    if (inMsg_ && (u.offset != got_ || u.msgLen != msgLen_ || u.origin != origin_)) {
        // An authenticated peer sent fragments that don't line up: abandon the message.
        ++broken_;
        inMsg_ = false;
        buf_.clear();
    }
    if (!inMsg_) {
        if (u.offset != 0) {
            ++broken_;
            return;
        }
        inMsg_ = true;
        origin_ = u.origin;
        msgLen_ = u.msgLen;
        got_ = 0;
        discard_ = msgLen_ > maxLen;   // decided before any allocation of msgLen_
        buf_.clear();
        if (discard_) {
            ++refused_;
        } else {
            buf_.reserve(msgLen_);
        }
    }
    if (!discard_) buf_.insert(buf_.end(), u.data.begin(), u.data.end());
    got_ += static_cast<uint32_t>(u.data.size());
    if (got_ == msgLen_) {
        if (!discard_) out->push_back(Delivery{ origin_, std::move(buf_) });
        buf_ = std::vector<uint8_t>();
        inMsg_ = false;
    }
}

uint64_t RecvStream::Mask() const {
    uint64_t m = 0;
    for (const auto& kv : ahead_) {
        const uint64_t bit = kv.first - next_ - 1;   // ahead_ holds only seqs past next_
        if (bit < 64) m |= uint64_t{ 1 } << bit;
    }
    return m;
}

}  // namespace sco::net
