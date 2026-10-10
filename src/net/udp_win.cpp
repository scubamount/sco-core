// sco::net UDP over Winsock (sco/net/udp.h): the real target. One of the files CONTRIBUTING lets
// include Windows headers.
#include "sco/net/udp.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <cstring>

#ifndef SIO_UDP_CONNRESET   // older mingw headers
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace sco::net::udp {

namespace {

SOCKET Fd(uintptr_t v) { return static_cast<SOCKET>(v); }

std::string WsaText(const char* what) { return std::string(what) + ": WSA error " + std::to_string(WSAGetLastError()); }

void ToEndpoint(const sockaddr_in& sa, Endpoint* out) {
    Endpoint e;
    e.family = Endpoint::kIpv4;
    e.port = ntohs(sa.sin_port);
    std::memcpy(e.addr, &sa.sin_addr.s_addr, 4);
    *out = e;
}

}  // namespace

std::unique_ptr<Socket> Socket::Open(uint16_t port, bool loopbackOnly, std::string* why) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        if (why) *why = "WSAStartup failed";
        return nullptr;
    }
    const SOCKET fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == INVALID_SOCKET) {
        if (why) *why = WsaText("socket");
        WSACleanup();
        return nullptr;
    }
    u_long nonBlocking = 1;
    BOOL noReset = FALSE;
    DWORD bytes = 0;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
    int n = static_cast<int>(sizeof sa);
    // SO_EXCLUSIVEADDRUSE: no other socket can bind the same port and read the session's traffic.
    BOOL exclusive = TRUE;
    if (::setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
                     static_cast<int>(sizeof exclusive)) != 0 ||
        ::ioctlsocket(fd, FIONBIO, &nonBlocking) != 0 ||
        ::WSAIoctl(fd, SIO_UDP_CONNRESET, &noReset, sizeof noReset, nullptr, 0, &bytes, nullptr, nullptr) != 0 ||
        ::bind(fd, reinterpret_cast<const sockaddr*>(&sa), static_cast<int>(sizeof sa)) != 0 ||
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &n) != 0) {
        if (why) *why = WsaText("bind");
        ::closesocket(fd);
        WSACleanup();
        return nullptr;
    }
    std::unique_ptr<Socket> s(new Socket());
    s->fd_ = static_cast<uintptr_t>(fd);
    s->port_ = ntohs(sa.sin_port);
    return s;
}

Socket::~Socket() {
    ::closesocket(Fd(fd_));
    WSACleanup();
}

bool Socket::Send(const Endpoint& to, const uint8_t* data, size_t len) {
    if (to.family != Endpoint::kIpv4 || len > 0x7FFFFFFF) return false;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(to.port);
    std::memcpy(&sa.sin_addr.s_addr, to.addr, 4);
    const int r = ::sendto(Fd(fd_), reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
                           reinterpret_cast<const sockaddr*>(&sa), static_cast<int>(sizeof sa));
    return r == static_cast<int>(len);
}

bool Socket::Receive(Endpoint* from, uint8_t* buf, size_t cap, size_t* len) {
    const int c = cap > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<int>(cap);
    for (;;) {
        sockaddr_in sa{};
        int n = static_cast<int>(sizeof sa);
        const int r = ::recvfrom(Fd(fd_), reinterpret_cast<char*>(buf), c, 0, reinterpret_cast<sockaddr*>(&sa), &n);
        if (r == SOCKET_ERROR) {
            const int e = WSAGetLastError();
            if (e == WSAEMSGSIZE) {   // cut to cap: the session drops it
                if (sa.sin_family != AF_INET) continue;
                ToEndpoint(sa, from);
                *len = cap + 1;
                return true;
            }
            if (e == WSAECONNRESET || e == WSAEINTR) continue;
            return false;   // WSAEWOULDBLOCK: nothing waiting
        }
        if (sa.sin_family != AF_INET) continue;
        ToEndpoint(sa, from);
        *len = static_cast<size_t>(r);
        return true;
    }
}

bool Socket::Wait(uint32_t ms) {
    WSAPOLLFD p{};
    p.fd = Fd(fd_);
    p.events = POLLRDNORM;
    const INT timeout = ms > 0x7FFFFFFFu ? 0x7FFFFFFF : static_cast<INT>(ms);
    return ::WSAPoll(&p, 1, timeout) > 0 && (p.revents & POLLRDNORM);
}

}  // namespace sco::net::udp
