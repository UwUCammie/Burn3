#pragma once

#include "takedown/guest/ee_memory.hpp"
#include "takedown/guest/r5900_state.hpp"
#include "takedown/runtime/native_function_dispatcher.hpp"
#include "takedown/runtime/native_function_preflight.hpp"
#include "takedown/runtime/retail_timer_owner_abi_trace.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace takedown::runtime {

// This is a source-private experiment. It has no dispatcher registration and
// is disabled unless a caller explicitly constructs it enabled. The route is
// the exact 43-word dynamic path observed at 0x001d3a60; the implementation
// below evaluates those instructions from the live entry state and image.
inline constexpr NativeFunctionLimits kRetailTimerOwnerNativeSemanticCost{43U, 43U};
inline constexpr std::size_t kRetailTimerOwnerNativeSemanticPathEntries = 43U;
inline constexpr std::uint64_t kRetailTimerOwnerNativeSemanticPathDigest =
    0x6bdf3a364a6791b4ULL;

struct RetailTimerOwnerNativeSemanticPathWord {
    guest::GuestAddress pc{};
    std::uint32_t word{};

    constexpr bool operator==(const RetailTimerOwnerNativeSemanticPathWord&) const = default;
};

inline constexpr std::array<RetailTimerOwnerNativeSemanticPathWord,
                            kRetailTimerOwnerNativeSemanticPathEntries>
    kRetailTimerOwnerNativeSemanticPath{{
        // lw a1,0(a0); lw v1,-0x5f34(gp); beq a1,v1,0x1d3b54; nop
        {0x001d3a60U, 0x8c850000U}, {0x001d3a64U, 0x8f83a0ccU},
        {0x001d3a68U, 0x10a3003aU}, {0x001d3a6cU, 0x00000000U},
        // lw a1,0x24(a0); lw a2,0x18(a0); beq a2,a1,0x1d3acc; nop
        {0x001d3a70U, 0x8c850024U}, {0x001d3a74U, 0x8c860018U},
        {0x001d3a78U, 0x10c50014U}, {0x001d3a7cU, 0x00000000U},
        // The selected path enters at 0x1d3acc and copies the global tick.
        {0x001d3accU, 0x8f83a0ccU}, {0x001d3ad0U, 0xac830000U},
        {0x001d3ad4U, 0x8c860000U}, {0x001d3ad8U, 0x8c830014U},
        {0x001d3adcU, 0x8c850004U}, {0x001d3ae0U, 0x00c31823U},
        {0x001d3ae4U, 0x00653823U}, {0x001d3ae8U, 0x18e0001aU},
        {0x001d3aecU, 0x00000000U},
        // Positive delta, enabled object, one subdivision.
        {0x001d3af0U, 0x90850028U}, {0x001d3af4U, 0x10a0003fU},
        {0x001d3af8U, 0x00000000U}, {0x001d3afcU, 0x8c860018U},
        {0x001d3b00U, 0x24050001U}, {0x001d3b04U, 0x10c50037U},
        {0x001d3b08U, 0x00000000U},
        // Add the delta to field 0x0c and return to the common tail.
        {0x001d3be4U, 0x8c85000cU}, {0x001d3be8U, 0x00a72821U},
        {0x001d3becU, 0x1000ffd8U}, {0x001d3bf0U, 0xac85000cU},
        {0x001d3b50U, 0xac830004U},
        // cvt/div/mula/madd and the swc1 delay slot.
        {0x001d3b54U, 0xc4810018U}, {0x001d3b58U, 0xc4800010U},
        {0x001d3b5cU, 0xc482000cU}, {0x001d3b60U, 0xc783a0d0U},
        {0x001d3b64U, 0x46800860U}, {0x001d3b68U, 0x46011843U},
        {0x001d3b6cU, 0x46800020U}, {0x001d3b70U, 0x468010a0U},
        {0x001d3b74U, 0x4601001aU}, {0x001d3b78U, 0x4603101cU},
        {0x001d3b7cU, 0x10000021U}, {0x001d3b80U, 0xe4800020U},
        {0x001d3c04U, 0x03e00008U}, {0x001d3c08U, 0x00000000U},
    }};

enum class RetailTimerOwnerNativeSemanticFailure : std::uint8_t {
    None,
    Disabled,
    WrongEntryAddress,
    EntryStateMismatch,
    CodeRangeInvalid,
    InstructionWordMismatch,
    DomainFingerprintMismatch,
    RawFingerprintMismatch,
    MemoryRangeInvalid,
    PathPredicateMismatch,
    PreflightRejected,
};

[[nodiscard]] std::string_view to_string(
    RetailTimerOwnerNativeSemanticFailure failure) noexcept;

struct RetailTimerOwnerNativeSemanticAdmission {
    RetailTimerOwnerNativeSemanticFailure failure{
        RetailTimerOwnerNativeSemanticFailure::None};
    NativeFunctionExecution execution{};
    std::uint32_t return_pc{};

    [[nodiscard]] bool admitted() const noexcept {
        return failure == RetailTimerOwnerNativeSemanticFailure::None;
    }
};

struct RetailTimerOwnerNativeSemanticResult {
    RetailTimerOwnerNativeSemanticAdmission admission{};
    bool applied{};
};

// A bounded, exact-route semantic evaluator. It intentionally accepts no
// captured invocation/proof: every branch input, output field, COP1 value,
// return PC, and ABI register residue is derived from the live call inputs.
class RetailTimerOwnerNativeSemantic {
  public:
    explicit RetailTimerOwnerNativeSemantic(bool enabled = false) noexcept
        : enabled_(enabled) {}

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    [[nodiscard]] RetailTimerOwnerNativeSemanticAdmission admit(
        const NativeFunctionPrepareContext& context,
        const NativeFunctionPreflightState& preflight) const noexcept;

    // Rejected calls leave CPU and memory untouched. On success the method
    // commits only the four stores in the reviewed path and the complete
    // architectural return residue of the live invocation.
    [[nodiscard]] RetailTimerOwnerNativeSemanticResult apply(
        NativeFunctionContext& context,
        const NativeFunctionPreflightState& preflight) const noexcept;

  private:
    bool enabled_{};
};

} // namespace takedown::runtime
