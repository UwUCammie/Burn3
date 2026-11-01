// 0x001478c0 (a0 = object, a1 and a2 = 16-bit counters): CyclicU16AheadTest, a leaf returning a boolean in v0.
// With d = (a1 & 0xffff) - (a2 & 0xffff) as a signed 32-bit value, S = the word at object+4, half = (u16 at S+12) >> 1
// and flag = u8 at S+14: flag clear gives d >= 0; flag set gives (d >= 0) ? d < half : d < -half. No loops, globals
// or stores. The temporaries (a0, a3, v1, at) are not reproduced.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kStatePointer = 4U;  // lw 4(a0)
constexpr GuestAddress kPeriod = 12U;       // lhu 12(state)
constexpr GuestAddress kFlag = 14U;         // lbu 14(state)

// Retired instruction counts of the paths, delay slots included.
constexpr std::uint32_t kFlagClearCost = 12U;      // lw..lbu, the srl, beq taken, slt, xori, jr and its nop
constexpr std::uint32_t kInsideWindowCost = 17U;   // flag set, d >= 0 and d < half
constexpr std::uint32_t kOutsideWindowCost = 22U;  // flag set, d >= half or d < 0 (the feasible maximum)

template <class Context>
n3::Run cyclic_u16_ahead_test(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const std::uint32_t ahead = call.arg(1) & 0xffffU;
    const std::uint32_t behind = call.arg(2) & 0xffffU;
    const auto d = static_cast<std::int32_t>(ahead - behind);
    const GuestAddress state = m.read_u32_le(object + kStatePointer);
    const auto half = static_cast<std::int32_t>(m.read_u16_le(state + kPeriod) >> 1U);
    const std::uint8_t flag = m.read_u8(state + kFlag);

    if (flag == 0U) {
        call.return_value(d >= 0 ? 1U : 0U);
        return call.finish(kFlagClearCost);
    }
    if (d >= 0) {
        if (d < half) {
            call.return_value(1U);
            return call.finish(kInsideWindowCost);
        }
        call.return_value(0U);
        return call.finish(kOutsideWindowCost);
    }
    call.return_value(d < -half ? 1U : 0U);
    return call.finish(kOutsideWindowCost);
}

} // namespace

// Declared cost 28 is the longest static path of the listing (brief.py); it includes a combination that no input
// reaches, so it is a safe bound above the feasible maximum of 22.
extern constinit const EngineFunction n3_001478c0{0x001478c0U, 144U, 0xfbe5ba43eafc5f64ULL, "CyclicU16AheadTest", 28U,
                                                  n3::live<&cyclic_u16_ahead_test<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(001478c0, cyclic_u16_ahead_test)
