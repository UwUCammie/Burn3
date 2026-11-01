// 0x00438fb0 (a0 = obj): FadeLayerStep, the per-frame crossfade update. obj+52 is a pointer P and the record is
// R = P + 32: R+8 the id on show, R+4 and R+12 its two params, R+16 its float level. obj+56 is the last frame time,
// obj+60 a countdown and obj+64/+68/+72 the requested id and its two params. dt = max(G - obj+56, +0.0), with G the
// float at 0x004f50a0; the countdown is spent by dt and, once it is at or below zero, the request is cleared. A
// request for the showing id raises the level by dt (from zero at least, capped at 1.25); a request for id 0 lowers
// it, and below zero clears the showing id and params; any other request lowers the level by dt, floored at 0.5625,
// and takes the showing slot once the level is at most 0.5625. Leaf: returns nothing (v1 and f0 are temporaries).

#include "takedown/guest/ee_cop1_arithmetic.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <cstdint>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kClockAddress = 0x004f50a0U;    // lui 0x004f, lwc1 offset 20640: the frame time G
constexpr std::uint32_t kRecordOffset = 32U;           // addiu $a2, P, 32
constexpr std::uint32_t kSwapLevelBits = 0x3f100000U;  // 0.5625f (lui 0x3f10)
constexpr std::uint32_t kMaxLevelBits = 0x3fa00000U;   // 1.25f (lui 0x3fa0)

template <class Context>
n3::Run fade_layer_step(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    auto& cpu = call.cpu();
    const GuestAddress obj = call.arg(0);

    // 0x438fb0..0x438fe4 (14 on every path): dt = max.s(G - last, +0.0), last = G, countdown -= dt.
    std::uint32_t retired = 14U;
    const GuestAddress record = m.read_u32_le(obj + 52U) + kRecordOffset;
    const std::uint32_t now = m.read_u32_le(kClockAddress);
    cpu.write_fpr_bits(0U, fpu::sub(now, m.read_u32_le(obj + 56U)));
    cpu.write_fpr_bits(1U, 0U);
    // MAX.S f3, f0, f1 runs on the FPU itself, as 00254340 does: the value is the EE's max ordering, and the
    // instruction also clears the O/U cause bits of FCR31.
    guest::ee_cop1::max_s(cpu, 3U, 0U, 1U);
    const std::uint32_t dt = cpu.read_fpr_bits(3U);
    m.write_u32_le(obj + 56U, now);
    const std::uint32_t countdown = fpu::sub(m.read_u32_le(obj + 60U), dt);
    m.write_u32_le(obj + 60U, countdown);
    if (fpu::le(countdown, 0U)) {
        // 0x439058..0x439064 (4): the request is cleared, then the common block runs.
        m.write_u32_le(obj + 64U, 0U);
        m.write_u32_le(obj + 68U, 0U);
        m.write_u32_le(obj + 72U, 0U);
        retired += 4U;
    }

    // 0x438fe8..0x438ff4 (4): the requested id and the showing id, read again after any clear.
    retired += 4U;
    const std::uint32_t requested = m.read_u32_le(obj + 64U);
    const std::uint32_t showing = m.read_u32_le(record + 8U);
    if (requested == showing) {
        // 0x4390b0..0x4390b4 (2): id 0 has nothing to fade.
        retired += 2U;
        if (requested != 0U) {
            // 0x4390b8..0x4390cc (6): a negative level is set to zero first.
            retired += 6U;
            if (fpu::lt(m.read_u32_le(record + 16U), 0U)) {
                // 0x4390f8..0x4390fc (2)
                retired += 2U;
                m.write_u32_le(record + 16U, 0U);
            }
            // 0x4390d0..0x4390ec (8): the level grows by dt and is stored (the store is in the delay slot).
            retired += 8U;
            const std::uint32_t level = fpu::add(m.read_u32_le(record + 16U), dt);
            m.write_u32_le(record + 16U, level);
            if (!fpu::le(level, kMaxLevelBits)) {
                // 0x4390f0..0x4390f4 (2), then 0x439050..0x439054 (2): capped at 1.25.
                retired += 4U;
                m.write_u32_le(record + 16U, kMaxLevelBits);
            }
        }
    } else if (requested == 0U) {
        // 0x438ff8..0x438ffc (2), then 0x439068..0x439080 (7): the level falls by dt and is stored.
        retired += 9U;
        const std::uint32_t level = fpu::sub(m.read_u32_le(record + 16U), dt);
        m.write_u32_le(record + 16U, level);
        if (fpu::lt(level, 0U)) {
            // 0x439084..0x439094 (5), then 0x439050..0x439054 (2): the showing id and params are cleared.
            retired += 7U;
            m.write_u32_le(record + 8U, 0U);
            m.write_u32_le(record + 4U, 0U);
            m.write_u32_le(record + 12U, 0U);
            m.write_u32_le(record + 16U, 0U);
        }
    } else {
        // 0x438ff8..0x438ffc (2), then 0x439000..0x439018 (7): a new request waits for the level.
        retired += 9U;
        const std::uint32_t level = m.read_u32_le(record + 16U);
        if (!fpu::le(level, kSwapLevelBits)) {
            // 0x439098..0x4390a4 (4): the level falls by dt and is stored, then floored at 0.5625.
            retired += 4U;
            const std::uint32_t lowered = fpu::sub(level, dt);
            m.write_u32_le(record + 16U, lowered);
            if (fpu::lt(lowered, kSwapLevelBits)) {
                // 0x4390a8..0x4390ac (2)
                retired += 2U;
                m.write_u32_le(record + 16U, kSwapLevelBits);
            }
        }
        // 0x43901c..0x439034 (7): the level is read again; at most 0.5625 lets the request take the slot.
        retired += 7U;
        if (fpu::le(m.read_u32_le(record + 16U), kSwapLevelBits)) {
            // 0x439038..0x43904c (6), then 0x439050..0x439054 (2).
            retired += 8U;
            m.write_u32_le(record + 8U, m.read_u32_le(obj + 64U));
            m.write_u32_le(record + 4U, m.read_u32_le(obj + 68U));
            m.write_u32_le(record + 12U, m.read_u32_le(obj + 72U));
        }
    }
    // 0x439100..0x439104 (2): jr $ra.
    return call.finish(retired + 2U);
}

} // namespace

extern constinit const EngineFunction n3_00438fb0{0x00438fb0U, 344U, 0x069e1755bdf39664ULL, "FadeLayerStep", 54U,
                                                  n3::live<&fade_layer_step<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00438fb0, fade_layer_step)
