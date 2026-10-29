#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace takedown::platform {

// PCSX2's PINE server uses this slot to derive its default Unix socket name.
inline constexpr int kPcsx2PineDefaultSlot = 28011;

// A deliberately small client-side limit. PINE itself accepts much larger
// packets, but diagnostic callers should make bounded, auditable snapshots.
inline constexpr std::size_t kPcsx2PineMaximumRead32Batch = 8192U;

enum class Pcsx2PineStatus : std::uint32_t {
    Running = 0U,
    Paused = 1U,
    Shutdown = 2U,
};

class Pcsx2PineClient;

// This move-only capability is created only after PINE reports Paused. It is
// consumed by read32_batch(), making the pause check explicit at each read
// call site. It is not a lock: callers must keep PCSX2 paused themselves.
class Pcsx2PinePausedReadPermit {
  public:
    Pcsx2PinePausedReadPermit(const Pcsx2PinePausedReadPermit&) = delete;
    Pcsx2PinePausedReadPermit& operator=(const Pcsx2PinePausedReadPermit&) = delete;
    Pcsx2PinePausedReadPermit(Pcsx2PinePausedReadPermit&& other) noexcept;
    Pcsx2PinePausedReadPermit& operator=(Pcsx2PinePausedReadPermit&& other) noexcept;
    ~Pcsx2PinePausedReadPermit() = default;

  private:
    friend class Pcsx2PineClient;

    explicit Pcsx2PinePausedReadPermit(Pcsx2PineClient* client) noexcept;

    Pcsx2PineClient* client_{};
    bool valid_{};
};

// A narrow POSIX client for PCSX2's PINE inspection protocol. Its public API
// intentionally contains no write-memory, savestate, or load-state operation.
// PINE relies on a Unix-domain socket, so connecting is unsupported on
// Windows; the constructors there throw and connected() stays false.
class Pcsx2PineClient {
  public:
    // Connects to an already-running PCSX2 PINE Unix-domain server.
    [[nodiscard]] static Pcsx2PineClient connect(const std::filesystem::path& socket_path);

    // Computes PCSX2's Linux default: $XDG_RUNTIME_DIR/pcsx2.sock (or
    // /tmp/pcsx2.sock) with ".<slot>" appended for non-default slots.
    [[nodiscard]] static std::filesystem::path
    default_socket_path(int slot = kPcsx2PineDefaultSlot);

    // Takes ownership of a connected POSIX stream socket. This is useful to
    // integrate with an existing launcher and to exercise the wire protocol
    // with a socketpair; the caller must not close descriptor afterwards.
    // The caller owns any socket-timeout policy for an adopted descriptor.
    [[nodiscard]] static Pcsx2PineClient adopt_connected_socket(int descriptor);

    ~Pcsx2PineClient();

    Pcsx2PineClient(const Pcsx2PineClient&) = delete;
    Pcsx2PineClient& operator=(const Pcsx2PineClient&) = delete;
    Pcsx2PineClient(Pcsx2PineClient&&) = delete;
    Pcsx2PineClient& operator=(Pcsx2PineClient&&) = delete;

    [[nodiscard]] bool connected() const noexcept;

    [[nodiscard]] std::string version();
    [[nodiscard]] Pcsx2PineStatus status();
    [[nodiscard]] std::string title();
    [[nodiscard]] std::string serial();

    // Queries PINE status and creates a capability only when it reports
    // Paused. A null result means that no memory read is authorized.
    [[nodiscard]] std::optional<Pcsx2PinePausedReadPermit> verify_paused();

    // Reads at most kPcsx2PineMaximumRead32Batch EE words. The permit must
    // come from verify_paused() on this client and is consumed. The client
    // checks PINE status again immediately before the read request; PINE has
    // no atomic status-and-read command, so the caller must retain the pause.
    [[nodiscard]] std::vector<std::uint32_t> read32_batch(Pcsx2PinePausedReadPermit&& permit,
                                                          std::span<const std::uint32_t> addresses);

  private:
    explicit Pcsx2PineClient(int descriptor, bool use_msg_nosignal) noexcept;

    [[nodiscard]] std::vector<std::uint8_t> transact(std::span<const std::uint8_t> request_payload);
    [[nodiscard]] std::string query_string(std::uint8_t command);
    void close() noexcept;

    int descriptor_{-1};
    bool use_msg_nosignal_{true};
};

} // namespace takedown::platform
