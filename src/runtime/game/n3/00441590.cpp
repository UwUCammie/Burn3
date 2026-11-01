// 0x00441590 (a0 = fade object, f12 = frame delta dt): UpdateFadeStateMachine, the per-frame tick of a four-phase
// fade. The object: state byte +48 and counter byte +49 (both signed), float timers +40 and +44, quad pointers Q3 at
// +12, Q1 at +16 and Q2 at +20. dt doubles first when the byte at 0x0051baa6 is set. G2 (gp-32696) is what phase 0
// writes to +40 on its way to phase 1; G1 (gp-32192) is what phase 1 writes to +44 on its way to phase 2.
// Phase 0: t = [44] - dt, stored. t < 0 -> phase 1 and [40] = G2. Otherwise alpha = 1 - 2.5 t sets the W lane of the
// vec4 at Q1+16 (through VU0). The whole vec4 is copied to Q2+16 (its W lane is alpha, or +0.0 when the counter is
// 0), and when the counter is 3 the W lane of the vec4 at Q3+16 is alpha as well.
// Phase 1: [40] = [40] - dt, stored. Below zero -> phase 2 and [44] = G1.
// Phase 2: t = [44] - dt, stored. Below zero -> phase 3. Otherwise alpha = 2.5 t, written as in phase 0 (the Q3
// write happens when the counter is 1).
// Phase 3 calls two game functions that never ran in the recorded game (cold): call_cost returns 0 for it, so the
// original runs it. Other state values do nothing. Returns nothing (v0 and f0 are scratch).

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::R5900State;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr unsigned kV1 = 3U;
constexpr unsigned kA0 = 4U;

constexpr std::uint32_t kTwo = 0x40000000U;          // 2.0f
constexpr std::uint32_t kOne = 0x3f800000U;          // 1.0f
constexpr std::uint32_t kTwoAndHalf = 0x40200000U;   // 2.5f

constexpr GuestAddress kScaleFlag = 0x0051baa6U;                    // lbu (lui 0x52, -17754)
constexpr std::uint32_t kGpG1 = static_cast<std::uint32_t>(-32192);  // float, gp-relative
constexpr std::uint32_t kGpG2 = static_cast<std::uint32_t>(-32696);  // float, gp-relative

// Object fields.
constexpr GuestAddress kOffQ3 = 12U;        // quad pointer
constexpr GuestAddress kOffQ1 = 16U;        // quad pointer
constexpr GuestAddress kOffQ2 = 20U;        // quad pointer
constexpr GuestAddress kOffTimer40 = 40U;   // float (phase 1)
constexpr GuestAddress kOffTimer44 = 44U;   // float (phases 0 and 2)
constexpr GuestAddress kOffState = 48U;     // signed byte
constexpr GuestAddress kOffCounter = 49U;   // signed byte

constexpr std::uint8_t kPhase1 = 1U;
constexpr std::uint8_t kPhase2 = 2U;
constexpr std::uint8_t kPhase3 = 3U;

// The longest path through the listing, phase 3 included (the function has no loops).
constexpr std::uint32_t kDeclaredCost = 83U;

// Exit costs: 0x44187c (beq, ld) then the epilogue at 0x4418d4..0x4418e0, or 0x4418d0 (ld) then the same epilogue.
constexpr std::uint32_t kExitBranchCost = 6U;
constexpr std::uint32_t kExitLoadCost = 5U;

struct Outcome {
    std::uint32_t retired;
    bool cold;  // phase 3: the game calls it makes never ran in the recording, so the original runs the call
};

// The memory view for call_cost: a dry run that declines any access the original would fault on.
template <class Base>
class CheckedDry {
public:
    explicit CheckedDry(const Base& memory) noexcept : memory_(memory), dry_(memory) {}
    [[nodiscard]] std::uint8_t read_u8(const GuestAddress a) const {
        if (!memory_.contains(a, 1U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u8(a);
    }
    void write_u8(const GuestAddress a, const std::uint8_t v) {
        if (!memory_.contains(a, 1U)) {
            faulted_ = true;
            return;
        }
        dry_.write_u8(a, v);
    }
    [[nodiscard]] std::uint32_t read_u32_le(const GuestAddress a) const {
        if ((a & 3U) != 0U || !memory_.contains(a, 4U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u32_le(a);
    }
    void write_u32_le(const GuestAddress a, const std::uint32_t v) {
        if ((a & 3U) != 0U || !memory_.contains(a, 4U)) {
            faulted_ = true;
            return;
        }
        dry_.write_u32_le(a, v);
    }
    void write_u128_le(const GuestAddress a, const std::array<std::uint32_t, 4>& words) {
        if ((a & 15U) != 0U || !memory_.contains(a, 16U)) {
            faulted_ = true;
            return;
        }
        dry_.write_u128_le(a, words);
    }
    void check_quad(const GuestAddress a) const {
        if ((a & 15U) != 0U || !memory_.contains(a, 16U)) {
            faulted_ = true;
        }
    }
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

private:
    const Base& memory_;
    n3::DryMemory dry_;
    mutable bool faulted_ = false;
};

template <class Memory>
void check_quad(Memory& m, const GuestAddress a) {
    if constexpr (requires { m.check_quad(a); }) m.check_quad(a);
}

void mfc1(R5900State& st, const unsigned reg, const std::uint32_t bits) { st.write_gpr_sign_extended_32(reg, bits); }
void qmtc2(R5900State& st, const unsigned reg, const unsigned vf) { st.write_vu0_vector(vf, st.read_gpr(reg)); }

// mfc1 a0, f0; qmtc2 a0, vf2; lqc2 vf1, 16(record); vmulx.w vf1, vf0, vf2x; sqc2 vf1, 16(record). The W lane of the
// vec4 at record+16 becomes `bits` (through the VU0 multiply); the other lanes stay as they are in memory.
template <class Memory>
void set_w_lane(Memory& m, R5900State& st, const GuestAddress record, const std::uint32_t bits) {
    mfc1(st, kA0, bits);
    qmtc2(st, kA0, 2U);
    check_quad(m, record + 16U);
    st.write_vu0_vector(1U, n3::load_quad_any(m, record + 16U));
    ee_cop2::vmul_broadcast(st, 1U, 0U, 2U, n3::kMaskW, 0U);
    check_quad(m, record + 16U);
    n3::store_quad_any(m, record + 16U, st.read_vu0_vector(1U));
}

// mfc1 v1, f (f is +0.0 in both phases that use it); qmtc2 v1, vf2; vmulx.w: the W lane of VF1 becomes +0.0.
void zero_w_lane(R5900State& st) {
    mfc1(st, kV1, 0U);
    qmtc2(st, kV1, 2U);
    ee_cop2::vmul_broadcast(st, 1U, 0U, 2U, n3::kMaskW, 0U);
}

// The counter byte, sign-extended, read where the original reads it (after the writes before it).
template <class Memory>
std::int32_t counter_of(const Memory& m, const GuestAddress object) {
    return static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(object + kOffCounter)));
}

// The vec4 in VF1 (its W lane already set) is stored at Q2+16. The original round-trips it through the stack.
template <class Memory>
void store_copy(Memory& m, R5900State& st, const GuestAddress q2) {
    check_quad(m, q2 + 16U);
    n3::store_quad_any(m, q2 + 16U, st.read_vu0_vector(1U));
}

// Phase 0 (state 0): t = [44] - dt, stored to [44] whatever the outcome.
template <class Memory>
Outcome phase0(Memory& m, R5900State& st, const GuestAddress object, const std::uint32_t gp, const std::uint32_t dt,
               std::uint32_t retired) {
    retired += 6U;  // 0x4415f8..0x44160c
    const std::uint32_t t = fpu::sub(m.read_u32_le(object + kOffTimer44), dt);
    m.write_u32_le(object + kOffTimer44, t);
    if (fpu::lt(t, 0U)) {
        retired += 4U;  // 0x441884..0x441890
        const std::uint32_t g2 = m.read_u32_le(gp + kGpG2);
        m.write_u8(object + kOffState, kPhase1);
        m.write_u32_le(object + kOffTimer40, g2);
        return {retired + kExitBranchCost, false};
    }
    retired += 17U;  // 0x441610..0x441650: adda.s, msub.s give alpha = 1 - 2.5 t
    const std::uint32_t alpha = fpu::msub(fpu::add(0U, kOne), kTwoAndHalf, t);
    set_w_lane(m, st, m.read_u32_le(object + kOffQ1), alpha);
    if (counter_of(m, object) == 0) {
        retired += 5U;  // 0x441654..0x441664
        zero_w_lane(st);
    }
    retired += 7U;  // 0x441668..0x441680
    store_copy(m, st, m.read_u32_le(object + kOffQ2));
    if (counter_of(m, object) != 3) {
        return {retired + kExitLoadCost, false};
    }
    retired += 8U;  // 0x441684..0x4416a0
    set_w_lane(m, st, m.read_u32_le(object + kOffQ3), alpha);
    return {retired + kExitBranchCost, false};
}

// Phase 1 (state 1): [40] = [40] - dt, stored whatever the outcome. Below zero: phase 2 and [44] = G1.
template <class Memory>
Outcome phase1(Memory& m, const GuestAddress object, const std::uint32_t gp, const std::uint32_t dt,
               std::uint32_t retired) {
    retired += 7U;  // 0x4416a4..0x4416bc
    const std::uint32_t t = fpu::sub(m.read_u32_le(object + kOffTimer40), dt);
    m.write_u32_le(object + kOffTimer40, t);
    if (!fpu::lt(t, 0U)) {
        return {retired + kExitLoadCost, false};
    }
    retired += 4U;  // 0x4416c0..0x4416cc
    const std::uint32_t g1 = m.read_u32_le(gp + kGpG1);
    m.write_u8(object + kOffState, kPhase2);
    m.write_u32_le(object + kOffTimer44, g1);
    return {retired + kExitBranchCost, false};
}

// Phase 2 (state 2): t = [44] - dt, stored whatever the outcome. Below zero: phase 3 (cold, see Outcome).
template <class Memory>
Outcome phase2(Memory& m, R5900State& st, const GuestAddress object, const std::uint32_t dt, std::uint32_t retired) {
    retired += 7U;  // 0x4416d0..0x4416e8
    const std::uint32_t t = fpu::sub(m.read_u32_le(object + kOffTimer44), dt);
    m.write_u32_le(object + kOffTimer44, t);
    if (fpu::lt(t, 0U)) {
        retired += 2U;  // 0x441894..0x441898
        m.write_u8(object + kOffState, kPhase3);
        return {retired + kExitBranchCost, false};
    }
    retired += 15U;  // 0x4416ec..0x441724: alpha = 2.5 t
    const std::uint32_t alpha = fpu::mul(kTwoAndHalf, t);
    set_w_lane(m, st, m.read_u32_le(object + kOffQ1), alpha);
    if (counter_of(m, object) == 0) {
        retired += 5U;  // 0x441728..0x441738
        zero_w_lane(st);
    }
    retired += 7U;  // 0x44173c..0x441754
    store_copy(m, st, m.read_u32_le(object + kOffQ2));
    if (counter_of(m, object) != 1) {
        return {retired + kExitLoadCost, false};
    }
    retired += 8U;  // 0x441758..0x441774
    set_w_lane(m, st, m.read_u32_le(object + kOffQ3), alpha);
    return {retired + kExitBranchCost, false};
}

// The whole function on guest memory (the implementation) or on a CheckedDry (call_cost).
template <class Memory>
Outcome run(Memory& m, R5900State& st, const GuestAddress object) {
    const std::uint32_t gp = st.read_gpr_u32(n3::kGlobalPointer);
    std::uint32_t dt = st.read_fpr_bits(12U);
    std::uint32_t retired = 8U;  // 0x441590..0x4415ac
    if (m.read_u8(kScaleFlag) != 0U) {
        dt = fpu::mul(dt, kTwo);  // mul.s f12, f12, 2.0
        st.write_fpr_bits(12U, dt);
        retired += 4U;  // 0x4415b0..0x4415bc
    }
    const auto state = static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(object + kOffState)));
    retired += 4U;  // 0x4415c0..0x4415cc
    if (state == 3) {
        return {retired, true};
    }
    retired += 3U;  // 0x4415d0..0x4415d8
    if (state == 2) {
        return phase2(m, st, object, dt, retired);
    }
    retired += 3U;  // 0x4415dc..0x4415e4
    if (state == 1) {
        return phase1(m, object, gp, dt, retired);
    }
    retired += 2U;  // 0x4415e8..0x4415ec
    if (state == 0) {
        return phase0(m, st, object, gp, dt, retired);
    }
    retired += 2U;  // 0x4415f0..0x4415f4: any other state does nothing
    return {retired + kExitBranchCost, false};
}

template <class Context>
std::uint32_t update_fade_state_machine_cost(const Context& context) {
    CheckedDry dry{context.memory};
    R5900State st = context.cpu;
    const Outcome out = run(dry, st, context.cpu.read_gpr_u32(4U));
    if (out.cold || dry.faulted()) {
        return 0U;
    }
    return out.retired;
}

template <class Context>
n3::Run update_fade_state_machine(Context& context) {
    auto call = n3::guest_call(context);
    const Outcome out = run(call.memory(), call.cpu(), call.arg(0));
    return call.finish(out.retired);
}

} // namespace

extern constinit const EngineFunction n3_00441590{0x00441590U, 852U, 0xf9a16f73ac2b60a3ULL, "UpdateFadeStateMachine",
                                                  kDeclaredCost, n3::live<&update_fade_state_machine<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &update_fade_state_machine_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00441590, update_fade_state_machine, update_fade_state_machine_cost)
