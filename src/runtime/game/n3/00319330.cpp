// 0x00319330 StepCounterBelowLimit (a0 = pointer to {u16 count, u16 limit}): when count + 1 is below the limit
// (signed compare of the 32-bit sums) the count is stored back incremented and 1 is returned, else 0.
// Both paths retire 10 instructions.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

constexpr std::uint32_t kLimitOffset = 2U;
constexpr std::uint32_t kRetired = 10U;

template <class Context>
n3::Run step_counter_below_limit(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const std::uint32_t counter_address = call.arg(0);
    const std::uint32_t next = static_cast<std::uint32_t>(m.read_u16_le(counter_address)) + 1U;
    const std::uint32_t limit = m.read_u16_le(counter_address + kLimitOffset);
    if (static_cast<std::int32_t>(next) < static_cast<std::int32_t>(limit)) {
        m.write_u16_le(counter_address, static_cast<std::uint16_t>(next));
        call.return_value(1U);
    } else {
        call.return_value(0U);
    }
    return call.finish(kRetired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(00319330, 48U, 0x4eba5c8899aabe70ULL, "StepCounterBelowLimit", 10U, step_counter_below_limit,
               EngineReturn::Integer)
