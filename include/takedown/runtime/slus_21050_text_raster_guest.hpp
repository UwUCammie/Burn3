#pragma once

#include "takedown/guest/types.hpp"
#include "takedown/runtime/slus_21050_text_draw.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace takedown::guest {

class EeMemory;

} // namespace takedown::guest

namespace takedown::runtime::slus_21050 {

inline constexpr guest::GuestAddress kSelectRasterClutVariantAddress = 0x00219a20U;
inline constexpr std::size_t kSelectRasterClutVariantBodyBytes = 616U;
inline constexpr std::uint64_t kSelectRasterClutVariantFingerprint = 0x74c68c19a290a38cULL;
inline constexpr guest::GuestAddress kSelectBaseRasterVariantAddress = 0x00219c90U;
inline constexpr std::size_t kSelectBaseRasterVariantBodyBytes = 8U;
inline constexpr std::uint64_t kSelectBaseRasterVariantFingerprint = 0xd4ab287ccd5249b2ULL;

// SubmitPlainTextLines passes this canonical raster address to the selector-
// zero trampoline before inspecting its text. The cached no-op path does not
// dereference the raster, but the typed boundary deliberately admits only a
// direct main-RAM identity.
struct BaseRasterSelectionGuestInput {
    guest::GuestAddress raster{};

    bool operator==(const BaseRasterSelectionGuestInput&) const = default;
};

// Exact values of the two cache cells consulted by the reviewed path.
struct BaseRasterSelectionCacheSnapshot {
    guest::GuestAddress raster{};
    std::uint32_t palette_variant_index{};

    bool operator==(const BaseRasterSelectionCacheSnapshot&) const = default;
};

// An immutable transaction for the cached selector-zero path through
// SelectBaseRasterVariant and SelectRasterClutVariant. It owns every guest
// value that decides admission and the fingerprints of both executed bodies.
struct BaseRasterSelectionGuestPlan {
    BaseRasterSelectionGuestInput input;
    BaseRasterSelectionCacheSnapshot cache;
    TextRasterBinding selection;
    std::uint64_t trampoline_function_fingerprint{};
    std::uint64_t target_function_fingerprint{};
    std::uint32_t exact_retired_instructions{};
    std::uint32_t exact_ee_cycles{};

    bool operator==(const BaseRasterSelectionGuestPlan&) const = default;
};

// Read-only compatibility evidence for the resident selector-zero path. The
// raw TEX0 value is intentionally confined to this guest-facing plan: it is a
// request input for a later residency registry, not proof of immutable texel
// or effective-palette residency and not a renderer resource identity.
struct ResidentBaseRasterResourceGuestPlan {
    TextRasterBinding logical;
    std::uint64_t tex0_bits{};
    std::uint32_t resident_path_word{};
    std::uint64_t trampoline_function_fingerprint{};
    std::uint64_t target_function_fingerprint{};

    bool operator==(const ResidentBaseRasterResourceGuestPlan&) const = default;
};

[[nodiscard]] std::optional<BaseRasterSelectionGuestPlan>
plan_select_base_raster_from_guest(const guest::EeMemory& memory,
                                   const BaseRasterSelectionGuestInput& input);

// Snapshots the base TEX0 and the nonzero resident-path discriminator for one
// selector-zero logical binding. This does not inspect or mutate selector
// caches or packet state, resolve GS memory, mint a residency token, or assign
// submission order.
[[nodiscard]] std::optional<ResidentBaseRasterResourceGuestPlan>
plan_resident_base_raster_resource_from_guest(const guest::EeMemory& memory,
                                              TextRasterBinding logical);

// Replans and exact-compares before returning the renderer-facing binding.
// The admitted retail path changes no persistent state, so this semantic
// commit intentionally performs no guest writes. A stale or forged plan
// throws std::invalid_argument.
[[nodiscard]] TextRasterBinding commit_select_base_raster(guest::EeMemory& memory,
                                                          const BaseRasterSelectionGuestPlan& plan);

namespace detail {

// Replays the complete EE ABI residue of the reviewed selector-zero cached
// path after a containing transaction has revalidated the plan and proved the
// live 48-byte stack frame safe. A0's low 64 bits must equal the zero-extended
// plan raster, GP's low word must be the canonical SLUS-210.50 global pointer,
// and SP and RA must still hold their retail call-site values. The helper
// deliberately does not replan: aggregate text callers need one stale check
// before their first nested stack spill.
void apply_select_base_raster_ee_abi_prevalidated(std::array<guest::Gpr128, 32U>& registers,
                                                  guest::EeMemory& memory,
                                                  const BaseRasterSelectionGuestPlan& plan);

} // namespace detail

} // namespace takedown::runtime::slus_21050
