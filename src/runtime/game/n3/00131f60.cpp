// 0x00131f60 (a0 object): NotBothFlagsSet. Returns in v0 1 unless both flags are set, that is
// v0 = !(flagA && flagB). flagA is the byte at a0 + (0x30000 - 9580) (lui 0x3, addu, lbu -9580).
// flagB is the global byte at 0x004eb1e0 (lui 0x4f, lbu -20000), read only when flagA is non-zero.
// Returns 0 or 1 in v0. No stores and no calls.
//
// Retired instructions: 8 when flagA is zero (the beq skips the global read), 11 when flagA is non-zero.
// Each count includes the delay slots (the nop after the beq, the xori after jr).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// lui 0x3 and addu, then lbu -9580: the flag sits at a0 + (0x30000 - 9580).
constexpr GuestAddress kFlagAOffset = 0x00030000U - 9580U;
// lui 0x4f and lbu -20000: a global byte at an absolute address.
constexpr GuestAddress kFlagBAddress = 0x004F0000U - 20000U;

template <class Context>
n3::Run not_both_flags_set(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);

    // lui, addu, lbu, sltu, beq and its delay slot nop.
    std::uint32_t retired = 6U;
    std::uint32_t result = 1U;
    if (m.read_u8(object + kFlagAOffset) != 0U) {
        // lui, lbu, sltu: the global is read only on this path.
        retired += 3U;
        result = m.read_u8(kFlagBAddress) != 0U ? 0U : 1U;
    }
    // jr $ra and its delay slot xori, which flips the 0/1 that sltu left in v0.
    retired += 2U;
    call.return_value(result);
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_00131f60{0x00131f60U, 44U, 0xff2b38a828adb2f5ULL, "NotBothFlagsSet", 11U,
                                                  n3::live<&not_both_flags_set<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00131f60, not_both_flags_set)
