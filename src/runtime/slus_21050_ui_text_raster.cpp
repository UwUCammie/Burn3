#include "takedown/runtime/slus_21050_ui_text_raster.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace takedown::runtime::slus_21050 {

std::optional<ResolvedUiTextDrawBatchesCandidate> associate_ui_text_raster_residency(
    SubmitUiTextDrawBatchesGuestCandidate candidate,
    std::span<const UiTextRasterResolution> raster_resolutions) {
    if (raster_resolutions.size() != candidate.submissions.size()) {
        return std::nullopt;
    }
    std::vector<ResolvedUiTextDrawBatchSubmission> submissions;
    submissions.reserve(candidate.submissions.size());
    for (std::size_t index = 0U; index != candidate.submissions.size(); ++index) {
        const UiTextRasterResolution& resolution = raster_resolutions[index];
        UiTextDrawBatchSubmission& draw = candidate.submissions[index];
        if (resolution.residency.value == 0U) {
            return std::nullopt;
        }
        if (resolution.key.value != draw.ui.key.value ||
            resolution.bucket_index != draw.ui.bucket_index ||
            resolution.node_index != draw.ui.node_index ||
            resolution.logical.raster.value != draw.batch.raster.raster.value ||
            resolution.logical.palette_variant_index != draw.batch.raster.palette_variant_index) {
            return std::nullopt;
        }
    }
    for (std::size_t index = 0U; index != candidate.submissions.size(); ++index) {
        submissions.push_back(ResolvedUiTextDrawBatchSubmission{
            .draw = std::move(candidate.submissions[index]),
            .raster = raster_resolutions[index],
        });
    }
    return ResolvedUiTextDrawBatchesCandidate{
        .ui_plan = std::move(candidate.ui_plan),
        .submissions = std::move(submissions),
    };
}

} // namespace takedown::runtime::slus_21050
