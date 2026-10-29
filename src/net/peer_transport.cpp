#include "takedown/net/peer_transport.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

namespace takedown::net {
namespace {

using namespace std::chrono_literals;

constexpr std::uint32_t kWire = 1U;
constexpr std::size_t kHeader = 48U;
constexpr std::size_t kRelayHeader = 28U;
constexpr std::size_t kMaxDatagram = 1200U;
constexpr std::size_t kMaxPayload = 1152U;
constexpr std::string_view kBuild = "v0.0.24";

// Packet kinds. Hello/Ping are answered with the matching acknowledgement.
enum class Kind : std::uint8_t { Hello = 1, HelloAck, Data, Ack, Ping, Pong };

std::uint32_t number(const content::JsonValue& value, const char* name) {
    const auto* field = value.find(name);
    if (field == nullptr || !field->is_number() || !std::isfinite(field->as_number()) ||
        field->as_number() < 1.0 || field->as_number() > 4294967295.0 ||
        std::floor(field->as_number()) != field->as_number()) {
        throw std::runtime_error("invalid native peer number");
    }
    return static_cast<std::uint32_t>(field->as_number());
}

std::string string(const content::JsonValue& value, const char* name) {
    const auto* field = value.find(name);
    if (field == nullptr || !field->is_string()) {
        throw std::runtime_error("invalid native peer text");
    }
    return field->as_string();
}

unsigned nibble(char c) {
    if (c >= '0' && c <= '9') {
        return static_cast<unsigned>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<unsigned>(c - 'a' + 10);
    }
    throw std::runtime_error("invalid native peer key");
}

SessionKey key(std::string_view text) {
    if (text.size() != 32U) {
        throw std::runtime_error("invalid native peer key");
    }
    SessionKey result{};
    for (unsigned i = 0U; i < 16U; ++i) {
        result[i] = static_cast<std::uint8_t>(nibble(text[i * 2U]) * 16U + nibble(text[i * 2U + 1U]));
    }
    return result;
}

void put16(std::uint8_t* out, unsigned at, std::uint16_t value) {
    out[at] = static_cast<std::uint8_t>(value >> 8U);
    out[at + 1U] = static_cast<std::uint8_t>(value);
}

void store_be32(std::uint8_t* out, std::uint32_t value) {
    for (unsigned i = 0U; i < 4U; ++i) {
        out[i] = static_cast<std::uint8_t>(value >> ((3U - i) * 8U));
    }
}

void put32(std::uint8_t* out, unsigned at, std::uint32_t value) {
    for (unsigned i = 0U; i < 4U; ++i) {
        out[at + i] = static_cast<std::uint8_t>(value >> ((3U - i) * 8U));
    }
}

std::uint16_t read16(const std::uint8_t* in) {
    return static_cast<std::uint16_t>((in[0] << 8U) | in[1]);
}

std::uint32_t read32(const std::uint8_t* in) {
    return (static_cast<std::uint32_t>(in[0]) << 24U) | (static_cast<std::uint32_t>(in[1]) << 16U) |
           (static_cast<std::uint32_t>(in[2]) << 8U) | in[3];
}

} // namespace

PeerAdmission decode_peer_admission(const content::JsonValue& welcome) {
    if (kBuild.substr(1) != string(welcome, "version") || number(welcome, "wire") != kWire) {
        throw std::runtime_error("native peer version mismatch");
    }
    const std::uint32_t port = number(welcome, "udp_port");
    if (port > 65535U) {
        throw std::runtime_error("invalid rendezvous port");
    }
    return PeerAdmission{number(welcome, "id"), key(string(welcome, "token")),
                         Endpoint{string(welcome, "server_address"),
                                  static_cast<std::uint16_t>(port)}};
}

// RECOVERY: behaviour and data match the original. The compiled code differs in how
// the roster item is zero-initialised (the original clears the endpoint port
// explicitly) and in where the variant accessors are re-read for the endpoint
// port, so the GIMPLE does not match function-for-function.
PeerRoom decode_peer_room(const content::JsonValue& event) {
    const auto* room = event.find("room");
    if (room == nullptr || !room->is_object()) {
        throw std::runtime_error("invalid native peer room");
    }
    PeerRoom out{};
    out.id = number(*room, "id");
    out.key = key(string(*room, "key"));
    const auto* players = room->find("players");
    if (players == nullptr || !players->is_array() || players->as_array().size() > 8U) {
        throw std::runtime_error("invalid native peer roster");
    }
    std::set<std::uint32_t> ids;
    for (const auto& entry : players->as_array()) {
        PeerMember item{};
        item.player = number(entry, "id");
        if (!ids.insert(item.player).second) {
            throw std::runtime_error("duplicate native peer");
        }
        const auto* endpoint = entry.find("endpoint");
        if (endpoint == nullptr) {
            throw std::runtime_error("missing native peer endpoint");
        }
        if (!endpoint->is_null()) {
            if (!endpoint->is_array() || endpoint->as_array().size() != 2U ||
                !endpoint->as_array()[0].is_string() ||
                endpoint->as_array()[0].as_string().size() > 15U ||
                !endpoint->as_array()[1].is_number()) {
                throw std::runtime_error("invalid native peer endpoint");
            }
            const double port = endpoint->as_array()[1].as_number();
            if (!std::isfinite(port) || port < 1.0 || port > 65535.0 || std::floor(port) != port) {
                throw std::runtime_error("invalid native peer endpoint");
            }
            item.endpoint = Endpoint{endpoint->as_array()[0].as_string(),
                                     static_cast<std::uint16_t>(endpoint->as_array()[1].as_number())};
        }
        out.players.push_back(std::move(item));
    }
    return out;
}

struct PeerTransport::Impl {
    struct Pending {
        std::vector<std::uint8_t> bytes;
        Time last{};
        Time first{};
        unsigned attempts{};
    };
    struct Peer {
        Endpoint endpoint;
        Time began{};
        Time hello_at{};
        Time heard{};
        Time direct_heard{};
        bool heard_any{};
        bool direct{};
        bool snapshot_seen{};
        std::array<std::uint32_t, 3> next{1U, 1U, 1U};
        std::uint32_t reliable_expected{1U};
        std::uint32_t snapshot_last{};
        // RECOVERY: eight bytes sit here in the compiled layout; no function in this
        // file reads or writes them, so their original name and type are unknown.
        std::uint64_t unused{};
        std::map<std::uint32_t, Pending> pending;
        std::map<std::uint32_t, std::vector<std::uint8_t>> reordered;
    };

    Socket socket{Socket::Kind::Datagram};
    std::optional<PeerAdmission> admission;
    PeerRoom room;
    std::map<std::uint32_t, Peer> peers;
    std::vector<PeerMessage> messages;
    Time register_at{};
    bool registered{};
    PeerStatistics stats;

    Impl() { socket.bind(); }

    std::vector<std::uint8_t> packet(std::uint32_t target, Kind kind,
                                                          PeerChannel channel, std::uint32_t seq,
                                                          std::span<const std::uint8_t> bytes) const {
        std::vector<std::uint8_t> out(kHeader + bytes.size());
        out[0] = 'B';
        out[1] = '3';
        out[2] = 'P';
        out[3] = 'N';
        put16(out.data(), 4U, 1U);
        put16(out.data(), 6U, 0U);
        put16(out.data(), 8U, 0U);
        put16(out.data(), 10U, 24U);
        put32(out.data(), 12U, room.id);
        put32(out.data(), 16U, admission->player);
        put32(out.data(), 20U, target);
        put32(out.data(), 24U, seq);
        out[28] = static_cast<std::uint8_t>(channel);
        out[29] = static_cast<std::uint8_t>(kind);
        std::copy(room.key.begin(), room.key.end(), out.begin() + 32);
        std::copy(bytes.begin(), bytes.end(), out.begin() + kHeader);
        return out;
    }

    void transmit(std::uint32_t id, Peer& peer,
                                       std::span<const std::uint8_t> bytes, Time now, bool handshake) {
        if (peer.endpoint.port != 0U &&
            (!peer.direct || now - peer.direct_heard < 1500ms || handshake)) {
            if (socket.send_to(bytes, peer.endpoint) != 0U) {
                ++stats.sent;
            }
        }
        if (now - peer.began >= 1s && (!peer.direct || now - peer.direct_heard >= 1500ms)) {
            std::vector<std::uint8_t> relay(kRelayHeader + bytes.size());
            relay[0] = 'B';
            relay[1] = '3';
            relay[2] = 'R';
            relay[3] = 'L';
            put32(relay.data(), 4U, admission->player);
            std::copy(admission->token.begin(), admission->token.end(), relay.begin() + 8);
            put32(relay.data(), 24U, id);
            std::copy(bytes.begin(), bytes.end(), relay.begin() + kRelayHeader);
            if (socket.send_to(relay, admission->rendezvous) != 0U) {
                ++stats.sent;
                ++stats.relayed;
            }
        }
    }

    void deliver(std::uint32_t sender, PeerChannel channel,
                                      std::span<const std::uint8_t> bytes) {
        if (channel == PeerChannel::Snapshot) {
            const auto existing = std::find_if(messages.begin(), messages.end(), [&](const auto& message) {
                return message.sender == sender && message.channel == PeerChannel::Snapshot;
            });
            if (existing != messages.end()) {
                existing->bytes.assign(bytes.begin(), bytes.end());
                return;
            }
        }
        if (messages.size() >= 512U) {
            throw std::runtime_error("native peer receive queue overflow");
        }
        messages.push_back({sender, channel, {bytes.begin(), bytes.end()}});
    }

    void receive(const Endpoint& source, std::span<const std::uint8_t> bytes,
                                      Time now) {
        if (bytes.size() == 24U && std::equal(bytes.begin(), bytes.begin() + 4, "B3NA")) {
            if (source == admission->rendezvous && read32(bytes.data() + 4) == admission->player &&
                std::equal(admission->token.begin(), admission->token.end(), bytes.begin() + 8)) {
                registered = true;
            } else {
                ++stats.rejected;
            }
            return;
        }
        if (bytes.size() < kHeader || bytes.size() > kMaxDatagram ||
            !std::equal(bytes.begin(), bytes.begin() + 4, "B3PN") ||
            read16(bytes.data() + 4) != 1U || read16(bytes.data() + 6) != 0U ||
            read16(bytes.data() + 8) != 0U || read16(bytes.data() + 10) != 24U ||
            read32(bytes.data() + 12) != room.id || read32(bytes.data() + 20) != admission->player ||
            bytes[28] > 2U || bytes[29] == 0U || bytes[29] > 6U || bytes[30] != 0U ||
            bytes[31] != 0U || !std::equal(room.key.begin(), room.key.end(), bytes.begin() + 32)) {
            ++stats.rejected;
            return;
        }
        const std::uint32_t sender = read32(bytes.data() + 16);
        const auto found = peers.find(sender);
        if (found == peers.end()) {
            ++stats.rejected;
            return;
        }
        Peer& peer = found->second;
        if (source != peer.endpoint && source != admission->rendezvous) {
            ++stats.rejected;
            return;
        }
        const auto channel = static_cast<PeerChannel>(bytes[28]);
        const auto kind = static_cast<Kind>(bytes[29]);
        const unsigned int seq = read32(bytes.data() + 24);
        if (kind != Kind::Data && bytes.size() != kHeader) {
            ++stats.rejected;
            return;
        }
        peer.heard = now;
        peer.heard_any = true;
        if (source == peer.endpoint) {
            peer.direct_heard = now;
            peer.direct = true;
        }
        ++stats.received;
        if (kind == Kind::Hello || kind == Kind::Ping) {
            const auto reply =
                packet(sender, kind == Kind::Hello ? Kind::HelloAck : Kind::Pong, channel, seq, {});
            transmit(sender, peer, reply, now, true);
            return;
        }
        if (kind == Kind::Ack) {
            const auto pending = peer.pending.find(seq);
            if (pending != peer.pending.end()) {
                peer.pending.erase(pending);
            }
            return;
        }
        if (kind != Kind::Data) {
            return;
        }
        const auto payload = bytes.subspan(kHeader);
        if (channel == PeerChannel::Datagram) {
            deliver(sender, PeerChannel::Datagram, payload);
            return;
        }
        if (channel == PeerChannel::Snapshot) {
            if (peer.snapshot_seen) {
                const unsigned int distance = seq - peer.snapshot_last;
                if (static_cast<signed int>(distance) <= 0) {
                    return;
                }
            }
            peer.snapshot_seen = true;
            peer.snapshot_last = seq;
            deliver(sender, PeerChannel::Snapshot, payload);
            return;
        }
        const unsigned int distance = seq - peer.reliable_expected;
        if (distance > 64U && static_cast<signed int>(distance) > 0) {
            ++stats.rejected;
            return;
        }
        const auto ack = packet(sender, Kind::Ack, PeerChannel::Reliable, seq, {});
        transmit(sender, peer, ack, now, false);
        if (peer.reliable_expected == seq) {
            deliver(sender, PeerChannel::Reliable, payload);
            ++peer.reliable_expected;
            for (;;) {
                const auto next = peer.reordered.find(peer.reliable_expected);
                if (next == peer.reordered.end()) {
                    break;
                }
                deliver(sender, PeerChannel::Reliable, next->second);
                peer.reordered.erase(next);
                ++peer.reliable_expected;
            }
        } else {
            const unsigned int distance = seq - peer.reliable_expected;
            if (static_cast<signed int>(distance) > 0 && peer.reordered.size() < 64U) {
                peer.reordered.try_emplace(seq, payload.begin(), payload.end());
            }
        }
    }
};

PeerTransport::PeerTransport() : impl_(std::make_unique<Impl>()) {}

PeerTransport::~PeerTransport() = default;

Endpoint PeerTransport::local_endpoint() const { return impl_->socket.local_endpoint(); }

bool PeerTransport::rendezvous_registered() const { return impl_->registered; }

void PeerTransport::admit(PeerAdmission admission) {
    if (admission.player == 0U || admission.rendezvous.port == 0U) {
        throw std::invalid_argument("invalid native peer admission");
    }
    leave();
    impl_->admission = std::move(admission);
    impl_->register_at = {};
    impl_->registered = false;
}

void PeerTransport::set_room(PeerRoom room, Time now) {
    if (!impl_->admission || room.id == 0U || room.players.size() > 8U) {
        throw std::invalid_argument("invalid native peer room");
    }
    std::set<std::uint32_t> ids;
    bool self = false;
    for (const auto& member : room.players) {
        if (member.player == 0U || !ids.insert(member.player).second) {
            throw std::invalid_argument("invalid native peer roster");
        }
        if (member.player == impl_->admission->player) {
            self = true;
        }
    }
    if (!self) {
        throw std::invalid_argument("native peer roster omits local player");
    }
    if (room.id != impl_->room.id || room.key != impl_->room.key) {
        impl_->peers.clear();
        impl_->messages.clear();
    }
    for (const auto& member : room.players) {
        if (member.player == impl_->admission->player) {
            continue;
        }
        const bool fresh = !impl_->peers.contains(member.player);
        auto& peer = impl_->peers[member.player];
        if (fresh) {
            peer.began = now;
            peer.hello_at = now;
        }
        if (peer.endpoint != member.endpoint) {
            peer.direct = false;
            peer.endpoint = member.endpoint;
            peer.hello_at = now;
        }
    }
    std::erase_if(impl_->peers, [&ids](const auto& entry) { return !ids.contains(entry.first); });
    impl_->room = std::move(room);
}

void PeerTransport::leave() {
    impl_->room = {};
    impl_->peers.clear();
    impl_->messages.clear();
}

void PeerTransport::disconnect() {
    leave();
    impl_->admission.reset();
    impl_->registered = false;
}

// RECOVERY: the probe, datagram validation, peer handling, hello and resend logic
// match the original's behaviour. The compiled block layout differs: the original
// declares its receive buffer and span in a different arrangement, so the GIMPLE
// does not match function-for-function.
void PeerTransport::update(Time now) {
    if (!impl_->admission) {
        return;
    }
    if (impl_->register_at <= now) {
        std::array<std::uint8_t, 24> probe{'B', '3', 'N', 'R'};
        store_be32(probe.data() + 4, impl_->admission->player);
        std::copy(impl_->admission->token.begin(), impl_->admission->token.end(),
                  probe.begin() + 8);
        impl_->socket.send_to(probe, impl_->admission->rendezvous);
        impl_->register_at = now + 2s;
    }
    std::span<const std::uint8_t> bytes{};
    for (unsigned i = 0U; i < 256U; ++i) {
        Endpoint source;
        std::array<std::uint8_t, kMaxDatagram> buffer;
        const std::size_t received = impl_->socket.receive_from(buffer, source);
        if (received == 0U) {
            break;
        }
        bytes = std::span<const std::uint8_t>(buffer.data(), received);
        impl_->receive(source, bytes, now);
    }
    for (auto& [id, peer] : impl_->peers) {
        if (now >= peer.hello_at) {
            const auto hello = impl_->packet(id, Kind::Hello, PeerChannel::Reliable, 0U, {});
            impl_->transmit(id, peer, hello, now, true);
            peer.hello_at = now + std::chrono::milliseconds(peer.direct ? 500 : 100);
        }
        for (auto& [seq, pending] : peer.pending) {
            const auto interval =
                std::chrono::milliseconds(std::min(100U + pending.attempts * 50U, 500U));
            if (now - pending.last >= interval) {
                impl_->transmit(id, peer, pending.bytes, now, false);
                pending.last = now;
                ++pending.attempts;
                ++impl_->stats.retransmitted;
            }
        }
    }
}

bool PeerTransport::send(std::uint32_t player, std::span<const std::uint8_t> bytes,
                         PeerChannel channel, Time now) {
    if (bytes.size() > kMaxPayload || static_cast<unsigned>(channel) > 2U) {
        return false;
    }
    const auto found = impl_->peers.find(player);
    if (found == impl_->peers.end()) {
        return false;
    }
    auto& peer = found->second;
    if (channel == PeerChannel::Reliable && peer.pending.size() >= 64U) {
        return false;
    }
    const auto seq = peer.next[static_cast<std::size_t>(channel)]++;
    auto packet = impl_->packet(player, Kind::Data, channel, seq, bytes);
    impl_->transmit(player, peer, packet, now, false);
    if (channel == PeerChannel::Reliable) {
        peer.pending.emplace(seq, Impl::Pending{std::move(packet), now, now});
    }
    return true;
}

bool PeerTransport::peer_connected(std::uint32_t player, Time now) const {
    const auto found = impl_->peers.find(player);
    return found != impl_->peers.end() && found->second.heard_any &&
           now - found->second.heard < 5s;
}

std::vector<PeerMessage> PeerTransport::poll() { return std::exchange(impl_->messages, {}); }

const PeerStatistics& PeerTransport::statistics() const { return impl_->stats; }

} // namespace takedown::net
