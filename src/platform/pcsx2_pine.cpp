#include "takedown/platform/pcsx2_pine.hpp"

#include <array>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

#if !defined(_WIN32)
#include <cerrno>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace takedown::platform {

namespace {

// PINE command numbers used by this client.
inline constexpr std::uint8_t kCommandRead32 = 0U;
inline constexpr std::uint8_t kCommandVersion = 8U;
inline constexpr std::uint8_t kCommandTitle = 11U;
inline constexpr std::uint8_t kCommandSerial = 12U;
inline constexpr std::uint8_t kCommandStatus = 15U;

// Largest reply this client is willing to buffer: a full read32 batch plus
// the size and result bytes.
inline constexpr std::size_t kMaximumReplyBytes = (kPcsx2PineMaximumRead32Batch * 4U) + 64U * 1024U;

#if defined(_WIN32)

void send_all(const int, std::span<const std::uint8_t>, const bool) {
    throw std::runtime_error("PCSX2 PINE requires a POSIX Unix-domain socket");
}

void receive_exact(const int, std::span<std::uint8_t>) {
    throw std::runtime_error("PCSX2 PINE requires a POSIX Unix-domain socket");
}

#else

void send_all(const int descriptor, std::span<const std::uint8_t> bytes, const bool use_msg_nosignal) {
    while (!bytes.empty()) {
        const int flags = use_msg_nosignal ? MSG_NOSIGNAL : 0;
        const ssize_t sent = ::send(descriptor, bytes.data(), bytes.size(), flags);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("PCSX2 PINE send failed");
        }
        bytes = bytes.subspan(static_cast<std::size_t>(sent));
    }
}

void receive_exact(const int descriptor, std::span<std::uint8_t> out) {
    while (!out.empty()) {
        const ssize_t received = ::recv(descriptor, out.data(), out.size(), 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("PCSX2 PINE receive failed");
        }
        if (received == 0) {
            throw std::runtime_error("PCSX2 PINE server closed the connection");
        }
        out = out.subspan(static_cast<std::size_t>(received));
    }
}

#endif

void append_u32le(std::vector<std::uint8_t>& out, const std::uint32_t value) {
    for (unsigned byte = 0U; byte < 4U; ++byte) {
        out.push_back(static_cast<std::uint8_t>(value >> (byte * 8U)));
    }
}

std::uint32_t read_u32le(const std::span<const std::uint8_t> bytes) {
    std::uint32_t value = 0U;
    for (std::size_t byte = 0U; byte < 4U; ++byte) {
        value |= static_cast<std::uint32_t>(bytes[byte]) << (byte * 8U);
    }
    return value;
}

// Reads one reply packet and returns its payload without the size and result byte.
std::vector<std::uint8_t> receive_reply(const int descriptor) {
    std::array<std::uint8_t, 4> size_bytes{};
    receive_exact(descriptor, size_bytes);
    const std::uint32_t reply_size = read_u32le(size_bytes);
    if (reply_size < 5U || reply_size > kMaximumReplyBytes) {
        throw std::runtime_error("PCSX2 PINE reply has an invalid length");
    }

    std::vector<std::uint8_t> reply(reply_size - 4U);
    receive_exact(descriptor, reply);
    if (reply.front() != 0U) {
        throw std::runtime_error("PCSX2 PINE reported a failed command");
    }
    reply.erase(reply.begin());
    return reply;
}

} // namespace

Pcsx2PinePausedReadPermit::Pcsx2PinePausedReadPermit(Pcsx2PineClient* const client) noexcept
    : client_(client), valid_(true) {}

Pcsx2PinePausedReadPermit::Pcsx2PinePausedReadPermit(Pcsx2PinePausedReadPermit&& other) noexcept
    : client_(std::exchange(other.client_, nullptr)), valid_(std::exchange(other.valid_, false)) {}

Pcsx2PinePausedReadPermit& Pcsx2PinePausedReadPermit::operator=(Pcsx2PinePausedReadPermit&& other) noexcept {
    if (this != &other) {
        client_ = std::exchange(other.client_, nullptr);
        valid_ = std::exchange(other.valid_, false);
    }
    return *this;
}

Pcsx2PineClient::Pcsx2PineClient(const int descriptor, const bool use_msg_nosignal) noexcept
    : descriptor_(descriptor), use_msg_nosignal_(use_msg_nosignal) {}

Pcsx2PineClient::~Pcsx2PineClient() {
    close();
}

Pcsx2PineClient Pcsx2PineClient::connect(const std::filesystem::path& socket_path) {
#if defined(_WIN32)
    static_cast<void>(socket_path);
    throw std::runtime_error("PCSX2 PINE requires a POSIX Unix-domain socket; not available on Windows");
#else
    const std::string native = socket_path.string();
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (native.empty() || native.size() >= sizeof(address.sun_path)) {
        throw std::invalid_argument("PCSX2 PINE socket path has an invalid length");
    }
    native.copy(address.sun_path, native.size());

    const int descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0) {
        throw std::runtime_error("PCSX2 PINE socket creation failed");
    }
    if (::connect(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(descriptor);
        throw std::runtime_error("PCSX2 PINE connection failed");
    }
    return Pcsx2PineClient(descriptor, true);
#endif
}

std::filesystem::path Pcsx2PineClient::default_socket_path(const int slot) {
    if (slot < 0) {
        throw std::invalid_argument("PCSX2 PINE slot must not be negative");
    }

#if defined(_WIN32)
    const char* const runtime_directory = std::getenv("TEMP");
#else
    const char* const runtime_directory = std::getenv("XDG_RUNTIME_DIR");
#endif
    const std::filesystem::path base = (runtime_directory != nullptr && *runtime_directory != '\0')
                                           ? std::filesystem::path(runtime_directory)
                                           : std::filesystem::path(".");
    auto filename = std::string("pcsx2.sock");
    if (slot != kPcsx2PineDefaultSlot) {
        filename += "." + std::to_string(slot);
    }
    return base / filename;
}

Pcsx2PineClient Pcsx2PineClient::adopt_connected_socket(int descriptor) {
#if defined(_WIN32)
    static_cast<void>(descriptor);
    throw std::runtime_error("PCSX2 PINE requires a POSIX Unix-domain socket; not available on Windows");
#else
    if (descriptor < 0) {
        throw std::invalid_argument("PCSX2 PINE cannot adopt an invalid descriptor");
    }
    return Pcsx2PineClient(descriptor, false);
#endif
}

bool Pcsx2PineClient::connected() const noexcept {
    return descriptor_ >= 0;
}

std::string Pcsx2PineClient::version() {
    return query_string(kCommandVersion);
}

Pcsx2PineStatus Pcsx2PineClient::status() {
    const std::array<std::uint8_t, 1> request{kCommandStatus};
    const std::vector<std::uint8_t> response = transact(request);
    if (response.size() != 4U) {
        throw std::runtime_error("PCSX2 PINE status reply has an unexpected size");
    }
    const std::uint32_t value = read_u32le(response);
    if (value > static_cast<std::uint32_t>(Pcsx2PineStatus::Shutdown)) {
        throw std::runtime_error("PCSX2 PINE reported an unknown status");
    }
    return static_cast<Pcsx2PineStatus>(value);
}

std::string Pcsx2PineClient::title() {
    return query_string(kCommandTitle);
}

std::string Pcsx2PineClient::serial() {
    return query_string(kCommandSerial);
}

std::optional<Pcsx2PinePausedReadPermit> Pcsx2PineClient::verify_paused() {
    if (status() != Pcsx2PineStatus::Paused) {
        return std::nullopt;
    }
    return Pcsx2PinePausedReadPermit(this);
}

std::vector<std::uint32_t> Pcsx2PineClient::read32_batch(Pcsx2PinePausedReadPermit&& permit,
                                                         const std::span<const std::uint32_t> addresses) {
    if (addresses.size() > kPcsx2PineMaximumRead32Batch) {
        throw std::length_error("PCSX2 PINE read32 batch exceeds the client limit");
    }
    if (!permit.valid_ || permit.client_ != this) {
        throw std::logic_error("PCSX2 PINE read32 batch requires this client's paused permit");
    }
    permit.valid_ = false;
    permit.client_ = nullptr;

    // PINE has no atomic status-and-read command; re-check right before the read.
    if (status() != Pcsx2PineStatus::Paused) {
        throw std::runtime_error("PCSX2 PINE is no longer paused");
    }

    std::vector<std::uint8_t> request;
    request.reserve(addresses.size() * 5U);
    for (const std::uint32_t address : addresses) {
        request.push_back(kCommandRead32);
        append_u32le(request, address);
    }
    const std::vector<std::uint8_t> response = transact(request);
    if (response.size() != addresses.size() * 4U) {
        throw std::runtime_error("PCSX2 PINE read32 reply has an unexpected size");
    }

    std::vector<std::uint32_t> values;
    values.reserve(addresses.size());
    for (std::size_t index = 0U; index < addresses.size(); ++index) {
        values.push_back(read_u32le(std::span<const std::uint8_t>(response).subspan(index * 4U)));
    }
    return std::move(values);
}

std::vector<std::uint8_t> Pcsx2PineClient::transact(const std::span<const std::uint8_t> request_payload) {
    if (!connected()) {
        throw std::logic_error("PCSX2 PINE client is disconnected");
    }
    if (request_payload.empty() || request_payload.size() > std::numeric_limits<std::uint32_t>::max() - 4U) {
        throw std::invalid_argument("PCSX2 PINE request payload has an invalid length");
    }

    // Packets are a little-endian u32 length (which counts itself) plus payload.
    std::vector<std::uint8_t> request;
    request.reserve(request_payload.size() + 4U);
    append_u32le(request, static_cast<std::uint32_t>(request_payload.size()) + 4U);
    request.insert(request.end(), request_payload.begin(), request_payload.end());
    send_all(descriptor_, request, use_msg_nosignal_);
    return receive_reply(descriptor_);
}

std::string Pcsx2PineClient::query_string(const std::uint8_t command) {
    const std::array<std::uint8_t, 1> request{command};
    const std::vector<std::uint8_t> response = transact(request);
    // Strings are a u32 length (including the terminator) and the bytes.
    if (response.size() < 4U) {
        throw std::runtime_error("PCSX2 PINE string reply is truncated");
    }
    const std::uint32_t length = read_u32le(response);
    if (length == 0U || length > response.size() - 4U) {
        throw std::runtime_error("PCSX2 PINE string reply has an invalid length");
    }
    return std::string(response.begin() + 4, response.begin() + 4 + (length - 1U));
}

void Pcsx2PineClient::close() noexcept {
    [[maybe_unused]] const int descriptor = std::exchange(descriptor_, -1);
#if !defined(_WIN32)
    if (descriptor >= 0) {
        ::close(descriptor);
    }
#endif
}

} // namespace takedown::platform
