#pragma once

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace takedown::runtime {

class NativeSimulationStatePublisher;

// Timing and scheduler identity attached to one scalar EE instruction.
struct Slus21050CollisionTraceContext {
    std::uint64_t committed_steps{};
    std::uint64_t ee_cycles{};
    std::uint64_t vblank_starts{};
    std::int32_t thread_id{};
};

// Value-owned raw state captured at the revision-checked world-contact
// return.  The arrays preserve the guest words exactly; no host float
// conversion or pointer borrowing is involved.
struct Slus21050VehicleWorldContactSnapshot {
    std::uint64_t sequence{};
    Slus21050CollisionTraceContext context{};
    guest::GuestAddress vehicle_object{};
    guest::GuestAddress physics_object{};
    guest::GuestAddress physics_component{};
    guest::GuestAddress owner_vehicle{};
    guest::GuestAddress world_node{};
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

// Opaque authority produced only after the outputless endpoint probe has
// completed an authenticated scalar return while bound to a nonzero session
// epoch. The publisher is a friend solely so it can copy the private payload;
// callers cannot fabricate, inspect, or mutate either field.
class Slus21050VehicleWorldContactPublicationToken final {
  public:
    Slus21050VehicleWorldContactPublicationToken(
        const Slus21050VehicleWorldContactPublicationToken&) = default;
    Slus21050VehicleWorldContactPublicationToken& operator=(
        const Slus21050VehicleWorldContactPublicationToken&) = default;
    Slus21050VehicleWorldContactPublicationToken(
        Slus21050VehicleWorldContactPublicationToken&&) noexcept = default;
    Slus21050VehicleWorldContactPublicationToken& operator=(
        Slus21050VehicleWorldContactPublicationToken&&) noexcept = default;

  private:
    friend class NativeSimulationStatePublisher;
    friend class Slus21050VehicleWorldContactEndpointProbe;

    Slus21050VehicleWorldContactPublicationToken(
        const std::uint64_t session_epoch,
        const Slus21050VehicleWorldContactSnapshot& snapshot) noexcept
        : session_epoch_(session_epoch), snapshot_(snapshot) {}

    std::uint64_t session_epoch_{};
    Slus21050VehicleWorldContactSnapshot snapshot_{};
};

struct Slus21050CollisionSnapshotDiagnostics {
    std::uint64_t return_observations{};
    std::uint64_t valid_completions{};
    std::uint64_t instruction_mismatches{};
    std::uint64_t pointer_rejections{};
    std::uint64_t cross_link_rejections{};
    std::uint64_t player_identity_rejections{};
    std::uint64_t incomplete_read_rejections{};
    std::uint64_t incomplete_completions{};
    std::uint64_t older_completions{};
    std::uint64_t token_rejections{};
    std::uint64_t sequence_overflows{};
};

struct Slus21050CollisionTraceConfig {
    // Preserve startup evidence even after a long race, then retain a rolling
    // tail around the final visible failure.
    std::size_t first_capacity{8'192U};
    std::size_t tail_capacity{24'576U};
};

struct Slus21050CollisionTraceStatistics {
    std::uint64_t total_samples{};
    std::size_t retained_samples{};
    std::uint64_t overwritten_samples{};
};

class Slus21050CollisionTrace {
  public:
    struct SampleToken {
        std::uint64_t sequence{};
        std::size_t index{};
        bool tail{};
    };

    explicit Slus21050CollisionTrace(
        std::filesystem::path output_path,
        Slus21050CollisionTraceConfig config = {});
    ~Slus21050CollisionTrace();

    Slus21050CollisionTrace(const Slus21050CollisionTrace&) = delete;
    Slus21050CollisionTrace& operator=(const Slus21050CollisionTrace&) = delete;
    Slus21050CollisionTrace(Slus21050CollisionTrace&&) noexcept;
    Slus21050CollisionTrace& operator=(Slus21050CollisionTrace&&) noexcept;

    // Returns the exact retail instruction word expected at a race-object
    // dispatch, world-contact decision, or downstream response comparison.
    // Every address is revision-checked against the supplied SLUS-21050 ELF.
    [[nodiscard]] static std::optional<std::uint32_t> expected_instruction(
        guest::GuestAddress pc) noexcept;

    // True only at a revision-checked instruction which begin_sample observes.
    // The hybrid runner supplies this predicate to its compiled-block scalar
    // barrier so target instructions cannot be executed or crossed by fusion.
    [[nodiscard]] static bool is_target(guest::GuestAddress pc) noexcept;

    // Called immediately before the scalar EE instruction. Non-target PCs are
    // a cheap no-op and allocate no sample.
    [[nodiscard]] std::optional<SampleToken> begin_sample(
        const guest::R5900State& state,
        const guest::EeMemory& memory,
        const Slus21050CollisionTraceContext& context);

    // Called immediately after the instruction. step_status is the numeric
    // EeStepStatus value, deliberately kept out of this public header so the
    // shipping/native executable does not acquire an interpreter dependency.
    void finish_sample(SampleToken token,
                       const guest::R5900State& state,
                       std::uint32_t step_status) noexcept;

    // Atomically publishes a versioned TSV through a sibling .partial file.
    // Refuses to overwrite either an existing final or partial artifact.
    void finalize();

    [[nodiscard]] Slus21050CollisionTraceStatistics statistics() const noexcept;
    [[nodiscard]] std::optional<Slus21050VehicleWorldContactSnapshot>
    latest_vehicle_snapshot() const noexcept;
    [[nodiscard]] Slus21050CollisionSnapshotDiagnostics
    snapshot_diagnostics() const noexcept;
    [[nodiscard]] const std::filesystem::path& output_path() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Outputless, diagnostic-only observer for the exact active-player vehicle
// state boundary used by the collision trace.  It retains only the newest
// value-owned snapshot in memory; it never changes guest execution, native
// dispatch, presentation, or simulation state.  The observer is intentionally
// separate from Slus21050CollisionTrace so a live run can authenticate this
// boundary without allocating a raw trace or writing an artifact.
// Diagnostic capture remains available while unbound; publication authority
// requires an explicit session-epoch bind.
class Slus21050VehicleWorldContactEndpointProbe {
  public:
    // Opaque copyable handle for one pre-instruction observation. Its sequence
    // and capture-valid bit stay private so callers cannot forge or mutate a
    // completion payload; finish_sample authenticates it against the one
    // serial pending record owned by the probe.
    struct SampleToken {
        SampleToken(const SampleToken&) = default;
        SampleToken& operator=(const SampleToken&) = default;
        SampleToken(SampleToken&&) noexcept = default;
        SampleToken& operator=(SampleToken&&) noexcept = default;

      private:
        friend class Slus21050VehicleWorldContactEndpointProbe;

        SampleToken(const std::uint64_t sequence, const bool snapshot_captured) noexcept
            : sequence_(sequence), snapshot_captured_(snapshot_captured) {}

        std::uint64_t sequence_{};
        bool snapshot_captured_{};
    };

    [[nodiscard]] static std::optional<std::uint32_t> expected_instruction(
        guest::GuestAddress pc) noexcept;
    [[nodiscard]] static bool is_target(guest::GuestAddress pc) noexcept;

    // Publication authority is disabled until a nonzero session epoch is
    // explicitly bound. Binding a new epoch invalidates any pending scalar
    // observation and clears the diagnostic latest value for that epoch.
    [[nodiscard]] bool bind_session_epoch(std::uint64_t session_epoch) noexcept;
    void reset() noexcept;

    // Returns a token at the exact pre-instruction boundary.  A token is
    // retained even when the pre-state is rejected so callers can use one
    // uniform post-step finish path; rejected observations never publish.
    [[nodiscard]] std::optional<SampleToken> begin_sample(
        const guest::R5900State& state,
        const guest::EeMemory& memory,
        const Slus21050CollisionTraceContext& context);

    // Publishes only an authenticated successful scalar completion.  The
    // post-state must be the exact sequential PC pair, must be outside a
    // delay slot, and must have status zero.  The post context must advance
    // steps/cycles by one while retaining the same VBlank and thread identity;
    // it becomes the snapshot's endpoint timing.
    std::optional<Slus21050VehicleWorldContactPublicationToken>
    finish_sample(SampleToken token,
                  const guest::R5900State& state,
                  std::uint32_t step_status,
                  const Slus21050CollisionTraceContext& post_context) noexcept;

    [[nodiscard]] std::optional<Slus21050VehicleWorldContactSnapshot>
    latest_snapshot() const noexcept;
    [[nodiscard]] Slus21050CollisionSnapshotDiagnostics diagnostics() const noexcept;

  private:
    struct PendingSample {
        std::uint64_t sequence{};
        Slus21050VehicleWorldContactSnapshot snapshot{};
    };

    std::uint64_t next_sequence_{};
    std::uint64_t bound_session_epoch_{};
    std::optional<PendingSample> pending_sample_;
    std::optional<Slus21050VehicleWorldContactSnapshot> latest_snapshot_;
    Slus21050CollisionSnapshotDiagnostics diagnostics_{};
};

} // namespace takedown::runtime
