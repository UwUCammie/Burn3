// 0x00274bc0 (a0 = block A, a1 = block B): LaunchVu0Program143. The word at gp-22808 chooses the path. When it is
// 0x004787d0 (the value the start below leaves there) the function loads ten quadwords, from B (+0, +16, +32, +48) into
// VF7, VF8, VF6, VF1 and from A (+0, +16, +32, +48, +64, +80) into VF13, VF14, VF9..VF12, runs VU0 microprogram 0x143
// (VCALLMS at 0x00274c70) and returns the low 16 bits of VI1. Otherwise it is the first call of the run: it stores
// 0x004787d0 at gp-22808, starts the DMA channel (CHCR 0x10008000, QWC 0x10008020, TADR 0x10008030) and waits on the
// busy bit of WordTableLookup10(0) through ReadBusyFlagOrWait. The busy bit clears when the emulator finishes the
// transfer, which a dry run does not model, so that path declines and the original runs it (as DrawSlotVu0Launch,
// 0x0026a7e0, does for its one-time start).
//
// Steady path: 28 instructions retired (0x00274bc0-0x00274be4 and 0x00274c48-0x00274c8c). It stores nothing outside the
// original's frame, which the check ignores. A dry run (call_cost) cannot run the microprogram, so it skips the launch;
// the count does not depend on the launch. A nested dry run (entry address 0: this native called from another native's
// dry run) declines, since its caller may branch on the VI1 result that a dry run does not compute.

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
constexpr GuestAddress kVcallmsAddress = 0x00274c70U;
constexpr std::uint16_t kMicroEntry = 0x143U;             // VCALLMS 0x143
// Retired on the steady path: 0x00274bc0-0x00274be4 (ten) and 0x00274c48-0x00274c8c (eighteen).
constexpr std::uint32_t kSteadyCost = 28U;

// lqc2 faults unless the address is 16-byte aligned and inside guest memory (ee_interpreter.cpp, Lqc2).
template <class Memory>
bool quad_loadable(const Memory& memory, const GuestAddress address) {
    return (address & 15U) == 0U && memory.contains(address, 16U);
}

template <class Context>
n3::Run launch_vu0_program_143(Context& context) {
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

    if constexpr (kLive) {
        cpu.write_vu0_vector(7U, n3::load_quad_any(m, a1 + 0U));
        cpu.write_vu0_vector(8U, n3::load_quad_any(m, a1 + 16U));
        cpu.write_vu0_vector(6U, n3::load_quad_any(m, a1 + 32U));
        cpu.write_vu0_vector(1U, n3::load_quad_any(m, a1 + 48U));
        cpu.write_vu0_vector(13U, n3::load_quad_any(m, a0 + 0U));
        cpu.write_vu0_vector(14U, n3::load_quad_any(m, a0 + 16U));
        cpu.write_vu0_vector(9U, n3::load_quad_any(m, a0 + 32U));
        cpu.write_vu0_vector(10U, n3::load_quad_any(m, a0 + 48U));
        cpu.write_vu0_vector(11U, n3::load_quad_any(m, a0 + 64U));
        cpu.write_vu0_vector(12U, n3::load_quad_any(m, a0 + 80U));
        cpu.set_pc(kVcallmsAddress);
        const auto launch = guest::run_vu0_micro_call(cpu, m, kMicroEntry);
        if (launch.status != guest::Vu0MicroRunStatus::Completed) {
            // The interpreter stops with a VU0 execution fault at this VCALLMS; a native cannot return that status.
            throw std::runtime_error("LaunchVu0Program143: VU0 microprogram 0x143 did not complete: " +
                                     std::string(guest::to_string(launch.status)));
        }
        call.return_value(ee_cop2::cfc2(cpu, 1U) & 0xffffU);
    } else if (context.entry_address == 0U) {
        // A dry run nested in another native's dry run: the caller may branch on the VI1 result, which a dry run lacks.
        return call.decline();
    }
    return call.finish(kSteadyCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00274bc0, 208U, 0xd148ff828d58921fULL, "LaunchVu0Program143", kSteadyCost, launch_vu0_program_143,
         EngineReturn::Integer)
