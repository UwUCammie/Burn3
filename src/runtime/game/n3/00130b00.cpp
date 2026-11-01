// 0x00130b00 (124 bytes, 31 instructions, straight-line code): ResetStateWithStubbedInits. a0 is the object S
// (at least 9744 bytes). It clears one word and six bytes of globals below $gp and sets S[0] = S[1] = 1 and
// S[2] = S[3] = 0 with the stores in listing order. The repeated stores to S+0..S+2 are kept, so the order is the
// original's. The three jal targets, 0x00275ed0, 0x002bece0 and 0x002f0360, are two-instruction leaves that touch no
// memory (0x00275ed0 only sets v0 = 1, and the original sets v0 = 1 again at 0x00130b30), so they are inlined here.
// The only path therefore retires 31 + 3 * 2 = 37 instructions. Returns v0 = 1.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// Distances below $gp of the stores (address = $gp - distance, 32-bit wrap; the same flags as 00130a00.cpp).
constexpr std::uint32_t kBelowWord = 24560U;  // sw $zero, -24560($gp)
constexpr std::uint32_t kBelowG0 = 24552U;
constexpr std::uint32_t kBelowG1 = 24548U;
constexpr std::uint32_t kBelowG2 = 24544U;
constexpr std::uint32_t kBelowG3 = 24540U;
constexpr std::uint32_t kBelowG4 = 24536U;
constexpr std::uint32_t kBelowG5 = 24532U;

// Retired original instructions on the only path: the 31 listing instructions plus the three 2-instruction stubs.
constexpr std::uint32_t kRetired = 37U;

template <class Context>
n3::Run reset_state_with_stubbed_inits(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);

    // 0x00130b10: sw $zero, -24560($gp)
    m.write_u32_le(gp - kBelowWord, 0U);
    // 0x00130b14..0x00130b28: the three stub calls, inlined; they write nothing.
    // 0x00130b2c: sb $zero, 9737($s0)
    m.write_u8(object + 9737U, 0U);
    // 0x00130b34: sw $zero, 9740($s0)
    m.write_u32_le(object + 9740U, 0U);
    // 0x00130b38: sb $zero, 2($s0)
    m.write_u8(object + 2U, 0U);
    // 0x00130b3c, 0x00130b40: sb $v0 (v0 = 1) to 0($s0) and 1($s0)
    m.write_u8(object + 0U, 1U);
    m.write_u8(object + 1U, 1U);
    // 0x00130b44, 0x00130b48: sb $zero to 2($s0) and 3($s0)
    m.write_u8(object + 2U, 0U);
    m.write_u8(object + 3U, 0U);
    // 0x00130b4c .. 0x00130b60: sb $zero to the six flags below $gp, in listing order.
    m.write_u8(gp - kBelowG0, 0U);
    m.write_u8(gp - kBelowG1, 0U);
    m.write_u8(gp - kBelowG2, 0U);
    m.write_u8(gp - kBelowG3, 0U);
    m.write_u8(gp - kBelowG4, 0U);
    m.write_u8(gp - kBelowG5, 0U);
    // 0x00130b64, 0x00130b68: sb $v0 (v0 = 1) to 0($s0) and 1($s0)
    m.write_u8(object + 0U, 1U);
    m.write_u8(object + 1U, 1U);

    call.return_value(1U);
    return call.finish(kRetired);
}

} // namespace

extern constinit const EngineFunction n3_00130b00{0x00130b00U, 124U, 0x912d3ac257227d16ULL, "ResetStateWithStubbedInits",
                                                  37U, n3::live<&reset_state_with_stubbed_inits<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00130b00, reset_state_with_stubbed_inits)
