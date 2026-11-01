// 0x00363870 (a0 = the record): StepRingCursor. Leaf, no loops, no calls. The record fields the listing uses:
//   +256  word: zero means nothing to do (the result is 0 and nothing is stored)
//   +260  word: the target of a downward step; the target is (word + 15) % 16 (remainder with the sign of the dividend)
//   +264  word: the target of an upward step
//   +556  halfword (sign-extended when read): the cursor; the only store the function makes
//   +558  byte: direction, nonzero for a downward step, zero for an upward one
// When the cursor is on its target the result is 0 and nothing changes. Otherwise the cursor moves one place modulo 16,
// down for a nonzero direction byte and up for a zero one, is stored, and the result is record + 16 * cursor, the entry
// the cursor was on. Retired counts: 7 (+256 zero), 15 and 18 + extra when on target, 20 + extra (up step) and
// 22 + extra + extra (down step). v0 is the result (EngineReturn::Integer). The listing's temporaries are not written.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kGate = 256U;        // word: zero means no step
constexpr GuestAddress kDownTarget = 260U;  // word: target of a downward step, read as (word + 15) % 16
constexpr GuestAddress kUpTarget = 264U;    // word: target of an upward step
constexpr GuestAddress kCursor = 556U;      // halfword: the cursor
constexpr GuestAddress kDirection = 558U;   // byte: nonzero steps down

// The listing's remainder of x by 16 (bgez on x, andi 15, then beq and addiu -16 for a negative x), which is C's x % 16.
// `extra` is the number of instructions it retires after its andi: 0 for x >= 0, 2 for a negative x with remainder 0,
// 3 otherwise.
struct Remainder16 {
    std::int32_t value;
    std::uint32_t extra;
};

Remainder16 remainder16(const std::int32_t x) {
    const std::int32_t low = x & 15;
    if (x >= 0) {
        return {low, 0U};
    }
    if (low == 0) {
        return {0, 2U};
    }
    return {low - 16, 3U};
}

template <class Context>
n3::Run step_ring_cursor(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0U);

    // lw $v0, 256($a0); beq $v0, $zero: a zero word leaves the record alone and v0 = 0.
    if (m.read_u32_le(record + kGate) == 0U) {
        call.return_value(0U);
        return call.finish(7U);
    }

    const auto cursor = static_cast<std::int32_t>(static_cast<std::int16_t>(m.read_u16_le(record + kCursor)));
    // The listing computes this entry address (sll 4, then addu with a0) before its branches; the paths that return 0
    // overwrite v0 with 0, the others return it.
    const GuestAddress entry = record + (static_cast<std::uint32_t>(cursor) << 4U);

    if (m.read_u8(record + kDirection) != 0U) {
        const Remainder16 down_target =
            remainder16(static_cast<std::int32_t>(m.read_u32_le(record + kDownTarget) + 15U));
        if (down_target.value == cursor) {
            call.return_value(0U);
            return call.finish(18U + down_target.extra);
        }
        const Remainder16 down_step = remainder16(cursor + 15);
        m.write_u16_le(record + kCursor, static_cast<std::uint16_t>(down_step.value));
        call.return_value(entry);
        return call.finish(22U + down_target.extra + down_step.extra);
    }

    const auto up_target = static_cast<std::int32_t>(m.read_u32_le(record + kUpTarget));
    if (up_target == cursor) {
        call.return_value(0U);
        return call.finish(15U);
    }
    const Remainder16 up_step = remainder16(cursor + 1);
    m.write_u16_le(record + kCursor, static_cast<std::uint16_t>(up_step.value));
    call.return_value(entry);
    return call.finish(20U + up_step.extra);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(00363870, 180U, 0x764c57dbaa2ab376ULL, "StepRingCursor", 28U, step_ring_cursor, EngineReturn::Integer)
