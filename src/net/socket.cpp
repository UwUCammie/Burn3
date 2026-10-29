#include "takedown/net/socket.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace takedown::net {

namespace {

using SocketLength = int;

// Winsock must be started once per live socket; the wrapper pairs it with cleanup.
struct SocketRuntime {
    SocketRuntime() {
        WSADATA data{};
        const int status = WSAStartup(MAKEWORD(2, 2), &data);
        if (status != 0) {
            throw std::system_error(status, std::system_category(), "Winsock startup");
        }
    }
    ~SocketRuntime() { WSACleanup(); }
    SocketRuntime(const SocketRuntime&) = delete;
    SocketRuntime& operator=(const SocketRuntime&) = delete;
};

[[noreturn]] void fail(const char* operation, int error) {
    throw std::system_error(error, std::system_category(), operation);
}

// Nonblocking sockets report "not ready yet" through these codes.
bool would_block(const int error) {
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEALREADY;
}

int checked_size(const std::size_t size) {
    if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("socket buffer too large");
    }
    return static_cast<int>(size);
}

sockaddr_in to_address(const Endpoint& endpoint) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(endpoint.port);
    if (inet_pton(AF_INET, endpoint.address.c_str(), &address.sin_addr) != 1) {
        throw std::invalid_argument("invalid IPv4 endpoint");
    }
    return address;
}

Endpoint to_endpoint(const sockaddr_in& address) {
    std::array<char, 22> text{};
    if (inet_ntop(AF_INET, &address.sin_addr, text.data(), text.size()) == nullptr) {
        fail("format socket address", WSAGetLastError());
    }
    return Endpoint{text.data(), ntohs(address.sin_port)};
}

struct AddressList {
    addrinfo* head;
    ~AddressList() { freeaddrinfo(head); }
};

} // namespace

struct Socket::Impl {
    SocketRuntime runtime;
    SOCKET handle = INVALID_SOCKET;
    Kind kind;
    std::vector<std::uint8_t> datagram_scratch;

    explicit Impl(Kind socket_kind) : kind(socket_kind) {
        handle = socket(AF_INET, kind == Kind::Stream ? SOCK_STREAM : SOCK_DGRAM, 0);
        if (handle == INVALID_SOCKET) {
            fail("create native socket", WSAGetLastError());
        }
        u_long nonblocking = 1;
        if (ioctlsocket(handle, FIONBIO, &nonblocking) != 0) {
            const int error = WSAGetLastError();
            closesocket(handle);
            handle = INVALID_SOCKET;
            fail("set socket nonblocking", error);
        }
    }

    ~Impl() {
        if (handle != INVALID_SOCKET) {
            closesocket(handle);
        }
    }

    bool wait(bool read, std::chrono::milliseconds timeout) const {
        if (timeout.count() < 0) {
            throw std::invalid_argument("negative socket wait");
        }
        fd_set set;
        FD_ZERO(&set);
        FD_SET(handle, &set);
        timeval interval{static_cast<long>(timeout.count() / 1000), static_cast<long>((timeout.count() % 1000) * 1000)};
        const int ready = select(0, read ? &set : nullptr, read ? nullptr : &set, nullptr, &interval);
        if (ready < 0) {
            fail("wait native socket", WSAGetLastError());
        }
        return ready != 0;
    }
};

Socket::Socket(Kind kind) : impl_(std::make_unique<Impl>(kind)) {}

Socket::~Socket() = default;

Socket::Socket(Socket&&) noexcept = default;

Socket& Socket::operator=(Socket&&) noexcept = default;

void Socket::bind(std::uint16_t port, std::string_view host) {
    const sockaddr_in target = to_address({std::string(host), port});
    if (::bind(impl_->handle, reinterpret_cast<const sockaddr*>(&target), sizeof(target)) != 0) {
        fail("bind native socket", WSAGetLastError());
    }
}

void Socket::connect(std::string_view host, std::uint16_t port, std::chrono::milliseconds timeout) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const int status = getaddrinfo(std::string(host).c_str(), std::to_string(port).c_str(), &hints, &result);
    if (status != 0 || result == nullptr) {
        throw std::runtime_error("could not resolve matchmaking host");
    }
    const AddressList addresses{result};
    if (::connect(impl_->handle, result->ai_addr, static_cast<int>(result->ai_addrlen)) != 0) {
        const int error = WSAGetLastError();
        if (!would_block(error)) {
            fail("connect native socket", error);
        }
        if (!impl_->wait(false, timeout)) {
            throw std::runtime_error("matchmaking connection timed out");
        }
        int pending = 0;
        SocketLength length = sizeof(pending);
        if (getsockopt(impl_->handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&pending), &length) != 0) {
            fail("get socket status", WSAGetLastError());
        }
        if (pending != 0) {
            fail("connect native socket", pending);
        }
    }
}

Endpoint Socket::local_endpoint() const {
    sockaddr_in value{};
    SocketLength size = sizeof(value);
    if (getsockname(impl_->handle, reinterpret_cast<sockaddr*>(&value), &size) != 0) {
        fail("get local socket address", WSAGetLastError());
    }
    return to_endpoint(value);
}

Endpoint Socket::remote_endpoint() const {
    sockaddr_in value{};
    SocketLength size = sizeof(value);
    if (getpeername(impl_->handle, reinterpret_cast<sockaddr*>(&value), &size) != 0) {
        fail("get remote socket address", WSAGetLastError());
    }
    return to_endpoint(value);
}

bool Socket::readable(std::chrono::milliseconds timeout) const { return impl_->wait(true, timeout); }

bool Socket::writable(std::chrono::milliseconds timeout) const { return impl_->wait(false, timeout); }

std::size_t Socket::receive(std::span<std::uint8_t> bytes) {
    if (bytes.empty()) {
        return 0;
    }
    const int received = recv(impl_->handle, reinterpret_cast<char*>(bytes.data()), checked_size(bytes.size()), 0);
    if (received < 0) {
        const int error = WSAGetLastError();
        if (would_block(error)) {
            return 0;
        }
        fail("receive native socket", error);
    }
    if (received == 0) {
        throw std::runtime_error("matchmaking server disconnected");
    }
    return static_cast<std::size_t>(received);
}

std::size_t Socket::send(std::span<const std::uint8_t> bytes) {
    const int sent = ::send(impl_->handle, reinterpret_cast<const char*>(bytes.data()), checked_size(bytes.size()), 0);
    if (sent < 0) {
        const int error = WSAGetLastError();
        if (would_block(error)) {
            return 0;
        }
        fail("send native socket", error);
    }
    return static_cast<std::size_t>(sent);
}

std::size_t Socket::receive_from(std::span<std::uint8_t> bytes, Endpoint& sender) {
    sockaddr_in source{};
    SocketLength size = sizeof(source);
    if (bytes.size() > 65535U) {
        throw std::length_error("datagram buffer too large");
    }
    // One spare byte lets a datagram larger than the caller's buffer be detected.
    auto& storage = impl_->datagram_scratch;
    storage.resize(bytes.size() + 1);
    const int received = recvfrom(impl_->handle, reinterpret_cast<char*>(storage.data()), checked_size(bytes.size() + 1), 0,
                                  reinterpret_cast<sockaddr*>(&source), &size);
    if (received < 0) {
        const int error = WSAGetLastError();
        if (would_block(error)) {
            return 0;
        }
        // Per-datagram ICMP errors and oversize reports are not socket failures.
        if (error == WSAEMSGSIZE || error == WSAECONNRESET || error == WSAECONNREFUSED) {
            return 0;
        }
        fail("receive native datagram", error);
    }
    if (static_cast<std::size_t>(received) > bytes.size()) {
        return 0;
    }
    sender = to_endpoint(source);
    std::copy_n(storage.begin(), received, bytes.begin());
    return static_cast<std::size_t>(received);
}

std::size_t Socket::send_to(std::span<const std::uint8_t> bytes, const Endpoint& target) {
    const sockaddr_in destination = to_address(target);
    const int sent = sendto(impl_->handle, reinterpret_cast<const char*>(bytes.data()), checked_size(bytes.size()), 0,
                            reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
    if (sent < 0) {
        const int error = WSAGetLastError();
        if (would_block(error)) {
            return 0;
        }
        fail("send native datagram", error);
    }
    return static_cast<std::size_t>(sent);
}

} // namespace takedown::net
