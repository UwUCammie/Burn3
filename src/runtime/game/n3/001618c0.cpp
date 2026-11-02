// 0x001618c0 (a0 = object O, a1 = output pointer, f12 = parameter): EvaluateCurveSegmentVu0.
//
// The byte at O+65 picks a 76-byte entry at 0x0052288c (255 picks address 0: the listing's lw from $a2 = 0). The word in
// that entry is a record R: the pair table at +0 (two halfwords per index), the quadword table at +12 and the segment
// count N at +16. A = trunc(parameter); when A >= N-1 the listing takes A = N-2 and the parameter float(N-1). The
// fraction t = parameter - float(A) gives the cubic basis VF2 = (t^3, t^2, t, 1) (seven VU0 steps, kept as VF19).
// VF1 = (T, then the frame words the listing reads before it writes them), T being the float at O+52. Four passes
// k = 0..3 clamp the pair index to [0, N-1] at A + k - 1 and load the quadwords at quadtable + 16 * halfword into
// VF2..VF9. VF11..VF14 and VF15..VF18 are the constant quadwords at 0x00521640 and 0x00521680.
// When the word at gp-22808 is not 0x004839e0 it is set, the DMA registers 0x10008020 / 0x10008030 / 0x10008000 are
// written, and WordTableLookup10(0) and ReadBusyFlagOrWait(word, 1) run until the busy bit clears. Then VCALLMS 0 runs
// the microprogram and VF2, VF3, VF4, VF5 are stored at out+0, +16, +32, +48 (out = a1).
//
// Retired counts (delay slots included): 16 (9 for slot 255), 13 (16 when A >= N-1), the pass loop 124 (a constant
// count, so it is straight code), the DMA test 5, the start 13, each wait pass 8 + callees, the tail 35. Only the busy
// wait is data dependent, and it is bounded by kCeiling. The one-time start (the flag not 0x004839e0) always declines:
// the busy word is the DMA channel's CHCR at 0x10008000, which the start has just set, so a dry run never sees it clear
// and the call runs in the original. The VCALLMS runs only on guest memory: a dry run skips it, since its outputs are
// not read by anything that changes the count.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/guest/vu0_micro_interpreter.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>

N3_CALLEE(00103d20)
N3_CALLEE(001042f0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kSlotTable = 0x0052288cU;      // lui 0x52, addiu 9824, addiu 556: 76-byte entries
constexpr std::uint32_t kSlotBytes = 76U;
constexpr std::uint32_t kNoSlot = 255U;
constexpr GuestAddress kSlotByte = 65U;               // O + 65
constexpr GuestAddress kCurveWord = 52U;              // O + 52: T, the float in VF2.x
constexpr GuestAddress kPairTable = 0U;               // record + 0
constexpr GuestAddress kQuadTable = 12U;              // record + 12
constexpr GuestAddress kCount = 16U;                  // record + 16
constexpr GuestAddress kConstantsA = 0x00521640U;     // VF11..VF14
constexpr GuestAddress kConstantsB = 0x00521680U;     // VF15..VF18
constexpr std::uint32_t kOneBits = 0x3f800000U;
constexpr std::uint8_t kX = 8U;                       // destination masks (x is the high bit)
constexpr std::uint8_t kY = 4U;
constexpr std::uint8_t kZ = 2U;
constexpr std::uint8_t kW = 1U;
constexpr std::uint32_t kFrameBytes = 384U;
constexpr GuestAddress kFlagOffset = 0U - 22808U;     // gp-relative: the DMA tag word
constexpr std::uint32_t kDmaTag = 0x004839e0U;
constexpr GuestAddress kDmaCountRegister = 0x10008020U;
constexpr GuestAddress kDmaTagRegister = 0x10008030U;
constexpr GuestAddress kDmaControlRegister = 0x10008000U;
constexpr std::uint32_t kDmaStart = 325U;
constexpr GuestAddress kVcallmsAddress = 0x00161af0U;
constexpr std::uint16_t kMicroEntry = 0U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kSlotCost = 16U;              // 0x001618c0-0x00161908 for a slot other than 255
constexpr std::uint32_t kNoSlotCost = 9U;             // 0x001618c0-0x00161908 for slot 255
constexpr std::uint32_t kInRangeCost = 13U;           // 0x00161908-0x00161948 with the bne taken (A < N-1)
constexpr std::uint32_t kClampCost = 16U;             // the same block, then 0x0016193c-0x00161944 (A = N-2)
constexpr std::uint32_t kPassesCost = 124U;           // 0x00161948-0x00161a18: 29 + 5 tests of 3 + 4 bodies of 20
constexpr std::uint32_t kGlobalTestCost = 5U;         // 0x00161a18-0x00161a28
constexpr std::uint32_t kStartCost = 13U;             // 0x00161a2c-0x00161a68 on the one-time start
constexpr std::uint32_t kWaitPassCost = 8U;           // 0x00161a6c-0x00161a88 without the callees
constexpr std::uint32_t kReturnCost = 35U;            // 0x00161a8c-0x00161b14

// The busy wait can spin for as long as the flag stays set: a run above this is left to the original (the dry run
// returns 0), so a live run never needs more than this.
constexpr std::uint32_t kCeiling = 20000U;

// A 32-bit result as the 128-bit register holds it: the low word, its sign extension, and the upper 64 bits kept.
[[nodiscard]] constexpr Gpr128 widened(const std::uint32_t bits, const std::uint32_t upper_a,
                                       const std::uint32_t upper_b) noexcept {
    return Gpr128{{bits, (bits & 0x80000000U) != 0U ? 0xffffffffU : 0U, upper_a, upper_b}};
}

template <class Context>
n3::Run evaluate_curve_segment_vu0(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress object = call.arg(0U);
    const GuestAddress out = call.arg(1U);
    const Gpr128 object_register = call.arg128(0U);
    const Gpr128 caller_t2 = cpu.read_gpr(10U);
    const std::uint32_t parameter = cpu.read_fpr_bits(12U);
    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;

    // lbu $a2,65($a0), then the slot: 255 reads address 0, any other index the entry at 0x0052288c + 76 * index.
    const std::uint32_t slot_index = m.read_u8(object + kSlotByte);
    const bool has_slot = slot_index != kNoSlot;
    const GuestAddress slot = has_slot ? kSlotTable + kSlotBytes * slot_index : 0U;
    std::uint32_t retired = has_slot ? kSlotCost : kNoSlotCost;
    const GuestAddress record = m.read_u32_le(slot);
    const std::uint32_t curve = m.read_u32_le(object + kCurveWord);
    const std::uint32_t count = m.read_u32_le(record + kCount);

    // lqc2 vf1,224($sp) (stale frame words in lanes y..w), qmtc2 $a0,$vf2, vaddx.x vf1,vf0,vf2x. The stored VF1 comes
    // back unchanged before the microprogram, so it is kept as a local.
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, frame + 224U));
    cpu.write_vu0_vector(2U, widened(curve, object_register.words[2], object_register.words[3]));
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 2U, kX, 0U, false);
    const Gpr128 vf1_input = cpu.read_vu0_vector(1U);

    // A = trunc(parameter), compared with N-1 as the listing does; t = f - float(A).
    const auto last = static_cast<std::int32_t>(count - 1U);
    std::int32_t a3 = fpu::to_int(parameter);
    std::uint32_t f12 = parameter;
    if (a3 < last) {
        retired += kInRangeCost;
    } else {
        retired += kClampCost;
        f12 = fpu::from_int(last);
        a3 = static_cast<std::int32_t>(count - 2U);
    }
    const std::uint32_t t = fpu::sub(f12, fpu::from_int(a3));

    // qmtc2 $a0,$vf1 (VF1.x = 1.0), vmulx.w, qmtc2 $t2,$vf1 (only its lane x is read below), then the cubic basis.
    cpu.write_vu0_vector(1U, widened(kOneBits, object_register.words[2], object_register.words[3]));
    ee_cop2::vmul_broadcast(cpu, 2U, 0U, 1U, kW, 0U);
    cpu.write_vu0_vector(1U, widened(t, caller_t2.words[2], caller_t2.words[3]));
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, kZ, 0U, false);
    ee_cop2::vmul_broadcast(cpu, 1U, 2U, 2U, kZ, 2U);
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, kY, 2U, false);
    ee_cop2::vmul_broadcast(cpu, 1U, 2U, 2U, kZ, 1U);
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, kX, 2U, false);
    const Gpr128 basis = cpu.read_vu0_vector(2U);

    // The four passes. The index is min(max(A + k - 1, 0), N - 1) (pmaxw, pminw); the pair is at table + 4 * index and
    // its two halfwords select quadwords at quadtable + 16 * halfword. Pass k fills VF(2 + 2k) and VF(3 + 2k).
    const GuestAddress pairs = m.read_u32_le(record + kPairTable);
    const GuestAddress quads = m.read_u32_le(record + kQuadTable);
    Gpr128 quadwords[8];
    for (std::uint32_t pass = 0U; pass < 4U; ++pass) {
        const auto raw = static_cast<std::int32_t>(static_cast<std::uint32_t>(a3) + pass - 1U);
        const std::int32_t index = std::min(std::max(raw, 0), last);
        const GuestAddress entry = pairs + 4U * static_cast<GuestAddress>(index);
        const std::uint32_t first = m.read_u16_le(entry);
        const std::uint32_t second = m.read_u16_le(entry + 2U);
        quadwords[2U * pass] = n3::load_quad_any(m, quads + (first << 4U));
        quadwords[2U * pass + 1U] = n3::load_quad_any(m, quads + (second << 4U));
    }
    retired += kPassesCost;

    // The DMA start, once per tag, then the busy wait: WordTableLookup10(0), ReadBusyFlagOrWait(word, 1), repeated
    // while the callee returns 1.
    retired += kGlobalTestCost;
    const GuestAddress flag = gp + kFlagOffset;
    if (m.read_u32_le(flag) != kDmaTag) {
        m.write_u32_le(flag, kDmaTag);
        m.write_u32_le(kDmaCountRegister, 0U);
        m.write_u32_le(kDmaTagRegister, kDmaTag);
        m.write_u32_le(kDmaControlRegister, kDmaStart);
        retired += kStartCost;
        for (;;) {
            if (retired > kCeiling) {
                return call.finish(retired);
            }
            call.set_arg64(0U, 0U);
            const n3::Run lookup = call.invoke([](auto& machine) { return n3::fn_00103d20(machine); }, kFrameBytes);
            if (lookup.declined) {
                return call.decline();
            }
            call.set_arg64(0U, call.result64());
            call.set_arg(1U, 1U);
            call.set_arg64(2U, 0U);
            const n3::Run busy = call.invoke([](auto& machine) { return n3::fn_001042f0(machine); }, kFrameBytes);
            if (busy.declined) {
                return call.decline();
            }
            retired += kWaitPassCost + lookup.retired + busy.retired;
            if (call.result64() == 0U) {
                break;
            }
        }
    }

    // 0x00161a8c: the microprogram's inputs (VF1, VF2..VF9, the constants in VF11..VF18, the basis in VF19), then
    // VCALLMS 0 on guest memory, then the four output quadwords.
    retired += kReturnCost;
    cpu.write_vu0_vector(1U, vf1_input);
    for (std::uint32_t j = 0U; j < 8U; ++j) {
        cpu.write_vu0_vector(2U + j, quadwords[j]);
    }
    for (std::uint32_t i = 0U; i < 4U; ++i) {
        cpu.write_vu0_vector(11U + i, n3::load_quad_any(m, kConstantsA + 16U * i));
        cpu.write_vu0_vector(15U + i, n3::load_quad_any(m, kConstantsB + 16U * i));
    }
    cpu.write_vu0_vector(19U, basis);
    if constexpr (std::is_same_v<std::remove_cvref_t<decltype(m)>, guest::EeMemory>) {
        cpu.set_pc(kVcallmsAddress);
        const auto launch = guest::run_vu0_micro_call(cpu, m, kMicroEntry);
        if (launch.status != guest::Vu0MicroRunStatus::Completed) {
            // The interpreter stops with a VU0 execution fault at this VCALLMS; a native cannot return that status.
            throw std::runtime_error("EvaluateCurveSegmentVu0: VU0 microprogram 0 did not complete: " +
                                     std::string(guest::to_string(launch.status)));
        }
    }
    n3::store_quad_any(m, out + 32U, cpu.read_vu0_vector(4U));
    n3::store_quad_any(m, out + 16U, cpu.read_vu0_vector(3U));
    n3::store_quad_any(m, out + 48U, cpu.read_vu0_vector(5U));
    n3::store_quad_any(m, out, cpu.read_vu0_vector(2U));
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_LIVE_WORK(001618c0, 600U, 0x4e234b65d70d4c61ULL, "EvaluateCurveSegmentVu0", takedown::runtime::kCeiling,
         takedown::runtime::evaluate_curve_segment_vu0, EngineReturn::Nothing)
