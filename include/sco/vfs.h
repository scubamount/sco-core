#pragma once
// sco::vfs: game-file overrides, engine-agnostic. A mounted game file is served as a *virtual*
// file: ranges of the real (base) file plus replacement bytes, described by splices in base
// offsets. sco-core owns the arithmetic and the rules here; an engine adapter (sco::game::pak,
// later) only routes the engine's open/read/seek/close on a mounted handle to these calls.
// Bytes, offsets and paths only: no engine, no Windows. Design: docs/design/vfs-datacore.md § 3.
//
//   auto table = sco::vfs::Table::Build(std::move(mounts));    // once, from the content index
//   mountTable.Publish(table);                                  // readers see it atomically
//   ...
//   // the adapter's open hook, on any thread:
//   auto snap = mountTable.Current();
//   if (snap && snap->Mounted(path)) {                          // no allocation for other files
//       if (auto file = snap->Open(path, baseIo, baseSize))     // composed once per path and base
//           handles[h] = sco::vfs::Reader(file);                // read/seek/tell answer from it
//   }
//
// Threads: Compose and the Reader functions are plain functions over the caller's objects; a
// Reader belongs to one handle (the engine serializes calls on one handle) and takes no lock.
// Table and MountTable are safe from any thread. A failure never produces a partial virtual file:
// a mount that can't apply is inert and its file passes through untouched.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace sco::vfs {

using Bytes = std::vector<uint8_t>;

constexpr uint64_t kMaxFileSize = 4ull << 30;   // default virtual file size limit (4 GiB)
constexpr size_t   kMaxPathLength = 1024;       // longer paths are never mounted

struct Limits {
    uint64_t bufferBytes = 64ull << 20;     // in-memory replacement bytes, all mounts of a table
    size_t   splicesPerPath = 65536;        // per mount, and per path after the merge
    uint64_t fileSize = kMaxFileSize;       // virtual file size
};

// Success when error is empty; otherwise error says what failed (and names the first bad splice).
struct Result {
    std::string error;
    bool ok() const { return error.empty(); }
    explicit operator bool() const { return error.empty(); }
};

// One edit in base offsets: `removed` base bytes at `at` are replaced by `bytes`.
struct Splice {
    uint64_t at = 0;
    uint64_t removed = 0;
    std::shared_ptr<const Bytes> bytes;   // inserted at `at`; null or empty: a pure deletion
    std::shared_ptr<const Bytes> old;     // optional: the `removed` base bytes this splice expects
};

// The virtual file as segments that tile [0, size): sorted by start, adjacent, no gaps, len > 0.
struct Segment {
    enum Kind : uint8_t { Base, Buffer };
    uint64_t       start = 0, len = 0;   // virtual offsets
    Kind           kind = Base;
    uint64_t       from = 0;             // Base: offset in the base file. Buffer: offset in src
    const uint8_t* src = nullptr;        // Buffer: the replacement bytes (kept alive by `buffers`)
};

struct Composed {
    uint64_t baseSize = 0, size = 0;
    std::vector<Segment> segments;
    std::vector<std::shared_ptr<const Bytes>> buffers;   // owns what Buffer segments point at
};

// Validates the splices (each removes or adds something, sorted by `at`, no two at the same
// offset, none overlapping, `old` absent or exactly `removed` bytes, at + removed <= baseSize,
// virtual size <= maxSize) and builds the segments. On failure out is untouched and the error
// names the first bad splice ("splice 3 (at 4096): overlaps splice 2 (at 4000, removes 100)").
Result Compose(uint64_t baseSize, std::span<const Splice> splices, Composed& out,
               uint64_t maxSize = kMaxFileSize);

// The real file under a mounted handle: the engine's original read and seek for that handle.
class BaseIo {
public:
    virtual ~BaseIo() = default;
    virtual bool   Seek(uint64_t pos) = 0;               // absolute; false on failure
    virtual size_t Read(void* dst, size_t n) = 0;        // at the current position; short = end or error
};

enum class Whence { Set, Cur, End };

// One open handle of a virtual file. Positions are 64-bit; seeking past the end is allowed and
// reads there return 0 (C fseek semantics). Holds the composition, so a handle keeps reading the
// file it opened whatever the mount table does after.
class Reader {
public:
    Reader() = default;
    explicit Reader(std::shared_ptr<const Composed> file) : file_(std::move(file)) {}

    // SET from 0, CUR from Tell(), END from Size(). False (position unchanged) for a negative or
    // overflowing result.
    bool Seek(int64_t off, Whence whence);
    // Copies up to n bytes from Tell() and advances by what it copied. Base segments read through
    // `base`, seeking it only when its position isn't already the one needed. A failed seek or a
    // short base read ends the call with the bytes read so far, never with garbage.
    size_t Read(void* dst, size_t n, BaseIo& base);

    uint64_t Tell() const { return pos_; }
    uint64_t Size() const { return file_ ? file_->size : 0; }
    bool     Eof() const { return pos_ >= Size(); }   // at or past the end (not C's sticky flag)
    const std::shared_ptr<const Composed>& File() const { return file_; }

private:
    static constexpr uint64_t kUnknown = ~0ull;
    std::shared_ptr<const Composed> file_;
    uint64_t pos_ = 0;
    uint64_t basePos_ = kUnknown;   // where `base` is, as far as this reader knows
};

// "\Data\Game2.DCB" -> "data/game2.dcb": ASCII lowercase, '/' separators, no leading or repeated
// '/'. Empty when longer than kMaxPathLength.
std::string NormalizePath(std::string_view path);

// ---- mount table --------------------------------------------------------------------------

// Fixed splices, optionally gated on the base's first bytes and on each splice's `old` bytes.
struct SpliceList {
    std::vector<Splice> splices;
    std::shared_ptr<const Bytes> header;   // optional: the base must start with these bytes
};
// Computes splices from the base at composition time (sco::datacore is one). Splices it returns
// are checked like a SpliceList's, `old` bytes included; an error or an exception makes it inert.
using Transform = std::function<Result(BaseIo& base, uint64_t baseSize, std::vector<Splice>& out)>;

struct Mount {
    std::string path;       // any form; Build normalizes it
    int         priority = 0;   // higher wins; equal priority: later in the Build list wins
    std::string source;     // pack id, for the report
    std::variant<SpliceList, Transform> producer;
};

enum class MountState {
    Pending,   // not composed yet (no Open of its path)
    Applied,   // in the virtual file (reason lists splices dropped to a higher-priority overlap)
    Inert,     // composed but not applied this run: expected bytes differ, transform failed, limits
    Refused,   // refused by Build: bad path or splices, or over a limit
};

struct MountInfo {
    std::string path, source;
    int         priority = 0;
    MountState  state = MountState::Pending;
    std::string reason;
};

// An immutable set of mounts. Composition happens once per path and base identity (base size
// plus a hash of its first 4 KiB) under a per-path guard: concurrent opens of one path compute it
// once and the others wait. The result is cached for the table's lifetime.
//
// Merge rules per path: every mount contributes splices against the same base, highest priority
// first. A splice overlapping (or at the same offset as) one already taken from a higher-priority
// mount is dropped, and the loser's reason names the winner. A mount whose expected bytes don't
// match, or whose transform fails, contributes nothing (inert). If the merged result breaks a
// limit, every mount of the path is inert and the file passes through.
class Table {
public:
    static std::shared_ptr<const Table> Build(std::vector<Mount> mounts, const Limits& limits = {});
    ~Table();
    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;

    // True when path (any form) has a mount Build didn't refuse. No allocation.
    bool Mounted(std::string_view path) const noexcept;
    // The virtual file for path over this base, or null when the file should pass through: path
    // not mounted, or every mount of it inert. Reads the base (its first 4 KiB, expected bytes,
    // transforms) only on the first open per base identity; leaves the base position unspecified
    // (a Reader seeks before its first base read). Never throws.
    std::shared_ptr<const Composed> Open(std::string_view path, BaseIo& base, uint64_t baseSize) const noexcept;

    std::vector<MountInfo> Mounts() const;   // in Build order
    uint64_t BufferBytes() const;            // replacement bytes counted against Limits::bufferBytes

private:
    Table();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The published snapshot. Readers take the current table without a lock; Publish swaps it
// atomically, and handles keep the table (and file) they opened with.
class MountTable {
public:
    std::shared_ptr<const Table> Current() const noexcept { return cur_.load(std::memory_order_acquire); }
    void Publish(std::shared_ptr<const Table> table) noexcept { cur_.store(std::move(table), std::memory_order_release); }

private:
    std::atomic<std::shared_ptr<const Table>> cur_;
};

}  // namespace sco::vfs
