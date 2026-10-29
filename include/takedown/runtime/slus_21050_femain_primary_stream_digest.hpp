#pragma once

#include "takedown/guest/gs_capture.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace takedown::runtime {

// The command values retained by the FEMain terminal coordinator. The
// underlying enum preserves the coordinator's existing source-level values;
// digest_slus_21050_femain_primary_stream() applies the reviewed wire values
// (GIF tag = 0, register write = 2, IMAGE transfer = 3) explicitly.
enum class Slus21050FemainPrimaryStreamCommandKind : std::uint8_t {
    GifTagBegin,
    RegisterWrite,
    ImageTransfer,
};

// The complete decoded command value used by the canonical primary stream
// digest. The nested coordinator schema aliases this value so the digest can
// be shared by other primary observers without changing the callback-owned
// evidence layout.
struct Slus21050FemainPrimaryStreamCommand {
    Slus21050FemainPrimaryStreamCommandKind kind{
        Slus21050FemainPrimaryStreamCommandKind::RegisterWrite};
    guest::GifPath path{guest::GifPath::Path1};
    std::uint64_t guest_sequence{};
    guest::GsRegisterWrite register_write{};
    guest::GsImageTransfer image_transfer{};

    bool operator==(const Slus21050FemainPrimaryStreamCommand&) const = default;
};

// Compact canonical entry retained by an execution observer after it has
// already digested the complete command payload. Keeping this schema separate
// from worker serials lets independent observers compare the same stream at a
// caller-supplied absolute primary ordinal.
struct Slus21050FemainPrimaryStreamDigestEntry {
    Slus21050FemainPrimaryStreamCommandKind kind{
        Slus21050FemainPrimaryStreamCommandKind::RegisterWrite};
    guest::GifPath path{guest::GifPath::Path1};
    std::uint64_t guest_sequence{};
    std::uint64_t command_digest{};

    bool operator==(const Slus21050FemainPrimaryStreamDigestEntry&) const = default;
};

// Digest one complete decoded command value. A zero return means the command
// kind is outside the reviewed schema; valid digests are always nonzero.
[[nodiscard]] std::uint64_t digest_slus_21050_femain_primary_command(
    const Slus21050FemainPrimaryStreamCommand& command) noexcept;
[[nodiscard]] std::uint64_t
digest_slus_21050_femain_primary_gif_tag(guest::GifPath path) noexcept;
[[nodiscard]] std::uint64_t digest_slus_21050_femain_primary_register_write(
    const guest::GsRegisterWrite& write) noexcept;
[[nodiscard]] std::uint64_t digest_slus_21050_femain_primary_image_transfer(
    const guest::GsImageTransfer& transfer) noexcept;

// Digest a non-empty, bounded primary command stream beginning at the supplied
// absolute primary ordinal. The stream digest includes each ordinal, command
// kind, path, guest sequence, and complete command-value digest in order.
// Invalid input returns zero; valid digests are always nonzero.
[[nodiscard]] std::uint64_t digest_slus_21050_femain_primary_stream(
    std::span<const Slus21050FemainPrimaryStreamCommand> commands,
    std::uint64_t primary_first_ordinal) noexcept;
[[nodiscard]] std::uint64_t digest_slus_21050_femain_primary_stream_entries(
    std::span<const Slus21050FemainPrimaryStreamDigestEntry> entries,
    std::uint64_t primary_first_ordinal) noexcept;

} // namespace takedown::runtime
