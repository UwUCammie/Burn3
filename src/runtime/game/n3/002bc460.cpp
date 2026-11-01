// 0x002bc460 (a0 = entity block E, f12 = time step dt): AdvancePhaseAndPublishEntries.
// Part 1: the phase float E+308 advances by 1.0*rate(E+312)*dt and keeps only its fractional part
// (cvt.w.s / cvt.s.w truncation, then sub.s); the result is stored back to E+308.
// Part 2: s4 = 0 only when both flag bytes 0x0064d378 and 0x0064d379 are zero, otherwise 1.
// Part 3: for i = 0 .. count-1 (signed word at 0x0064d3a0, re-read every pass):
//   s4 == 0: copy the raw float E+208+64*i to 0x00666120+160*i and go on;
//   s4 != 0: the original calls the guest routines 0x00251870 / 0x002bb420 / 0x002baf30 (never ran in the
//            recorded game) and then does a long VU0 sequence. That path is NOT implemented here: the
//            call_cost resolver returns 0 for any input that reaches it, so the original runs it.
// f20-f24, s0-s7, fp and ra are saved and restored by the original; v0/f0 are scratch.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kFlagA = 0x0064d379U;   // lui 0x65, -11399 (byte)
constexpr GuestAddress kFlagB = 0x0064d378U;   // lui 0x65, -11400 (byte)
constexpr GuestAddress kCount = 0x0064d3a0U;   // 0x00522660 + 0x0012ad40 (signed word, loaded each pass)
constexpr GuestAddress kCopyBase = 0x00666120U; // 0x00665ec0 + 608
constexpr GuestAddress kPhase = 308U;
constexpr GuestAddress kRate = 312U;
constexpr GuestAddress kSource = 208U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kPrologueCost = 33U;     // up to and including the first flag branch
constexpr std::uint32_t kSecondFlagCost = 4U;
constexpr std::uint32_t kClearFlagCost = 1U;
constexpr std::uint32_t kConstantsCost = 37U;
constexpr std::uint32_t kTestCost = 8U;
constexpr std::uint32_t kCopyCost = 13U;         // bne s4 + nop (2), address setup (1), copy block (10)
constexpr std::uint32_t kExitCost = 2U + 16U;    // beq + ld ra, epilogue and return

// Calls retiring more than this are left to the original (call_cost returns 0).
constexpr std::uint32_t kCeiling = 20000U;

struct Result {
    std::uint32_t retired;
    bool declined;
};

template <class Memory>
Result advance(Memory& m, const GuestAddress entity, const std::uint32_t dt) {
    // Part 1.
    const std::uint32_t rate = m.read_u32_le(entity + kRate);
    const std::uint32_t phase = m.read_u32_le(entity + kPhase);
    const std::uint32_t acc = fpu::add(0U, 0x3f800000U);
    const std::uint32_t step = fpu::madd(acc, rate, dt);
    const std::uint32_t sum = fpu::add(phase, step);
    const std::uint32_t whole = fpu::from_int(fpu::to_int(sum));
    m.write_u32_le(entity + kPhase, fpu::sub(sum, whole));

    // Part 2.
    std::uint32_t retired = kPrologueCost;
    bool active = true;
    if (m.read_u8(kFlagA) == 0U) {
        retired += kSecondFlagCost;
        if (m.read_u8(kFlagB) == 0U) {
            retired += kClearFlagCost;
            active = false;
        }
    }
    retired += kConstantsCost;

    // Part 3.
    std::uint32_t index = 0U;
    for (;;) {
        retired += kTestCost;
        if (static_cast<std::int32_t>(index) >= static_cast<std::int32_t>(m.read_u32_le(kCount))) {
            break;
        }
        if (active) {
            return {retired, true};
        }
        if (retired > kCeiling) {
            return {retired, true};
        }
        const std::uint32_t value = m.read_u32_le(entity + (index << 6U) + kSource);
        m.write_u32_le(kCopyBase + index * 160U, value);
        ++index;
        retired += kCopyCost;
    }
    return {retired + kExitCost, false};
}

template <class Context>
std::uint32_t advance_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const Result result = advance(dry, context.cpu.read_gpr_u32(4U), context.cpu.read_fpr_bits(12U));
    return (result.declined || result.retired > kCeiling) ? 0U : result.retired;
}

template <class Context>
n3::Run advance_phase(Context& context) {
    auto call = n3::guest_call(context);
    const Result result = advance(call.memory(), call.arg(0), call.cpu().read_fpr_bits(12U));
    return call.finish(result.retired);
}

} // namespace

extern constinit const EngineFunction n3_002bc460{0x002bc460U, 1296U, 0xbe036a06c55df5f5ULL,
                                                  "AdvancePhaseAndPublishEntries", kCeiling, n3::live<&advance_phase<NativeFunctionContext>>,
                                                  EngineReturn::Nothing, {}, 1e-4, 1e-6, {}, &advance_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(002bc460, advance_phase, advance_cost)
