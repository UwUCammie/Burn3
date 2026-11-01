// 0x00369120 (a0 = the object being reset): ResetSlotTableDefaults. A leaf that only stores constants; its one
// caller (0x00369484) ignores the result. Six 32-byte records at a0 + 32*i each get word +24 = 0, word +20 = -1.0f
// and words +28..+48 = -1.0f (the last one lands on the next record's +16, or on a0+208 for the sixth). Then the
// fixed fields at a0+0..a0+296 are stored and six words at a0+248+4*k = -1.0f. The three loops have constant trip
// counts (6, 6 and 6, each counted up from zero), so this is straight code: every call retires the same 379
// instructions. -1.0f is 0xbf800000, and $f0 holds those bits when the original returns, so the native sets $f0 too.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kMinusOne = 0xbf800000U;    // -1.0f bits (lui 0xbf80)
constexpr std::uint8_t kZeroByte = 0U;

// Retired instructions of every path, delay slots included: entry 3, then 6 * (outer body 5 + inner loop 6 * 7 +
// outer tail 4), the straight tail 26, the last loop 6 * 7, and the return 2.
constexpr std::uint32_t kRetired = 379U;

template <class Context>
n3::Run reset_slot_table_defaults(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);

    for (std::uint32_t i = 0U; i < 6U; ++i) {
        const GuestAddress record = object + 32U * i;
        m.write_u32_le(record + 24U, 0U);
        m.write_u32_le(record + 20U, kMinusOne);
        for (std::uint32_t j = 0U; j < 6U; ++j) {
            m.write_u32_le(record + 28U + 4U * j, kMinusOne);
        }
    }
    m.write_u32_le(object + 212U, 0U);
    m.write_u32_le(object + 216U, kMinusOne);
    m.write_u32_le(object + 220U, kMinusOne);
    m.write_u32_le(object + 224U, 7U);
    m.write_u32_le(object + 228U, 0U);
    m.write_u32_le(object + 232U, 0U);
    m.write_u8(object + 236U, kZeroByte);
    m.write_u32_le(object + 240U, 0U);
    m.write_u32_le(object + 244U, kMinusOne);
    m.write_u32_le(object + 0U, 0U);
    m.write_u8(object + 4U, kZeroByte);
    m.write_u8(object + 5U, kZeroByte);
    m.write_u8(object + 6U, kZeroByte);
    m.write_u8(object + 7U, kZeroByte);
    m.write_u8(object + 9U, kZeroByte);
    m.write_u32_le(object + 12U, 0U);
    m.write_u32_le(object + 272U, kMinusOne);
    m.write_u32_le(object + 276U, 0U);
    m.write_u32_le(object + 280U, kMinusOne);
    m.write_u32_le(object + 284U, 0U);
    m.write_u32_le(object + 288U, kMinusOne);
    m.write_u32_le(object + 292U, 0U);
    m.write_u32_le(object + 296U, 0U);
    for (std::uint32_t k = 0U; k < 6U; ++k) {
        m.write_u32_le(object + 248U + 4U * k, kMinusOne);
    }
    call.cpu().write_fpr_bits(0U, kMinusOne);
    return call.finish(kRetired);
}

} // namespace

extern constinit const EngineFunction n3_00369120{0x00369120U, 216U, 0xff110f3a45296745ULL, "ResetSlotTableDefaults",
                                                  kRetired, n3::live<&reset_slot_table_defaults<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00369120, reset_slot_table_defaults)
