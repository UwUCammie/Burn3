// 0x00140980 (a0 = object of at least 1112 bytes, a1 = a word): InitLookupTables. Stores a1 at +1108, then fills
// three ranges in a loop that always runs 256 times: the counter t0 goes 0..255 and nothing in the loop reads guest
// data, so the trip count is fixed and the retired count is the same on every path (no call_cost). Byte +320+i = 0xff,
// halfword +576+2i = 32767 (computed once: f1, f2 and f3 are not written in the loop), byte +64+i = 0. Then vf1 becomes
// (0, 0, 0, vf1.w) by four VADD.xyz, each followed by the quadword store of vf1 the original makes at that point, and
// the words and bytes at +1088..+1104 are cleared (byte +1091 is not written). Returns v0 = 1 (EngineReturn::Integer).

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <algorithm>
#include <bit>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint32_t kEntries = 256U;                    // the loop at 0x001409ac: t0 = 0..255, fixed
constexpr std::uint32_t kRetired = 4124U;                   // 11 + 256 * 16 + 17 on every path
constexpr std::uint32_t kLimitBits = 0x43fa0000U;           // lui 0x43fa: f2 = 500.0
constexpr std::uint32_t kNegativeLimitBits = 0xc3fa0000U;   // lui 0xc3fa: f3 = -500.0
constexpr std::int32_t kScale = 32767;                      // addiu 32767: f1 = 32767.0 (cvt.s.w)
constexpr std::uint8_t kMarkByte = 0xffU;                   // addiu a2 = 255: the byte fill

// MAX.S on raw bits, ordered as guest::ee_cop1::max_s orders them.
std::uint32_t max_bits(const std::uint32_t left, const std::uint32_t right) {
    const auto signed_left = std::bit_cast<std::int32_t>(left);
    const auto signed_right = std::bit_cast<std::int32_t>(right);
    return std::bit_cast<std::uint32_t>(signed_left < 0 && signed_right < 0 ? std::min(signed_left, signed_right)
                                                                            : std::max(signed_left, signed_right));
}

// MIN.S on raw bits, ordered as guest::ee_cop1::min_s orders them.
std::uint32_t min_bits(const std::uint32_t left, const std::uint32_t right) {
    const auto signed_left = std::bit_cast<std::int32_t>(left);
    const auto signed_right = std::bit_cast<std::int32_t>(right);
    return std::bit_cast<std::uint32_t>(signed_left < 0 && signed_right < 0 ? std::max(signed_left, signed_right)
                                                                            : std::min(signed_left, signed_right));
}

// The halfword every iteration stores, from the loop's own operations on f1 = 32767.0, f2 = 500.0 and f3 = -500.0.
// Those registers are not written in the loop, so the value is the same on every iteration and is computed once.
std::uint32_t lookup_halfword() {
    const std::uint32_t scale = fpu::from_int(kScale);  // cvt.s.w f1,f0
    // max.s f0,f2,f3 then min.s f0,f0,f2
    const std::uint32_t clamped = min_bits(max_bits(kLimitBits, kNegativeLimitBits), kLimitBits);
    const std::uint32_t product = fpu::mul(scale, clamped);        // mul.s f0,f1,f0
    const std::uint32_t quotient = fpu::div(product, kLimitBits);  // div.s f0,f0,f2
    return static_cast<std::uint32_t>(fpu::to_int(quotient));      // cvt.w.s f0,f0
}

template <class Context>
n3::Run init_lookup_tables(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    auto& cpu = call.cpu();
    const GuestAddress object = call.arg(0);
    m.write_u32_le(object + 1108U, call.arg(1));  // sw a1,1108(a0)
    const auto halfword = static_cast<std::uint16_t>(lookup_halfword());
    for (std::uint32_t i = 0U; i < kEntries; ++i) {
        m.write_u8(object + 320U + i, kMarkByte);          // sb a2,320(a3)
        m.write_u16_le(object + 576U + 2U * i, halfword);  // sh a1,576(v1)
        m.write_u8(object + 64U + i, 0U);                  // sb zero,64(a3): the loop branch's delay slot
    }
    // vadd.xyz vf1,vf0,vf0 makes xyz zero (vf0 is (0, 0, 0, 1)) and keeps w. Each sqc2 stores vf1 as it is then.
    ee_cop2::vadd_vector(cpu, 1U, 0U, 0U, n3::kMaskXyz, false);
    n3::store_quad_any(m, object + 16U, cpu.read_vu0_vector(1U));  // sqc2 vf1,16(a0)
    ee_cop2::vadd_vector(cpu, 1U, 0U, 0U, n3::kMaskXyz, false);
    n3::store_quad_any(m, object + 0U, cpu.read_vu0_vector(1U));   // sqc2 vf1,0(a0)
    ee_cop2::vadd_vector(cpu, 1U, 0U, 0U, n3::kMaskXyz, false);
    n3::store_quad_any(m, object + 32U, cpu.read_vu0_vector(1U));  // sqc2 vf1,32(a0)
    ee_cop2::vadd_vector(cpu, 1U, 0U, 0U, n3::kMaskXyz, false);
    m.write_u32_le(object + 1092U, 0U);                        // sw zero,1092(a0)
    m.write_u32_le(object + 1096U, 0U);                        // sw zero,1096(a0)
    m.write_u32_le(object + 1100U, 0U);                        // sw zero,1100(a0)
    n3::store_quad_any(m, object + 48U, cpu.read_vu0_vector(1U));  // sqc2 vf1,48(a0)
    m.write_u8(object + 1088U, 0U);                            // sb zero,1088(a0)
    m.write_u8(object + 1089U, 0U);                            // sb zero,1089(a0)
    m.write_u8(object + 1090U, 0U);                            // sb zero,1090(a0)
    m.write_u32_le(object + 1104U, 0U);                        // sw zero,1104(a0): the jr's delay slot
    call.return_value(1U);                                     // addiu v0,zero,1
    return call.finish(kRetired);
}

} // namespace

extern constinit const EngineFunction n3_00140980{0x00140980U, 176U, 0x348cc96631d8b1fdULL, "InitLookupTables",
                                                  kRetired, n3::live<&init_lookup_tables<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00140980, init_lookup_tables)
