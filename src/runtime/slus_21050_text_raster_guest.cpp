#include "takedown/runtime/slus_21050_text_raster_guest.hpp"

#include "takedown/guest/ee_memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace takedown::runtime::slus_21050 {

namespace {

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
// Cache cells read by the selector-zero path: the bound raster and its palette.
constexpr guest::GuestAddress kSelectedRasterCellAddress = 5122288U;
constexpr guest::GuestAddress kSelectedPaletteCellAddress = 5122292U;
constexpr std::size_t kSelectedRasterMinimumMemoryBytes = 5122296U;
constexpr std::uint32_t kSelectionExactRetiredInstructions = 20U;
constexpr std::uint32_t kSelectionExactEeCycles = 20U;

// FNV-1a over a domain tag, the address, the body length and the body bytes,
// after checking that the body lies inside guest memory.
std::optional<std::uint64_t> function_fingerprint(const guest::EeMemory& memory,
                                                  const guest::GuestAddress address,
                                                  const std::uint64_t body_bytes) {
    const auto begin = static_cast<std::uint64_t>(address);
    const std::uint64_t end = body_bytes + begin;
    if (begin > end) {
        return std::nullopt;
    }
    if (end > memory.size()) {
        return std::nullopt;
    }
    std::uint64_t hash = kFnvOffsetBasis;
    const char* domain = "takedown-ee-function-v1";
    for (std::size_t index = 0U; index != 24U; ++index) {
        hash = (hash ^ static_cast<std::uint64_t>(static_cast<unsigned char>(domain[index]))) * kFnvPrime;
    }
    for (unsigned int shift = 0U; shift != 32U; shift += 8U) {
        hash = (hash ^ ((address >> shift) & 255U)) * kFnvPrime;
    }
    const auto body_length = static_cast<std::uint32_t>(body_bytes);
    for (unsigned int shift = 0U; shift != 32U; shift += 8U) {
        hash = (hash ^ ((body_length >> shift) & 255U)) * kFnvPrime;
    }
    for (std::uint64_t offset = 0U; offset != body_bytes; ++offset) {
        const auto byte_address = static_cast<guest::GuestAddress>(address + static_cast<std::uint32_t>(offset));
        hash = (hash ^ static_cast<std::uint64_t>(memory.read_u8(byte_address))) * kFnvPrime;
    }
    return hash;
}

std::uint32_t sign_word(const std::uint32_t value) noexcept {
    return static_cast<std::int32_t>(value) < 0 ? 0xFFFFFFFFU : 0U;
}

} // namespace

std::optional<BaseRasterSelectionGuestPlan>
plan_select_base_raster_from_guest(const guest::EeMemory& memory,
                                   const BaseRasterSelectionGuestInput& input) {
    const auto raster = static_cast<std::uint64_t>(input.raster);
    if (memory.size() < raster + 1U) {
        return std::nullopt;
    }
    if (memory.size() <= kSelectedRasterMinimumMemoryBytes - 1U) {
        return std::nullopt;
    }
    const auto trampoline = function_fingerprint(memory, kSelectBaseRasterVariantAddress,
                                                 kSelectBaseRasterVariantBodyBytes);
    if (!trampoline.has_value() || *trampoline != kSelectBaseRasterVariantFingerprint) {
        return std::nullopt;
    }
    const auto target = function_fingerprint(memory, kSelectRasterClutVariantAddress,
                                             kSelectRasterClutVariantBodyBytes);
    if (!target.has_value() || *target != kSelectRasterClutVariantFingerprint) {
        return std::nullopt;
    }
    const std::uint32_t cache_raster = memory.read_u32_le(kSelectedRasterCellAddress);
    const std::uint32_t cache_palette = memory.read_u32_le(kSelectedPaletteCellAddress);
    if (cache_raster != input.raster || cache_palette != 0U) {
        return std::nullopt;
    }

    BaseRasterSelectionGuestPlan plan{};
    plan.input = input;
    plan.cache.raster = cache_raster;
    plan.cache.palette_variant_index = cache_palette;
    plan.selection.raster.value = raster;
    plan.selection.palette_variant_index = 0U;
    plan.trampoline_function_fingerprint = kSelectBaseRasterVariantFingerprint;
    plan.target_function_fingerprint = kSelectRasterClutVariantFingerprint;
    plan.exact_retired_instructions = kSelectionExactRetiredInstructions;
    plan.exact_ee_cycles = kSelectionExactEeCycles;
    return plan;
}

std::optional<ResidentBaseRasterResourceGuestPlan>
plan_resident_base_raster_resource_from_guest(const guest::EeMemory& memory,
                                              TextRasterBinding logical) {
    if (logical.palette_variant_index != 0U) {
        return std::nullopt;
    }
    const std::uint64_t raster_value = logical.raster.value;
    if (raster_value - 1U > 4294967294ULL) {
        return std::nullopt;
    }
    if ((raster_value & 3U) != 0U) {
        return std::nullopt;
    }
    if (raster_value > 4294967152ULL) {
        return std::nullopt;
    }
    const auto raster = static_cast<std::uint32_t>(raster_value);
    const auto tex0_address = static_cast<guest::GuestAddress>(raster + 60U);
    const auto resident_path_word_address = static_cast<guest::GuestAddress>(raster + 140U);
    if (memory.size() < static_cast<std::uint64_t>(tex0_address) + 8U) {
        return std::nullopt;
    }
    if (static_cast<std::uint64_t>(resident_path_word_address) + 4U > memory.size()) {
        return std::nullopt;
    }
    const auto trampoline = function_fingerprint(memory, kSelectBaseRasterVariantAddress,
                                                 kSelectBaseRasterVariantBodyBytes);
    if (!trampoline.has_value() || *trampoline != kSelectBaseRasterVariantFingerprint) {
        return std::nullopt;
    }
    const auto target = function_fingerprint(memory, kSelectRasterClutVariantAddress,
                                             kSelectRasterClutVariantBodyBytes);
    if (!target.has_value() || *target != kSelectRasterClutVariantFingerprint) {
        return std::nullopt;
    }
    const std::uint32_t resident_path_word = memory.read_u32_le(resident_path_word_address);
    if (resident_path_word == 0U) {
        return std::nullopt;
    }
    const std::uint32_t tex0_low = memory.read_u32_le(tex0_address);
    const std::uint32_t tex0_high = memory.read_u32_le(static_cast<guest::GuestAddress>(raster + 64U));

    ResidentBaseRasterResourceGuestPlan plan{};
    plan.logical = logical;
    plan.tex0_bits = static_cast<std::uint64_t>(tex0_low) | (static_cast<std::uint64_t>(tex0_high) << 32U);
    plan.resident_path_word = resident_path_word;
    plan.trampoline_function_fingerprint = kSelectBaseRasterVariantFingerprint;
    plan.target_function_fingerprint = kSelectRasterClutVariantFingerprint;
    return plan;
}

TextRasterBinding commit_select_base_raster(guest::EeMemory& memory,
                                            const BaseRasterSelectionGuestPlan& plan) {
    const auto current = plan_select_base_raster_from_guest(memory, plan.input);
    if (!current.has_value() || !(*current == plan)) {
        throw std::invalid_argument(
            "SLUS-21050 base-raster selection plan is stale or outside the reviewed path");
    }
    return plan.selection;
}

namespace detail {

void apply_select_base_raster_ee_abi_prevalidated(std::array<guest::Gpr128, 32U>& registers,
                                                  guest::EeMemory& memory,
                                                  const BaseRasterSelectionGuestPlan& plan) {
    const std::uint32_t stack_top = registers[29].words[0];
    const std::uint32_t frame_base = stack_top - 48U;
    const std::uint64_t return_address =
        (static_cast<std::uint64_t>(registers[31].words[1]) << 32U) | registers[31].words[0];

    registers[5].words[0] = 0U;
    registers[5].words[1] = 0U;
    registers[29].words[0] = frame_base;
    registers[29].words[1] = sign_word(frame_base);
    memory.write_u64_le(stack_top - 16U, return_address);
    memory.write_u128_le(stack_top - 32U, registers[17].words);
    memory.write_u128_le(frame_base, registers[16].words);
    registers[3].words[0] = plan.cache.raster;
    registers[3].words[1] = sign_word(plan.cache.raster);
    registers[3].words[0] = plan.cache.palette_variant_index;
    registers[3].words[1] = sign_word(plan.cache.palette_variant_index);
    registers[29].words[0] = stack_top;
    registers[29].words[1] = sign_word(stack_top);
}

} // namespace detail

} // namespace takedown::runtime::slus_21050
