#pragma once
#include "takedown/guest/ee_memory.hpp"
#include "takedown/runtime/slus_21050_model_producer.hpp"
#include "takedown/runtime/slus_21050_model_geometry_executor.hpp"
#include "takedown/runtime/slus_21050_model_source.hpp"
#include <memory>

namespace takedown::runtime { class NativeFunctionDispatcher; }
namespace takedown::runtime::slus_21050 {
struct ModelLiveStatistics {
    std::uint64_t admitted{},published{},declined_allocator{},declined_source{},declined_geometry{};
    std::uint64_t resolved{},cancelled{},source_uploads{},source_reuses{};
    std::size_t pending{};
};
struct ModelLiveGeometryReuseStatistics {
    bool enabled{};
    std::uint64_t preparations{},reuses{};
};
// Transitional parent/queue adapter. The game-level producer is independent
// of this class. An admitted parent owns its entire model; the small ordered
// marker/outer-state restore is the only compatibility packet it produces.
// The genuine queue and timed source snapshot consume/release that packet.
// No queue read cursor, busy bit or GPU completion is fabricated here.
class ModelLive final : public guest::Vif1Path2SourceMarkerObserver {
public:
    using Sink=std::function<bool(const TakedownNative::ModelPublication&)>;
    ModelLive(guest::Vif1Path2SourceMarkerObserver& forward,Sink);
    ~ModelLive() override;
    bool register_parent(NativeFunctionDispatcher&,const guest::EeMemory&);
    bool failed() const noexcept;
    void throw_if_failed() const;
    ModelLiveStatistics statistics() const noexcept;
    ModelProducerStatistics producer_statistics() const noexcept;
    ModelGeometryExecutorStatistics geometry_statistics() const noexcept;
    ModelLiveGeometryReuseStatistics geometry_reuse_statistics() const noexcept;
    ModelAssetResourceStatistics asset_statistics() const noexcept;
    bool has_vif1_dmac_pre_snapshot_candidates() const noexcept override;
    std::size_t on_vif1_dmac_pre_snapshot_candidates(
        const guest::Vif1DmacPreSnapshotObservation&,
        std::span<guest::Vif1DmacPreSnapshotCandidate>) noexcept override;
    void on_vif1_dmac_pre_snapshot_markers_resolved(
        std::span<const guest::Vif1DmacPreSnapshotResolvedMarker>,
        std::span<guest::Vif1DmacPreSnapshotMarkerDecision>) noexcept override;
    void on_vif1_path2_source_marker_reached(const guest::Vif1Path2SourceMarkerReached&) noexcept override;
    void on_vif1_path2_source_marker_cancelled(const guest::Vif1Path2SourceMarkerCancelled&) noexcept override;
    void on_vif1_path2_source_marker_memory_cleared() noexcept override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
