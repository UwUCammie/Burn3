// 0x00274af0 (a0 = block A, a1 = block B): LaunchVu0Program14A, the sibling of LaunchVu0Program143 (0x00274bc0). The
// word at gp-22808 chooses the path. When it is 0x004787d0 (the tag the start below leaves there) the function loads
// four quadwords from B (+0, +16, +32, +48) into VF1..VF4 and six from A (+32, +48, +64, +80, +0, +16) into VF9..VF14,
// runs VU0 microprogram 0x14a (VCALLMS at 0x00274ba0) and returns the low 16 bits of VI1. Otherwise it is the first
// call of the run: it stores the tag, starts the DMA channel (CHCR 0x10008000, QWC 0x10008020, TADR 0x10008030) and
// loops on WordTableLookup10(0) and ReadBusyFlagOrWait(word, 1) until the busy bit clears. A dry run never sees that
// bit clear, so that path declines and the original runs it (as LaunchVu0Program143 and DrawSlotVu0Launch do for their
// one-time start).
//
// Steady path: 28 instructions retired (0x00274af0-0x00274b14 and 0x00274b78-0x00274bbc), no loop, no call. The
// original's frame saves and reloads sit below the entry $sp, which the check ignores, and the registers they restore
// are never changed here. A dry run (call_cost) cannot run the microprogram, so it skips the launch; the count does not
// depend on the launch. A nested dry run (entry address 0: this native called from another native's dry run) declines,
// since its caller may branch on the VI1 result that a dry run does not compute.

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
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint32_t kStartedTag = 0x004787d0U;        // lui 0x48, addiu -30768: the word the start leaves at gp-22808
constexpr GuestAddress kStartedFlagOffset = 0U - 22808U;  // gp-relative
constexpr GuestAddress kVcallmsAddress = 0x00274ba0U;
constexpr std::uint16_t kMicroEntry = 0x14aU;             // VCALLMS 0x14a
// Retired on the steady path: 0x00274af0-0x00274b14 (ten) and 0x00274b78-0x00274bbc (eighteen).
constexpr std::uint32_t kSteadyCost = 28U;

// lqc2 faults unless the address is 16-byte aligned and inside guest memory (ee_interpreter.cpp, Lqc2).
template <class Memory>
bool quad_loadable(const Memory& memory, const GuestAddress address) {
    return (address & 15U) == 0U && memory.contains(address, 16U);
}

template <class Context>
n3::Run launch_vu0_program_14a(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    constexpr bool kLive = std::is_same_v<std::remove_cvref_t<decltype(m)>, guest::EeMemory>;

    const GuestAddress gp = cpu.read_gpr_u32(n3::kGlobalPointer);
    if (m.read_u32_le(gp + kStartedFlagOffset) != kStartedTag) {
        // The first call of a run: the DMA start and its busy wait stay with the original.
        return call.decline();
    }
    const GuestAddress a0 = call.arg(0U);
    const GuestAddress a1 = call.arg(1U);
    const bool loadable = quad_loadable(m, a1 + 0U) && quad_loadable(m, a1 + 16U) && quad_loadable(m, a1 + 32U) &&
                          quad_loadable(m, a1 + 48U) && quad_loadable(m, a0 + 0U) && quad_loadable(m, a0 + 16U) &&
                          quad_loadable(m, a0 + 32U) && quad_loadable(m, a0 + 48U) && quad_loadable(m, a0 + 64U) &&
                          quad_loadable(m, a0 + 80U);
    if (!loadable) {
        return call.decline();  // the original's lqc2 faults on this address
    }
    call.set_arg(0U, kStartedTag);  // lui/addiu $a0 leave a0 = 0x004787d0 on every path, the steady one included

    if constexpr (kLive) {
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, a1 + 0U));    // lqc2 vf1,0(s0)
        cpu.write_vu0_vector(2U, n3::load_quad_any(m, a1 + 16U));   // lqc2 vf2,16(s0)
        cpu.write_vu0_vector(3U, n3::load_quad_any(m, a1 + 32U));   // lqc2 vf3,32(s0)
        cpu.write_vu0_vector(4U, n3::load_quad_any(m, a1 + 48U));   // lqc2 vf4,48(s0)
        cpu.write_vu0_vector(9U, n3::load_quad_any(m, a0 + 32U));   // lqc2 vf9,32(s1)
        cpu.write_vu0_vector(10U, n3::load_quad_any(m, a0 + 48U));  // lqc2 vf10,48(s1)
        cpu.write_vu0_vector(11U, n3::load_quad_any(m, a0 + 64U));  // lqc2 vf11,64(s1)
        cpu.write_vu0_vector(12U, n3::load_quad_any(m, a0 + 80U));  // lqc2 vf12,80(s1)
        cpu.write_vu0_vector(13U, n3::load_quad_any(m, a0 + 0U));   // lqc2 vf13,0(s1)
        cpu.write_vu0_vector(14U, n3::load_quad_any(m, a0 + 16U));  // lqc2 vf14,16(s1)
        cpu.set_pc(kVcallmsAddress);
        const auto launch = guest::run_vu0_micro_call(cpu, m, kMicroEntry);
        if (launch.status != guest::Vu0MicroRunStatus::Completed) {
            // The interpreter stops with a VU0 execution fault at this VCALLMS; a native cannot return that status.
            throw std::runtime_error("LaunchVu0Program14A: VU0 microprogram 0x14a did not complete: " +
                                     std::string(guest::to_string(launch.status)));
        }
        call.return_value(ee_cop2::cfc2(cpu, 1U) & 0xffffU);  // cfc2.i v0,vi1 then andi v0,v0,0xffff
    } else if (context.entry_address == 0U) {
        // A dry run nested in another native's dry run: the caller may branch on the VI1 result, which a dry run lacks.
        return call.decline();
    }
    return call.finish(kSteadyCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_LIVE_WORK(00274af0, 208U, 0x51c344670db35b65ULL, "LaunchVu0Program14A", kSteadyCost, launch_vu0_program_14a,
         EngineReturn::Integer)
