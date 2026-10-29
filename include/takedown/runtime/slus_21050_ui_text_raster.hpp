#pragma once

#include "takedown/runtime/slus_21050_ui_text_draw_guest.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace takedown::runtime::slus_21050 {

// Renderer-neutral residency supplied for one planned UI text draw. logical
// identifies the requested game raster while residency identifies the exact
// immutable sampled-image generation that satisfies it.
struct UiTextRasterResolution {
    UiSubmissionKey key;
    std::size_t bucket_index{};
    std::size_t node_index{};
    TextRasterBinding logical;
    TextRasterResidencyToken residency;

    bool operator==(const UiTextRasterResolution&) const = default;
};

struct ResolvedUiTextDrawBatchSubmission {
    UiTextDrawBatchSubmission draw;
    UiTextRasterResolution raster;

    bool operator==(const ResolvedUiTextDrawBatchSubmission&) const = default;
};

struct ResolvedUiTextDrawBatchesCandidate {
    SubmitUiDrawBucketsGuestPlan ui_plan;
    std::vector<ResolvedUiTextDrawBatchSubmission> submissions;

    bool operator==(const ResolvedUiTextDrawBatchesCandidate&) const = default;
};

// Positionally associates a complete set of caller-asserted raster resolutions
// with an owned UI text candidate. A missing, reordered, mismatched, or
// unresolved entry rejects the whole association. This checks identifiers; it
// does not authenticate store membership or capture image ownership. Passing an
// rvalue moves the potentially large geometry into the result after every entry
// validates.
[[nodiscard]] std::optional<ResolvedUiTextDrawBatchesCandidate>
associate_ui_text_raster_residency(SubmitUiTextDrawBatchesGuestCandidate candidate,
                                   std::span<const UiTextRasterResolution> raster_resolutions);

} // namespace takedown::runtime::slus_21050
