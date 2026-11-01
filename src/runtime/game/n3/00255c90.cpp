// 0x00255c90 (a0 = fade object F): AdvanceFadeTimer, a leaf with no result. F holds a float elapsed time at +0 and an
// int state at +8. Each call stores elapsed + step * count at +0, where step is the float at 0x004f509c and count the
// int word at 0x0051bac4 converted to float (ADDA.S from +0.0, then MADD.S). The state then decides the rest: states
// 1 and 2 reach the call to 0x002276c0 on every path (never seen in the recorded game), so call_cost declines them and
// the original runs. Any other state returns here in 22 retired instructions.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr unsigned kV1 = 3U;
constexpr unsigned kA0 = 4U;
constexpr unsigned kA1 = 5U;

constexpr GuestAddress kStep = 0x004f509cU;   // lui 0x4f, 20636 (float)
constexpr GuestAddress kCount = 0x0051bac4U;  // lui 0x52, -17724 (int word)
constexpr GuestAddress kElapsed = 0U;         // F + 0 (float)
constexpr GuestAddress kState = 8U;           // F + 8 (int)
constexpr std::uint32_t kColdStateA = 1U;
constexpr std::uint32_t kColdStateB = 2U;

// Retired instructions on the path that returns: 16 up to the state test (its delay slot included), the state 2 test
// (addiu, bne and its delay slot), then ld ra, jr and the delay slot of jr.
constexpr std::uint32_t kReturnCost = 22U;
// The listing's longest path (brief.py). Declined calls never run natively, so this only bounds the reported count.
constexpr std::uint32_t kDeclaredCost = 65U;

// The values the prologue leaves behind.
struct FadeStep {
    std::uint32_t step{};   // f3
    std::uint32_t count{};  // f2: the count word through CVT.S.W
    std::uint32_t acc{};    // ACC after ADDA.S
    std::uint32_t next{};   // f1, stored at +0
    std::uint32_t state{};  // a1
};

// The prologue on guest memory (the implementation) or on a DryMemory (call_cost). The loads all come before the store,
// as in the listing, and the state is read after it.
template <class Memory>
FadeStep advance_prologue(Memory& m, const GuestAddress fade) {
    FadeStep s;
    s.step = m.read_u32_le(kStep);
    s.count = fpu::from_int(static_cast<std::int32_t>(m.read_u32_le(kCount)));
    s.acc = fpu::add(0U, m.read_u32_le(fade + kElapsed));
    s.next = fpu::madd(s.acc, s.step, s.count);
    m.write_u32_le(fade + kElapsed, s.next);
    s.state = m.read_u32_le(fade + kState);
    return s;
}

// Declines the cold states, and the objects the original would fault on (a misaligned or unmapped one), so the
// original runs those calls.
template <class Context>
std::uint32_t advance_fade_timer_cost(const Context& context) {
    const GuestAddress fade = context.cpu.read_gpr_u32(kA0);
    if ((fade & 3U) != 0U || !context.memory.contains(fade, 12U)) {
        return 0U;
    }
    n3::DryMemory dry{context.memory};
    const FadeStep s = advance_prologue(dry, fade);
    return (s.state == kColdStateA || s.state == kColdStateB) ? 0U : kReturnCost;
}

template <class Context>
n3::Run advance_fade_timer(Context& context) {
    auto call = n3::guest_call(context);
    const FadeStep s = advance_prologue(call.memory(), call.arg(0));
    auto& cpu = call.cpu();
    cpu.write_fpr_bits(0U, 0U);                    // mtc1 zero,f0
    cpu.write_fpr_bits(1U, s.next);                // madd.s f1
    cpu.write_fpr_bits(2U, s.count);               // cvt.s.w f2
    cpu.write_fpr_bits(3U, s.step);                // lwc1 f3
    cpu.set_fpu_accumulator_bits(s.acc);           // adda.s
    cpu.write_gpr_sign_extended_32(kA1, s.state);  // lw a1,8(a0)
    cpu.write_gpr_sign_extended_32(kV1, 2U);       // addiu v1,zero,2
    // call_cost declines states 1 and 2, so only the other states reach this point.
    return call.finish(kReturnCost);
}

} // namespace

extern constinit const EngineFunction n3_00255c90{0x00255c90U, 452U, 0xe8a3c94ce9ae4afdULL, "AdvanceFadeTimer",
                                                  kDeclaredCost, n3::live<&advance_fade_timer<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &advance_fade_timer_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00255c90, advance_fade_timer, advance_fade_timer_cost)
