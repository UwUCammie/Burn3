// 0x001c6f90 StepControlRamp: per-frame update of one control ramp record (a0 = record, a1 = flag A,
// a2 = flag B, f12 = input scale, f13 = rate scale); returns a float in f0 (the ramped output scaled by the
// record's multiplier). Written block by block from the listing with the original's labels (goto), keeping
// every float register in an array so the values flowing between blocks are exactly the original's.
//
// Record fields used (byte offsets): +4.. float table indexed by the mode word (+128), +32/+36/+40/+44 weights,
// +56/+64/+68/+72 targets and current, +76/+80 blend, +84 ramp position, +88/+136/+140 timers, +92 active
// flag, +96/+112 blockers, +104/+108 pseudo random state, +128 mode, +132 mode limit, +144 owner object.
// The pseudo random step (three adds over +104/+108) appears six times; it is `lcg_step` below.
//
// Every retired count is the listing's block length (delay slots included); the function has no loop
// (the branches form a DAG), so the longest path (381) is the declared cost.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kTimerDecrement = 0x0052267cU;  // lui 0x52 + 9852

// The three-step update of the pseudo random words at +104/+108; returns the final +104 (v0).
template <class Memory>
std::uint32_t lcg_step(Memory& m, const GuestAddress a0) {
    const std::uint32_t a3 = m.read_u32_le(a0 + 104U);
    std::uint32_t v1 = a3 << 16U;
    std::uint32_t v0 = static_cast<std::uint32_t>(static_cast<std::int32_t>(a3) >> 16);
    v0 = v1 + v0;
    m.write_u32_le(a0 + 104U, v0);
    v1 = m.read_u32_le(a0 + 104U);
    v0 = m.read_u32_le(a0 + 108U);
    v0 = v1 + v0;
    m.write_u32_le(a0 + 104U, v0);
    v1 = m.read_u32_le(a0 + 108U);
    v0 = m.read_u32_le(a0 + 104U);
    v0 = v1 + v0;
    m.write_u32_le(a0 + 108U, v0);
    return m.read_u32_le(a0 + 104U);
}

// The unsigned-to-float conversion that follows (bltz / cvt.s.w / halve-and-double); adds its own count.
inline std::uint32_t u32_to_float(const std::uint32_t v0, std::uint32_t& retired) {
    if (static_cast<std::int32_t>(v0) >= 0) {
        retired += 3U;
        return fpu::from_int(static_cast<std::int32_t>(v0));
    }
    retired += 7U;
    const std::uint32_t half = (v0 >> 1U) | (v0 & 1U);
    const std::uint32_t f = fpu::from_int(static_cast<std::int32_t>(half));
    return fpu::add(f, f);
}

template <class Context>
n3::Run step_control_ramp(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    auto& cpu = call.cpu();
    const GuestAddress a0 = call.arg(0);
    const std::uint32_t a1_in = call.arg(1);
    const std::uint32_t a2 = call.arg(2);
    const std::uint32_t gp = cpu.read_gpr_u32(n3::kGlobalPointer);

    std::uint32_t f[32] = {};
    for (unsigned i = 0U; i < 14U; ++i) {
        f[i] = cpu.read_fpr_bits(i);
    }
    std::uint32_t acc = 0U;
    std::uint32_t a1 = a1_in;
    std::uint32_t v0 = 0U;
    std::uint32_t v1 = 0U;
    std::uint32_t a3 = 0U;
    bool cc = false;
    std::uint32_t n = 0U;

    auto rd = [&](const std::uint32_t offset) { return m.read_u32_le(a0 + offset); };
    auto wr = [&](const std::uint32_t offset, const std::uint32_t value) { m.write_u32_le(a0 + offset, value); };
    auto g = [&](const std::int32_t offset) { return m.read_u32_le(gp + static_cast<std::uint32_t>(offset)); };

    // ---- 0x001c6f90
    n += 2U;
    if (a1 != 0U) {
        n += 4U;
        f[0] = g(-31484);
        cc = fpu::lt(f[12], f[0]);
        if (cc) {
            n += 1U;
            f[12] = f[0];
        }
    }
    // ---- 0x001c6fac
    n += 4U;
    v1 = rd(128U);
    if (v1 == 1U) {
        n += 5U;
        f[0] = 0U;
        cc = fpu::lt(f[13], f[0]);
        if (cc) {
            n += 4U;
            f[13] = fpu::mul(f[13], 0x42c80000U);
        }
    }
    // ---- 0x001c6fe0
    n += 8U;
    {
        v0 = (v1 << 2U) + a0;
        f[0] = m.read_u32_le(v0 + 4U);
        f[1] = rd(32U);
        f[0] = fpu::mul(f[13], f[0]);
        f[2] = fpu::mul(f[1], f[0]);
        // bne at -> 0x001c7000 is the next instruction either way
    }
    // ---- 0x001c7000
    n += 8U;
    f[1] = rd(36U);
    f[4] = g(-30960);
    f[3] = rd(44U);
    f[0] = rd(40U);
    f[1] = fpu::mul(f[4], f[1]);
    f[6] = fpu::mul(f[4], f[3]);
    f[5] = fpu::mul(f[4], f[0]);
    if (v1 == 0U) {
        n += 3U;
        f[0] = rd(64U);
        f[0] = fpu::mul(f[4], f[0]);
        f[2] = fpu::mul(f[12], f[0]);
    }
    // ---- 0x001c702c
    n += 3U;
    v0 = rd(92U);
    if (v0 != 0U) {
        goto L7308;
    }
    n += 3U;
    v0 = rd(112U);
    if (v0 != 0U) {
        goto L7128;
    }
    // ---- 0x001c7044
    n += 12U;
    f[3] = m.read_u32_le(kTimerDecrement);
    f[0] = rd(140U);
    cc = fpu::lt(f[2], f[6]);
    f[0] = fpu::sub(f[0], f[3]);
    wr(140U, f[0]);
    f[3] = m.read_u32_le(kTimerDecrement);
    f[0] = rd(136U);
    f[0] = fpu::sub(f[0], f[3]);
    wr(136U, f[0]);
    if (cc) {
        goto L733c;
    }
L7074:
    n += 7U;
    f[0] = 0x3f400000U;
    f[0] = fpu::mul(f[0], f[6]);
    cc = fpu::lt(f[2], f[0]);
    if (cc) {
        goto L73a4;
    }
L7090:
    n += 3U;
    cc = fpu::le(f[2], f[5]);
    if (cc) {
        goto L7128;
    }
    n += 3U;
    v1 = rd(128U);
    if (static_cast<std::int32_t>(v1) <= 0) {
        goto L7128;
    }
    n += 4U;
    v0 = rd(132U);
    if (!(static_cast<std::int32_t>(v1) < static_cast<std::int32_t>(v0))) {
        goto L7128;
    }
    n += 3U;
    v0 = rd(96U);
    if (v0 != 0U) {
        goto L7128;
    }
    n += 6U;
    f[3] = rd(136U);
    f[0] = 0U;
    cc = fpu::le(f[3], f[0]);
    if (!cc) {
        goto L7128;
    }
    // ---- 0x001c70dc
    n += 11U;
    a3 = (v1 << 2U) + a0;
    v1 += 1U;
    f[2] = m.read_u32_le(a3 + 4U);
    v0 = 1U;
    f[0] = rd(32U);
    f[2] = fpu::mul(f[13], f[2]);
    wr(128U, v1);
    wr(92U, v0);
    f[2] = fpu::mul(f[0], f[2]);
    if (a1 == 0U) {
        // ---- 0x001c7118
        n += 4U;
        f[0] = g(-32456);
        v0 = 0x3f800000U;
        wr(88U, f[0]);
        wr(140U, v0);
        goto L7128;
    }
    n += 4U;
    f[0] = g(-31320);
    wr(88U, f[0]);
    wr(140U, f[0]);
    goto L7128;

L733c:
    n += 4U;
    v0 = rd(128U);
    if (static_cast<std::int32_t>(v0) < 2) {
        goto L7074;
    }
    n += 6U;
    f[0] = 0x3f000000U;
    cc = fpu::lt(f[12], f[0]);
    if (!cc) {
        goto L7074;
    }
    n += 1U;
    f[3] = rd(140U);
L7368:
    n += 5U;
    f[0] = 0U;
    cc = fpu::le(f[3], f[0]);
    if (!cc) {
        goto L7128;
    }
    n += 10U;
    a3 = rd(128U);
    f[0] = g(-32456);
    v1 = 1U;
    v0 = 0x3f800000U;
    a3 -= 1U;
    wr(128U, a3);
    wr(92U, v1);
    wr(88U, f[0]);
    wr(136U, v0);
    goto L7128;

L73a4:
    n += 4U;
    v0 = rd(128U);
    if (static_cast<std::int32_t>(v0) < 2) {
        goto L7090;
    }
    n += 2U;
    f[3] = rd(140U);
    goto L7368;

L7308:
    n += 8U;
    f[4] = m.read_u32_le(kTimerDecrement);
    f[3] = rd(88U);
    f[0] = 0U;
    f[3] = fpu::sub(f[3], f[4]);
    cc = fpu::le(f[3], f[0]);
    wr(88U, f[3]);
    if (cc) {
        n += 2U;
        wr(92U, 0U);
        goto L7128;
    }
    n += 3U;
    f[0] = g(-31320);
    f[12] = fpu::mul(f[12], f[0]);
    // falls into 0x001c7128

L7128:
    n += 6U;
    f[5] = rd(64U);
    v1 = rd(128U);
    f[0] = g(-30960);
    v0 = 0xffffffffU;
    f[0] = fpu::mul(f[0], f[5]);
    if (v1 == v0) {
        n += 3U;
        f[3] = g(-31792);
        f[0] = fpu::mul(f[0], f[3]);
        goto L7170;
    }
    n += 2U;
    if (v1 == 0U) {
        goto L7170;
    }
    n += 2U;
    if (a1 == 0U) {
        n += 3U;
        v0 = rd(112U);
        if (v0 == 0U) {
            goto L7170;
        }
        n += 2U;
        f[3] = g(-30952);
        f[0] = fpu::mul(f[0], f[3]);
        goto L7170;
    }
    n += 3U;
    f[3] = g(-30956);
    f[0] = fpu::mul(f[0], f[3]);

L7170:
    n += 6U;
    f[3] = g(-31104);
    f[4] = rd(84U);
    f[3] = fpu::sub(f[0], f[3]);
    cc = fpu::lt(f[4], f[3]);
    if (cc) {
        goto L750c;
    }
    n += 4U;
    f[1] = rd(56U);
    cc = fpu::eq(f[5], f[1]);
    if (cc) {
        n += 2U;
        if (a1 == 0U) {
            n += 2U;
            if (v1 != 0U) {
                // ---- 0x001c746c
                n += 18U;
                v0 = lcg_step(m, a0);
                f[3] = 0x2f800000U;
                f[1] = u32_to_float(v0, n);
                n += 12U;
                f[6] = fpu::mul(f[3], f[1]);
                v1 = 0x42480000U;
                v0 = 0x428c0000U;
                f[4] = rd(56U);
                f[3] = 0U;
                f[5] = v1;
                f[1] = v0;
                acc = fpu::add(f[3], f[4]);
                f[3] = fpu::msub(acc, f[5], f[6]);
                f[1] = fpu::sub(f[3], f[1]);
                wr(64U, f[1]);
                goto L7198;
            }
            // ---- 0x001c73d8
            n += 18U;
            v0 = lcg_step(m, a0);
            f[3] = 0x2f800000U;
            f[1] = u32_to_float(v0, n);
            n += 9U;
            f[5] = fpu::mul(f[3], f[1]);
            f[4] = rd(56U);
            f[1] = 0U;
            f[3] = g(-31320);
            acc = fpu::add(f[1], f[4]);
            f[1] = fpu::mul(f[4], f[5]);
            f[1] = fpu::msub(acc, f[3], f[1]);
            wr(64U, f[1]);
            goto L7198;
        }
    }
L7198:
    n += 15U;
    f[4] = 0x41a00000U;
    f[5] = rd(84U);
    f[3] = 0xbf800000U;
    f[1] = 0U;
    f[2] = fpu::mul(f[3], f[2]);
    f[12] = fpu::div(f[2], f[0]);
    f[3] = fpu::sub(f[5], f[4]);
    wr(84U, f[3]);
    cc = fpu::le(f[12], f[1]);
    if (!cc) {
        n += 1U;
        f[12] = f[1];
    }
L71d8:
    n += 5U;
    f[1] = rd(68U);
    f[5] = rd(84U);
    cc = fpu::lt(f[5], f[1]);
    if (cc) {
        n += 10U;
        f[2] = fpu::div(f[5], f[1]);
        f[3] = 0x3f000000U;
        f[1] = 0U;
        acc = fpu::add(f[1], f[3]);
        f[3] = fpu::madd(acc, f[3], f[2]);
        goto L7250;
    }
    n += 4U;
    f[1] = rd(72U);
    cc = fpu::le(f[5], f[1]);
    if (cc) {
        n += 4U;
        f[3] = 0x3f800000U;
        goto L7250;
    }
    n += 11U;
    f[1] = fpu::sub(f[0], f[1]);
    v0 = 0x3f400000U;
    v1 = 0x3e800000U;
    f[4] = fpu::sub(f[0], f[5]);
    f[4] = fpu::div(f[4], f[1]);
    f[2] = v0;
    f[1] = 0U;
    f[3] = v1;
    acc = fpu::add(f[1], f[2]);
    f[3] = fpu::madd(acc, f[3], f[4]);
L7250:
    n += 2U;
    if (a2 != 0U) {
        n += 2U;
        f[3] = 0x40000000U;
    }
    n += 4U;
    v1 = rd(128U);
    v0 = 0xffffffffU;
    if (v1 == v0) {
        n += 3U;
        f[1] = fpu::sub(f[0], f[5]);
        f[3] = fpu::div(f[1], f[0]);
    }
    n += 2U;
    if (a1 == 0U) {
        goto L77c4;
    }
    n += 11U;
    a1 = rd(144U);
    f[1] = g(-30980);
    {
        const auto quad = n3::load_quad_any(m, a1 + 176U);
        f[0] = m.read_u32_le(a1 + 5076U);
        f[2] = quad.words[3];
    }
    f[1] = fpu::mul(f[1], f[2]);
    cc = fpu::lt(f[1], f[0]);
    if (!cc) {
        goto L77c4;
    }
    n += 6U;
    v1 = m.read_u32_le(a1 + 5108U);
    f[1] = m.read_u32_le(v1 + 4300U);
    v0 = m.read_u8(v1 + 4577U);
    f[0] = m.read_u32_le(v1 + 4528U);
    f[1] = fpu::sub(f[1], f[0]);
    if (v0 != 0U) {
        n += 5U;
        f[1] = rd(76U);
        v0 = 0x3f800000U;
        f[0] = v0;
        f[3] = fpu::add(f[0], f[1]);
        goto L77c4;
    }
    n += 4U;
    f[0] = rd(80U);
    cc = fpu::lt(f[1], f[0]);
    if (!cc) {
        goto L77c4;
    }
    n += 12U;
    f[0] = fpu::div(f[1], f[0]);
    v0 = 0x3f800000U;
    f[2] = v0;
    f[3] = rd(76U);
    f[1] = fpu::sub(f[2], f[0]);
    f[0] = 0U;
    acc = fpu::add(f[0], f[2]);
    f[3] = fpu::madd(acc, f[3], f[1]);
    goto L77c4;

L750c:
    n += 5U;
    f[3] = rd(56U);
    wr(64U, f[3]);
    v0 = rd(128U);
    if (v0 != 0U) {
        n += 2U;
        if (a1 == 0U) {
            n += 6U;
            f[8] = 0x41700000U;
            f[7] = 0x41000000U;
            goto L75ec;
        }
        n += 6U;
        v1 = 0x42340000U;
        v0 = 0x41000000U;
        f[8] = v1;
        f[7] = v0;
        f[4] = rd(84U);
        goto L75f0;
    }
    n += 7U;
    v0 = 6000U;
    f[3] = v0;
    f[4] = rd(56U);
    f[3] = fpu::from_int(6000);
    cc = fpu::lt(f[4], f[3]);
    if (cc) {
        n += 4U;
        f[8] = 0x41b40000U;
        f[7] = g(-30948);
    } else {
        n += 4U;
        v1 = 0x42340000U;
        v0 = 0x41800000U;
        f[8] = v1;
        f[7] = v0;
    }
    n += 18U;
    v0 = lcg_step(m, a0);
    f[4] = 0x2f800000U;
    f[3] = u32_to_float(v0, n);
    n += 8U;
    f[6] = fpu::mul(f[4], f[3]);
    f[4] = g(-31112);
    f[5] = g(-31416);
    f[3] = 0U;
    acc = fpu::add(f[3], f[4]);
    f[3] = fpu::madd(acc, f[5], f[6]);
    f[2] = fpu::mul(f[2], f[3]);
L75ec:
    n += 1U;
    f[4] = rd(84U);
L75f0:
    n += 4U;
    f[3] = fpu::add(f[4], f[8]);
    cc = fpu::le(f[2], f[3]);
    if (!cc) {
        n += 2U;
        wr(84U, f[3]);
        goto L7624;
    }
    n += 4U;
    f[3] = fpu::sub(f[4], f[7]);
    cc = fpu::lt(f[2], f[3]);
    if (!cc) {
        n += 1U;
        wr(84U, f[2]);
    } else {
        n += 4U;
        f[2] = rd(84U);
        f[2] = fpu::sub(f[2], f[7]);
        wr(84U, f[2]);
    }
L7624:
    n += 4U;
    f[2] = rd(84U);
    cc = fpu::lt(f[2], f[1]);
    if (!cc) {
        n += 12U;
        a3 = rd(144U);
        f[2] = 0x3f800000U;
        f[3] = g(-30980);
        {
            const auto quad = n3::load_quad_any(m, a3 + 176U);
            f[4] = quad.words[3];
        }
        f[3] = fpu::mul(f[3], f[4]);
        cc = fpu::lt(f[3], f[2]);
        if (!cc) {
            goto L71d8;
        }
    }
    n += 6U;
    f[3] = g(-28564);
    f[2] = rd(84U);
    f[3] = fpu::add(f[1], f[3]);
    cc = fpu::le(f[2], f[3]);
    if (cc) {
        n += 18U;
        v0 = lcg_step(m, a0);
        f[3] = 0x2f800000U;
        f[2] = u32_to_float(v0, n);
        n += 8U;
        f[4] = fpu::mul(f[3], f[2]);
        f[3] = g(-28560);
        f[2] = 0U;
        acc = fpu::add(f[2], f[1]);
        f[1] = fpu::madd(acc, f[3], f[4]);
        wr(84U, f[1]);
    }
    n += 5U;
    f[1] = 0U;
    cc = fpu::le(f[12], f[1]);
    if (!cc) {
        goto L71d8;
    }
    n += 4U;
    v1 = rd(128U);
    v0 = 1U;
    if (v1 != v0) {
        n += 3U;
        v0 = 0xffffffffU;
        if (v1 != v0) {
            n += 3U;
            v0 = rd(112U);
            if (v0 == 0U) {
                goto L71d8;
            }
        }
    }
    n += 6U;
    f[1] = g(-30944);
    wr(128U, 0U);
    v0 = 1U;
    wr(92U, v0);
    wr(88U, f[1]);
    goto L71d8;

L77c4:
    n += 25U;
    {
        // 0x001c77c4: the random step starts from the current +104 word
        v0 = lcg_step(m, a0);
        f[1] = rd(60U);
        v1 = rd(128U);
        f[0] = rd(32U);
        f[1] = fpu::mul(f[12], f[1]);
        v1 = (v1 << 2U) + a0;
        f[2] = fpu::mul(f[3], f[1]);
        f[1] = m.read_u32_le(v1 + 4U);
        f[1] = fpu::mul(f[1], f[2]);
        f[4] = fpu::mul(f[0], f[1]);
        f[1] = u32_to_float(v0, n);
    }
    n += 12U;
    f[0] = 0x2f800000U;
    f[2] = g(-31320);
    f[3] = fpu::mul(f[0], f[1]);
    f[1] = g(-31992);
    f[0] = 0U;
    acc = fpu::add(f[0], f[1]);
    f[0] = fpu::madd(acc, f[2], f[3]);
    f[0] = fpu::mul(f[4], f[0]);
    cpu.write_fpr_bits(0U, f[0]);
    return call.finish(n);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(001c6f90, 2288U, 0x413673ce29274cccULL, "StepControlRamp", 381U, step_control_ramp,
               EngineReturn::Float)
