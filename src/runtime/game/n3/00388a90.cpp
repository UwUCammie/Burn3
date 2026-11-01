// 0x00388a90 InitialiseContactRecord (a0 = contact record): resets the record that the collision response code
// (0x00278020, 0x002789c0) fills in. Stores, as offsets from a0:
//   +0, +4, +8        0x00800000 (FLT_MIN)
//   +12, +16, +20, +24  0
//   +28, +32, +36     0xbf800000 (-1.0f)
//   +40               0x00800000 (FLT_MIN)
//   +52               0
//   +56               0x42480000 (50.0f)
//   +60               0x41700000 (15.0f)
//   +64               0xffffffff
//   +68, +69, +70     bytes 0
// Words +44 and +48 are not written. The function is straight-line code (its only branch is the final jr, whose
// delay slot is the last store), so every call retires 24 instructions and returns nothing. It leaves the
// temporaries as the listing does (t0, a1, a2, a3, v1).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFloatMin = 0x00800000U;  // lui $t0, 0x0080
constexpr std::uint32_t kMinusOne = 0xbf800000U;  // lui $a3, 0xbf80
constexpr std::uint32_t kFifty = 0x42480000U;     // lui $a2, 0x4248
constexpr std::uint32_t kFifteen = 0x41700000U;   // lui $a1, 0x4170
// Instructions on the only path: 22 before the jr, plus the jr and its delay slot.
constexpr std::uint32_t kRetired = 24U;

template <class Context>
n3::Run initialise_contact_record(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    m.write_u32_le(record + 0U, kFloatMin);
    m.write_u32_le(record + 4U, kFloatMin);
    m.write_u32_le(record + 8U, kFloatMin);
    m.write_u32_le(record + 12U, 0U);
    m.write_u32_le(record + 16U, 0U);
    m.write_u32_le(record + 20U, 0U);
    m.write_u32_le(record + 24U, 0U);
    m.write_u32_le(record + 28U, kMinusOne);
    m.write_u32_le(record + 32U, kMinusOne);
    m.write_u32_le(record + 36U, kMinusOne);
    m.write_u32_le(record + 40U, kFloatMin);
    m.write_u32_le(record + 52U, 0U);
    m.write_u32_le(record + 56U, kFifty);
    m.write_u32_le(record + 60U, kFifteen);
    m.write_u32_le(record + 64U, 0xffffffffU);
    m.write_u8(record + 68U, std::uint8_t{0U});
    m.write_u8(record + 69U, std::uint8_t{0U});
    m.write_u8(record + 70U, std::uint8_t{0U});
    // The temporaries the listing leaves behind: lui sign-extends its 32-bit result to 64 bits, and
    // addiu $v1, $zero, -1 is all ones.
    call.cpu().write_gpr_sign_extended_32(8U, kFloatMin);    // t0
    call.cpu().write_gpr_sign_extended_32(7U, kMinusOne);    // a3
    call.cpu().write_gpr_sign_extended_32(6U, kFifty);       // a2
    call.cpu().write_gpr_sign_extended_32(5U, kFifteen);     // a1
    call.cpu().write_gpr_sign_extended_32(3U, 0xffffffffU);  // v1
    return call.finish(kRetired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(00388a90, 96U, 0xe4230eebd68873f9ULL, "InitialiseContactRecord", 24U, initialise_contact_record,
               EngineReturn::Nothing)
