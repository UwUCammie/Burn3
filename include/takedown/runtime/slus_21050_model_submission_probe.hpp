#pragma once
#include "takedown/runtime/slus_21050_model_submission.hpp"
#include <memory>
#include <vector>

namespace takedown::guest { class EeMemory; class R5900State; }
namespace takedown::runtime::slus_21050 {
// Temporary read-only resource boundary. Native resource keys are dense indices;
// guest addresses are retained separately for diagnostic comparisons only.
struct DecodedModelSubmission {
    std::vector<ModelSubmissionMaterial> materials;
    std::vector<std::uint32_t> material_addresses, geometry_addresses;
    std::vector<std::vector<std::vector<ModelSubmissionPart>>> parts;
    std::vector<std::vector<ModelSubmissionMesh>> meshes;
    std::vector<ModelSubmissionGroup> groups;
    ModelSubmissionResources resources;
    ModelSubmissionSelection selection;
};
[[nodiscard]] std::unique_ptr<DecodedModelSubmission> decode_model_submission(
    const guest::EeMemory&, const guest::R5900State&);

// Bounded, opt-in shadow verification. It never executes native results in the
// game, writes guest state or changes scheduler admission.
class ModelSubmissionProbe {
public:
    ModelSubmissionProbe();
    ~ModelSubmissionProbe();
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] bool wants_pc(std::uint32_t) const noexcept;
    void observe(const guest::R5900State&, const guest::EeMemory&, bool measured);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool enabled_{};
};
}
