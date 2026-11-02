// 0x00275d20 (a0 = Q0, a1 = Q1, a2 = Q2, a3 = Q3, t0 = output quadword): LaunchVu0Program12. It loads VU0 registers
// VF1..VF6 from the four quadwords and runs VU0 microprogram 0x0c (VCALLMS at 0x00275e7c). VF8 is stored at the output
// quadword and the result is VI1 != 0.
//
// The inputs are built as the listing builds them. VF1 is Q0 with its w lane squared (VMULw.w). That square s goes
// through f0 and back through a GPR: mfc1 sign-extends it into the low half, and the GPR keeps the squared quadword's z
// and w in its upper half. So VF1 becomes (s, sign of s, Q0.z, s), and three VADDx ops add zero to it into x, y and z
// of VF2. VF3 is that VF2 quadword after the listing stores and reloads it. Its w lane is the dead-frame word VF2 starts
// from; the listing writes that word back unchanged, so the native reads it from memory at entry and writes nothing.
// VF2 then becomes Q0, and VF4..VF6 hold the x, y and z words of Q1..Q3 with w = 0.
//
// Only the steady path is native. The first call of a run (gp-22808 not yet 0x004787d0) starts the DMA channel and
// busy-waits through 00103d20 and 001042f0, so it declines, as 00274bc0 does. A nested dry run (entry address 0)
// declines too: its caller may branch on the VI1 result, which a dry run does not compute. The listing's frame copies
// are not reproduced, and only a live run touches VU0. A dry run retires the same 72 instructions without the
// microprogram.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/guest/vu0_micro_interpreter.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace ee_cop2 = guest::ee_cop2;

// A VU0 register or a quadword in memory: word 0 is lane x, word 3 is lane w.
using Vec4 = Gpr128;

constexpr std::uint32_t kStartedTag = 0x004787d0U;        // lui 0x48, addiu -30768: the word the start leaves at gp-22808
constexpr GuestAddress kStartedFlagOffset = 0U - 22808U;  // gp-relative
constexpr GuestAddress kVcallmsAddress = 0x00275e7cU;
constexpr std::uint16_t kMicroEntry = 0x0cU;              // VCALLMS 0xc
constexpr std::uint32_t kFrameBytes = 224U;               // the frame is [entry sp - 224, entry sp)
constexpr std::uint32_t kStaleOffset = 144U;              // frame-relative: the dead quadword VF2 is first loaded from
// Retired on the steady path: 0x00275d20-0x00275d5c (sixteen, the taken branch's delay slot included) and
// 0x00275dc0-0x00275e9c (fifty-six, the return's delay slot included).
constexpr std::uint32_t kSteadyCost = 72U;

// VU destination masks (x is the high bit).
constexpr std::uint8_t kX = 8U;
constexpr std::uint8_t kY = 4U;
constexpr std::uint8_t kZ = 2U;
constexpr std::uint8_t kW = 1U;

// The four input quadwords (the listing's lq copies of a0..a3) and the dead frame quadword VF2 starts from.
struct Inputs {
    Vec4 q0;
    Vec4 q1;
    Vec4 q2;
    Vec4 q3;
    Vec4 stale;
};

// The sign extension of a 32-bit value: the upper word of the sign-extended 64-bit GPR value.
[[nodiscard]] constexpr std::uint32_t sign_word(const std::uint32_t bits) noexcept {
    return (bits & 0x80000000U) != 0U ? 0xffffffffU : 0U;
}

// lq and sq ignore the low four address bits; the aligned quadword must be in memory.
template <class Memory>
bool quad_mapped(const Memory& memory, const GuestAddress address) {
    return memory.contains(address & ~15U, 16U);
}

// The listing reaches its frame with lqc2, sqc2, lwc1, lwu and sq: the frame must be 16-byte aligned and mapped.
template <class Memory>
bool frame_usable(const Memory& memory, const GuestAddress frame) {
    return (frame & 15U) == 0U && memory.contains(frame, kFrameBytes);
}

// VF4..VF6 from a quadword: lwu of words 0..2 and PCPYLD give (x, y, z, 0).
Vec4 packed_operand(const Vec4& quad) {
    return Vec4{{quad.words[0], quad.words[1], quad.words[2], 0U}};
}

// The VU0 side of the listing before VCALLMS, in its order. The four flag-publishing VU ops (VMULw.w and the three
// VADDx) run in the listing's order, so the MAC and STATUS flags come out as the listing leaves them. Returns f0, the
// squared w lane, which the listing also leaves in f0.
std::uint32_t load_vu0_inputs(guest::R5900State& cpu, const Inputs& in) {
    cpu.write_vu0_vector(1U, in.q0);
    ee_cop2::vmul_broadcast(cpu, 1U, 1U, 1U, kW, 3U);  // vmulw.w vf1, vf1, vf1w
    const Vec4 squared = cpu.read_vu0_vector(1U);
    const std::uint32_t f0 = squared.words[3];
    const Vec4 through_a3{{f0, sign_word(f0), squared.words[2], squared.words[3]}};
    const Vec4 through_t0{{f0, sign_word(f0), in.q1.words[2], in.q1.words[3]}};
    cpu.write_vu0_vector(2U, in.stale);
    cpu.write_vu0_vector(1U, through_a3);
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, kX, 0U, false);  // vaddx.x vf2, vf0, vf1x
    cpu.write_vu0_vector(1U, through_t0);
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, kY, 0U, false);  // vaddx.y vf2, vf0, vf1x
    cpu.write_vu0_vector(1U, through_a3);
    ee_cop2::vadd_broadcast(cpu, 2U, 0U, 1U, kZ, 0U, false);  // vaddx.z vf2, vf0, vf1x
    cpu.write_vu0_vector(3U, cpu.read_vu0_vector(2U));        // the listing stores vf2 to the frame and reloads it as vf3
    cpu.write_vu0_vector(2U, in.q0);
    cpu.write_vu0_vector(4U, packed_operand(in.q1));
    cpu.write_vu0_vector(5U, packed_operand(in.q2));
    cpu.write_vu0_vector(6U, packed_operand(in.q3));
    return f0;
}

// VCALLMS 0xc, run as the interpreter runs it. A fault at the launch cannot be returned as a status, so it throws, as
// 00274bc0 does.
void launch_microprogram(guest::R5900State& cpu, guest::EeMemory& memory) {
    cpu.set_pc(kVcallmsAddress);
    const auto launch = guest::run_vu0_micro_call(cpu, memory, kMicroEntry);
    if (launch.status != guest::Vu0MicroRunStatus::Completed) {
        throw std::runtime_error("LaunchVu0Program12: VU0 microprogram 0xc did not complete: " +
                                 std::string(guest::to_string(launch.status)));
    }
}

template <class Context>
n3::Run launch_vu0_program_12(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    constexpr bool kLive = std::is_same_v<std::remove_cvref_t<decltype(m)>, guest::EeMemory>;

    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    if (m.read_u32_le(gp + kStartedFlagOffset) != kStartedTag) {
        // The first call of a run: the DMA start and its busy wait stay with the original.
        return call.decline();
    }
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    const GuestAddress q0_address = call.arg(0U);
    const GuestAddress q1_address = call.arg(1U);
    const GuestAddress q2_address = call.arg(2U);
    const GuestAddress q3_address = call.arg(3U);
    const GuestAddress output = call.arg(4U);
    const bool usable = quad_mapped(m, q0_address) && quad_mapped(m, q1_address) && quad_mapped(m, q2_address) &&
                        quad_mapped(m, q3_address) && frame_usable(m, frame) && quad_mapped(m, output);
    if (!usable) {
        return call.decline();  // the original's lq, lqc2, sqc2 or sq would fault on one of these addresses
    }

    if constexpr (kLive) {
        const Inputs in{
            .q0 = n3::load_quad_any(m, q0_address),
            .q1 = n3::load_quad_any(m, q1_address),
            .q2 = n3::load_quad_any(m, q2_address),
            .q3 = n3::load_quad_any(m, q3_address),
            .stale = n3::load_quad_any(m, frame + kStaleOffset),
        };
        const std::uint32_t f0 = load_vu0_inputs(cpu, in);
        launch_microprogram(cpu, m);
        const std::uint32_t vi1 = ee_cop2::cfc2(cpu, 1U);
        const Vec4 vf8 = cpu.read_vu0_vector(8U);
        n3::store_quad_any(m, output, vf8);
        cpu.write_fpr_bits(0U, f0);
        // qmfc2 $v0, $vf8 wrote all of v0; the sltu after it replaced only the low doubleword with the result.
        cpu.write_gpr(2U, Vec4{{vi1 != 0U ? 1U : 0U, 0U, vf8.words[2], vf8.words[3]}});
    } else if (context.entry_address == 0U) {
        // A dry run nested in another native's dry run: its caller may branch on VI1, which a dry run does not compute.
        return call.decline();
    }
    return call.finish(kSteadyCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_LIVE_WORK(00275d20, 384U, 0x6f99b47213e528bbULL, "LaunchVu0Program12", kSteadyCost, launch_vu0_program_12,
         EngineReturn::Integer)
