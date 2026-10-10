#pragma once
// sco::net UDP: the datagram Transport over a real socket. Host side and internal: the sco.net
// service opens one when the product calls Host or Join, and the loopback tests use it directly.
// IPv4 only in 1.0. The platform halves are src/net/udp_win.cpp (Winsock, the real target) and
// src/net/udp_posix.cpp (BSD sockets, for the Linux and macOS tests); this header includes no
// platform header.
//
// The socket is non-blocking: Send and Receive never wait. Wait(ms) blocks until a datagram is
// waiting or ms pass, for a caller that has nothing else to do (the loopback tests). A datagram
// longer than Receive's cap is reported with *len = cap + 1 so the session drops it, as the
// Transport contract asks. Windows' "port unreachable" resets (WSAECONNRESET on a UDP socket)
// are switched off, so one peer going away never stops the socket. No address filtering here:
// the service wraps the socket in a ScopedTransport (sco/net/scope.h).
#include "sco/net/transport.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace sco::net::udp {

class Socket final : public Transport {
public:
    // Binds UDP on port (0: the OS picks one; Port() tells which), on every IPv4 interface, or
    // on 127.0.0.1 only with loopbackOnly. nullptr with *why set when the OS refuses (port in
    // use, no network stack).
    static std::unique_ptr<Socket> Open(uint16_t port, bool loopbackOnly, std::string* why);
    ~Socket() override;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    bool Send(const Endpoint& to, const uint8_t* data, size_t len) override;
    bool Receive(Endpoint* from, uint8_t* buf, size_t cap, size_t* len) override;

    // true when a datagram is waiting (now or within ms), false on timeout or error.
    bool Wait(uint32_t ms);
    uint16_t Port() const { return port_; }

private:
    Socket() = default;
    uintptr_t fd_ = 0;
    uint16_t  port_ = 0;
};

}  // namespace sco::net::udp
