#pragma once
// sco::net reliable streams: exactly-once, in-order delivery of messages up to kMaxReliable bytes
// over lossy, duplicating, reordering datagrams. One SendStream / RecvStream pair per link,
// channel and direction. Platform-free and clock-free: the caller passes the time.
//
// A message is cut into units of at most kFragBytes; every unit has a stream sequence number
// (unit seq, from 0, consecutive across messages) and carries the message length and its own
// offset. The receiver buffers units inside a window of kWindow after the first one it misses,
// delivers messages in unit order, and acknowledges with (next expected unit, bitmask of the
// kWindow - 1 units after it). The sender keeps at most kWindow units in flight and resends a
// unit that isn't acknowledged after its timeout, doubling it up to a ceiling. Retransmissions are
// new packets with new packet seqs (the replay window never sees a seq twice); the unit seq makes
// them idempotent here.
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

namespace sco::net {

inline constexpr uint32_t kFragBytes     = 1200;   // data bytes per unit
inline constexpr uint32_t kWindow        = 64;     // units in flight / buffered ahead
inline constexpr uint32_t kMaxQueuedUnits = 1024;  // per stream, ~1.2 MB; then Push refuses

struct OutUnit {
    uint64_t seq = 0;
    uint64_t origin = 0;        // the peer whose message this is (the host relays others')
    uint32_t msgLen = 0;
    uint32_t offset = 0;
    bool     toHost = false;
    std::vector<uint8_t> data;
    bool     acked = false;
    uint32_t sends = 0;
    uint64_t lastSendMs = 0;
    uint32_t rtoMs = 0;
};

class SendStream {
public:
    // Cuts len bytes into units and queues them. false (nothing queued) when len is over
    // kMaxReliable or the queue would pass kMaxQueuedUnits.
    bool Push(uint64_t origin, bool toHost, const uint8_t* data, uint32_t len);
    // Applies an acknowledgement. Units at or past the next unit to assign are ignored, so a
    // bogus ack can't mark unsent data acknowledged.
    void OnAck(uint64_t next, uint64_t mask);
    // Units to (re)send now, oldest first: inside the window and never sent, or unacknowledged for
    // their timeout. Marks them sent at nowMs (first timeout firstRtoMs, then doubling, at most
    // maxRtoMs). The pointers are valid until the next call on this stream.
    std::vector<const OutUnit*> Due(uint64_t nowMs, uint32_t firstRtoMs, uint32_t maxRtoMs);
    size_t Queued() const { return q_.size(); }
    uint64_t NextSeq() const { return next_; }

private:
    std::deque<OutUnit> q_;   // unacknowledged units, consecutive seqs from q_.front().seq
    uint64_t next_ = 0;
};

class RecvStream {
public:
    enum class Verdict : uint8_t {
        Accepted,     // new, buffered (and maybe delivered)
        Duplicate,    // already had it; acknowledge again
        Ahead,        // past the window; dropped, the sender resends
        Malformed,    // fragment fields inconsistent with each other; dropped
    };
    struct Delivery {
        uint64_t origin;
        std::vector<uint8_t> data;
    };
    // Takes one unit. maxLen is the receiver's limit for this channel: a message whose length is
    // over it is consumed and dropped (counted in Refused()) without allocating its size.
    // Complete messages are appended to *out in order.
    Verdict OnUnit(uint64_t seq, uint64_t origin, uint32_t msgLen, uint32_t offset, const uint8_t* data,
                   uint32_t len, uint32_t maxLen, std::vector<Delivery>* out);
    uint64_t Next() const { return next_; }
    // Bit i: unit Next() + 1 + i is buffered.
    uint64_t Mask() const;
    bool ackDue = false;
    uint64_t Refused() const { return refused_; }
    uint64_t Broken() const { return broken_; }

private:
    struct Unit {
        uint64_t origin;
        uint32_t msgLen, offset;
        std::vector<uint8_t> data;
    };
    void Consume(const Unit& u, uint32_t maxLen, std::vector<Delivery>* out);

    uint64_t next_ = 0;
    std::map<uint64_t, Unit> ahead_;
    // The message being assembled.
    bool     inMsg_ = false;
    bool     discard_ = false;
    uint64_t origin_ = 0;
    uint32_t msgLen_ = 0;
    uint32_t got_ = 0;
    std::vector<uint8_t> buf_;
    uint64_t refused_ = 0;
    uint64_t broken_ = 0;
};

}  // namespace sco::net
