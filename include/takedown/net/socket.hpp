#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace takedown::net {
struct Endpoint {
    std::string address;
    std::uint16_t port{};
    bool operator==(const Endpoint&) const = default;
};
// Host-native sockets. The game-facing revision adapter deals in these
// endpoints and byte buffers; no console network devices or RPC are emulated.
class Socket {
  public:
    enum class Kind { Stream, Datagram };
    explicit Socket(Kind kind);
    ~Socket();
    Socket(Socket&&) noexcept;
    Socket& operator=(Socket&&) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    void bind(std::uint16_t port = 0, std::string_view address = "0.0.0.0");
    void connect(std::string_view host, std::uint16_t port, std::chrono::milliseconds timeout);
    [[nodiscard]] Endpoint local_endpoint() const;
    [[nodiscard]] Endpoint remote_endpoint() const;
    [[nodiscard]] bool readable(std::chrono::milliseconds timeout) const;
    [[nodiscard]] bool writable(std::chrono::milliseconds timeout) const;
    // Nonblocking I/O: zero means would-block. A stream EOF throws; a
    // truncated datagram is rejected rather than passed to a protocol decoder.
    std::size_t receive(std::span<std::uint8_t> bytes);
    std::size_t send(std::span<const std::uint8_t> bytes);
    std::size_t receive_from(std::span<std::uint8_t> bytes, Endpoint& sender);
    std::size_t send_to(std::span<const std::uint8_t> bytes, const Endpoint& target);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace takedown::net
