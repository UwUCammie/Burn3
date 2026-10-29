#include "takedown/runtime/slus_21050_femain_primary_stream_digest.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace takedown::runtime {
namespace {

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::uint64_t kInvalidDigest = std::numeric_limits<std::uint64_t>::max();


[[nodiscard]] std::uint64_t mix_word(const std::uint64_t hash, const std::uint64_t value) noexcept {
    return (hash ^ value) * kFnvPrime;
}

[[nodiscard]] std::uint64_t mix_bytes(std::uint64_t hash, const std::uint64_t value, const std::size_t byte_count) noexcept {
    for (std::size_t shift = 0U; shift != byte_count * 8U; shift += 8U) {
        hash = mix_word(hash, (value >> shift) & 255U);
    }
    return hash;
}

[[nodiscard]] std::uint64_t mix_domain(std::uint64_t hash, const std::string_view domain) noexcept {
    for (const char byte : domain) {
        hash = mix_word(hash, static_cast<std::uint8_t>(byte));
    }
    return hash;
}

[[nodiscard]] std::uint64_t finish(const std::uint64_t hash) noexcept {
    return hash != 0U ? hash : kInvalidDigest;
}

[[nodiscard]] std::uint64_t register_write_digest(const guest::GsRegisterWrite& write) noexcept {
    std::uint64_t hash = mix_domain(kFnvOffsetBasis, "slus-21050-femain-primary-register-write-v1");
    hash = mix_bytes(hash, write.sequence, 8U);
    hash = mix_word(hash, static_cast<std::uint64_t>(write.path));
    hash = mix_word(hash, static_cast<std::uint64_t>(write.source));
    hash = mix_word(hash, write.address);
    hash = mix_bytes(hash, write.value, 8U);
    hash = mix_bytes(hash, write.packed_high, 8U);
    return finish(hash);
}

[[nodiscard]] std::uint64_t image_transfer_digest(const guest::GsImageTransfer& transfer) noexcept {
    std::uint64_t hash = mix_domain(kFnvOffsetBasis, "slus-21050-femain-primary-image-transfer-v1");
    hash = mix_bytes(hash, transfer.sequence, 8U);
    hash = mix_word(hash, static_cast<std::uint64_t>(transfer.path));
    hash = mix_bytes(hash, transfer.destination_base_pointer, 4U);
    hash = mix_bytes(hash, transfer.destination_buffer_width, 4U);
    hash = mix_bytes(hash, transfer.destination_psm, 4U);
    hash = mix_bytes(hash, transfer.destination_x, 4U);
    hash = mix_bytes(hash, transfer.destination_y, 4U);
    hash = mix_bytes(hash, transfer.width, 4U);
    hash = mix_bytes(hash, transfer.height, 4U);
    hash = mix_bytes(hash, static_cast<std::uint64_t>(transfer.pixels.size()), 8U);
    for (const std::uint8_t pixel : transfer.pixels) {
        hash = mix_word(hash, pixel);
    }
    return finish(hash);
}

[[nodiscard]] std::uint8_t wire_kind(const Slus21050FemainPrimaryStreamCommandKind kind) noexcept {
    // Reviewed wire values: GIF tag = 0, register write = 2, IMAGE transfer = 3.
    switch (kind) {
    case Slus21050FemainPrimaryStreamCommandKind::GifTagBegin:
        return 0U;
    case Slus21050FemainPrimaryStreamCommandKind::RegisterWrite:
        return 2U;
    case Slus21050FemainPrimaryStreamCommandKind::ImageTransfer:
        return 3U;
    default:
        return 255U;
    }
}

[[nodiscard]] bool stream_bounds_valid(const std::size_t count, const std::uint64_t primary_first_ordinal) noexcept {
    return count != 0U && count <= 65536U && primary_first_ordinal != 0U;
}

[[nodiscard]] std::uint64_t stream_preamble(const std::size_t count) noexcept {
    std::uint64_t hash = mix_bytes(kFnvOffsetBasis, 42U, 8U);
    hash = mix_domain(hash, "slus-21050-femain-packet-primary-stream-v1");
    return mix_bytes(hash, static_cast<std::uint64_t>(count), 8U);
}

} // namespace

std::uint64_t digest_slus_21050_femain_primary_gif_tag(const guest::GifPath path) noexcept {
    const std::uint64_t hash = mix_domain(kFnvOffsetBasis, "slus-21050-femain-primary-gif-tag-begin-v1");
    const std::uint64_t mixed = hash ^ static_cast<std::uint64_t>(path);
    return mixed != 0U ? mixed * kFnvPrime : kInvalidDigest;
}

std::uint64_t digest_slus_21050_femain_primary_register_write(const guest::GsRegisterWrite& write) noexcept {
    return register_write_digest(write);
}

std::uint64_t digest_slus_21050_femain_primary_image_transfer(const guest::GsImageTransfer& transfer) noexcept {
    return image_transfer_digest(transfer);
}

std::uint64_t digest_slus_21050_femain_primary_command(const Slus21050FemainPrimaryStreamCommand& command) noexcept {
    switch (command.kind) {
    case Slus21050FemainPrimaryStreamCommandKind::GifTagBegin:
        return digest_slus_21050_femain_primary_gif_tag(command.path);
    case Slus21050FemainPrimaryStreamCommandKind::RegisterWrite:
        return register_write_digest(command.register_write);
    case Slus21050FemainPrimaryStreamCommandKind::ImageTransfer:
        return image_transfer_digest(command.image_transfer);
    default:
        return 0U;
    }
}

std::uint64_t digest_slus_21050_femain_primary_stream(
    const std::span<const Slus21050FemainPrimaryStreamCommand> commands,
    const std::uint64_t primary_first_ordinal) noexcept {
    if (!stream_bounds_valid(commands.size(), primary_first_ordinal)) {
        return 0U;
    }
    std::uint64_t hash = stream_preamble(commands.size());
    std::uint64_t ordinal = primary_first_ordinal;
    for (const auto& command : commands) {
        if (ordinal == kInvalidDigest) {
            return 0U;
        }
        const std::uint64_t digest = digest_slus_21050_femain_primary_command(command);
        hash = mix_bytes(hash, ordinal, 8U);
        hash = mix_word(hash, wire_kind(command.kind));
        hash = mix_word(hash, static_cast<std::uint64_t>(command.path));
        hash = mix_bytes(hash, command.guest_sequence, 8U);
        if (digest == 0U) {
            return 0U;
        }
        hash = mix_bytes(hash, digest, 8U);
        ordinal += 1U;
    }
    return finish(hash);
}

std::uint64_t digest_slus_21050_femain_primary_stream_entries(
    const std::span<const Slus21050FemainPrimaryStreamDigestEntry> entries,
    const std::uint64_t primary_first_ordinal) noexcept {
    if (!stream_bounds_valid(entries.size(), primary_first_ordinal)) {
        return 0U;
    }
    std::uint64_t hash = stream_preamble(entries.size());
    std::uint64_t ordinal = primary_first_ordinal;
    for (const auto& entry : entries) {
        if (ordinal == kInvalidDigest) {
            return 0U;
        }
        if (entry.command_digest == 0U) {
            return 0U;
        }
        hash = mix_bytes(hash, ordinal, 8U);
        hash = mix_word(hash, wire_kind(entry.kind));
        hash = mix_word(hash, static_cast<std::uint64_t>(entry.path));
        hash = mix_bytes(hash, entry.guest_sequence, 8U);
        hash = mix_bytes(hash, entry.command_digest, 8U);
        ordinal += 1U;
    }
    return finish(hash);
}

} // namespace takedown::runtime
