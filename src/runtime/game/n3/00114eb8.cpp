// 0x00114eb8 (no arguments): EnableInterruptsReturnPrevious, the EE kernel's EIntr wrapper. Returns in v0 whether
// Status.EIE was set on entry (1) or already clear (0). The original is `mfc0 v0,$status; lui v1,1; and v0,v0,v1;
// ei; jr ra; sltu v0,zero,v0`. The flag is read before the EI, and the EI is the architectural one:
// R5900State::execute_cop0_ei applies the kernel-mode check itself, so Status is not written by hand here. The native
// checker does not compare COP0, so a passing check does not prove this write; it is the interpreter's EI path.
// v1 ends as the lui left it (0x10000).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

constexpr unsigned kCop0Status = 12U;
constexpr unsigned kV1 = 3U;
constexpr std::uint32_t kStatusEie = 0x00010000U;

// No branches: mfc0, lui, and, ei, jr and its delay slot (sltu) always retire, so every call counts 6.
constexpr std::uint32_t kCost = 6U;

template <class Context>
n3::Run enable_interrupts_return_prev(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    // Read Status before the EI: the result is whether EIE was already set on entry.
    const std::uint32_t status = cpu.read_cop0(kCop0Status);
    cpu.execute_cop0_ei();
    cpu.write_gpr_u64(kV1, kStatusEie);
    call.return_value((status & kStatusEie) != 0U ? 1U : 0U);
    return call.finish(kCost);
}

} // namespace

extern constinit const EngineFunction n3_00114eb8{0x00114eb8U, 24U, 0x260c30e6c0c0dc70ULL,
                                                  "EnableInterruptsReturnPrevious", kCost,
                                                  n3::live<&enable_interrupts_return_prev<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00114eb8, enable_interrupts_return_prev)
