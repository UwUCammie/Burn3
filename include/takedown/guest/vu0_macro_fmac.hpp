#pragma once

#include "takedown/guest/types.hpp"

#include <cstddef>
#include <cstdint>

namespace takedown::guest {

class R5900State;

// Result of one macro-mode VU0 FMAC instruction.  Keeping the value and the
// flags together prevents compiled/native paths from reproducing the vector
// lanes while accidentally retaining stale MAC/STATUS state.
struct Vu0MacroFmacResult {
    Gpr128 value{};
    std::uint16_t mac_flags{};
};

// Convert one raw binary32 lane with macro-mode VFTOI semantics.  The
// instruction encodings use 0, 4, 12, or 15 fractional bits; keeping the
// parameter explicit also lets reviewed native paths share the same primitive.
[[nodiscard]] std::uint32_t
convert_vu0_macro_float_to_integer_bits(std::uint32_t bits, unsigned fractional_bits) noexcept;

[[nodiscard]] Vu0MacroFmacResult execute_vu0_macro_broadcast_add_subtract(
    const Gpr128& destination, const Gpr128& left, const Gpr128& right,
    std::uint8_t destination_mask, std::size_t right_lane, bool subtract) noexcept;

[[nodiscard]] Vu0MacroFmacResult execute_vu0_macro_vector_add_subtract(
    const Gpr128& destination, const Gpr128& left, const Gpr128& right,
    std::uint8_t destination_mask, bool subtract) noexcept;

[[nodiscard]] Vu0MacroFmacResult execute_vu0_macro_vector_multiply(
    const Gpr128& destination, const Gpr128& left, const Gpr128& right,
    std::uint8_t destination_mask) noexcept;

[[nodiscard]] Vu0MacroFmacResult execute_vu0_macro_broadcast_multiply(
    const Gpr128& destination, const Gpr128& left, const Gpr128& right,
    std::uint8_t destination_mask, std::size_t right_lane) noexcept;

// VMADD/VMSUB (and the ACC-writing forms, with `destination` = ACC): ACC
// plus or minus a separately chopped FS*FT product.
[[nodiscard]] Vu0MacroFmacResult execute_vu0_macro_vector_multiply_accumulate(
    const Gpr128& destination, const Gpr128& accumulator, const Gpr128& left,
    const Gpr128& right, std::uint8_t destination_mask, bool subtract) noexcept;

[[nodiscard]] Vu0MacroFmacResult execute_vu0_macro_broadcast_multiply_accumulate(
    const Gpr128& destination, const Gpr128& accumulator, const Gpr128& left,
    const Gpr128& right, std::uint8_t destination_mask, std::size_t right_lane,
    bool subtract) noexcept;

// VOPMULA (ACC.xyz = FS.yzx * FT.zxy) and VOPMSUB (FD.xyz = ACC - that
// product). W keeps `destination`'s lane and reports no flags.
[[nodiscard]] Vu0MacroFmacResult execute_vu0_macro_outer_product_multiply(
    const Gpr128& destination, const Gpr128& left, const Gpr128& right) noexcept;

[[nodiscard]] Vu0MacroFmacResult execute_vu0_macro_outer_product_subtract(
    const Gpr128& destination, const Gpr128& accumulator, const Gpr128& left,
    const Gpr128& right) noexcept;

// Macro-mode SYNCMSFLAGS publication: replace the current MAC value, publish
// the STATUS summary, accumulate its sticky summary, and seed all four MAC
// history slots used by a following VCALLMS launch.
void publish_vu0_macro_fmac_result(R5900State& state,
                                   std::uint16_t mac_flags) noexcept;

} // namespace takedown::guest
