#include "takedown/presentation/gs_display_policy.hpp"

namespace takedown::presentation {

std::string_view to_string(const GsSourceTopology topology) noexcept {
    switch (topology) {
    case GsSourceTopology::Unknown:
        return "unknown";
    case GsSourceTopology::ObservedProgressive:
        return "observed progressive";
    case GsSourceTopology::ObservedInterlaced:
        return "observed interlaced";
    case GsSourceTopology::CrtHintProgressive:
        return "CRT hint: progressive";
    case GsSourceTopology::CrtHintInterlaced:
        return "CRT hint: interlaced";
    case GsSourceTopology::ConflictingEvidence:
        return "conflicting CRT evidence";
    default:
        return "unknown";
    }
}

std::string_view to_string(const HostFrameTopology topology) noexcept {
    switch (topology) {
    case HostFrameTopology::Progressive:
        return "progressive";
    }
    return "unknown";
}

std::string_view to_string(const ProgressiveConversion conversion) noexcept {
    switch (conversion) {
    case ProgressiveConversion::Undetermined:
        return "undetermined";
    case ProgressiveConversion::NotRequiredWhenScanoutExists:
        return "not required";
    case ProgressiveConversion::RequiredBeforeScanout:
        return "required before scanout";
    default:
        return "unknown";
    }
}

std::string_view to_string(const ScanoutPixels pixels) noexcept {
    switch (pixels) {
    case ScanoutPixels::Unavailable:
        return "unavailable";
    }
    return "unknown";
}

GsDisplayPresentationMetadata evaluate_gs_display_policy(const guest::GsPrivilegedDisplayState& state,
                                                         const GsCrtcHint crt_hint) noexcept {
    GsDisplayPresentationMetadata metadata;
    metadata.privileged_revision = state.revision;
    metadata.smode2_seen = state.smode2.seen;
    metadata.pmode_seen = state.pmode.seen;
    metadata.display_slots_seen = state.display[0].seen || state.display[1].seen;
    if (state.smode2.seen) {
        const guest::GsSmode2Fields smode2 = guest::decode_gs_smode2(state.smode2.raw);
        metadata.source_field_frame_mode = smode2.field_frame_mode;
        const GsSourceTopology observed_topology = smode2.interlaced ? GsSourceTopology::ObservedInterlaced
                                                            : GsSourceTopology::ObservedProgressive;
        if (crt_hint.configured && smode2.interlaced != crt_hint.interlaced) {
            metadata.source_topology = GsSourceTopology::ConflictingEvidence;
        } else {
            metadata.source_topology = observed_topology;
            // Observed progressive maps to NotRequiredWhenScanoutExists and
            // observed interlaced to RequiredBeforeScanout (same numeric values).
            metadata.progressive_conversion = static_cast<ProgressiveConversion>(observed_topology);
        }
    } else if (crt_hint.configured) {
        metadata.source_topology = crt_hint.interlaced ? GsSourceTopology::CrtHintInterlaced
                                                       : GsSourceTopology::CrtHintProgressive;
    }
    return metadata;
}

} // namespace takedown::presentation
