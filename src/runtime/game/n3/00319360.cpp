// 0x00319360 TakeCountdownStep (a0 = pointer to a halfword counter): returns 0 when the counter is already 0,
// otherwise counts it down by one and returns 1.
//
//   00319360  lhu $v0, 0($a0)
//   00319364  bne $v0, $zero, 0x00319374
//   0031936c  beq $zero, $zero, 0x00319380    counter == 0: v0 = 0 (delay slot), 7 instructions retired
//   00319374..0031937c                         v1 = counter - 1, v0 = 1, sh v1, 0($a0): 8 instructions retired

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

constexpr std::uint32_t kEmptyPath = 7U;
constexpr std::uint32_t kStepPath = 8U;

template <class Context>
n3::Run take_countdown_step(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const std::uint32_t counter_address = call.arg(0);
    const std::uint32_t counter = m.read_u16_le(counter_address);
    if (counter == 0U) {
        call.return_value(0U);
        return call.finish(kEmptyPath);
    }
    m.write_u16_le(counter_address, static_cast<std::uint16_t>(counter - 1U));
    call.return_value(1U);
    return call.finish(kStepPath);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(00319360, 40U, 0xc6c58dfa5dd1e4fdULL, "TakeCountdownStep", 8U, take_countdown_step,
               EngineReturn::Integer)
