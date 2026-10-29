#pragma once

#include "takedown/runtime/native_simulation_driver.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>

namespace takedown::runtime {

class Slus21050VehicleWorldContactPublicationToken;

enum class NativeSimulationStateFailure : std::uint8_t {
    None,
    Disabled,
    InvalidEpoch,
    InvalidRate,
    BatchState,
    Capacity,
    NonAuthoritativeGuestTime,
    InvalidFrame,
    InvalidStep,
    NonMonotonicSimulationTick,
    NonMonotonicReferenceTick,
    NonMonotonicGuestTime,
    SequenceOverflow,
    InvalidPhaseOrigin,
    InvalidVehicleSnapshot,
    InvalidVehicleEpoch,
};

[[nodiscard]] std::string_view to_string(NativeSimulationStateFailure failure) noexcept;

// Result of attempting to attach an authenticated vehicle endpoint to the
// latest fixed-step state. A valid endpoint can arrive between fixed-step
// boundaries; that is a deferred observation rather than a state failure.
// Rejected covers malformed, stale, replayed, or wrong-epoch tokens.
enum class NativeSimulationVehiclePublicationResult : std::uint8_t {
    Published,
    Deferred,
    Rejected,
};

// Immutable origin for one native-state publication lifetime. A rate change
// requires a new epoch so readers never interpolate across incompatible time
// domains. The ordinals and guest cycle endpoint describe the state before the
// first publication in the epoch.
struct NativeSimulationStateEpoch {
    std::uint64_t session_epoch{};
    FixedStepRate simulation_rate{};
    FixedStepRate reference_rate{};
    FixedStepRate guest_time_rate{294'912'000U, 1U};
    float reference_delta_seconds{1.0F / 60.0F};
    std::uint64_t simulation_tick{};
    std::uint64_t reference_tick{};
    std::uint64_t guest_time_cycles{};
    std::uint64_t guest_time_cycle_remainder{};
    RetailReferenceTimerState reference_timer_state{};
    std::uint64_t simulation_phase_nanoseconds{};
    std::optional<NativeSimulationPhaseOrigin> simulation_phase_origin{};
};

// Value-owned active-player state copied from the authenticated world-contact
// endpoint. The arrays retain guest bit patterns; no guest pointer or host
// float reference crosses the publication boundary.
struct NativeSimulationVehicleState {
    std::uint64_t source_sequence{};
    std::uint64_t committed_steps{};
    std::uint64_t guest_time_cycles{};
    std::uint64_t vblank_starts{};
    std::int32_t thread_id{};
    std::uint32_t vehicle_object{};
    std::uint32_t physics_object{};
    std::uint32_t physics_component{};
    std::uint32_t owner_vehicle{};
    std::uint32_t world_node{};
    std::uint16_t world_triangle{};
    std::array<std::uint32_t, 16U> transform_words{};
    std::array<std::uint32_t, 16U> vector_words{};
    std::array<std::uint32_t, 8U> contact_words{};
    std::array<std::uint32_t, 16U> component_words{};
    bool world_node_valid{};
    bool world_triangle_valid{};
    bool role_valid{};
    bool complete{};
};

// One owned copy of the metadata associated with a completed simulation step.
// Presentation interpolation state is published separately because a bounded
// host batch can execute several steps but has only one final accumulator.
struct NativeSimulationStateSnapshot {
    std::uint64_t session_epoch{};
    std::uint64_t publication_sequence{};
    FixedStepRate simulation_rate{};
    FixedStepRate reference_rate{};
    SimulationStep step{};
    RetailReferenceTimerState timer_state{};
    std::uint64_t guest_time_cycles{};
    std::uint64_t guest_time_cycle_remainder{};
    bool authoritative{};
    std::optional<NativeSimulationVehicleState> vehicle_state{};
};

struct NativeSimulationPresentationState {
    std::uint64_t session_epoch{};
    std::uint64_t associated_publication_sequence{};
    std::uint64_t active_time_backlog_nanoseconds{};
    std::uint64_t presentation_delta_nanoseconds{};
    double interpolation_alpha{};
    bool catch_up_limited{};
    bool presentation_allowed{};
    bool authoritative{};
};

struct NativeSimulationStatePair {
    NativeSimulationStateSnapshot previous{};
    NativeSimulationStateSnapshot current{};
};

struct NativeSimulationInterpolationState {
    NativeSimulationStatePair pair{};
    NativeSimulationPresentationState presentation{};
};

// Allocation-free transactional publication sink. The single simulation
// producer stages one complete driver batch in caller-owned storage. end_batch
// publishes at most the two newest immutable snapshots under one short lock,
// so readers can never observe half of a catch-up batch. It is passive until a
// valid epoch begins and has no boot-entry registration.
class NativeSimulationStatePublisher final : public NativeSimulationStepSink {
  public:
    NativeSimulationStatePublisher() noexcept = default;
    explicit NativeSimulationStatePublisher(
        std::span<NativeSimulationStateSnapshot> batch_storage) noexcept
        : batch_storage_(batch_storage) {}

    NativeSimulationStatePublisher(const NativeSimulationStatePublisher&) = delete;
    NativeSimulationStatePublisher& operator=(const NativeSimulationStatePublisher&) = delete;

    // Lifecycle operations require producer quiescence: no driver callback may
    // overlap begin_epoch() or reset(). Readers may continue copying the
    // committed publication because that storage is independently locked.
    [[nodiscard]] bool begin_epoch(const NativeSimulationStateEpoch& epoch) noexcept;
    void reset() noexcept;

    [[nodiscard]] bool enabled() const noexcept { return !batch_storage_.empty(); }
    [[nodiscard]] bool epoch_active() const noexcept {
        return epoch_active_.load(std::memory_order_acquire);
    }
    [[nodiscard]] NativeSimulationStateFailure failure() const noexcept {
        return failure_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t capacity() const noexcept { return batch_storage_.size(); }
    [[nodiscard]] std::uint64_t publication_count() const noexcept;
    [[nodiscard]] bool read_latest(NativeSimulationStateSnapshot& snapshot) const noexcept;
    [[nodiscard]] bool read_pair(NativeSimulationStatePair& pair) const noexcept;
    [[nodiscard]] bool read_presentation_state(
        NativeSimulationPresentationState& state) const noexcept;
    [[nodiscard]] bool read_interpolation_state(
        NativeSimulationInterpolationState& state) const noexcept;

    // Copies one authenticated, epoch-bound vehicle endpoint onto the current
    // authoritative publication. A valid endpoint with no current fixed-step
    // boundary, or one ahead of it, is deferred without replacing state.
    // Invalid, stale, replayed, or wrong-epoch tokens fail closed.
    [[nodiscard]] NativeSimulationVehiclePublicationResult
    publish_vehicle_snapshot_result(
        const Slus21050VehicleWorldContactPublicationToken& token) noexcept;

    // Compatibility convenience for callers that only need to know whether a
    // publication happened. Deferred and rejected both return false; callers
    // that must distinguish them should use publish_vehicle_snapshot_result().
    [[nodiscard]] bool publish_vehicle_snapshot(
        const Slus21050VehicleWorldContactPublicationToken& token) noexcept;

    [[nodiscard]] bool begin_batch(const NativeSimulationDriverFrame& frame,
                                   std::uint32_t step_count) noexcept override;
    [[nodiscard]] bool record_step(
        const NativeSimulationTraceStep& step) noexcept override;
    [[nodiscard]] bool end_batch() noexcept override;
    void abort_batch() noexcept override;

  private:
    void fail(NativeSimulationStateFailure failure) noexcept;
    [[nodiscard]] bool check_step(const NativeSimulationTraceStep& step) noexcept;

    std::span<NativeSimulationStateSnapshot> batch_storage_{};
    NativeSimulationStateEpoch epoch_{};
    NativeSimulationDriverFrame batch_frame_{};
    std::uint32_t batch_expected_{};
    std::uint32_t batch_recorded_{};
    std::uint64_t last_simulation_tick_{};
    std::uint64_t last_reference_tick_{};
    std::uint64_t last_guest_time_cycles_{};
    std::uint64_t last_guest_time_cycle_remainder_{};
    std::uint64_t batch_previous_simulation_tick_{};
    std::uint64_t batch_previous_reference_tick_{};
    std::uint64_t batch_previous_guest_time_cycles_{};
    std::uint64_t batch_previous_guest_time_cycle_remainder_{};
    RetailReferenceTimerState last_timer_state_{};
    RetailReferenceTimerState batch_previous_timer_state_{};
    std::atomic_bool epoch_active_{};
    bool batch_open_{};

    mutable std::mutex publication_mutex_;
    NativeSimulationStateSnapshot previous_{};
    NativeSimulationStateSnapshot current_{};
    NativeSimulationPresentationState presentation_{};
    std::uint64_t publication_count_{};
    std::size_t retained_count_{};
    bool have_presentation_{};
    std::uint64_t last_vehicle_source_sequence_{};
    bool have_vehicle_source_sequence_{};
    std::atomic<NativeSimulationStateFailure> failure_{NativeSimulationStateFailure::None};
};

} // namespace takedown::runtime
