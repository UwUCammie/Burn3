// 0x00299550 (a0 = a 32-bit flag mask): LowestSetBitIndex. Returns v0 = the index (0..31) of the lowest set bit of
// a0, or 32 when none of the 32 tested bits is set. The original counts v0 = 0, 1, 2, ... and tests
// (a0 & (1 << v0)) != 0 on all 64 bits of a0: sllv leaves a 32-bit result that the R5900 sign-extends, so index 31
// tests 0xFFFFFFFF80000000 and a0 bits 32..63 can satisfy that test. The sllv in the delay slot of the loop branch
// runs on every pass, including the exit pass, but it only sets v1, a temporary. A leaf: no calls, no memory access,
// no stores. Its retired count depends on a0, so call_cost resolves it from the register.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 3U;   // 0x00299550..0x00299558: daddu, beq to the loop head, addiu in its slot
constexpr std::uint32_t kHeadCost = 4U;    // 0x00299570..0x0029957c: andi, slti, bne to the test, sllv in its slot
constexpr std::uint32_t kTestCost = 3U;    // 0x0029955c..0x00299564: and, bne to the return, nop
constexpr std::uint32_t kStepCost = 2U;    // 0x00299568..0x0029956c: addiu, andi (the tested bit is clear)
constexpr std::uint32_t kReturnCost = 2U;  // 0x00299580..0x00299584: jr $ra and its nop
constexpr std::uint32_t kBitCount = 32U;   // the head's slti exits once v0 reaches 32

struct LowestSetBit {
    std::uint32_t index;    // v0
    std::uint32_t retired;  // original instructions retired for this a0
};

// sllv's 32-bit result is sign-extended into the 64-bit register that the and tests.
std::uint64_t sign_extended(const std::uint32_t value) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
}

LowestSetBit lowest_set_bit(const std::uint64_t flags) {
    std::uint32_t index = 0U;
    std::uint32_t retired = kEntryCost;
    for (;;) {
        retired += kHeadCost;
        if (index >= kBitCount) {
            break;
        }
        retired += kTestCost;
        if ((flags & sign_extended(std::uint32_t{1U} << index)) != 0U) {
            break;
        }
        retired += kStepCost;
        ++index;
    }
    return LowestSetBit{index, retired + kReturnCost};
}

template <class Context>
std::uint32_t lowest_set_bit_cost(const Context& context) {
    return lowest_set_bit(context.cpu.read_gpr_u64(4U)).retired;
}

template <class Context>
n3::Run lowest_set_bit_index(Context& context) {
    auto call = n3::guest_call(context);
    const LowestSetBit result = lowest_set_bit(call.arg64(0U));
    call.return_value(result.index);
    return call.finish(result.retired);
}

} // namespace

extern constinit const EngineFunction n3_00299550{0x00299550U, 56U, 0xf13492215fe4923cULL, "LowestSetBitIndex", 297U,
                                                  n3::live<&lowest_set_bit_index<NativeFunctionContext>>, EngineReturn::Integer, {}, 1e-4, 1e-6, {},
                                                  &lowest_set_bit_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00299550, lowest_set_bit_index, lowest_set_bit_cost)
