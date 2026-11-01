// 0x00354460 (a0 = record object X): RecordOverlapPairs. X holds a signed count of 112-byte records at X+4144
// (record i at X+736+112*i). A record whose source S = [record+64] has a type byte [S+533] of 1..4 is copied into
// the next 64-byte scratchpad slot n (0x70002000+64n): the quadword [S+176] at slot+0, the quadword [L+48] at slot+16
// (L = [S+516]), the square of word 3 of the record's quadword at +0 at slot+32, zeros at slot+36, +40, +44 and the
// record at slot+48. Other records get zeros at +68, +80, +84. Then each pair of slots k < j (j below the count) is
// scanned on VU0 from the slots' quadwords: dA = A_j - A_k (A at +0), dB = B_k - B_j (B at +16), t = dA.dB / |dA|^2,
// h2 = |dB|^2 - (dA.dB)^2 / |dA|^2. A pair with |dA|^2 below 2^-32 is skipped, and so is one with t <= 0 or with
// h2 not below the sum of the two slots' values at +32. Otherwise mix = 1 - h2 / sum, and a slot whose +36 is below
// mix takes mix at +36, t at +40 and the other record at +44. Finally every slot's +36, +40, +44 go to its record's
// +80, +84, +68.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

constexpr GuestAddress kSlotBase = 0x70002000U;   // scratchpad slots, 64 bytes each
constexpr std::uint32_t kSlotStride = 64U;
constexpr GuestAddress kRecordCount = 4144U;      // object + 0x1030: signed record count
constexpr GuestAddress kRecordFirst = 736U;       // record i at object + 736 + 112 * i
constexpr std::uint32_t kRecordStride = 112U;
constexpr GuestAddress kRecordSource = 64U;       // record + 64: source pointer
constexpr GuestAddress kRecordPartner = 68U;      // record + 68 (word)
constexpr GuestAddress kRecordMix = 80U;          // record + 80 (float)
constexpr GuestAddress kRecordWeight = 84U;       // record + 84 (float)
constexpr GuestAddress kSourceType = 533U;        // source + 533 (byte): types 1..4 take a slot
constexpr GuestAddress kSourceQuad = 176U;        // source + 176 (quadword)
constexpr GuestAddress kSourceLink = 516U;        // source + 516 (pointer)
constexpr GuestAddress kLinkQuad = 48U;           // link + 48 (quadword)
constexpr GuestAddress kSlotQuadA = 0U;           // slot + 0 (quadword)
constexpr GuestAddress kSlotQuadB = 16U;          // slot + 16 (quadword)
constexpr GuestAddress kSlotSquare = 32U;         // slot + 32 (float)
constexpr GuestAddress kSlotMix = 36U;            // slot + 36 (float)
constexpr GuestAddress kSlotWeight = 40U;         // slot + 40 (float)
constexpr GuestAddress kSlotPartner = 44U;        // slot + 44 (word): the other record
constexpr GuestAddress kSlotRecord = 48U;         // slot + 48 (word): the slot's record

constexpr std::uint32_t kCeiling = 20000U;
constexpr std::uint32_t kZeroBits = 0U;               // f3
constexpr std::uint32_t kOneBits = 0x3f800000U;       // f1: 1.0
constexpr std::uint32_t kMinusOneBits = 0xbf800000U;  // f4: -1.0
constexpr std::uint32_t kTinyBits = 0x2f800000U;      // f5: 2^-32

// Retired instruction counts of the listing's blocks, delay slots included (0x354460..0x35475c).
constexpr std::uint32_t kPrologueCost = 13U;          // 0x354460..0x354490
constexpr std::uint32_t kRecordCheckCost = 4U;        // 0x354538..0x354544, each time the record count is tested
constexpr std::uint32_t kStoreRecordCost = 33U;       // type 1 on the store path; types 2..4 add kTypeStepCost each
constexpr std::uint32_t kTypeStepCost = 2U;
constexpr std::uint32_t kElseRecordCost = 22U;        // any other type: 0x354494..0x354534 without the store
constexpr std::uint32_t kSecondPrologueCost = 13U;    // 0x354548..0x354578
constexpr std::uint32_t kSlotCheckCost = 3U;          // 0x35457c..0x354584
constexpr std::uint32_t kSlotBodyCost = 3U;           // 0x354588..0x354590
constexpr std::uint32_t kSlotExitCost = 2U;           // 0x354714..0x354718
constexpr std::uint32_t kPairCheckCost = 4U;          // 0x354594..0x3545a0
constexpr std::uint32_t kPairBodyCost = 18U;          // 0x3545a4..0x3545e8
constexpr std::uint32_t kTinyCaseCost = 8U;           // 0x3545ec..0x354608
constexpr std::uint32_t kSquareCaseCost = 32U;        // 0x35460c..0x354688
constexpr std::uint32_t kJoinCost = 3U;               // 0x35468c..0x354694
constexpr std::uint32_t kSkipCost = 2U;               // 0x3546f0..0x3546f4 (the next pair)
constexpr std::uint32_t kCompareCost = 6U;            // 0x354698..0x3546ac
constexpr std::uint32_t kDivideCost = 8U;             // 0x3546b0..0x3546cc
constexpr std::uint32_t kMixKWriteCost = 5U;          // 0x354700..0x354710
constexpr std::uint32_t kMixJCheckCost = 4U;          // 0x3546d0..0x3546dc
constexpr std::uint32_t kMixJWriteCost = 4U;          // 0x3546e0..0x3546ec
constexpr std::uint32_t kWriteBackEntryCost = 2U;     // 0x35471c..0x354720
constexpr std::uint32_t kWriteBackCheckCost = 3U;     // 0x35474c..0x354754
constexpr std::uint32_t kWriteBackCost = 10U;         // 0x354724..0x354748
constexpr std::uint32_t kReturnCost = 2U;             // 0x354758..0x35475c

// lqc2 faults unless the address is 16-byte aligned and inside guest memory (see the interpreter's Lqc2).
template <class Memory>
bool quad_loadable(const Memory& memory, const GuestAddress address) {
    return (address & 15U) == 0U && memory.contains(address, 16U);
}

// A C.xx.S sets FCR31's condition bit to its result, as ee_cop1::compare_s does.
void set_condition(guest::R5900State& cpu, const bool condition) {
    cpu.set_fcr31_bits(condition ? (cpu.fcr31_bits() | guest::ee_cop1::kFcr31Condition)
                                 : (cpu.fcr31_bits() & ~guest::ee_cop1::kFcr31Condition));
}

// One pair (slot_k, slot_j) from the j body (0x3545a4) on: the VU0 operations in their listed order, the compares
// and the FPU updates of the branch taken. Returns the iteration's count after its j check and body, which the
// caller adds. The caller has checked that the four quadwords are loadable.
template <class Memory>
std::uint32_t pair_step(Memory& m, guest::R5900State& cpu, const GuestAddress slot_k, const GuestAddress slot_j) {
    cpu.write_vu0_vector(4U, n3::load_quad_any(m, slot_k + kSlotQuadB));     // lqc2 vf4,16(slot_k)
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, slot_j + kSlotQuadB));     // lqc2 vf1,16(slot_j)
    cpu.write_vu0_vector(5U, n3::load_quad_any(m, slot_k + kSlotQuadA));     // lqc2 vf5,0(slot_k)
    cpu.write_vu0_vector(6U, n3::load_quad_any(m, slot_j + kSlotQuadA));     // lqc2 vf6,0(slot_j)
    ee_cop2::vadd_vector(cpu, 1U, 4U, 1U, 14U, true);                        // vsub.xyz vf1,vf4,vf1
    ee_cop2::vadd_vector(cpu, 5U, 6U, 5U, 14U, true);                        // vsub.xyz vf5,vf6,vf5
    const guest::Gpr128 difference_b = cpu.read_vu0_vector(1U);             // qmfc2 t2,vf1
    ee_cop2::vmul(cpu, 1U, 5U, 5U, 14U);                                     // vmul.xyz vf1,vf5,vf5
    ee_cop2::vadda_broadcast(cpu, 1U, 1U, 8U, 1U);                           // vadday.x acc,vf1,vf1y
    ee_cop2::vmadd_broadcast(cpu, 1U, 3U, 1U, 8U, 2U, false, false, false);  // vmaddz.x vf1,vf3,vf1z
    const std::uint32_t square_a = cpu.read_vu0_vector(1U).words[0];        // VF1.x, then f0
    const bool tiny = fpu::lt(square_a, kTinyBits);
    set_condition(cpu, tiny);                                                // c.olt.s f0,f5
    if (tiny) {
        cpu.write_vu0_vector(1U, difference_b);                              // qmtc2 t2,vf1
        ee_cop2::vmul(cpu, 1U, 1U, 1U, 14U);                                 // vmul.xyz vf1,vf1,vf1
        ee_cop2::vadda_broadcast(cpu, 1U, 1U, 8U, 1U);                       // vadday.x acc,vf1,vf1y
        ee_cop2::vmadd_broadcast(cpu, 1U, 9U, 1U, 8U, 2U, false, false, false);  // vmaddz.x vf1,vf9,vf1z
        // The weight is f7 = f4 (-1.0), so the join skips the pair.
        set_condition(cpu, fpu::le(kMinusOneBits, kZeroBits));              // c.ole.s f7,f3
        return kTinyCaseCost + kJoinCost + kSkipCost;
    }

    ee_cop2::vmul(cpu, 1U, 5U, 5U, 14U);                                     // vmul.xyz vf1,vf5,vf5
    ee_cop2::vadda_broadcast(cpu, 1U, 1U, 8U, 1U);                           // vadday.x acc,vf1,vf1y
    ee_cop2::vmadd_broadcast(cpu, 1U, 2U, 1U, 8U, 2U, false, false, false);  // vmaddz.x vf1,vf2,vf1z
    const std::uint32_t square_b = cpu.read_vu0_vector(1U).words[0];        // VF1.x (the qmfc2/qmtc2 pair is a no-op)
    ee_cop2::vdiv(cpu, 0U, 3U, 1U, 0U, true);                                // vrsqrt q,vf0w,vf1x
    const std::uint32_t length = fpu::sqrt(square_b);                        // mtc1 f0; sqrt.s f0,f0 (f6 takes it)
    ee_cop2::vmul_control(cpu, 5U, 5U, 14U, ee_cop2::kControlQ);             // vmulq.xyz vf5,vf5,q
    cpu.write_vu0_vector(1U, cpu.read_vu0_vector(5U));                       // qmfc2 t3,vf5; qmtc2 t3,vf1
    cpu.write_vu0_vector(4U, difference_b);                                  // qmtc2 t2,vf4
    ee_cop2::vmul(cpu, 1U, 1U, 4U, 14U);                                     // vmul.xyz vf1,vf1,vf4
    ee_cop2::vadda_broadcast(cpu, 1U, 1U, 8U, 1U);                           // vadday.x acc,vf1,vf1y
    ee_cop2::vmadd_broadcast(cpu, 1U, 8U, 1U, 8U, 2U, false, false, false);  // vmaddz.x vf1,vf8,vf1z
    const std::uint32_t dot = cpu.read_vu0_vector(1U).words[0];             // f2
    ee_cop2::vmul(cpu, 1U, 4U, 4U, 14U);                                     // vmul.xyz vf1,vf4,vf4
    ee_cop2::vadda_broadcast(cpu, 1U, 1U, 8U, 1U);                           // vadday.x acc,vf1,vf1y
    ee_cop2::vmadd_broadcast(cpu, 1U, 7U, 1U, 8U, 2U, false, false, false);  // vmaddz.x vf1,vf7,vf1z
    const std::uint32_t square_difference_b = cpu.read_vu0_vector(1U).words[0];   // f0
    const std::uint32_t weight = fpu::div(dot, length);                      // div.s f7,f2,f6
    const std::uint32_t accumulated = fpu::add(kZeroBits, square_difference_b);   // adda.s f3,f0
    cpu.set_fpu_accumulator_bits(accumulated);
    const std::uint32_t h2 = fpu::msub(accumulated, dot, dot);               // msub.s f6,f2,f2

    std::uint32_t cost = kSquareCaseCost + kJoinCost;
    const bool non_positive = fpu::le(weight, kZeroBits);
    set_condition(cpu, non_positive);                                        // c.ole.s f7,f3
    if (non_positive) {
        return cost + kSkipCost;
    }
    cost += kCompareCost;
    const std::uint32_t square_k = m.read_u32_le(slot_k + kSlotSquare);     // lwc1 f2,32(slot_k)
    const std::uint32_t square_j = m.read_u32_le(slot_j + kSlotSquare);     // lwc1 f0,32(slot_j)
    const std::uint32_t sum = fpu::add(square_k, square_j);                  // add.s f0,f2,f0
    const bool inside = fpu::lt(h2, sum);
    set_condition(cpu, inside);                                              // c.olt.s f6,f0
    if (!inside) {
        return cost + kSkipCost;
    }
    cost += kDivideCost;
    const std::uint32_t mix = fpu::sub(kOneBits, fpu::div(h2, sum));         // div.s f0,f6,f0; sub.s f2,f1,f0
    const std::uint32_t mix_k = m.read_u32_le(slot_k + kSlotMix);            // lwc1 f0,36(slot_k)
    const bool k_below = fpu::le(mix, mix_k);
    set_condition(cpu, k_below);                                             // c.ole.s f2,f0
    if (!k_below) {
        cost += kMixKWriteCost;
        m.write_u32_le(slot_k + kSlotMix, mix);
        m.write_u32_le(slot_k + kSlotWeight, weight);
        m.write_u32_le(slot_k + kSlotPartner, m.read_u32_le(slot_j + kSlotRecord));
    }
    cost += kMixJCheckCost;
    const std::uint32_t mix_j = m.read_u32_le(slot_j + kSlotMix);           // lwc1 f0,36(slot_j)
    const bool j_below = fpu::le(mix, mix_j);
    set_condition(cpu, j_below);                                             // c.ole.s f2,f0
    if (j_below) {
        return cost + kSkipCost;
    }
    cost += kMixJWriteCost;
    m.write_u32_le(slot_j + kSlotMix, mix);
    m.write_u32_le(slot_j + kSlotWeight, weight);
    m.write_u32_le(slot_j + kSlotPartner, m.read_u32_le(slot_k + kSlotRecord));
    return cost + kSkipCost;
}

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost, with a copy of the cpu). Returns
// the retired count, or declines when a quadword the VU0 would load is not loadable.
template <class Memory>
n3::Run scan_records(Memory& m, guest::R5900State& cpu, const GuestAddress object) {
    std::uint32_t retired = kPrologueCost;
    std::int32_t slots = 0;   // the listing's a2: slots written so far
    for (std::int32_t record = 0;; ++record) {
        const auto count = static_cast<std::int32_t>(m.read_u32_le(object + kRecordCount));
        retired += kRecordCheckCost;
        if (record >= count) {
            break;
        }
        if (retired > kCeiling) {
            return {retired, false};
        }
        const GuestAddress address = object + kRecordFirst + kRecordStride * static_cast<std::uint32_t>(record);
        const GuestAddress source = m.read_u32_le(address + kRecordSource);
        const std::uint32_t type_index = static_cast<std::uint32_t>(m.read_u8(source + kSourceType)) - 1U;
        if (type_index < 4U) {
            retired += kStoreRecordCost + kTypeStepCost * type_index;
            const guest::Gpr128 source_quad = n3::load_quad_any(m, source + kSourceQuad);
            const GuestAddress slot = kSlotBase + kSlotStride * static_cast<std::uint32_t>(slots);
            ++slots;
            n3::store_quad_any(m, slot + kSlotQuadA, source_quad);
            const GuestAddress link = m.read_u32_le(m.read_u32_le(address + kRecordSource) + kSourceLink);
            n3::store_quad_any(m, slot + kSlotQuadB, n3::load_quad_any(m, link + kLinkQuad));
            // The original stores this quadword to its frame and reads word 3 back, which is the same word.
            const std::uint32_t word3 = n3::load_quad_any(m, address).words[3];
            m.write_u32_le(slot + kSlotSquare, fpu::mul(word3, word3));
            m.write_u32_le(slot + kSlotMix, 0U);
            m.write_u32_le(slot + kSlotWeight, 0U);
            m.write_u32_le(slot + kSlotPartner, 0U);
            m.write_u32_le(slot + kSlotRecord, address);
        } else {
            retired += kElseRecordCost;
            m.write_u32_le(address + kRecordMix, 0U);
            m.write_u32_le(address + kRecordWeight, 0U);
            m.write_u32_le(address + kRecordPartner, 0U);
        }
    }

    // The second section's prelude: vf9, vf3, vf2, vf8 and vf7 get 1.0 in lane x (vaddw.x of vf0.w), in this order.
    retired += kSecondPrologueCost;
    ee_cop2::vadd_broadcast(cpu, 9U, 0U, 0U, 8U, 3U, false);   // vaddw.x vf9,vf0,vf0w
    ee_cop2::vadd_broadcast(cpu, 3U, 0U, 0U, 8U, 3U, false);   // vaddw.x vf3,vf0,vf0w
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 0U, 8U, 3U, false);   // vaddw.x vf2,vf0,vf0w
    ee_cop2::vadd_broadcast(cpu, 8U, 0U, 0U, 8U, 3U, false);   // vaddw.x vf8,vf0,vf0w
    ee_cop2::vadd_broadcast(cpu, 7U, 0U, 0U, 8U, 3U, false);   // vaddw.x vf7,vf0,vf0w

    // The k loop over the slots, and the j loop over the records above k (below the count).
    for (std::int32_t k = 0;; ++k) {
        retired += kSlotCheckCost;
        if (k >= slots) {
            break;
        }
        if (retired > kCeiling) {
            return {retired, false};
        }
        retired += kSlotBodyCost;
        const GuestAddress slot_k = kSlotBase + kSlotStride * static_cast<std::uint32_t>(k);
        for (std::int32_t j = k + 1;; ++j) {
            const auto count = static_cast<std::int32_t>(m.read_u32_le(object + kRecordCount));
            retired += kPairCheckCost;
            if (j >= count) {
                break;
            }
            if (retired > kCeiling) {
                return {retired, false};
            }
            retired += kPairBodyCost;
            const GuestAddress slot_j = kSlotBase + kSlotStride * static_cast<std::uint32_t>(j);
            if (!quad_loadable(m, slot_k + kSlotQuadA) || !quad_loadable(m, slot_k + kSlotQuadB) ||
                !quad_loadable(m, slot_j + kSlotQuadA) || !quad_loadable(m, slot_j + kSlotQuadB)) {
                return n3::kDeclined;
            }
            retired += pair_step(m, cpu, slot_k, slot_j);
        }
        retired += kSlotExitCost;
    }

    // Write-back: each slot's mix, weight and partner go to its record.
    retired += kWriteBackEntryCost;
    for (std::int32_t s = 0;; ++s) {
        retired += kWriteBackCheckCost;
        if (s >= slots) {
            break;
        }
        if (retired > kCeiling) {
            return {retired, false};
        }
        retired += kWriteBackCost;
        const GuestAddress slot = kSlotBase + kSlotStride * static_cast<std::uint32_t>(s);
        const GuestAddress record = m.read_u32_le(slot + kSlotRecord);
        m.write_u32_le(record + kRecordMix, m.read_u32_le(slot + kSlotMix));
        m.write_u32_le(record + kRecordWeight, m.read_u32_le(slot + kSlotWeight));
        m.write_u32_le(record + kRecordPartner, m.read_u32_le(slot + kSlotPartner));
    }
    return {retired + kReturnCost, false};
}

template <class Context>
n3::Run record_overlap_pairs(Context& context) {
    auto call = n3::guest_call(context);
    const n3::Run run = scan_records(call.memory(), call.cpu(), call.arg(0));
    return run.declined ? call.decline() : call.finish(run.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00354460, 768U, 0x256ba8a3b00e8aebULL, "RecordOverlapPairs", kCeiling, record_overlap_pairs, EngineReturn::Nothing)
