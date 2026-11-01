// 0x0014a130 (a0 = object O, 16-byte aligned with at least 344 bytes; a1 = a word): ClearSixSlotVectorsKeepW.
// Zeroes the xyz lanes of vf1 and vf2 and keeps their w lanes. Each VADD.xyz vf1,vf0,vf0 (vf0 is (0, 0, 0, 1), so
// x, y and z become +0) runs on the real VU0 state right before the quadword store the original makes after it, so
// the w lane that is stored is whatever the register held on entry. The stores, in the original's order: vf1 at
// +224, +240, +192, +208, +272, +304, +256, +288, +320, word a1 at +340, bytes 0 at +337 and +336, then vf2 at +16*i
// and vf1 at +96+16*i for i = 0..5. The loop at 0x0014a190 always runs six passes (a1 counts 0..5 against slti 6)
// and reads no guest data, so it is straight code with a fixed count and no call_cost. Returns v0 = 1
// (EngineReturn::Integer). The temporaries the original leaves (a1 = 6, v1 = O+80) are not written back.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;

// Retired instructions on the only path: the first block (24, sw and sb included), six loop passes of 7 (the sqc2
// in the bne's delay slot runs on every pass) and the jr with its addiu delay slot: 24 + 6 * 7 + 2.
constexpr std::uint32_t kRetired = 68U;
constexpr std::uint32_t kLoopPasses = 6U;  // slti a1,6 at 0x0014a1a0: a1 counts 0..5

template <class Context>
n3::Run clear_six_slot_vectors_keep_w(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    auto& cpu = call.cpu();
    const GuestAddress object = call.arg(0);
    ee_cop2::vadd_vector(cpu, 1U, 0U, 0U, n3::kMaskXyz, false);  // 0014a130 vadd.xyz vf1,vf0,vf0
    m.write_u32_le(object + 340U, call.arg(1));                  // 0014a134 sw a1,340(a0)
    n3::store_quad_any(m, object + 224U, cpu.read_vu0_vector(1U));   // 0014a13c sqc2 vf1,224(a0)
    ee_cop2::vadd_vector(cpu, 1U, 0U, 0U, n3::kMaskXyz, false);  // 0014a140 vadd.xyz vf1,vf0,vf0
    n3::store_quad_any(m, object + 240U, cpu.read_vu0_vector(1U));   // 0014a144 sqc2 vf1,240(a0)
    ee_cop2::vadd_vector(cpu, 1U, 0U, 0U, n3::kMaskXyz, false);  // 0014a148 vadd.xyz vf1,vf0,vf0
    n3::store_quad_any(m, object + 192U, cpu.read_vu0_vector(1U));   // 0014a14c sqc2 vf1,192(a0)
    ee_cop2::vadd_vector(cpu, 1U, 0U, 0U, n3::kMaskXyz, false);  // 0014a150 vadd.xyz vf1,vf0,vf0
    n3::store_quad_any(m, object + 208U, cpu.read_vu0_vector(1U));   // 0014a154 sqc2 vf1,208(a0)
    ee_cop2::vadd_vector(cpu, 1U, 0U, 0U, n3::kMaskXyz, false);  // 0014a158 vadd.xyz vf1,vf0,vf0
    n3::store_quad_any(m, object + 272U, cpu.read_vu0_vector(1U));   // 0014a15c sqc2 vf1,272(a0)
    ee_cop2::vadd_vector(cpu, 1U, 0U, 0U, n3::kMaskXyz, false);  // 0014a160 vadd.xyz vf1,vf0,vf0
    n3::store_quad_any(m, object + 304U, cpu.read_vu0_vector(1U));   // 0014a164 sqc2 vf1,304(a0)
    ee_cop2::vadd_vector(cpu, 1U, 0U, 0U, n3::kMaskXyz, false);  // 0014a168 vadd.xyz vf1,vf0,vf0
    n3::store_quad_any(m, object + 256U, cpu.read_vu0_vector(1U));   // 0014a16c sqc2 vf1,256(a0)
    ee_cop2::vadd_vector(cpu, 1U, 0U, 0U, n3::kMaskXyz, false);  // 0014a170 vadd.xyz vf1,vf0,vf0
    n3::store_quad_any(m, object + 288U, cpu.read_vu0_vector(1U));   // 0014a174 sqc2 vf1,288(a0)
    ee_cop2::vadd_vector(cpu, 1U, 0U, 0U, n3::kMaskXyz, false);  // 0014a178 vadd.xyz vf1,vf0,vf0
    n3::store_quad_any(m, object + 320U, cpu.read_vu0_vector(1U));   // 0014a17c sqc2 vf1,320(a0)
    m.write_u8(object + 337U, 0U);                               // 0014a180 sb zero,337(a0)
    m.write_u8(object + 336U, 0U);                               // 0014a184 sb zero,336(a0)
    ee_cop2::vadd_vector(cpu, 2U, 0U, 0U, n3::kMaskXyz, false);  // 0014a188 vadd.xyz vf2,vf0,vf0
    ee_cop2::vadd_vector(cpu, 1U, 0U, 0U, n3::kMaskXyz, false);  // 0014a18c vadd.xyz vf1,vf0,vf0
    for (std::uint32_t i = 0U; i < kLoopPasses; ++i) {
        const GuestAddress slot = object + 16U * i;              // v1 = a0 + (a1 << 4) with a1 = i
        n3::store_quad_any(m, slot, cpu.read_vu0_vector(2U));        // 0014a19c sqc2 vf2,0(v1)
        n3::store_quad_any(m, slot + 96U, cpu.read_vu0_vector(1U));  // 0014a1a8 sqc2 vf1,96(v1), the bne's delay slot
    }
    call.return_value(1U);                                       // 0014a1b0 addiu v0,zero,1
    return call.finish(kRetired);
}

} // namespace

extern constinit const EngineFunction n3_0014a130{0x0014a130U, 132U, 0xc0bacb4d274e1643ULL, "ClearSixSlotVectorsKeepW",
                                                  kRetired, n3::live<&clear_six_slot_vectors_keep_w<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(0014a130, clear_six_slot_vectors_keep_w)
