#include "takedown/runtime/slus_21050_model_submission.hpp"

namespace takedown::runtime::slus_21050 {
std::optional<ModelSubmission> ModelSubmission::prepare(
    ModelSubmissionResources resources, ModelSubmissionSelection selection) noexcept {
    if (!(resources.first_group_end <= resources.second_group_end) ||
        resources.second_group_end > resources.groups.size() ||
        selection.pass_count < 0 || selection.pass_count > 128) {
        return std::nullopt;
    }
    ModelSubmission result;
    result.resources_ = resources;
    result.group_ = selection.group_mode == 0 ? 0 : selection.group_mode == 1 ? std::size_t{resources.first_group_end} : std::size_t{resources.second_group_end};
    result.group_end_ = selection.group_mode == 0 ? std::size_t{resources.first_group_end}
        : selection.group_mode == 1 ? std::size_t{resources.second_group_end} : resources.groups.size();
    result.first_pass_ = selection.first_pass ? 0 : 1;
    result.pass_end_ = selection.additional_passes ? selection.pass_count : 1;
    for (auto group = result.group_; group < result.group_end_; ++group) {
        for (const auto& mesh : resources.groups[group].meshes) {
            for (const auto& part : mesh.parts) {
                if (part.material >= resources.materials.size()) return std::nullopt;
            }
        }
    }
    return result;
}

std::optional<ModelSubmissionStep> ModelSubmission::next(ModelSubmissionPassState& state) noexcept {
    for (;;) {
        switch (stage_) {
        case Stage::Begin:
            stage_ = Stage::Part;
            return ModelSubmissionStep{.kind = ModelSubmissionStepKind::BeginBatch};
        case Stage::Part:
            while (group_ < group_end_) {
                const auto meshes = resources_.groups[group_].meshes;
                while (mesh_ < meshes.size()) {
                    const auto parts = meshes[mesh_].parts;
                    if (part_ < parts.size()) {
                        const auto part = parts[part_];
                        const auto material = resources_.materials[part.material];
                        state.primary = material.primary;
                        state.secondary = material.secondary;
                        pass_ = first_pass_;
                        stage_ = Stage::Pass;
                        return ModelSubmissionStep{
                            .kind = ModelSubmissionStepKind::BeginPart, .material = part.material};
                    }
                    ++mesh_;
                    part_ = 0;
                }
                ++group_;
                mesh_ = 0;
            }
            stage_ = Stage::End;
            break;
        case Stage::Pass:
            if (pass_ >= pass_end_) {
                ++part_;
                stage_ = Stage::Part;
                return ModelSubmissionStep{.kind = ModelSubmissionStepKind::EndPart};
            }
            stage_ = Stage::Draw;
            if (pass_ != state.selected || state.primary != state.submitted_primary ||
                state.secondary != state.submitted_secondary) {
                state.selected = static_cast<std::int8_t>(pass_);
                state.submitted_primary = state.primary;
                state.submitted_secondary = state.secondary;
                return ModelSubmissionStep{.kind = ModelSubmissionStepKind::ApplyPass,
                    .pass = pass_, .primary = state.submitted_primary,
                    .secondary = state.submitted_secondary};
            }
            break;
        case Stage::Draw: {
            const auto part = resources_.groups[group_].meshes[mesh_].parts[part_];
            stage_ = Stage::Pass;
            return ModelSubmissionStep{.kind = ModelSubmissionStepKind::DrawPart,
                .geometry = part.geometry, .pass = pass_++};
        }
        case Stage::End:
            stage_ = Stage::Complete;
            return ModelSubmissionStep{.kind = ModelSubmissionStepKind::EndBatch};
        case Stage::Complete:
            return std::nullopt;
        }
    }
}
}
