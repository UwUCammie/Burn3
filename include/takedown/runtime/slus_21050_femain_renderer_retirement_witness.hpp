#pragma once

#include "takedown/runtime/slus_21050_femain_graphics_source_probe.hpp"

#include <cstdint>

namespace takedown::runtime {

// A value-only witness for the terminal primary-command delivery boundary of
// one FEMain frame.  The producer of this value is intentionally caller-owned
// and remains outside this passive adapter. terminal_primary_event_ordinal is
// in the producer's private diagnostic ordinal domain alongside the source
// probe's packet-completion ordinal; it is not an event from the separate
// shared serialized event domain. It must be after the completed PATH2 packet
// within that private diagnostic range. primary_first_ordinal and
// primary_command_count identify the exact contiguous primary range whose
// digest is carried below; primary_stream_digest is an opaque nonzero producer
// witness for that complete frame-specific primary stream.
// Neither field claims semantic GS acceptance or host presentation. The public
// deterministic authentication tags are consistency/tamper checks rather than
// authority, and no production path currently emits a successful terminal
// witness because the terminal producer hook is intentionally absent.
struct Slus21050FemainRendererTerminalPrimaryDeliveryWitness {
    Slus21050FemainFrameGraphicsSourceEvidence source_evidence{};
    std::uint64_t frame_id{};
    std::uint64_t emulation_thread_identity{};
    std::uint64_t worker_epoch{};
    std::uint64_t runtime_epoch{};
    std::uint64_t terminal_primary_event_ordinal{};
    std::uint64_t primary_first_ordinal{};
    std::uint64_t primary_command_count{};
    std::uint64_t primary_stream_digest{};
    std::uint64_t authentication_tag{};

    bool operator==(
        const Slus21050FemainRendererTerminalPrimaryDeliveryWitness&) const = default;
};

[[nodiscard]] std::uint64_t
authenticate_slus_21050_femain_renderer_terminal_primary_delivery_witness(
    const Slus21050FemainRendererTerminalPrimaryDeliveryWitness& witness) noexcept;

// Validate the producer-owned private primary range carried by the witness.
// The first primary follows the source probe's private packet-completion
// ordinal, and the producer-private terminal witness ordinal seals exactly
// count primary records. Authentication is checked separately so callers can
// classify a forged tag independently from a malformed range.
[[nodiscard]] bool validate_slus_21050_femain_renderer_terminal_primary_delivery_range(
    const Slus21050FemainRendererTerminalPrimaryDeliveryWitness& witness,
    const Slus21050FemainFrameGraphicsSourceEvidence& source) noexcept;

} // namespace takedown::runtime
