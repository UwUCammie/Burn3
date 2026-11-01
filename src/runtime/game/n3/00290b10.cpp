// 0x00290b10 (a0 = pad button mask): PadButtonMaskToClass. The low 16 bits of a0 are compared, in
// this order, with the fourteen single pad button bits 0x1000 .. 0x0001. A matching bit gives its
// class code in v0; any other value, including 0 and multi-bit masks, gives 0. The class code is the
// constant in the delay slot of that bit's beq (it executes whether or not the branch is taken), not
// the constant loaded for the next compare. The masked value left in v1 is a temporary and is not
// reproduced.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

// One compared bit, in the original's compare order. `retired` is the instruction count of the path
// that matches it, from the listing: 8 for the first compare (andi, addiu, beq and its delay slot,
// the jump block and the return), 3 more for each later compare (addiu, beq and its delay slot), and
// 2 fewer for 0x0001, whose beq goes straight to the return. No match retires 47.
struct ButtonClass {
    std::uint32_t button;
    std::uint32_t code;
    std::uint32_t retired;
};

constexpr ButtonClass kButtonClasses[] = {
    {0x1000U, 4U, 8U},   {0x2000U, 4U, 11U},  {0x0800U, 4U, 14U},  {0x0400U, 4U, 17U},
    {0x0200U, 16U, 20U}, {0x0100U, 16U, 23U}, {0x0080U, 8U, 26U},  {0x0040U, 8U, 29U},
    {0x0020U, 4U, 32U},  {0x0010U, 4U, 35U},  {0x0008U, 2U, 38U},  {0x0004U, 2U, 41U},
    {0x0002U, 1U, 44U},  {0x0001U, 1U, 45U},
};

constexpr std::uint32_t kNoMatchCost = 47U;

template <class Context>
n3::Run pad_button_mask_to_class(Context& context) {
    auto call = n3::guest_call(context);
    const std::uint32_t mask = call.arg(0) & 0xffffU;
    for (const ButtonClass& entry : kButtonClasses) {
        if (mask == entry.button) {
            call.return_value(entry.code);
            return call.finish(entry.retired);
        }
    }
    call.return_value(0U);
    return call.finish(kNoMatchCost);
}

} // namespace

extern constinit const EngineFunction n3_00290b10{0x00290b10U, 348U, 0xe34328cb6f501f28ULL, "PadButtonMaskToClass",
                                                  47U, n3::live<&pad_button_mask_to_class<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00290b10, pad_button_mask_to_class)
