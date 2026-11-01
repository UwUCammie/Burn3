// 0x00131ca0 (a0 object): kind test on the word at a0 + 0x2DA48 (lui 0x3, addu, lw -9656).
// Returns 1 in v0 only when that word is non-zero and equals the constant tag 0x0051AEC0
// (lui 0x52, addiu -20800). The tag is compared by value and never dereferenced. Otherwise
// returns 0. No stores and no calls.
//
// Retired instructions: 8 when the word is zero (the first beq skips the tag compare),
// 12 when it is non-zero and not the tag, 13 when it is the tag.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// lui 0x3 and addu, then lw -9656: the word sits at a0 + (0x30000 - 9656).
constexpr GuestAddress kKindOffset = 0x00030000U - 9656U;
// lui 0x52 and addiu -20800: a constant tag, compared by value only.
constexpr std::uint32_t kKindTag = 0x00520000U - 20800U;

template <class Context>
n3::Run is_expected_kind_at(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const std::uint32_t kind = m.read_u32_le(object + kKindOffset);

    // lui, addu, lw, beq and its delay slot nop.
    std::uint32_t retired = 5U;
    std::uint32_t result = 0U;
    if (kind == 0U) {
        // The beq to the result-0 path skips the tag compare: daddu $v0, $zero, $zero.
        retired += 1U;
    } else {
        // lui, addiu, bne and its delay slot nop.
        retired += 4U;
        if (kind == kKindTag) {
            // Unconditional beq; addiu $v0, $zero, 1 sits in its delay slot.
            result = 1U;
            retired += 2U;
        } else {
            // daddu $v0, $zero, $zero.
            retired += 1U;
        }
    }
    // jr $ra and its delay slot nop: every path ends here.
    retired += 2U;
    call.return_value(result);
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_00131ca0{0x00131ca0U, 56U, 0x4067e479e0a7c051ULL, "IsExpectedKindAt", 13U,
                                                  n3::live<&is_expected_kind_at<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00131ca0, is_expected_kind_at)
