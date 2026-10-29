#pragma once

#include "takedown/runtime/slus_21050_femain_movie_transaction_owner.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace takedown::runtime {

// This identity is the exact retail route that a future live FEMain bridge
// must prove before it can be considered for registration. The executable and
// function fingerprints are the reviewed SLUS_210.50 values; they are not a
// request to patch or dispatch the route.
inline constexpr std::string_view kSlus21050FemainLiveRevision = "SLUS_210.50";
inline constexpr std::string_view kSlus21050FemainLiveFunction = "FE_JoinDrainPump";
inline constexpr guest::GuestAddress kSlus21050FemainLiveFunctionAddress = 0x001f2408U;
inline constexpr std::uint32_t kSlus21050FemainLiveFunctionBytes = 536U;
inline constexpr std::uint64_t kSlus21050FemainLiveFunctionFingerprint =
    0x955f080b7957d7dcULL;
inline constexpr std::uint64_t kSlus21050FemainLiveExecutableFingerprint =
    0xbc028b51459c3948ULL;

struct Slus21050FemainLiveSourceIdentity {
    std::string_view revision{};
    std::string_view function{};
    guest::GuestAddress function_address{};
    std::uint32_t function_bytes{};
    std::uint64_t function_fingerprint{};
    std::uint64_t executable_fingerprint{};

    bool operator==(const Slus21050FemainLiveSourceIdentity&) const = default;
};

inline constexpr Slus21050FemainLiveSourceIdentity
    kSlus21050FemainLiveExpectedSource{
        .revision = kSlus21050FemainLiveRevision,
        .function = kSlus21050FemainLiveFunction,
        .function_address = kSlus21050FemainLiveFunctionAddress,
        .function_bytes = kSlus21050FemainLiveFunctionBytes,
        .function_fingerprint = kSlus21050FemainLiveFunctionFingerprint,
        .executable_fingerprint = kSlus21050FemainLiveExecutableFingerprint,
    };

enum class Slus21050FemainLiveAdmissionFailure : std::uint8_t {
    None,
    Disabled,
    SourceIdentityInvalid,
    SourceRevisionMismatch,
    SourceFunctionMismatch,
    SourceFunctionAddressMismatch,
    SourceFunctionExtentMismatch,
    SourceFunctionFingerprintMismatch,
    SourceExecutableFingerprintMismatch,
    InvalidEmulationThreadIdentity,
    EmulationThreadIdentityMismatch,
    ContextParticipantMissing,
    GuestMemoryMissing,
    GuestMemoryMutationMismatch,
    PlayerInactive,
    OwnerNotPrepared,
    OwnerGuestCommitted,
    OwnerPlayerMismatch,
    OwnerPlayerSessionMismatch,
    PreparedFrameMissing,
    PlayerIdentityMismatch,
    PlayerSessionMismatch,
    FrameLeaseMismatch,
    FramePlanMismatch,
    MediaSessionMismatch,
    MediaClockInvalid,
    MediaClockMismatch,
    LayoutGenerationMismatch,
    LayoutSnapshotMismatch,
    LedgerInactive,
    LedgerIdentityMismatch,
    LedgerSessionMismatch,
    LedgerSnapshotMismatch,
    GraphicsEvidenceMissing,
    GraphicsOrderInvalid,
    GraphicsEvidenceMismatch,
    AuthenticationMismatch,
    Replay,
};

[[nodiscard]] std::string_view to_string(
    Slus21050FemainLiveAdmissionFailure failure) noexcept;

// All participant pointers are borrowed. A context is therefore a read-only
// observation of a currently prepared owner transaction, not a capability to
// mutate the owner, ledger, guest memory, or dispatch table. A successful
// diagnostic admission may advance the player's private persistent replay
// high-water mark; it never changes player receipt/accounting state.
struct Slus21050FemainLiveAdmissionContext {
    Slus21050FemainLiveSourceIdentity source{};
    // These two witnesses are supplied by the serialized emulation-thread
    // caller. This diagnostic validates their consistency, but it does not
    // yet authenticate them against a live scheduler or graphics callback.
    std::uint64_t emulation_thread_identity{};
    std::uint64_t media_clock_nanoseconds{};
    // The caller increments this for each clock sample within a media
    // session. It makes equal elapsed values from separate samples distinct.
    std::uint64_t media_clock_generation{};
    const Slus21050FemainMovieTransactionOwner* transaction_owner{};
    const Slus21050MoviePlayer* player{};
    const Slus21050FemainMovieConsumerPlan* consumer_plan{};
    const Slus21050FemainFrameTransactionLedger* ledger{};
    const guest::EeMemory* memory{};
    // This must be the same boundary evidence sampled by the emulation thread
    // for the graphics completion/consume edge. A missing pointer is an
    // intentional fail-closed state. Like the thread identity, it remains a
    // caller-supplied diagnostic witness until a live callback supplies the
    // authenticated evidence.
    const Slus21050FemainMovieTransactionBoundaries* graphics_boundaries{};
};

// The fixed-value snapshot binds source, execution identity, player checkout,
// media clock, layout, passive ledger state, and graphics ordering evidence.
// Its integrity tag is a deterministic domain-separated FNV-1a tag; this is
// diagnostic tamper evidence and not a cryptographic authorization primitive.
struct Slus21050FemainLiveAdmissionSnapshot {
    Slus21050FemainLiveSourceIdentity source{};
    std::uint64_t emulation_thread_identity{};
    std::uint64_t player_identity{};
    std::uint64_t player_session_generation{};
    Slus21050MovieFrameLease frame_lease{};
    std::uint64_t frame_generation{};
    std::uint64_t media_ordinal{};
    std::uint64_t source_sequence{};
    std::uint32_t source_checksum{};
    std::uint64_t payload_digest{};
    std::uint64_t media_session_generation{};
    std::uint64_t media_clock_nanoseconds{};
    std::uint64_t media_clock_generation{};
    Slus21050FemainMovieLayoutSnapshot layout{};
    std::optional<Slus21050FemainHandoffToken> handoff{};
    Slus21050FemainFrameLedgerSnapshot ledger{};
    Slus21050FemainMovieTransactionBoundaries graphics_boundaries{};
    bool graphics_evidence_present{};
    guest::GuestAddress first_buffer{};
    guest::GuestAddress second_buffer{};
    std::size_t buffer_bytes{};
    guest::EeMainRamMutationToken work_mutation{};
    guest::EeMainRamMutationToken buffer_mutation{};
    std::optional<std::uint64_t> timeline_ordinal{};
    std::uint64_t authentication_tag{};

    bool operator==(const Slus21050FemainLiveAdmissionSnapshot&) const = default;
};

// Captures no guest bytes and performs no owner/player/ledger mutation. The
// optional is empty when the observation cannot prove a complete passive
// transaction boundary.
[[nodiscard]] std::optional<Slus21050FemainLiveAdmissionSnapshot>
capture_slus_21050_femain_live_admission(
    const Slus21050FemainLiveAdmissionContext& context) noexcept;

[[nodiscard]] std::uint64_t authenticate_slus_21050_femain_live_admission(
    const Slus21050FemainLiveAdmissionSnapshot& snapshot) noexcept;

struct Slus21050FemainLiveAdmissionResult {
    Slus21050FemainLiveAdmissionFailure failure{
        Slus21050FemainLiveAdmissionFailure::None};

    [[nodiscard]] bool admitted() const noexcept {
        return failure == Slus21050FemainLiveAdmissionFailure::None;
    }
};

// Default construction is disabled. Enabling this diagnostic only validates
// a snapshot against current read-only state; it never performs guest writes,
// acknowledges the player, changes the ledger, registers a callback, or
// selects legacy background mode 3.
class Slus21050FemainLiveAdmissionDiagnostic {
  public:
    explicit Slus21050FemainLiveAdmissionDiagnostic(bool enabled = false) noexcept
        : enabled_(enabled) {}

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    [[nodiscard]] Slus21050FemainLiveAdmissionResult admit(
        const Slus21050FemainLiveAdmissionSnapshot& snapshot,
        const Slus21050FemainLiveAdmissionContext& current) const noexcept;

  private:
    bool enabled_{};
    mutable std::optional<Slus21050FemainLiveAdmissionSnapshot> last_admitted_{};
};

} // namespace takedown::runtime
