// sco.ipc on POSIX (host tests): shm_open with mode 0600. An existing object of the same name is
// used only if the current user owns it and it is large enough. The owner unlinks the name on
// close; a peer keeps its own mapping, where it sees the channel marked closed.
#include "shm.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace sco::ipc::shm {

Result Create(const std::string& name, uint64_t bytes, Mapping& out, std::string& why) {
    out = Mapping{};
    const int fd = shm_open(name.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
        why = std::string("shm_open: ") + std::strerror(errno);
        return Result::Failed;
    }
    struct stat st {};
    if (fstat(fd, &st) != 0) {
        why = std::string("fstat: ") + std::strerror(errno);
        close(fd);
        return Result::Failed;
    }
    if (st.st_uid != geteuid()) {
        close(fd);
        why = "a mapping of that name already exists and belongs to another account";
        return Result::Failed;
    }
    const bool existed = st.st_size != 0;
    if (existed && static_cast<uint64_t>(st.st_size) < bytes) {
        close(fd);
        why = "a mapping of that name already exists and is smaller";
        return Result::Failed;
    }
    if ((existed && fchmod(fd, 0600) != 0) || (!existed && ftruncate(fd, static_cast<off_t>(bytes)) != 0)) {
        why = std::string("sizing: ") + std::strerror(errno);
        close(fd);
        if (!existed) shm_unlink(name.c_str());
        return Result::Failed;
    }
    void* base = mmap(nullptr, static_cast<size_t>(bytes), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        why = std::string("mmap: ") + std::strerror(errno);
        close(fd);
        if (!existed) shm_unlink(name.c_str());
        return Result::Failed;
    }
    out.base = base;
    out.bytes = bytes;
    out.handle = static_cast<uintptr_t>(fd) + 1;
    out.existed = existed;
    out.name = name;
    return Result::Ok;
}

void Close(Mapping& m) {
    if (m.base) munmap(m.base, static_cast<size_t>(m.bytes));
    if (m.handle) {
        close(static_cast<int>(m.handle - 1));
        shm_unlink(m.name.c_str());
    }
    m = Mapping{};
}

uint64_t NowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000u + static_cast<uint64_t>(ts.tv_nsec) / 1000000u;
}

uint32_t Pid() { return static_cast<uint32_t>(getpid()); }

}  // namespace sco::ipc::shm
