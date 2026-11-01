// 0x0035f020 (a0 = pointer to a NUL-terminated array of 16-bit units, a1 = the character): FindWideChar,
// wcschr for 16-bit strings. v1 = a1 & 0xffff. Each unit is loaded and the loop stops at a zero unit before
// comparing it, so a character of 0 is never found. Returns a0 advanced to the first unit equal to the
// character, or 0. Reads only.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 2U;      // beq (always taken) and its andi delay slot
constexpr std::uint32_t kLoadCost = 5U;       // lhu, nop, nop, bne and its nop: one unit read and tested
constexpr std::uint32_t kCompareCost = 2U;    // beq and its nop: the unit against the character
constexpr std::uint32_t kStepCost = 1U;       // addiu a0, a0, 2 (the unit did not match)
constexpr std::uint32_t kEndCost = 4U;        // zero unit: beq and its daddu (v0 = 0), then jr and its nop
constexpr std::uint32_t kMatchCost = 3U;      // match: daddu v0, a0, zero, then jr and its nop

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

struct Scan {
    std::uint32_t retired;  // retired instructions on the path taken (above kCeiling once it is passed)
    std::uint32_t steps;    // units passed over before the stop unit: the match index when found
    bool found;             // the stop unit equals the character
};

// The scan on guest memory (the implementation) or on a DryMemory (call_cost). The ceiling test sits in
// the loop, so an unterminated string ends the dry run after at most kCeiling / 8 units.
template <class Memory>
Scan scan_units(const Memory& m, const GuestAddress start, const std::uint32_t character) {
    const std::uint32_t target = character & 0xffffU;
    GuestAddress address = start;
    std::uint32_t retired = kEntryCost;
    std::uint32_t steps = 0U;
    for (;;) {
        retired += kLoadCost;
        const std::uint32_t unit = m.read_u16_le(address);
        if (unit == 0U) {
            return Scan{retired + kEndCost, steps, false};
        }
        retired += kCompareCost;
        if (unit == target) {
            return Scan{retired + kMatchCost, steps, true};
        }
        retired += kStepCost;
        address += 2U;
        ++steps;
        if (retired > kCeiling) {
            return Scan{retired, steps, false};
        }
    }
}

// call_cost: the same scan on a DryMemory, with this call's a0 and a1.
template <class Context>
std::uint32_t find_wide_char_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const Scan scan = scan_units(dry, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u32(5U));
    return scan.retired <= kCeiling ? scan.retired : 0U;
}

template <class Context>
n3::Run find_wide_char(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress start = call.arg(0);
    const Scan scan = scan_units(call.memory(), start, call.arg(1));
    if (!scan.found) {
        call.return_value64(0U);
    } else if (scan.steps == 0U) {
        // The original copies the whole a0 register (daddu v0, a0, zero).
        call.return_value64(call.arg64(0));
    } else {
        // a0 was moved by addiu, whose 32-bit result is sign-extended into the register.
        call.return_value(start + 2U * scan.steps);
    }
    return call.finish(scan.retired);
}

} // namespace

extern constinit const EngineFunction n3_0035f020{0x0035f020U, 60U, 0x028a1a0369a8c22dULL, "FindWideChar", kCeiling,
                                                  n3::live<&find_wide_char<NativeFunctionContext>>, EngineReturn::Integer, {}, 1e-4, 1e-6, {},
                                                  &find_wide_char_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(0035f020, find_wide_char, find_wide_char_cost)
