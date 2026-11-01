// 0x002540e0 (a0 = tuning block B, at least 464 bytes): InitTuningConstantsBlock. The original first calls the
// 20-instruction leaf 0x002524e0 with the same B (its jal is unconditional; the leaf only reads two $gp words and
// writes B), then overwrites B+184..B+460 with fixed constants and raw words of float globals read through $gp.
// Both parts are reproduced here in the listing's order, and the count includes the leaf's 20 instructions. The
// floats the original builds with CVT.S.W are exact integers, so fpu::from_int gives their bits. Returns nothing:
// the temporaries (v1, f0 and the rest) are not reproduced, as callers do not read them.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

// Retired original instructions on the only path: 151 of this function (the jal, its delay slot and the epilogue
// included) plus the 20 of 0x002524e0 that the jal runs.
constexpr std::uint32_t kRetiredCount = 171U;

template <class Context>
n3::Run init_tuning_constants_block(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress block = call.arg(0);
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    // The raw word of the global at $gp + offset, read where the listing reads it.
    const auto global = [&m, gp](const std::int32_t offset) {
        return m.read_u32_le(gp + static_cast<GuestAddress>(offset));
    };
    const std::uint32_t int_60000 = fpu::from_int(60000);
    const std::uint32_t int_5000 = fpu::from_int(5000);
    const std::uint32_t int_6800 = fpu::from_int(6800);
    const std::uint32_t int_4200 = fpu::from_int(4200);
    const std::uint32_t int_630 = fpu::from_int(630);
    const std::uint32_t int_1100 = fpu::from_int(1100);
    const std::uint32_t int_minus_5500 = fpu::from_int(-5500);
    const std::uint32_t int_6500 = fpu::from_int(6500);

    // The leaf 0x002524e0 on B: sb B+0, then B+136..B+168 in its own order.
    m.write_u8(block, std::uint8_t{0U});
    const std::uint32_t leaf_g1 = global(-30832);
    m.write_u32_le(block + 136U, 0x447a0000U);
    m.write_u32_le(block + 152U, leaf_g1);
    const std::uint32_t leaf_g2 = global(-30264);
    m.write_u32_le(block + 144U, int_60000);
    m.write_u32_le(block + 148U, int_5000);
    m.write_u32_le(block + 140U, leaf_g2);
    m.write_u32_le(block + 164U, leaf_g1);
    m.write_u32_le(block + 156U, int_60000);
    m.write_u32_le(block + 160U, int_5000);
    m.write_u32_le(block + 168U, leaf_g2);

    // The function's own stores, 0x0025413c..0x00254308, in listing order.
    m.write_u32_le(block + 184U, 0x447a0000U);
    const std::uint32_t g1 = global(-30832);
    m.write_u32_le(block + 200U, g1);
    m.write_u32_le(block + 192U, int_60000);
    const std::uint32_t g2 = global(-30264);
    m.write_u32_le(block + 196U, int_5000);
    const std::uint32_t g_31540 = global(-31540);
    m.write_u32_le(block + 188U, g2);
    m.write_u32_le(block + 212U, g1);
    m.write_u32_le(block + 204U, int_60000);
    m.write_u32_le(block + 208U, int_5000);
    m.write_u32_le(block + 216U, g2);
    m.write_u32_le(block + 224U, 0xbf800000U);
    m.write_u32_le(block + 228U, 0U);
    const std::uint32_t g_30016 = global(-30016);
    m.write_u32_le(block + 232U, 0x40a00000U);
    m.write_u32_le(block + 236U, 0x40a00000U);
    m.write_u32_le(block + 240U, 0x40a00000U);
    m.write_u32_le(block + 244U, 0x40a00000U);
    m.write_u32_le(block + 248U, 0x40a00000U);
    m.write_u32_le(block + 252U, 0x40a00000U);
    m.write_u32_le(block + 256U, 0x40a00000U);
    const std::uint32_t g_30012 = global(-30012);
    m.write_u32_le(block + 260U, 0x447a0000U);
    const std::uint32_t g_30008 = global(-30008);
    m.write_u32_le(block + 264U, int_6800);
    m.write_u32_le(block + 268U, int_4200);
    m.write_u32_le(block + 280U, int_6800);
    m.write_u32_le(block + 284U, int_630);
    m.write_u32_le(block + 288U, int_5000);
    m.write_u32_le(block + 292U, int_6500);
    m.write_u32_le(block + 296U, 0x40000000U);
    const std::uint32_t g_30004 = global(-30004);
    m.write_u32_le(block + 300U, 0x3f800000U);
    const std::uint32_t g_30000 = global(-30000);
    m.write_u32_le(block + 304U, g_31540);
    m.write_u32_le(block + 308U, 0U);
    const std::uint32_t g_31432 = global(-31432);
    m.write_u32_le(block + 312U, g_30016);
    m.write_u32_le(block + 316U, g_30012);
    m.write_u32_le(block + 320U, g_30008);
    m.write_u32_le(block + 324U, g_30004);
    m.write_u32_le(block + 440U, 0U);
    const std::uint32_t g_29996 = global(-29996);
    m.write_u32_le(block + 328U, 0x41280000U);
    const std::uint32_t g_30736 = global(-30736);
    m.write_u32_le(block + 336U, 0x41b80000U);
    const std::uint32_t g_29992 = global(-29992);
    m.write_u32_le(block + 332U, 0x41600000U);
    const std::uint32_t g_32704 = global(-32704);
    m.write_u32_le(block + 340U, 0x41400000U);
    const std::uint32_t g_31792 = global(-31792);
    m.write_u32_le(block + 344U, g_30000);
    m.write_u32_le(block + 400U, 0x40000000U);
    const std::uint32_t g_30484 = global(-30484);
    m.write_u32_le(block + 352U, g_31432);
    m.write_u32_le(block + 356U, 0x3f400000U);
    m.write_u32_le(block + 360U, 0x44480000U);
    m.write_u32_le(block + 364U, int_1100);
    m.write_u32_le(block + 368U, g_29996);
    m.write_u32_le(block + 372U, g_30736);
    m.write_u32_le(block + 404U, 0x42820000U);
    m.write_u32_le(block + 408U, 0x3f000000U);
    m.write_u32_le(block + 416U, 0x42340000U);
    m.write_u32_le(block + 380U, 0x420c0000U);
    m.write_u32_le(block + 384U, 0x42480000U);
    m.write_u32_le(block + 388U, 0x3f800000U);
    m.write_u32_le(block + 392U, g_29992);
    m.write_u32_le(block + 376U, g_32704);
    m.write_u32_le(block + 396U, 0x40800000U);
    m.write_u32_le(block + 348U, 0x40000000U);
    m.write_u32_le(block + 412U, int_minus_5500);
    m.write_u32_le(block + 420U, 0x430d0000U);
    m.write_u32_le(block + 424U, g_31792);
    m.write_u32_le(block + 428U, 0U);
    m.write_u32_le(block + 436U, 0x40a00000U);
    m.write_u32_le(block + 432U, g_31792);
    m.write_u32_le(block + 444U, g_30484);
    m.write_u32_le(block + 448U, 0x3f800000U);
    m.write_u32_le(block + 452U, 0x41c00000U);
    m.write_u32_le(block + 456U, 0x42480000U);
    m.write_u32_le(block + 460U, 0x3f000000U);

    return call.finish(kRetiredCount);
}

} // namespace

extern constinit const EngineFunction n3_002540e0{0x002540e0U, 604U, 0xc7e8a795aa348f79ULL,
                                                  "InitTuningConstantsBlock", kRetiredCount,
                                                  n3::live<&init_tuning_constants_block<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(002540e0, init_tuning_constants_block)
