// 0x0026a710 (a0 = object O, a1 = record index k): SlotRecordVu0Step. The signed byte at O+132 is the slot, and the
// index byte goes to O+slot+135 before anything else is read. The record is 560 bytes at 0x00665ec0 + 560*k (32-bit
// wrap), with its quadwords at t1 = record + 1136 and t1 + 384. c.ole.s compares f1 (the W word of the second quadword)
// with f0 (the float at O+144); when f1 is not <= f0, the first quadword's W lane becomes the VU0 product of vf0.w and
// (first.w - (f1 - f0)), made on the real VU0 state. VF1..VF6 are then loaded from t1 (+32, +48, +64, +80, the chosen
// first quadword, +16), the VU0 microprogram at entry 0x18 runs (VCALLMS), and the low byte of VI1 is stored at O+132.
// Returns v0 = 1. No loops and no call_cost: 40 instructions retired without the W patch, 51 with it.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/guest/vu0_micro_interpreter.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kRecordTable = 0x00665ec0U;   // lui 0x66, addiu 24256
constexpr std::uint32_t kRecordBytes = 560U;         // (k*7)*5*16
constexpr GuestAddress kSlotByte = 132U;             // signed slot byte; the VI1 byte is stored back here
constexpr GuestAddress kIndexByte = 135U;            // index byte at O + slot + 135
constexpr GuestAddress kFloatOffset = 144U;          // f0 is the float at O + 144
constexpr GuestAddress kFirstQuad = 1136U;           // t1 = record + 1136
constexpr GuestAddress kSecondQuadOffset = 384U;     // the second quadword is t1 + 384 (record + 1520)
constexpr std::uint16_t kMicroEntry = 0x18U;         // VCALLMS 0x18
constexpr GuestAddress kVcallmsAddress = 0x0026a7c4U;
constexpr std::uint32_t kSkipCost = 40U;             // retired without the W patch
constexpr std::uint32_t kPatchCost = 51U;            // retired with it

template <class Context>
n3::Run slot_record_vu0_step(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    if constexpr (!std::is_same_v<std::remove_cvref_t<decltype(m)>, guest::EeMemory>) {
        // The VU0 microprogram runs only on guest memory, so another native's dry run cannot include this call:
        // it declines, and the caller is left to the original.
        return call.decline();
    } else {
    const GuestAddress object = call.arg(0);
    const std::uint32_t record_index = call.arg(1);

    // lb $t0,132($a0), then sb $a1,135($a2) with a2 = O + slot: the index byte goes out before anything else is read.
    const auto slot_byte = static_cast<std::int8_t>(m.read_u8(object + kSlotByte));
    const auto slot = static_cast<GuestAddress>(static_cast<std::int32_t>(slot_byte));
    m.write_u8(object + slot + kIndexByte, static_cast<std::uint8_t>(record_index));

    // lq of the two quadwords (t1 and t1 + 384 are 16-byte aligned), lwc1 of f0, and f1 = word 3 of the second quadword.
    const GuestAddress t1 = kRecordTable + kRecordBytes * record_index + kFirstQuad;
    const guest::Gpr128 first = n3::load_quad_any(m, t1);
    const std::uint32_t f0 = m.read_u32_le(object + kFloatOffset);
    const guest::Gpr128 second = n3::load_quad_any(m, t1 + kSecondQuadOffset);
    const std::uint32_t f1 = second.words[3];

    // c.ole.s $f1,$f0 and bc1t: when f1 <= f0 the W patch is skipped (its delay-slot store is the first quadword).
    guest::Gpr128 patched = first;
    std::uint32_t retired = kSkipCost;
    if (!fpu::le(f1, f0)) {
        const std::uint32_t d = fpu::sub(f1, f0);                          // sub.s $f1,$f1,$f0
        const std::uint32_t w = fpu::sub(first.words[3], d);               // sub.s $f0,$f0,$f1 with f0 = first.w
        cpu.write_vu0_vector(2U, first);                                   // qmtc2 $a1,$vf2
        cpu.write_gpr_sign_extended_32(2U, w);                             // mfc1 $v0,$f0
        cpu.write_vu0_vector(1U, cpu.read_gpr(2U));                        // qmtc2 $v0,$vf1
        ee_cop2::vmul_broadcast(cpu, 2U, 0U, 1U, n3::kMaskW, 0U);         // vmulx.w $vf2,$vf0,$vf1x
        patched = cpu.read_vu0_vector(2U);                                 // sqc2 $vf2 and lq back as VF5
        retired = kPatchCost;
    }

    // qmtc2 / qmtc2.i of the record's quadwords into VF1..VF6, then the VCALLMS 0x18 launch. The VCALLMS sits at
    // 0x0026a7c4; its pc is set so the VU0 observer sees the same caller as the interpreter.
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, t1 + 32U));
    cpu.write_vu0_vector(2U, n3::load_quad_any(m, t1 + 48U));
    cpu.write_vu0_vector(3U, n3::load_quad_any(m, t1 + 64U));
    cpu.write_vu0_vector(4U, n3::load_quad_any(m, t1 + 80U));
    cpu.write_vu0_vector(5U, patched);
    cpu.write_vu0_vector(6U, n3::load_quad_any(m, t1 + 16U));
    cpu.set_pc(kVcallmsAddress);
    const auto launch = guest::run_vu0_micro_call(cpu, m, kMicroEntry);
    if (launch.status != guest::Vu0MicroRunStatus::Completed) {
        // The interpreter stops with a VU0 execution fault at this VCALLMS; a native cannot return that status.
        throw std::runtime_error("SlotRecordVu0Step: VU0 microprogram 0x18 did not complete: " +
                                 std::string(guest::to_string(launch.status)));
    }

    // cfc2.i $v0,$vi1 then sb $v0,132($a0): the low byte of VI1 (a zero-extended 16-bit register) replaces the slot byte.
    m.write_u8(object + kSlotByte, static_cast<std::uint8_t>(ee_cop2::cfc2(cpu, 1U)));
    call.return_value(1U);
    return call.finish(retired);
    }
}

} // namespace

extern constinit const EngineFunction n3_0026a710{0x0026a710U, 204U, 0xbcf6e25297cd1494ULL, "SlotRecordVu0Step",
                                                  kPatchCost, n3::live<&slot_record_vu0_step<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(0026a710, slot_record_vu0_step)
