// 0x0015c9a0 (a0 = record R, a1 = a 16-bit value): InitRecordDefaults. Sets the defaults of a
// record (the stores reach +199) and returns 1. The halfword at +0 takes the low 16 bits of a1 and the one at +2
// is 0xffff. The float bits at gp-31324 fill +28..+60 (nine words) and the word at 0x0064d37c
// fills +68. Leaf: no calls.
//
// The three loops run a fixed 8, 4 and 3 times (their tests look at a counter only), so every path
// retires the same 153 instructions: 10 + 8 x 7 + 35 + 4 x 7 + 1 + 3 x 7 + 2. The loads and stores
// keep the original's order.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kGpFloatOffset = 31324U;   // lwc1 f0, -31324($gp)
constexpr GuestAddress kGlobalWord = 0x0064d37cU;  // lui 0x65, lw -11396
constexpr std::uint32_t kRetired = 153U;

// `count` consecutive words from `first` upwards, each set to `bits`, stored in address order.
template <class Memory>
void fill_words(Memory& m, const GuestAddress first, const std::uint32_t count, const std::uint32_t bits) {
    for (std::uint32_t i = 0U; i < count; ++i) {
        m.write_u32_le(first + 4U * i, bits);
    }
}

template <class Context>
n3::Run init_record_defaults(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    const std::uint32_t value = call.arg(1);

    m.write_u16_le(record + 0U, static_cast<std::uint16_t>(value));
    m.write_u16_le(record + 2U, static_cast<std::uint16_t>(0xffffU));
    fill_words(m, record + 8U, 3U, 0U);
    m.write_u32_le(record + 20U, 0xffffffffU);
    m.write_u32_le(record + 24U, 0U);

    const std::uint32_t float_bits = m.read_u32_le(call.cpu().read_gpr_u32(n3::kGlobalPointer) - kGpFloatOffset);
    call.cpu().write_fpr_bits(0U, float_bits);
    fill_words(m, record + 28U, 8U, float_bits);
    m.write_u32_le(record + 60U, float_bits);
    m.write_u32_le(record + 64U, 0xffffffffU);

    const std::uint32_t global_word = m.read_u32_le(kGlobalWord);
    m.write_u32_le(record + 68U, global_word);
    m.write_u32_le(record + 72U, 0xbf800000U);
    fill_words(m, record + 76U, 9U, 0U);
    fill_words(m, record + 112U, 3U, 4U);
    fill_words(m, record + 124U, 2U, 0U);
    fill_words(m, record + 132U, 2U, 0xffffffffU);
    fill_words(m, record + 140U, 2U, 0U);
    m.write_u8(record + 148U, 0U);
    m.write_u8(record + 149U, 0U);
    fill_words(m, record + 152U, 2U, 0U);
    m.write_u8(record + 160U, 0U);
    m.write_u8(record + 161U, 0U);
    m.write_u8(record + 162U, 0U);
    fill_words(m, record + 172U, 4U, 0U);
    fill_words(m, record + 188U, 3U, 0U);

    call.return_value(1U);
    return call.finish(kRetired);
}

} // namespace

extern constinit const EngineFunction n3_0015c9a0{0x0015c9a0U, 276U, 0x6b6be9808720c4fcULL, "InitRecordDefaults",
                                                  kRetired, n3::live<&init_record_defaults<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(0015c9a0, init_record_defaults)
