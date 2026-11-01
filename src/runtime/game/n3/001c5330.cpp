// 0x001c5330 (a0 = record R, a1 = owner O): FilterAndQueueRecord. R's id record is the pointer at R+96 and the id is the
// u16 at its +4; the type is the id's low byte. Types 34 and 35, and ids with 0x1000 set, return 1 without queueing.
// Types 21..32 first run a VU0 test: the dot product of O+176 and R+16 over x, y and z must be at most 0.5, or the
// result is 1. The float at R+20 (read back through the frame copy of R+16) is then compared with the float at
// $gp-30892: below it the result is 1; otherwise R is queued on O by QueuePushRecord (0x001ab090) and that call's
// result is returned. The original's ra save and restore in its frame are not reproduced: ra comes back unchanged.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(001ab090)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace ee_cop2 = guest::ee_cop2;
namespace fpu = n3::fpu;

constexpr std::uint32_t kFrameBytes = 112U;         // addiu $sp, $sp, -112
constexpr std::int32_t kThresholdOffset = -30892;   // lwc1 at $gp - 30892: the threshold float
constexpr GuestAddress kIdRecordOffset = 96U;       // record + 96: pointer to the id record
constexpr GuestAddress kIdOffset = 4U;              // id record + 4: the u16 id
constexpr GuestAddress kVectorOffset = 16U;         // record + 16: the vector (lqc2 into VF2, lq into the stack copy)
constexpr GuestAddress kOwnerVectorOffset = 176U;   // owner + 176: lqc2 into VF3
constexpr GuestAddress kStackOffset = 16U;          // sp + 16: the frame copy of the vector
constexpr GuestAddress kProbeOffset = 20U;          // word 1 of the vector: the probed float (sp + 20 once copied)
constexpr std::uint32_t kFlagBit = 0x1000U;         // id & 0x1000: the record is skipped
constexpr std::uint32_t kHalfBits = 0x3f000000U;    // 0.5f
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kPrefixCost = 9U;           // 0x1c5330..0x1c5350: frame, the two id loads, the type 35 test
constexpr std::uint32_t kType34Cost = 3U;           // 0x1c5354..0x1c535c: the type 34 test
constexpr std::uint32_t kFlagCost = 4U;             // 0x1c5360..0x1c536c: the 0x1000 test
constexpr std::uint32_t kHeadCost = kPrefixCost + kType34Cost + kFlagCost;
constexpr std::uint32_t kSetOneCost = 2U;           // 0x1c5390..0x1c5394: v0 = 1, jump to the return
constexpr std::uint32_t kReturnCost = 3U;           // 0x1c5424..0x1c542c: ld ra, jr ra, sp restore
constexpr std::uint32_t kEarlyCost = kSetOneCost + kReturnCost;
constexpr std::uint32_t kRangeCost = 3U;            // 0x1c5370..0x1c5378: type < 21
constexpr std::uint32_t kUpperCost = 3U;            // 0x1c537c..0x1c5384: type < 33 (after type >= 21)
constexpr std::uint32_t kJumpCost = 2U;             // 0x1c5388..0x1c538c (v0 = 1) or 0x1c53c8..0x1c53cc (v0 = 0)
constexpr std::uint32_t kDispatchCost = 2U;         // 0x1c5398..0x1c539c: bne v0 to the VU0 test
constexpr std::uint32_t kVuCost = 16U;              // 0x1c53d0..0x1c540c: the VU0 test
constexpr std::uint32_t kVuPassCost = 2U;           // 0x1c5410..0x1c5414: length above 0.5, v0 = 1
constexpr std::uint32_t kProbeCost = 8U;            // 0x1c53a0..0x1c53bc: lq, the probe and its compare
constexpr std::uint32_t kProbePassCost = 2U;        // 0x1c53c0..0x1c53c4: probe below threshold, v0 = 1
constexpr std::uint32_t kPushCost = 3U;             // 0x1c5418..0x1c5420: a0/a1, jal and its delay slot

// The loads and stores the original makes, checked as the interpreter checks them (ee_interpreter.cpp). A call the
// original would fault on is declined, so the original runs it and faults itself.
template <class Memory>
bool word_loadable(const Memory& m, const GuestAddress address) {
    return (address & 3U) == 0U && m.contains(address, 4U);
}

template <class Memory>
bool half_loadable(const Memory& m, const GuestAddress address) {
    return (address & 1U) == 0U && m.contains(address, 2U);
}

// lqc2 needs a 16-byte aligned address.
template <class Memory>
bool vector_loadable(const Memory& m, const GuestAddress address) {
    return (address & 15U) == 0U && m.contains(address, 16U);
}

// lq and sq ignore the low four address bits and only need the aligned quadword to be in guest memory.
template <class Memory>
bool quad_contained(const Memory& m, const GuestAddress address) {
    return m.contains(address & ~15U, 16U);
}

template <class Context>
n3::Run filter_and_queue_record(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    const GuestAddress owner = call.arg(1);

    // lw $v1, 96($a0); lhu $a0, 4($v1)
    if (!word_loadable(m, record + kIdRecordOffset)) {
        return call.decline();
    }
    const GuestAddress id_record = m.read_u32_le(record + kIdRecordOffset);
    if (!half_loadable(m, id_record + kIdOffset)) {
        return call.decline();
    }
    const std::uint32_t id = m.read_u16_le(id_record + kIdOffset);
    const std::uint32_t type = id & 0xffU;

    if (type == 35U) {
        call.return_value(1U);
        return call.finish(kPrefixCost + kEarlyCost);
    }
    if (type == 34U) {
        call.return_value(1U);
        return call.finish(kPrefixCost + kType34Cost + kEarlyCost);
    }
    if ((id & kFlagBit) != 0U) {
        call.return_value(1U);
        return call.finish(kHeadCost + kEarlyCost);
    }

    // Types 21..32 take the VU0 test; the other types go straight to the probe.
    std::uint32_t retired = kHeadCost + kRangeCost;
    const bool vu_test = type >= 21U && type <= 32U;
    if (type < 21U) {
        retired += kJumpCost;
    } else {
        retired += kUpperCost + kJumpCost;
    }
    retired += kDispatchCost;

    if (vu_test) {
        if (!vector_loadable(m, owner + kOwnerVectorOffset) || !vector_loadable(m, record + kVectorOffset)) {
            return call.decline();
        }
        cpu.write_vu0_vector(3U, n3::load_quad_any(m, owner + kOwnerVectorOffset));  // lqc2 vf3, 176($a1)
        cpu.write_vu0_vector(2U, n3::load_quad_any(m, record + kVectorOffset));      // lqc2 vf2, 16($a2)
        ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, 8U, 3U, false);                   // vaddw.x vf1, vf0, vf0w
        ee_cop2::vmul(cpu, 2U, 3U, 2U, 14U);                                        // vmul.xyz vf2, vf3, vf2
        const guest::Gpr128 saved = cpu.read_vu0_vector(1U);                        // qmfc2 v0, vf1
        ee_cop2::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);                              // vadday.x acc, vf2, vf2y
        cpu.write_vu0_vector(1U, saved);                                            // qmtc2 v0, vf1
        ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, 8U, 2U, false, false, false);     // vmaddz.x vf1, vf1, vf2z
        const std::uint32_t length = cpu.read_vu0_vector(1U).words[0];              // qmfc2 v0, vf1; mtc1 v0, f1
        cpu.write_fpr_bits(1U, length);
        cpu.write_fpr_bits(0U, kHalfBits);                                           // lui v0, 0x3f00; mtc1 v0, f0
        retired += kVuCost;
        if (!fpu::le(length, kHalfBits)) {                                           // c.ole.s false: bc1t not taken
            call.return_value(1U);
            return call.finish(retired + kVuPassCost + kReturnCost);
        }
    }

    // 0x1c53a0..0x1c53bc: lq $v1, 16($a2); lwc1 $f0 = threshold; sq to sp+16; lwc1 $f1 = sp+20 (the probe).
    retired += kProbeCost;
    const GuestAddress threshold_address =
        cpu.read_gpr_u32(n3::kGlobalPointer) + static_cast<GuestAddress>(kThresholdOffset);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    if (!quad_contained(m, record + kVectorOffset) || !word_loadable(m, threshold_address) ||
        !quad_contained(m, frame + kStackOffset) || !word_loadable(m, frame + kProbeOffset)) {
        return call.decline();
    }
    const guest::Gpr128 quad = n3::load_quad_any(m, record + kVectorOffset);
    const std::uint32_t threshold = m.read_u32_le(threshold_address);
    n3::store_quad_any(m, frame + kStackOffset, quad);
    const std::uint32_t probe = m.read_u32_le(frame + kProbeOffset);
    cpu.write_fpr_bits(0U, threshold);
    cpu.write_fpr_bits(1U, probe);
    if (fpu::lt(probe, threshold)) {                                                 // c.olt.s true: bc1f not taken
        call.return_value(1U);
        return call.finish(retired + kProbePassCost + kReturnCost);
    }

    // 0x1c5418..0x1c5420: QueuePushRecord(owner, record); its v0 is this routine's result.
    call.set_arg(0, owner);
    call.set_arg(1, record);
    const n3::Run queued = call.invoke([](auto& machine) { return n3::fn_001ab090(machine); }, kFrameBytes);
    if (queued.declined) {
        return call.decline();
    }
    return call.finish(retired + kPushCost + queued.retired + kReturnCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(001c5330, 256U, 0xe4937e04c1742287ULL, "FilterAndQueueRecord", kCeiling, filter_and_queue_record,
         EngineReturn::Integer)
