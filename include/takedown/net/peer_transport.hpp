#pragma once
#include "takedown/content/json.hpp"
#include "takedown/net/socket.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>
namespace takedown::net {
using SessionKey = std::array<std::uint8_t, 16>;
struct PeerAdmission {
    std::uint32_t player{};
    SessionKey token{};
    Endpoint rendezvous;
};
struct PeerMember {
    std::uint32_t player{};
    Endpoint endpoint;
};
struct PeerRoom {
    std::uint32_t id{};
    SessionKey key{};
    std::vector<PeerMember> players;
};
[[nodiscard]] PeerAdmission decode_peer_admission(const content::JsonValue& welcome);
[[nodiscard]] PeerRoom decode_peer_room(const content::JsonValue& event);
enum class PeerChannel : std::uint8_t { Reliable, Snapshot, Datagram };
struct PeerMessage {
    std::uint32_t sender{};
    PeerChannel channel{};
    std::vector<std::uint8_t> bytes;
};
struct PeerStatistics {
    std::uint64_t sent{}, received{}, retransmitted{}, rejected{}, relayed{};
};
// The local simulation never waits for another player's input. Small reliable
// events and latest-state snapshots use independent sequence spaces. Lobby
// registration discovers the observed UDP endpoint; direct hole punching is
// preferred, with an opaque relay fallback when a NAT blocks the direct path.
class PeerTransport {
  public:
    using Time = std::chrono::steady_clock::time_point;
    PeerTransport();
    ~PeerTransport();
    PeerTransport(const PeerTransport&) = delete;
    PeerTransport& operator=(const PeerTransport&) = delete;
    [[nodiscard]] Endpoint local_endpoint() const;
    [[nodiscard]] bool rendezvous_registered() const;
    void admit(PeerAdmission admission);
    void set_room(PeerRoom room, Time now = std::chrono::steady_clock::now());
    void leave();
    void disconnect();
    void update(Time now = std::chrono::steady_clock::now());
    [[nodiscard]] bool send(std::uint32_t player, std::span<const std::uint8_t> bytes,
                            PeerChannel channel, Time now = std::chrono::steady_clock::now());
    [[nodiscard]] bool peer_connected(std::uint32_t player,
                                      Time now = std::chrono::steady_clock::now()) const;
    [[nodiscard]] std::vector<PeerMessage> poll();
    [[nodiscard]] const PeerStatistics& statistics() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace takedown::net
