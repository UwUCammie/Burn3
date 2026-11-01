// 0x002fbcd0 (a0 = fixed-point record, a1 = slot, f12 = float): stores f12 * 65536, truncated to an
// integer, as word a1 of the record. In mode 1 (the word at +112) it also recomputes the derived word
// at +12 after slot 1 and at +16 after slot 2; slots 3 to 6 store nothing. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::uint32_t kScale = 0x47800000U;  // 65536.0f
constexpr GuestAddress kModeOffset = 112U;     // slot 28 is this word, so a store there changes the mode

// The stored word: f12 * 65536, truncated.
std::uint32_t to_fixed(const std::uint32_t value_bits) {
    return static_cast<std::uint32_t>(fpu::to_int(fpu::mul(kScale, value_bits)));
}

// The derived word after slot 1, from the words at +4 and +72 read as integers: the quotient
// (word4 / 65536) / word72 scaled by 65536 and truncated gives x. The original's chain of shifts and
// adds on the sign-extended 64-bit x is 411774 * x, so the result is (x * 411774) >> 16, arithmetic,
// truncated to 32 bits.
std::uint32_t slot_one_derived(const std::uint32_t word4, const std::uint32_t word72) {
    const std::uint32_t quotient = fpu::div(fpu::from_int(static_cast<std::int32_t>(word4)), kScale);
    const std::uint32_t scaled = fpu::mul(fpu::div(quotient, fpu::from_int(static_cast<std::int32_t>(word72))), kScale);
    const auto x = static_cast<std::int64_t>(fpu::to_int(scaled));
    return static_cast<std::uint32_t>((x * 411774) >> 16);
}

// The derived word after slot 2: (65536 / word8) scaled by 65536 and truncated, where word8 is the
// word at +8 read as an integer.
std::uint32_t slot_two_derived(const std::uint32_t word8) {
    const std::uint32_t quotient = fpu::div(kScale, fpu::from_int(static_cast<std::int32_t>(word8)));
    return static_cast<std::uint32_t>(fpu::to_int(fpu::mul(quotient, kScale)));
}

template <class Context>
n3::Run set_fixed_point_parameter(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    // The original's beq and bne tests on a1 compare the whole register, while sll (and so the store
    // address) uses its low word. Both are kept: slot_register for the tests, slot for the address.
    const std::uint64_t slot_register = call.arg64(1);
    const std::uint32_t slot = call.arg(1);
    const std::uint32_t mode = m.read_u32_le(record + kModeOffset);
    const std::uint32_t fixed = to_fixed(call.cpu().read_fpr_bits(12U));

    // lui, lw, mtc1, nop, mul.s, addiu, cvt.w.s, mfc1, and the bne with its delay slot.
    std::uint32_t retired = 10U;
    if (mode == 1U) {
        // Slots 3, 4 and 5 store nothing: addiu, beq and its nop, then jr and its nop.
        retired += 3U;
        if (slot_register == 3U) {
            return call.finish(retired + 2U);
        }
        retired += 3U;
        if (slot_register == 4U) {
            return call.finish(retired + 2U);
        }
        retired += 3U;
        if (slot_register == 5U) {
            return call.finish(retired + 2U);
        }
        // addiu, bne and its nop. The bne goes to the store unless the slot is 6.
        retired += 3U;
        if (slot_register == 6U) {
            // beq and its nop to the exit, then jr and its nop.
            return call.finish(retired + 4U);
        }
    }

    // sll, addiu, addu, sw. The mode is read again because slot 28 is the mode word.
    m.write_u32_le(record + (slot << 2U), fixed);
    const std::uint32_t mode_after = m.read_u32_le(record + kModeOffset);
    // The four store instructions, then lw, beq and its nop.
    retired += 7U;
    if (mode_after != 1U) {
        // Two beq (each with its nop) to the exit, then jr and its nop.
        return call.finish(retired + 6U);
    }

    // beq for slot 1 and its nop.
    retired += 2U;
    if (slot_register == 1U) {
        const std::uint32_t word4 = m.read_u32_le(record + 4U);
        const std::uint32_t word72 = m.read_u32_le(record + 72U);
        m.write_u32_le(record + 12U, slot_one_derived(word4, word72));
        // The 28-instruction slot-1 block, then addiu, bne and its nop, and jr with its nop.
        return call.finish(retired + 28U + 5U);
    }
    // addiu, bne and its nop. The bne is taken unless the slot is 2.
    retired += 3U;
    if (slot_register != 2U) {
        // jr and its nop.
        return call.finish(retired + 2U);
    }
    m.write_u32_le(record + 16U, slot_two_derived(m.read_u32_le(record + 8U)));
    // The 11-instruction slot-2 block, then beq and its nop, and jr with its nop.
    return call.finish(retired + 11U + 4U);
}

} // namespace

// Declared cost 77 = the longest control-flow path (bin/brief.py); the listing-feasible maximum is 64, a
// larger ceiling is safe (the exact count is what a call reports).
extern constinit const EngineFunction n3_002fbcd0{0x002fbcd0U, 324U, 0x71cf4f5287d42404ULL, "SetFixedPointParameter",
                                                   77U, n3::live<&set_fixed_point_parameter<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(002fbcd0, set_fixed_point_parameter)
