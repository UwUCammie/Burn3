// 0x003d4fe0 (a0 = effect object): UpdateEffectFadeAndDrift. t = float at a0+336. The record P at a0+12 gets a
// fade float at +44 (0.25 - t scaled by a gp float while t < 0.25, else 0) and its vec4 at +16 gets its w lane set
// to 5*t (1.0 from the gp(-32156) threshold on) through VU0. The record Q at a0+16 (when set) gets a float at +36
// (1.0, or gp(-31980)*t while t < 1.5). The position (a0+24, a0+28) minus half the extent (a0+32, a0+36) is a pair
// that, while t < gp(-32700), gets a VU0 polynomial drift (sine-style, angles gp(-31976)*t and gp(-31972)*t,
// scaled by gp(-31968)*(gp(-32700)-t)) added to it; the pair is stored to P+0, P+4. When the flag at a0+56 is set
// and t > 0.5, the record R at a0+20 gets its vec4 w lane set to 4*(t-0.5) (t < 0.75) or 1.0, through VU0. The
// cold path (flag set, t > 0.5, R == 0) calls 0x003d53e0, which never ran in the recorded game: call_cost returns
// 0 for any input that reaches it, so the original runs it. Returns v0 = 1 when the t re-read at the end is < 1.5.
// The body runs on the real CPU state in the implementation and on a local copy in call_cost, so the dry run
// never touches the real state.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
using guest::R5900State;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr std::uint32_t kQuarter = 0x3e800000U;
constexpr std::uint32_t kHalf = 0x3f000000U;
constexpr std::uint32_t kThreeQuarters = 0x3f400000U;
constexpr std::uint32_t kOne = 0x3f800000U;
constexpr std::uint32_t kOneAndHalf = 0x3fc00000U;
constexpr std::uint32_t kFour = 0x40800000U;
constexpr std::uint32_t kFive = 0x40a00000U;

constexpr std::uint32_t kGpThreshold = static_cast<std::uint32_t>(-32156);
constexpr std::uint32_t kGpFadeScale = static_cast<std::uint32_t>(-31984);
constexpr std::uint32_t kGpQScale = static_cast<std::uint32_t>(-31980);
constexpr std::uint32_t kGpDriftLimit = static_cast<std::uint32_t>(-32700);
constexpr std::uint32_t kGpAngleX = static_cast<std::uint32_t>(-31976);
constexpr std::uint32_t kGpAngleZ = static_cast<std::uint32_t>(-31972);
constexpr std::uint32_t kGpDriftScale = static_cast<std::uint32_t>(-31968);

// GPR numbers.
constexpr unsigned kV0 = 2U, kV1 = 3U, kA0 = 4U, kA1 = 5U, kA2 = 6U, kA3 = 7U;

constexpr std::uint32_t kCeiling = 253U;

// A load/store the original would fault on is not performed by the dry run: the wrapper records it and
// call_cost declines, so the original runs and faults itself.
template <class Base>
class CheckedDry {
public:
    explicit CheckedDry(const Base& memory) noexcept : memory_(memory), dry_(memory) {}
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

struct Outcome {
    std::uint32_t retired;
    std::uint32_t result;
    bool cold;
};

// lqc2 vf1, 16(P); vmulx.w vf1, vf0, vf2x; sqc2 vf1, 16(P) with vf2 = qmtc2 of the GPR v1 holding `bits`.
template <class Memory>
void set_w_lane(Memory& m, R5900State& st, const GuestAddress record, const std::uint32_t bits) {
    st.write_gpr_sign_extended_32(kV1, bits);                       // mfc1 v1, f1
    st.write_vu0_vector(2U, st.read_gpr(kV1));                      // qmtc2 v1, vf2
    check_quad(m, record + 16U);
    st.write_vu0_vector(1U, n3::load_quad_any(m, record + 16U));    // lqc2 vf1, 16(v0)
    ee_cop2::vmul_broadcast(st, 1U, 0U, 2U, n3::kMaskW, 0U);        // vmulx.w vf1, vf0, vf2x
    check_quad(m, record + 16U);
    n3::store_quad_any(m, record + 16U, st.read_vu0_vector(1U));    // sqc2 vf1, 16(v0)
}

void ctc2_i(R5900State& st, const unsigned reg) { ee_cop2::ctc2(st, ee_cop2::kControlI, st.read_gpr_u32(reg)); }
void lui(R5900State& st, const unsigned reg, const std::uint32_t high) {
    st.write_gpr_sign_extended_32(reg, high << 16U);
}
void ori(R5900State& st, const unsigned rd, const unsigned rs, const std::uint32_t imm) {
    st.write_gpr_u64(rd, st.read_gpr_u64(rs) | imm);
}
void mfc1(R5900State& st, const unsigned reg, const std::uint32_t bits) { st.write_gpr_sign_extended_32(reg, bits); }
void qmtc2(R5900State& st, const unsigned reg, const unsigned vf) { st.write_vu0_vector(vf, st.read_gpr(reg)); }
void qmfc2(R5900State& st, const unsigned reg, const unsigned vf) { st.write_gpr(reg, st.read_vu0_vector(vf)); }
void load_constant(R5900State& st, const unsigned reg, const std::uint32_t value) {
    lui(st, reg, value >> 16U);
    if ((value & 0xffffU) != 0U) {
        ori(st, reg, reg, value & 0xffffU);
    }
}

// 0x51ac..0x53a8: the drift. Returns the x and z lanes of the final vector (VF1) in `x` and `z`.
template <class Memory>
void drift(Memory& m, R5900State& st, const std::uint32_t gp, const std::uint32_t t, const std::uint32_t limit,
           std::uint32_t& e232, std::uint32_t& e236) {
    std::uint32_t f1 = m.read_u32_le(gp + kGpAngleX);
    std::uint32_t f0 = m.read_u32_le(gp + kGpAngleZ);
    load_constant(st, kV0, 0x3fc90fdbU);
    f1 = fpu::mul(f1, t);
    mfc1(st, kV1, f1);
    f0 = fpu::mul(f0, t);
    qmtc2(st, kV1, 1U);
    ee_cop2::vadd_broadcast(st, 2U, 0U, 1U, 8U, 0U, false);   // vaddx.x vf2, vf0, vf1x
    mfc1(st, kV1, f0);
    qmtc2(st, kV1, 1U);
    ee_cop2::vadd_broadcast(st, 2U, 0U, 1U, 2U, 0U, false);   // vaddx.z vf2, vf0, vf1x
    ee_cop2::vadd_broadcast(st, 2U, 0U, 2U, 4U, 0U, false);   // vaddx.y vf2, vf0, vf2x
    ee_cop2::vmul_broadcast(st, 2U, 0U, 2U, 1U, 2U);          // vmulz.w vf2, vf0, vf2z
    qmfc2(st, kV1, 2U);
    ctc2_i(st, kV0);
    qmtc2(st, kV1, 1U);
    ee_cop2::vsub_i(st, 1U, 1U, 10U);                         // vsubi.xz vf1, vf1, i
    qmfc2(st, kV0, 1U);
    qmtc2(st, kV0, 1U);
    ee_cop2::vabs(st, 1U, 1U, 15U);
    lui(st, kV0, 0xbe22U);
    ori(st, kV1, kV0, 0xf983U);
    lui(st, kV0, 0x4b40U);
    qmfc2(st, kA2, 1U);
    (void)ee_cop2::vmax_w(st, 1U, 0U, 0U, 15U);               // vmaxw.xyzw vf1, vf0, vf0w
    qmfc2(st, kA1, 1U);
    ctc2_i(st, kV1);
    qmtc2(st, kA2, 2U);
    ee_cop2::vmula_broadcast(st, 2U, 0U, 15U, true);          // vmulai acc, vf2, i
    ctc2_i(st, kV0);
    qmtc2(st, kA1, 1U);
    ee_cop2::vmadd_broadcast(st, 0U, 1U, 0U, 15U, 0U, true, true, true);    // vmsubai acc, vf1, i
    ee_cop2::vmadd_broadcast(st, 0U, 1U, 0U, 15U, 0U, true, true, false);   // vmaddai acc, vf1, i
    ctc2_i(st, kV1);
    ee_cop2::vmadd_broadcast(st, 0U, 2U, 0U, 15U, 0U, true, true, true);    // vmsubai acc, vf2, i
    ctc2_i(st, kA0);
    ee_cop2::vmadd_broadcast(st, 1U, 1U, 0U, 15U, 0U, true, false, true);   // vmsubi vf1, vf1, i
    qmfc2(st, kV0, 1U);
    qmtc2(st, kV0, 1U);
    ee_cop2::vabs(st, 1U, 1U, 15U);
    lui(st, kV0, 0x3e80U);
    qmfc2(st, kV1, 1U);
    ctc2_i(st, kV0);
    qmtc2(st, kV1, 1U);
    ee_cop2::vsub_i(st, 1U, 1U, 15U);                         // vsubi.xyzw vf1, vf1, i
    qmfc2(st, kV0, 1U);
    qmtc2(st, kV0, 3U);
    ee_cop2::vmul(st, 4U, 3U, 3U, 15U);
    load_constant(st, kV0, 0xc2992661U);
    ee_cop2::vmul(st, 2U, 4U, 4U, 15U);
    ee_cop2::vmul(st, 1U, 2U, 2U, 15U);
    qmfc2(st, kA3, 1U);
    ctc2_i(st, kV0);
    ee_cop2::vmul_control(st, 1U, 3U, 15U, ee_cop2::kControlI);
    qmfc2(st, kV0, 1U);
    qmtc2(st, kV0, 1U);
    ee_cop2::vmul(st, 1U, 1U, 4U, 15U);
    load_constant(st, kV0, 0xc2255de0U);
    qmfc2(st, kA2, 1U);
    ctc2_i(st, kV0);
    ee_cop2::vmul_control(st, 1U, 3U, 15U, ee_cop2::kControlI);
    qmfc2(st, kA1, 1U);
    load_constant(st, kV0, 0x42a33457U);
    ctc2_i(st, kV0);
    ee_cop2::vmul_control(st, 1U, 3U, 15U, ee_cop2::kControlI);
    qmfc2(st, kA0, 1U);
    load_constant(st, kV0, 0x421ed7b7U);
    ctc2_i(st, kV0);
    ee_cop2::vmul_control(st, 1U, 3U, 15U, ee_cop2::kControlI);
    qmfc2(st, kV1, 1U);
    qmtc2(st, kA1, 1U);
    load_constant(st, kV0, 0x40c90fdaU);
    ee_cop2::vmula(st, 1U, 4U, 15U);
    qmtc2(st, kA2, 1U);
    ee_cop2::vmadd(st, 0U, 1U, 2U, 15U, true);                // vmadda acc, vf1, vf2
    ctc2_i(st, kV0);
    qmtc2(st, kA0, 1U);
    ee_cop2::vmadd(st, 0U, 1U, 2U, 15U, true);
    ee_cop2::vmadd_broadcast(st, 0U, 3U, 0U, 15U, 0U, true, true, false);   // vmaddai acc, vf3, i
    qmtc2(st, kV1, 2U);
    qmtc2(st, kA3, 1U);
    ee_cop2::vmadd(st, 1U, 2U, 1U, 15U, false);               // vmadd vf1, vf2, vf1
    qmfc2(st, kV0, 1U);
    const std::uint32_t f3 = fpu::sub(limit, t);
    qmtc2(st, kV0, 1U);
    std::uint32_t f2 = m.read_u32_le(gp + kGpDriftScale);
    f2 = fpu::mul(f2, f3);
    mfc1(st, kV1, f2);
    qmtc2(st, kV1, 2U);
    ee_cop2::vmul_broadcast(st, 1U, 1U, 2U, 15U, 0U);         // vmulx.xyzw vf1, vf1, vf2x
    const Gpr128 vf1 = st.read_vu0_vector(1U);                 // sqc2 vf1, 0(sp+48); lwc1 f2, 56(sp); qmfc2 v0
    qmfc2(st, kV0, 1U);
    e232 = fpu::add(e232, vf1.words[0]);
    e236 = fpu::add(e236, vf1.words[2]);
}

// The whole function. `st` is the state the VU0 and GPR work runs on (a copy of the CPU's).
template <class Memory>
Outcome run(Memory& m, R5900State& st, const GuestAddress a0) {
    const std::uint32_t gp = st.read_gpr_u32(n3::kGlobalPointer);
    std::uint32_t retired = 0U;
    // --- A (0x4fe0..0x5054)
    const std::uint32_t t = m.read_u32_le(a0 + 336U);
    std::uint32_t f2 = 0U;
    retired += 13U;
    if (fpu::lt(t, kQuarter)) {
        f2 = fpu::mul(m.read_u32_le(gp + kGpFadeScale), fpu::sub(kQuarter, t));
        retired += 3U;
    }
    const std::uint32_t threshold = m.read_u32_le(gp + kGpThreshold);
    const GuestAddress p = m.read_u32_le(a0 + 12U);
    std::uint32_t f1 = kOne;
    m.write_u32_le(p + 44U, f2);
    retired += 9U;
    if (fpu::lt(t, threshold)) {
        f1 = fpu::mul(kFive, t);
        retired += 4U;
    }
    // --- B (0x5054..0x50a8)
    set_w_lane(m, st, m.read_u32_le(a0 + 12U), f1);
    retired += 10U;
    const GuestAddress q = m.read_u32_le(a0 + 16U);
    if (q != 0U) {
        std::uint32_t qf = kOne;
        retired += 8U;
        if (fpu::lt(t, kOneAndHalf)) {
            qf = fpu::mul(m.read_u32_le(gp + kGpQScale), t);
            retired += 2U;
        }
        m.write_u32_le(q + 36U, qf);
        retired += 1U;
    }
    // --- C (0x50a8..0x5100)
    const std::uint32_t extent_x = m.read_u32_le(a0 + 32U);
    const std::uint32_t extent_z = m.read_u32_le(a0 + 36U);
    const std::uint32_t limit = m.read_u32_le(gp + kGpDriftLimit);
    const std::uint32_t pos_x = m.read_u32_le(a0 + 24U);
    const std::uint32_t pos_z = m.read_u32_le(a0 + 28U);
    lui(st, kA0, 0x3f00U);
    std::uint32_t e232 = fpu::sub(pos_x, fpu::mul(extent_x, kHalf));
    std::uint32_t e236 = fpu::sub(pos_z, fpu::mul(extent_z, kHalf));
    retired += 23U;
    if (fpu::lt(t, limit)) {
        drift(m, st, gp, t, limit, e232, e236);
        retired += 128U;
    }
    // --- E (0x5104..0x53d4)
    const GuestAddress p2 = m.read_u32_le(a0 + 12U);
    m.write_u32_le(p2 + 0U, e232);
    m.write_u32_le(p2 + 4U, e236);
    retired += 8U;
    if (m.read_u32_le(a0 + 56U) != 0U) {
        retired += 3U;
        if (!fpu::le(t, kHalf)) {
            retired += 3U;
            if (m.read_u32_le(a0 + 20U) == 0U) {
                return {retired, 0U, true};
            }
            retired += 1U;
            std::uint32_t rf = kOne;
            retired += 5U;
            if (fpu::lt(t, kThreeQuarters)) {
                rf = fpu::mul(kFour, fpu::sub(t, kHalf));
                retired += 5U;
            }
            set_w_lane(m, st, m.read_u32_le(a0 + 20U), rf);
            retired += 7U;
        }
    }
    const bool below = fpu::lt(m.read_u32_le(a0 + 336U), kOneAndHalf);
    retired += 7U + (below ? 1U : 0U) + 2U;
    return {retired + 7U, below ? 1U : 0U, false};
}

template <class Context>
n3::Run update_effect_fade_and_drift(Context& context) {
    auto call = n3::guest_call(context);
    // The VU0 work runs on the real CPU state: this call owns it, and the GPRs it clobbers are temporaries.
    const Outcome out = run(call.memory(), call.cpu(), call.arg(0));
    call.return_value(out.result);
    return call.finish(out.retired);
}

template <class Context>
std::uint32_t update_effect_fade_and_drift_cost(const Context& context) {
    CheckedDry dry{context.memory};
    R5900State st = context.cpu;
    const Outcome out = run(dry, st, context.cpu.read_gpr_u32(4U));
    if (out.cold || dry.faulted() || out.retired > kCeiling) {
        return 0U;
    }
    return out.retired;
}

} // namespace

extern constinit const EngineFunction n3_003d4fe0{0x003d4fe0U, 1016U, 0xed2f8ef216397fffULL,
                                                  "UpdateEffectFadeAndDrift", kCeiling,
                                                  n3::live<&update_effect_fade_and_drift<NativeFunctionContext>>, EngineReturn::Integer, {}, 1e-4,
                                                  1e-6, {}, &update_effect_fade_and_drift_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(003d4fe0, update_effect_fade_and_drift, update_effect_fade_and_drift_cost)
