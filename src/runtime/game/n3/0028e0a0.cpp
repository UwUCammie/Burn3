// 0x0028e0a0 (a0 = object): IsObjectTypeInSet. The first byte of the object is its type id, zero-extended. The id is
// compared, one compare per step, with 17, 13, 12, 8, 6, 3, 2 and 1 in that order; v0 is 1 when it equals one of them,
// else 0. Nothing else is read or written (the original's v1 is only a scratch copy of the byte).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

// The type ids, in the order the original tests them.
constexpr std::uint8_t kTypeIds[] = {17U, 13U, 12U, 8U, 6U, 3U, 2U, 1U};

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kLoadCost = 1U;   // lbu
constexpr std::uint32_t kStepCost = 3U;   // addiu, beq and its delay-slot nop, for each id tested
constexpr std::uint32_t kMatchCost = 3U;  // addiu v0,1 at the match, then jr and its delay-slot nop
constexpr std::uint32_t kMissCost = 4U;   // beq and its delay-slot daddu (to the return), then jr and its nop

template <class Context>
n3::Run is_object_type_in_set(Context& context) {
    auto call = n3::guest_call(context);
    const std::uint8_t type = call.memory().read_u8(call.arg(0));
    std::uint32_t retired = kLoadCost;
    for (const std::uint8_t id : kTypeIds) {
        retired += kStepCost;
        if (type == id) {
            call.return_value(1U);
            return call.finish(retired + kMatchCost);
        }
    }
    call.return_value(0U);
    return call.finish(retired + kMissCost);
}

} // namespace

extern constinit const EngineFunction n3_0028e0a0{0x0028e0a0U, 120U, 0x4a104eb0f89224d1ULL, "IsObjectTypeInSet", 29U,
                                                  n3::live<&is_object_type_in_set<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(0028e0a0, is_object_type_in_set)
