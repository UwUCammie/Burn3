#pragma once

#include "takedown/guest/gs_privileged_display.hpp"

#include <cstdint>
#include <string_view>

namespace takedown::presentation {

// A project-owned copy of the SetGsCrt facts the policy needs. Keeping it as
// plain data prevents this presentation-only evaluator from depending on the
// kernel HLE, guest clocks, or any renderer implementation.
struct GsCrtcHint {
    bool configured{};
    bool interlaced{};
    bool field_mode{};

    bool operator==(const GsCrtcHint&) const = default;
};

enum class GsSourceTopology : std::uint8_t {
    Unknown,
    ObservedProgressive,
    ObservedInterlaced,
    CrtHintProgressive,
    CrtHintInterlaced,
    ConflictingEvidence,
};

enum class HostFrameTopology : std::uint8_t {
    Progressive,
};

enum class ProgressiveConversion : std::uint8_t {
    Undetermined,
    NotRequiredWhenScanoutExists,
    RequiredBeforeScanout,
};

enum class ScanoutPixels : std::uint8_t {
    Unavailable,
};

[[nodiscard]] std::string_view to_string(GsSourceTopology topology) noexcept;
[[nodiscard]] std::string_view to_string(HostFrameTopology topology) noexcept;
[[nodiscard]] std::string_view to_string(ProgressiveConversion conversion) noexcept;
[[nodiscard]] std::string_view to_string(ScanoutPixels pixels) noexcept;

// This result has deliberately no framebuffer addresses, dimensions, cadence,
// or pixels. It establishes one host contract: later scanout code may submit
// complete progressive host frames only. It cannot alter guest timing.
struct GsDisplayPresentationMetadata {
    std::uint64_t privileged_revision{};
    GsSourceTopology source_topology{GsSourceTopology::Unknown};
    HostFrameTopology host_frame_topology{HostFrameTopology::Progressive};
    ProgressiveConversion progressive_conversion{ProgressiveConversion::Undetermined};
    ScanoutPixels scanout_pixels{ScanoutPixels::Unavailable};
    bool field_submission_forbidden{true};
    bool smode2_seen{};
    bool source_field_frame_mode{};
    bool pmode_seen{};
    bool display_slots_seen{};

    bool operator==(const GsDisplayPresentationMetadata&) const = default;
};

// Pure evaluation of source-field evidence. SMODE2.INT takes precedence when
// observed; a conflicting SetGsCrt hint blocks a scanout decision. An HLE hint
// alone is useful diagnostics, not proof of a source frame topology.
[[nodiscard]] GsDisplayPresentationMetadata
evaluate_gs_display_policy(const guest::GsPrivilegedDisplayState& state,
                           GsCrtcHint crt_hint) noexcept;

} // namespace takedown::presentation
