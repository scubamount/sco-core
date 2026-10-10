// sco::net UDP over BSD sockets (sco/net/udp.h): Linux and macOS, for the tests.
#include "sco/net/udp.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

namespace sco::net::udp {

namespace {

int Fd(uintptr_t v) { return static_cast<int>(v); }

void ToEndpoint(const sockaddr_in& sa, Endpoint* out) {
    Endpoint e;
    e.family = Endpoint::kIpv4;
    e.port = ntohs(sa.sin_port);
    std::memcpy(e.addr, &sa.sin_addr.s_addr, 4);
    *out = e;
}

}  // namespace

std::unique_ptr<Socket> Socket::Open(uint16_t port, bool loopbackOnly, std::string* why) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        if (why) *why = std::string("socket: ") + std::strerror(errno);
        return nullptr;
    }
    const int flags = ::fcntl(fd, F_GETFL, 0);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
    socklen_t n = sizeof sa;
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        ::bind(fd, reinterpret_cast<const sockaddr*>(&sa), sizeof sa) < 0 ||
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &n) < 0) {
        if (why) *why = std::string("bind: ") + std::strerror(errno);
        ::close(fd);
        return nullptr;
    }
    std::unique_ptr<Socket> s(new Socket());
    s->fd_ = static_cast<uintptr_t>(fd);
    s->port_ = ntohs(sa.sin_port);
    return s;
}

Socket::~Socket() { ::close(Fd(fd_)); }

bool Socket::Send(const Endpoint& to, const uint8_t* data, size_t len) {
    if (to.family != Endpoint::kIpv4) return false;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(to.port);
    std::memcpy(&sa.sin_addr.s_addr, to.addr, 4);
    const ssize_t r = ::sendto(Fd(fd_), data, len, 0, reinterpret_cast<const sockaddr*>(&sa), sizeof sa);
    return r == static_cast<ssize_t>(len);
}

bool Socket::Receive(Endpoint* from, uint8_t* buf, size_t cap, size_t* len) {
    for (;;) {
        sockaddr_in sa{};
        iovec iov{ buf, cap };
        msghdr msg{};
        msg.msg_name = &sa;
        msg.msg_namelen = sizeof sa;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        const ssize_t r = ::recvmsg(Fd(fd_), &msg, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;   // EAGAIN: nothing waiting; anything else is a loss to the session
        }
        if (sa.sin_family != AF_INET) continue;
        ToEndpoint(sa, from);
        *len = (msg.msg_flags & MSG_TRUNC) ? cap + 1 : static_cast<size_t>(r);
        return true;
    }
}

bool Socket::Wait(uint32_t ms) {
    pollfd p{ Fd(fd_), POLLIN, 0 };
    const int timeout = ms > 0x7FFFFFFFu ? 0x7FFFFFFF : static_cast<int>(ms);
    return ::poll(&p, 1, timeout) > 0 && (p.revents & POLLIN);
}

}  // namespace sco::net::udp
