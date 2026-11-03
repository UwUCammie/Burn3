// 0x00434dd0 (a0 = object O, f12 = delta time): StepFloatPairAndSetSlots. A cold-decline function: loop 2 calls
// 0x001d6080, which no recorded run reached. f3 = 3.0 * f12. Loop 1 runs exactly twice (p = 0, 1) over the float pairs at
// O+4p+36 (current) and O+4p+44 (target). A target at or below the current value lowers the current value by f3, and stores
// the float at gp-32076 (FLOOR) instead when the lowered value is below FLOOR. Any other target (above it, or unordered)
// raises the current value by f3, and stores the target instead when the raised value is not at or below the target.
// Loop 2's first pass then makes that call, and no input skips it (no branch precedes it), so call_cost declines every call
// and the original always runs. The body below is loop 1 and the path to the call. It never writes v0, v1 or f0.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::uint32_t kThreeBits = 0x40400000U;     // lui 0x4040 in the listing: the float 3.0
constexpr std::int32_t kFloorGpOffset = -32076;       // lwc1 -32076($gp): FLOOR
constexpr GuestAddress kCurrentOffset = 36U;          // the current value, at O+4p+36
constexpr GuestAddress kTargetOffset = 44U;           // the target value, at O+4p+44
constexpr std::uint32_t kPairCount = 2U;              // loop 1 runs exactly this many passes (slti 2)

// Retired instruction counts of the listing's blocks (brief.py's table), delay slots included.
constexpr std::uint32_t kPrologueCost = 13U;          // 0x00434dd0-0x00434e00
constexpr std::uint32_t kEqualPassCost = 11U;         // target == current: 0x00434e04-0x00434e20, 0x00434e24-0x00434e2c
constexpr std::uint32_t kLowerPassCost = 16U;         // lowered value not below FLOOR: as the equal pass, then 0x00434e30-0x00434e40
constexpr std::uint32_t kLowerFloorPassCost = 17U;    // lowered value below FLOOR: as kLowerPassCost, then 0x00434e44
constexpr std::uint32_t kRaiseKeptPassCost = 15U;     // raised value at or below target: 0x00434e04-0x00434e20, 0x00434fa8-0x00434fc0
constexpr std::uint32_t kRaiseClampedPassCost = 17U;  // raised value above target: as kRaiseKeptPassCost, then 0x00434fc4-0x00434fc8
constexpr std::uint32_t kCounterCost = 4U;            // 0x00434e48-0x00434e54: loop 1 counter, test and delay slot
constexpr std::uint32_t kLoop2EntryCost = 1U;         // 0x00434e58
constexpr std::uint32_t kLoop2CallCost = 46U;         // 0x00434e5c-0x00434f10: loop 2's first pass to the call and its delay slot

// Declared cost of the entry. Every call is declined, so the original reports the count and this only bounds the entry.
constexpr std::uint32_t kCeiling = 20000U;

// The retired count of the path to the call, and whether that path reaches the call (every input does).
struct StepResult {
    std::uint32_t retired{};
    bool declined{};
};

// Loop 1, then loop 2's first pass up to the call, on guest memory (the implementation) or on a DryMemory (call_cost).
// The floor is read before any store, as the original reads it at 0x00434e00.
template <class Memory>
StepResult step_pairs(Memory& m, const GuestAddress object, const std::uint32_t delta_bits,
                      const GuestAddress floor_address) {
    const std::uint32_t delta_step = fpu::mul(kThreeBits, delta_bits);
    const std::uint32_t floor_bits = m.read_u32_le(floor_address);
    std::uint32_t retired = kPrologueCost;
    for (std::uint32_t pass = 0U; pass < kPairCount; ++pass) {
        const GuestAddress current_address = object + 4U * pass + kCurrentOffset;
        const GuestAddress target_address = object + 4U * pass + kTargetOffset;
        const std::uint32_t current = m.read_u32_le(current_address);
        const std::uint32_t target = m.read_u32_le(target_address);
        if (fpu::le(target, current)) {
            if (fpu::lt(target, current)) {
                const std::uint32_t lowered = fpu::sub(current, delta_step);
                m.write_u32_le(current_address, lowered);
                if (fpu::lt(lowered, floor_bits)) {
                    m.write_u32_le(current_address, floor_bits);
                    retired += kLowerFloorPassCost;
                } else {
                    retired += kLowerPassCost;
                }
            } else {
                retired += kEqualPassCost;
            }
        } else {
            const std::uint32_t raised = fpu::add(current, delta_step);
            m.write_u32_le(current_address, raised);
            if (fpu::le(raised, target)) {
                retired += kRaiseKeptPassCost;
            } else {
                m.write_u32_le(current_address, target);
                retired += kRaiseClampedPassCost;
            }
        }
        retired += kCounterCost;
    }
    return {retired + kLoop2EntryCost + kLoop2CallCost, true};
}

// call_cost: the body on a DryMemory, with the call's a0 and f12 and FLOOR's address from gp. Every call reaches the cold
// call, so this is always 0 (declined) and the original runs the function.
template <class Context>
std::uint32_t step_float_pair_and_set_slots_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const StepResult result = step_pairs(
        dry, context.cpu.read_gpr_u32(4U), context.cpu.read_fpr_bits(12U),
        context.cpu.read_gpr_u32(n3::kGlobalPointer) + static_cast<std::uint32_t>(kFloorGpOffset));
    return result.declined ? 0U : result.retired;
}

// Never reached for a call: the dispatcher runs the implementation only for a call that call_cost accepts, and call_cost
// declines every call (the original runs it). It runs the same body, so its count is only the path's count to the call.
template <class Context>
n3::Run step_float_pair_and_set_slots(Context& context) {
    auto call = n3::guest_call(context);
    const StepResult result = step_pairs(
        call.memory(), call.arg(0), call.cpu().read_fpr_bits(12U),
        call.cpu().read_gpr_u32(n3::kGlobalPointer) + static_cast<std::uint32_t>(kFloorGpOffset));
    return call.finish(result.retired);
}

} // namespace

extern constinit const EngineFunction n3_00434dd0{0x00434dd0U, 532U, 0x4cac29c02558ec9dULL,
                                                  "StepFloatPairAndSetSlots", kCeiling, n3::live<&step_float_pair_and_set_slots<NativeFunctionContext>>,
                                                  EngineReturn::Nothing, {}, 1e-4, 1e-6, {},
                                                  &step_float_pair_and_set_slots_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00434dd0, step_float_pair_and_set_slots, step_float_pair_and_set_slots_cost)
