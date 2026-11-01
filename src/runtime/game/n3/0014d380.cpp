// 0x0014d380 (a0 = object P, at least 680 bytes): InitSlotDefaults.
// Writes a block of default field values into P, and four VU0 quadwords. vadd.xyz against VF0 (0,0,0,1) zeroes
// the x, y and z lanes of VF4, VF3, VF2 and VF1 and keeps each W lane; sqc2 then stores VF4, VF3, VF2 and VF1 at
// P+544, P+560, P+576 and P+592. The halfword at P+472 is copied to P+528 and P+536. Returns nothing. One
// straight-line path of 35 retired instructions, delay slots included: no loops, no calls.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kSourceHalfword = 472U;  // lhu, read at 0x0014d3b4 and 0x0014d3c0
constexpr std::size_t kObjectBytes = 680U;      // every access ends at or before +680 (the last is sh at +678)

// Retired instruction count of the one path, 0x0014d380..0x0014d408.
constexpr std::uint32_t kCost = 35U;

// Declines the calls the original faults on: its sqc2 needs a 16-byte aligned P, and every store and load needs
// the object in guest memory. The original runs those and faults at the same instruction.
template <class Context>
std::uint32_t init_slot_defaults_cost(const Context& context) {
    const GuestAddress object = context.cpu.read_gpr_u32(4U);
    if ((object & 15U) != 0U || !context.memory.contains(object, kObjectBytes)) {
        return 0U;
    }
    return kCost;
}

template <class Context>
n3::Run init_slot_defaults(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);

    // The listing's order, VU0 adds included. The adds update the MAC and STATUS flags in that order, and each
    // sqc2 reads the register its add has just written.
    m.write_u32_le(object + 504U, 2U);                                       // sw a1,504(a0) with a1 = 2
    m.write_u32_le(object + 508U, 2U);                                       // sw a1,508(a0)
    ee_cop2::vadd_vector(cpu, 4U, 0U, 0U, n3::kMaskXyz, false);              // vadd.xyz vf4,vf0,vf0
    m.write_u32_le(object + 500U, 5U);                                       // sw v1,500(a0) with v1 = 5
    m.write_u8(object + 499U, 0U);                                           // sb zero,499(a0)
    m.write_u16_le(object + 532U, 0xffffU);                                  // sh a3,532(a0) with a3 = 0xffff
    m.write_u16_le(object + 534U, 0xffffU);                                  // sh a3,534(a0)
    ee_cop2::vadd_vector(cpu, 3U, 0U, 0U, n3::kMaskXyz, false);              // vadd.xyz vf3,vf0,vf0
    // The listing reads the halfword twice, before the store to +536 and before the store to +528. No store in
    // between touches +472, so one read gives both values.
    const std::uint16_t halfword = m.read_u16_le(object + kSourceHalfword);  // lhu a2,472(a0)
    ee_cop2::vadd_vector(cpu, 2U, 0U, 0U, n3::kMaskXyz, false);              // vadd.xyz vf2,vf0,vf0
    m.write_u16_le(object + 536U, halfword);                                 // sh a2,536(a0)
    ee_cop2::vadd_vector(cpu, 1U, 0U, 0U, n3::kMaskXyz, false);              // vadd.xyz vf1,vf0,vf0
    m.write_u16_le(object + 528U, halfword);                                 // sh a2,528(a0)
    m.write_u16_le(object + 676U, 0xffffU);                                  // sh a3,676(a0)
    m.write_u16_le(object + 678U, 0xffffU);                                  // sh a3,678(a0)
    n3::store_quad_any(m, object + 544U, cpu.read_vu0_vector(4U));               // sqc2 vf4,544(a0)
    n3::store_quad_any(m, object + 560U, cpu.read_vu0_vector(3U));               // sqc2 vf3,560(a0)
    n3::store_quad_any(m, object + 576U, cpu.read_vu0_vector(2U));               // sqc2 vf2,576(a0)
    m.write_u8(object + 608U, 255U);                                         // sb a1,608(a0) with a1 = 255
    m.write_u16_le(object + 612U, 0xffffU);                                  // sh a3,612(a0)
    m.write_u32_le(object + 620U, 16U);                                      // sw v1,620(a0) with v1 = 16
    m.write_u8(object + 609U, 255U);                                         // sb a1,609(a0)
    m.write_u16_le(object + 614U, 0xffffU);                                  // sh a3,614(a0)
    m.write_u32_le(object + 624U, 16U);                                      // sw v1,624(a0)
    m.write_u8(object + 610U, 255U);                                         // sb a1,610(a0)
    m.write_u16_le(object + 616U, 0xffffU);                                  // sh a3,616(a0)
    m.write_u32_le(object + 628U, 16U);                                      // sw v1,628(a0)
    // jr ra (0x0014d404): its delay slot still runs, so this last store comes before the return.
    n3::store_quad_any(m, object + 592U, cpu.read_vu0_vector(1U));               // sqc2 vf1,592(a0)

    // The temporaries end as the original leaves them.
    cpu.write_gpr_sign_extended_32(5U, 255U);                                // a1
    cpu.write_gpr_sign_extended_32(3U, 16U);                                 // v1
    cpu.write_gpr_sign_extended_32(6U, halfword);                            // a2
    cpu.write_gpr_sign_extended_32(7U, 0xffffU);                             // a3
    return call.finish(kCost);
}

} // namespace

extern constinit const EngineFunction n3_0014d380{0x0014d380U, 140U, 0x9c1410eb923c3694ULL, "InitSlotDefaults",
                                                  kCost, n3::live<&init_slot_defaults<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &init_slot_defaults_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(0014d380, init_slot_defaults, init_slot_defaults_cost)
