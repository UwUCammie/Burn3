#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace takedown::runtime::slus_21050 {
// Immutable native resource views. Keys identify host-owned resources; they are
// not guest addresses. The views must outlive the submission and its pauses.
struct ModelSubmissionMaterial { std::uint64_t primary{}, secondary{}; };
struct ModelSubmissionPart { std::uint32_t material{}, geometry{}; };
struct ModelSubmissionMesh { std::span<const ModelSubmissionPart> parts; };
struct ModelSubmissionGroup { std::span<const ModelSubmissionMesh> meshes; };
struct ModelSubmissionResources {
    std::span<const ModelSubmissionGroup> groups;
    std::span<const ModelSubmissionMaterial> materials;
    std::uint32_t first_group_end{}, second_group_end{};
};
struct ModelSubmissionSelection {
    std::int32_t group_mode{};
    bool first_pass{}, additional_passes{};
    std::int32_t pass_count{};
};
struct ModelSubmissionPassState {
    std::int8_t selected{-1};
    std::uint64_t primary{}, secondary{}, submitted_primary{}, submitted_secondary{};
    bool operator==(const ModelSubmissionPassState&) const = default;
};
enum class ModelSubmissionStepKind { BeginBatch, BeginPart, ApplyPass, DrawPart, EndPart, EndBatch };
struct ModelSubmissionStep {
    ModelSubmissionStepKind kind{};
    std::uint32_t material{}, geometry{};
    std::int32_t pass{};
    std::uint64_t primary{}, secondary{};
    bool operator==(const ModelSubmissionStep&) const = default;
};

// Owns traversal and pass reuse for the recovered model submission operation.
// A consumer must finish each returned operation before requesting the next;
// it can suspend for transfer completion without running an instruction loop.
// The consumer supplies pass state after each leaf completes, including the
// batch-begin reset and any state invalidation performed by a leaf.
// Leaf operations and the temporary console scheduler are deliberately outside
// this component. It is not yet registered as a live guest replacement.
class ModelSubmission {
public:
    [[nodiscard]] static std::optional<ModelSubmission> prepare(
        ModelSubmissionResources, ModelSubmissionSelection) noexcept;
    [[nodiscard]] std::optional<ModelSubmissionStep> next(ModelSubmissionPassState&) noexcept;
private:
    enum class Stage { Begin, Part, Pass, Draw, End, Complete };
    ModelSubmissionResources resources_;
    std::size_t group_{}, group_end_{}, mesh_{}, part_{};
    std::int32_t first_pass_{}, pass_end_{}, pass_{};
    Stage stage_{Stage::Begin};
};
}
