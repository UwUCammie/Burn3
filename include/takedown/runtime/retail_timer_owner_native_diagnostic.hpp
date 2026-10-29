#pragma once

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/runtime/native_function_dispatcher.hpp"
#include "takedown/runtime/native_function_preflight.hpp"
#include "takedown/runtime/retail_timer_owner_abi_trace.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace takedown::runtime {

// The owner ABI trace observed one 43-instruction/cycle path. Keep this
// diagnostic contract separate from the dispatcher: no native registration is
// safe until the captured residue can be generalized beyond one exact entry
// witness. The path digest is the trace's domain-separated FNV-1a digest for
// the exact PC/word sequence below.
inline constexpr NativeFunctionLimits kRetailTimerOwnerNativeCost{43U, 43U};
inline constexpr std::size_t kRetailTimerOwnerNativePathEntries = 43U;
inline constexpr std::uint64_t kRetailTimerOwnerNativePathDigest = 0x6bdf3a364a6791b4ULL;
// The reviewed ABI run observed the owner only as ordinary thread 1 work. The
// native prepare/preflight API carries no thread or callback identity, so this
// diagnostic accepts exactly that captured route and rejects every other
// context until the dispatcher can carry an owner identity through admission.
inline constexpr RetailTimerOwnerAbiTraceContext kRetailTimerOwnerNativeSupportedContext{
    .thread_id = 1, .callback_active = false, .callback_identity = 0U};

enum class RetailTimerOwnerNativeDiagnosticFailure : std::uint8_t {
    None,
    Disabled,
    WrongEntryAddress,
    DomainFingerprintMismatch,
    RawFingerprintMismatch,
    IncompleteResidue,
    UnsupportedContext,
    PathShapeMismatch,
    CostMismatch,
    ReturnPcMismatch,
    MemoryRangeInvalid,
    StaleEntryState,
    StaleObjectWitness,
    StaleGlobalWitness,
    StaleStackWitness,
    PreflightRejected,
};

[[nodiscard]] std::string_view to_string(
    RetailTimerOwnerNativeDiagnosticFailure failure) noexcept;

// A proof owns the complete architectural residue of one observed scalar
// invocation. Construction and storage are private: callers can only obtain a
// proof through the validating trace factory, and the adapter is the only
// consumer allowed to inspect the captured residue. It is deliberately
// exact-entry only; a sample cannot be turned into a generalized native
// implementation without a broader residue/profile proof.
class RetailTimerOwnerNativeResidueProof {
  public:
    RetailTimerOwnerNativeResidueProof(const RetailTimerOwnerNativeResidueProof&) = default;
    RetailTimerOwnerNativeResidueProof& operator=(
        const RetailTimerOwnerNativeResidueProof&) = default;
    RetailTimerOwnerNativeResidueProof(RetailTimerOwnerNativeResidueProof&&) noexcept = default;
    RetailTimerOwnerNativeResidueProof& operator=(
        RetailTimerOwnerNativeResidueProof&&) noexcept = default;

  private:
    RetailTimerOwnerNativeResidueProof() = default;

    friend std::optional<RetailTimerOwnerNativeResidueProof>
    make_retail_timer_owner_native_residue_proof(
        const RetailTimerOwnerAbiTrace::Invocation& invocation,
        const guest::EeMemory& memory) noexcept;
    friend class RetailTimerOwnerNativeAdapterDiagnostic;

    guest::R5900State entry_state_{};
    guest::R5900State return_jr_state_{};
    guest::R5900State return_state_{};
    RetailTimerOwnerAbiTraceContext context_{};
    std::uint32_t object_base_{};
    std::uint32_t global_base_{};
    std::uint32_t stack_base_{};
    std::array<std::uint8_t, kRetailTimerOwnerWitnessBytes> object_entry_{};
    std::array<std::uint8_t, kRetailTimerOwnerWitnessBytes> object_return_{};
    std::array<std::uint8_t, kRetailTimerOwnerWitnessBytes> global_entry_{};
    std::array<std::uint8_t, kRetailTimerOwnerWitnessBytes> global_return_{};
    std::array<std::uint8_t, kRetailTimerOwnerWitnessBytes> stack_entry_{};
    std::array<std::uint8_t, kRetailTimerOwnerWitnessBytes> stack_return_{};
    std::array<RetailTimerOwnerAbiTrace::PathEntry, kRetailTimerOwnerNativePathEntries> path_{};
    std::uint64_t entry_guest_steps_{};
    std::uint64_t entry_ee_cycles_{};
    std::uint32_t retired_instructions_{};
    std::uint32_t ee_cycles_{};
    std::uint64_t function_fingerprint_{};
    std::uint64_t raw_memory_fingerprint_{};
    std::uint64_t path_digest_{};
    std::uint32_t return_jr_word_{};
    std::uint32_t return_delay_word_{};
    std::uint32_t return_pc_{};
    bool complete_{};
};

// Converts an already captured ABI-trace invocation into a proof only when
// all exact path, cost, return, and witness conditions hold. The invocation is
// a serialized trace record, not provenance: a record without the same live
// EeMemory image cannot be promoted to a proof. This function is read-only and
// returns nullopt for an incomplete, unsupported, or independently unreplayed
// trace.
[[nodiscard]] std::optional<RetailTimerOwnerNativeResidueProof>
make_retail_timer_owner_native_residue_proof(
    const RetailTimerOwnerAbiTrace::Invocation& invocation,
    const guest::EeMemory& memory) noexcept;

struct RetailTimerOwnerNativeDiagnosticAdmission {
    RetailTimerOwnerNativeDiagnosticFailure failure{
        RetailTimerOwnerNativeDiagnosticFailure::None};
    NativeFunctionExecution execution{};
    std::uint64_t function_fingerprint{};
    std::uint64_t raw_memory_fingerprint{};
    std::uint64_t path_digest{};
    std::uint32_t return_pc{};

    [[nodiscard]] bool admitted() const noexcept {
        return failure == RetailTimerOwnerNativeDiagnosticFailure::None;
    }
};

struct RetailTimerOwnerNativeDiagnosticResult {
    RetailTimerOwnerNativeDiagnosticAdmission admission{};
    bool applied{};
};

// Default construction is safe and disabled. Tests and offline proof tooling
// can opt in explicitly, but this class has no dispatcher registration entry
// point.
class RetailTimerOwnerNativeAdapterDiagnostic {
  public:
    explicit RetailTimerOwnerNativeAdapterDiagnostic(bool enabled = false) noexcept
        : enabled_(enabled) {}

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    // Read-only admission. The caller supplies the same scheduler snapshot
    // used by NativeFunctionPreflight; no CPU or memory bytes are changed.
    [[nodiscard]] RetailTimerOwnerNativeDiagnosticAdmission admit(
        const NativeFunctionPrepareContext& context,
        const NativeFunctionPreflightState& preflight,
        const RetailTimerOwnerNativeResidueProof& proof) const noexcept;

    // Applies only an exact proof that admits against the current state. A
    // rejected or stale call returns with no architectural or memory change.
    [[nodiscard]] RetailTimerOwnerNativeDiagnosticResult apply(
        NativeFunctionContext& context,
        const NativeFunctionPreflightState& preflight,
        const RetailTimerOwnerNativeResidueProof& proof) const noexcept;

  private:
    bool enabled_{};
};

} // namespace takedown::runtime
