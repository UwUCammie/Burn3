// 0x00263c40 (a0 = object O, a1 = address of quad B, a2 = address of quad A, a3 selects the ring set,
// f12 = header float, f13 = float input): RecordBallisticTrailSample. The ring set S is O (a3 == 0) or O + 76,
// and its slot index at S+64 picks the 16-byte entry E = S + 16*index: {group base @0, limit @4, cursor @8,
// float @12}. The cursor c selects sample c&3 of group c/4 (a group is 144 bytes: a header of four floats, then
// four 32-byte samples of two quads). A sample's first quad is B.xyz and its second is A.xyz; each gets a w
// lane from a ballistic solve with the constants of P = *(O+152) (g = P[80], k = P[84]), or from B.y and A.y
// when the solve has no root in range. The cursor advances and wraps to 0 at the limit, S+64 takes (index+1)&3,
// and the group's header float and S+68 take f12. Returns nothing. The stale frame words that lqc2 reads are
// mirrored, and the VU0 registers vf1..vf5 and the FPU accumulator are left as the original leaves them.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint32_t kFrameBytes = 288U;            // addiu sp,sp,-288
constexpr std::uint32_t kGroupBytes = 144U;            // 16-byte header, then four 32-byte samples
constexpr std::uint32_t kUncachedAlias = 0x20000000U;  // or'd into the group address: the store uses it
constexpr GuestAddress kParams = 152U;                 // P = *(O + 152)
constexpr GuestAddress kParamG = 80U;                  // P[80]
constexpr GuestAddress kParamK = 84U;                  // P[84]
constexpr std::uint32_t kTwo = 0x40000000U;            // 2.0f
constexpr std::uint32_t kMinusOne = 0xbf800000U;       // -1.0f
constexpr std::uint32_t kHalf = 0x3f000000U;           // 0.5f
constexpr std::uint8_t kMaskY = 4U;                    // VU destination mask of the y lane

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kHeadCost = 6U;            // 0x263c40..0x263c54: the quads and their frame copies
constexpr std::uint32_t kSetZeroCost = 1U;         // a3 == 0: daddu at 0x263c60
constexpr std::uint32_t kSetNonzeroCost = 2U;      // a3 != 0: the jump and addiu in its delay slot
constexpr std::uint32_t kGroupCost = 6U;           // 0x263c64..0x263c78: index, entry, cursor, sign test
constexpr std::uint32_t kNegativeCursorCost = 2U;  // cursor < 0: addiu and sra at 0x263c7c..0x263c80
constexpr std::uint32_t kSlotCost = 18U;           // 0x263c84..0x263cc8: group, slot, cursor store, wrap test
constexpr std::uint32_t kWrapCost = 2U;            // cursor at the limit: store zero and the jump back
constexpr std::uint32_t kCopyCost = 13U;           // 0x263ccc..0x263cfc: quad copy, index step, sign of A.y
constexpr std::uint32_t kNegativeYCost = 3U;       // A.y < 0: the f13 test and its branch
constexpr std::uint32_t kNegateCost = 1U;          // A.y < 0 and f13 <= 0: neg.s
constexpr std::uint32_t kSolveCost = 20U;          // 0x263d10..0x263d5c: discriminant and its test
constexpr std::uint32_t kBoundCost = 7U;           // 0x263d60..0x263d78: the root bound test
constexpr std::uint32_t kMainCost = 40U;           // 0x263d7c..0x263e18: the solved quad
constexpr std::uint32_t kFallbackCost = 11U;       // 0x263e48..0x263e70: the fallback quads
constexpr std::uint32_t kTailCost = 11U;           // 0x263e1c..0x263e3c (stores) and the return

// The emulator's 32-bit GPR results (lui, mfc1, ...) replace the low doubleword, sign-extended, and keep the
// high one. That high doubleword is what qmtc2 copies into VU0 along with the low one.
guest::Gpr128 with_sign_extended_low_word(guest::Gpr128 value, const std::uint32_t bits) {
    value.words[0] = bits;
    value.words[1] = (bits & 0x80000000U) != 0U ? 0xffffffffU : 0U;
    return value;
}

template <class Context>
n3::Run record_ballistic_trail_sample(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object = call.arg(0U);
    const GuestAddress quad_b_address = call.arg(1U);
    const GuestAddress quad_a_address = call.arg(2U);
    const std::uint64_t select = call.arg64(3U);  // beq compares the whole doubleword
    const std::uint32_t header = cpu.read_fpr_bits(12U);
    const std::uint32_t f13 = cpu.read_fpr_bits(13U);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    // This function never writes a2's high doubleword (32-bit results keep it); the VU0 moves below copy it.
    const guest::Gpr128 entry_a2 = call.arg128(2U);

    std::uint32_t retired = kHeadCost + (select == 0U ? kSetZeroCost : kSetNonzeroCost);
    const guest::Gpr128 quad_b = n3::load_quad_any(m, quad_b_address);
    const guest::Gpr128 quad_a = n3::load_quad_any(m, quad_a_address);
    n3::store_quad_any(m, frame + 272U, quad_b);
    n3::store_quad_any(m, frame + 256U, quad_a);

    // The ring set, its slot index and entry E, and the cursor c with the group and sample it selects.
    const GuestAddress set = select == 0U ? object : object + 76U;
    const std::uint32_t index = m.read_u32_le(set + 64U);
    const GuestAddress entry = set + (index << 4U);
    const std::uint32_t cursor_bits = m.read_u32_le(entry + 8U);
    const auto cursor = static_cast<std::int32_t>(cursor_bits);
    const auto group = static_cast<std::uint32_t>(cursor >= 0 ? cursor >> 2 : (cursor + 3) >> 2);
    const std::uint32_t base = m.read_u32_le(entry);
    const std::uint32_t lane = cursor_bits & 3U;
    const GuestAddress header_group = (base + group * kGroupBytes) | kUncachedAlias;
    const GuestAddress slot = header_group + (lane << 5U);
    retired += kGroupCost + (cursor < 0 ? kNegativeCursorCost : 0U) + kSlotCost;

    // The cursor advances, is read back, and wraps to zero when it reaches the limit.
    m.write_u32_le(entry + 8U, cursor_bits + 1U);
    const GuestAddress out = slot + 16U;
    const std::uint32_t cursor_after = m.read_u32_le(entry + 8U);
    const std::uint32_t limit = m.read_u32_le(entry + 4U);
    if (cursor_after == limit) {
        m.write_u32_le(entry + 8U, 0U);
        retired += kWrapCost;
    }

    // A is copied to sp+112 and the index steps. f5 is A.y, negated when A.y < 0 and f13 <= 0.
    const std::uint32_t index_again = m.read_u32_le(set + 64U);
    const guest::Gpr128 copy_a = n3::load_quad_any(m, frame + 256U);
    n3::store_quad_any(m, frame + 112U, copy_a);
    m.write_u32_le(set + 64U, index_again + 1U);
    std::uint32_t f5 = m.read_u32_le(frame + 116U);
    const std::uint32_t index_stepped = m.read_u32_le(set + 64U);
    const bool y_negative = fpu::lt(f5, 0U);  // c.olt.s f5,f0 with f0 = +0
    m.write_u32_le(set + 64U, index_stepped & 3U);  // the delay slot of that branch: stored on every path
    retired += kCopyCost;
    if (y_negative) {
        retired += kNegativeYCost;
        if (fpu::le(f13, 0U)) {  // c.ole.s f13,f0
            f5 = fpu::neg(f5);
            retired += kNegateCost;
        }
    }

    // The discriminant D = f5*f5 - (2g)*f13, with ACC = f5*f5 from mula.s. The VU0 moves of the listing
    // run alongside it; they touch only VU0 registers and the frame.
    const GuestAddress params = m.read_u32_le(object + kParams);
    const std::uint32_t acc_square = fpu::mul(f5, f5);
    cpu.write_vu0_vector(5U, n3::load_quad_any(m, frame + 272U));
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, frame + 240U));  // stale until stored below
    const std::uint32_t g = m.read_u32_le(params + kParamG);
    const std::uint32_t two_g = fpu::mul(kTwo, g);
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, frame + 256U));
    ee_cop2::vadd_vector(cpu, 3U, 0U, 5U, n3::kMaskXyz, false);  // vadd.xyz vf3,vf0,vf5
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, frame + 224U));    // stale until stored below
    const std::uint32_t discriminant = fpu::msub(acc_square, two_g, f13);
    const bool no_root = fpu::le(discriminant, 0U);  // c.ole.s f1,f0
    n3::store_quad_any(m, frame + 240U, cpu.read_vu0_vector(3U));
    ee_cop2::vadd_vector(cpu, 2U, 0U, 1U, n3::kMaskXyz, false);  // vadd.xyz vf2,vf0,vf1
    const guest::Gpr128 vf3_copy = cpu.read_vu0_vector(3U);      // qmfc2 a0,vf3
    n3::store_quad_any(m, frame + 224U, cpu.read_vu0_vector(2U));
    const guest::Gpr128 vf2_copy = cpu.read_vu0_vector(2U);      // qmfc2 t0,vf2
    retired += kSolveCost;

    // The solve is used only when 0 < D < (f5 + E[12]*g)^2. On that path ACC becomes f5 (adda.s).
    std::uint32_t final_acc = acc_square;
    bool fallback = no_root;
    if (!fallback) {
        retired += kBoundCost;
        const std::uint32_t acc = fpu::add(0U, f5);  // adda.s f0,f5 with f0 = +0
        final_acc = acc;
        const std::uint32_t e12 = m.read_u32_le(entry + 12U);
        const std::uint32_t root_bound = fpu::madd(acc, e12, g);  // madd.s f0,f0,f2
        const std::uint32_t bound = fpu::mul(root_bound, root_bound);
        fallback = !fpu::lt(discriminant, bound);  // c.olt.s f1,f0 and bc1f
    }

    if (!fallback) {
        retired += kMainCost;
        // T = (-1/g) * (sqrt(D) + f5): the w lanes of both quads are built from it.
        const std::uint32_t root = fpu::add(fpu::sqrt(discriminant), f5);
        const std::uint32_t inverse = fpu::div(kMinusOne, g);
        const std::uint32_t t = fpu::mul(inverse, root);
        const std::uint32_t k = m.read_u32_le(params + kParamK);
        const std::uint32_t half_g = fpu::mul(kHalf, g);
        const std::uint32_t g_t = fpu::mul(g, t);
        const std::uint32_t sum = fpu::add(f5, g_t);
        const std::uint32_t scaled = fpu::mul(fpu::neg(k), sum);
        const std::uint32_t f2p = fpu::sub(scaled, g_t);
        const std::uint32_t half_g_t = fpu::mul(half_g, t);
        const std::uint32_t f2p_t = fpu::mul(f2p, t);
        const std::uint32_t quad_w_term = fpu::mul(t, half_g_t);

        cpu.write_vu0_vector(4U, vf3_copy);  // qmtc2 a0,vf4
        cpu.write_vu0_vector(3U, vf2_copy);  // qmtc2 t0,vf3
        ee_cop2::vadd_broadcast(cpu, 4U, 0U, 0U, n3::kMaskW, 3U, true);  // vsubw.w vf4,vf0,vf0w
        ee_cop2::vadd_broadcast(cpu, 3U, 0U, 0U, n3::kMaskW, 3U, true);  // vsubw.w vf3,vf0,vf0w
        cpu.write_vu0_vector(1U, with_sign_extended_low_word(entry_a2, f13));  // qmtc2 a2,vf1
        ee_cop2::vadd_broadcast(cpu, 2U, 5U, 1U, kMaskY, 0U, true);  // vsubx.y vf2,vf5,vf1x
        cpu.write_vu0_vector(1U, with_sign_extended_low_word(entry_a2, f2p_t));  // qmtc2 a2,vf1
        ee_cop2::vadd_broadcast(cpu, 2U, 2U, 1U, kMaskY, 0U, true);  // vsubx.y vf2,vf2,vf1x
        cpu.write_vu0_vector(1U, with_sign_extended_low_word(vf3_copy, quad_w_term));  // qmtc2 a0,vf1
        ee_cop2::vadd_broadcast(cpu, 1U, 2U, 1U, kMaskY, 0U, true);  // vsubx.y vf1,vf2,vf1x
        ee_cop2::vadd_broadcast(cpu, 4U, 4U, 1U, n3::kMaskW, 1U, false);  // vaddy.w vf4,vf4,vf1y
        cpu.write_vu0_vector(1U, with_sign_extended_low_word(entry_a2, f2p));  // qmtc2 a2,vf1
        ee_cop2::vadd_broadcast(cpu, 3U, 3U, 1U, n3::kMaskW, 0U, false);  // vaddx.w vf3,vf3,vf1x
        n3::store_quad_any(m, frame + 16U, cpu.read_vu0_vector(4U));
        n3::store_quad_any(m, frame + 0U, cpu.read_vu0_vector(3U));
    } else {
        retired += kFallbackCost;
        cpu.write_vu0_vector(4U, n3::load_quad_any(m, frame + 240U));
        cpu.write_vu0_vector(2U, n3::load_quad_any(m, frame + 224U));
        cpu.write_vu0_vector(3U, n3::load_quad_any(m, frame + 272U));
        ee_cop2::vadd_broadcast(cpu, 4U, 0U, 0U, n3::kMaskW, 3U, true);  // vsubw.w vf4,vf0,vf0w
        ee_cop2::vadd_broadcast(cpu, 4U, 4U, 3U, n3::kMaskW, 1U, false);  // vaddy.w vf4,vf4,vf3y
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, frame + 256U));
        ee_cop2::vadd_broadcast(cpu, 2U, 0U, 0U, n3::kMaskW, 3U, true);  // vsubw.w vf2,vf0,vf0w
        ee_cop2::vadd_broadcast(cpu, 2U, 2U, 1U, n3::kMaskW, 1U, false);  // vaddy.w vf2,vf2,vf1y
        n3::store_quad_any(m, frame + 16U, cpu.read_vu0_vector(4U));
        n3::store_quad_any(m, frame + 0U, cpu.read_vu0_vector(2U));
    }

    // The solved or fallback quads go to the sample, then the header float and S+68 take f12.
    cpu.set_fpu_accumulator_bits(final_acc);
    const guest::Gpr128 quad0 = n3::load_quad_any(m, frame + 16U);
    const guest::Gpr128 quad1 = n3::load_quad_any(m, frame + 0U);
    n3::store_quad_any(m, out, quad0);
    n3::store_quad_any(m, out + 16U, quad1);
    m.write_u32_le(header_group + (lane << 2U), header);
    m.write_u32_le(set + 68U, header);
    retired += kTailCost;
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_00263c40{0x00263c40U, 572U, 0x80d24c37742d1908ULL,
                                                  "RecordBallisticTrailSample", 131U,
                                                  n3::live<&record_ballistic_trail_sample<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00263c40, record_ballistic_trail_sample)
