#pragma once
// Private to src/ipc/: the platform half of sco.ipc. One named mapping the current user alone
// may open: shm_win.cpp (CreateFileMappingW with a DACL for the current user, the real target)
// and shm_posix.cpp (shm_open, mode 0600, for the host tests). Nothing here knows the wire.
#include "sco/runtime.h"
#include <cstdint>
#include <string>

namespace sco::ipc::shm {

struct Mapping {
    void*       base = nullptr;   // the view: page-aligned, at least `bytes`
    uint64_t    bytes = 0;        // what Create was asked for
    uintptr_t   handle = 0;       // the HANDLE (Windows) or fd + 1 (POSIX); 0 when closed
    bool        existed = false;  // the name was already there (the peer kept it open)
    std::string name;
};

// Creates `name` with `bytes` bytes for the current user only, or opens it when it already
// exists, it belongs to the current user and it is at least `bytes` long; maps it read-write.
// Failed (with why) otherwise.
Result Create(const std::string& name, uint64_t bytes, Mapping& out, std::string& why);

// Unmaps and closes (POSIX: also unlinks the name). No-op when closed.
void Close(Mapping& m);

// Milliseconds of the machine-wide monotonic clock both sides of a channel use.
uint64_t NowMs();

uint32_t Pid();

}  // namespace sco::ipc::shm
