// 0x0030b190 (a0 = car timer record S, a1 = car): CarInputTimersUpdate. A void per-frame update
// of the eight float timers and blends at S+4, S+8, S+12, S+16, S+20, S+24, S+28 and S+32. Each
// one ramps by the frame step dt (the word at 0x004f509c) under the car's speed (V = the word at
// S+0, speed V+56, flag byte V+82) and the state of the car's class record P (the word at
// 0x0055efec + 48 * the signed class byte at car+6572). The body mirrors the original's basic
// blocks one for one: each label is a branch target of the original and each block adds its
// listed instruction count (delay slots included) to the retired total, so every path retires
// exactly the original's instructions and every store happens in the original's order.

#include "takedown/runtime/game/n3_support.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

// Globals, at the addresses the original's lui or gp-relative loads name.
constexpr GuestAddress kFrameStep = 0x004f509cU;   // dt: lui 0x4f, 20636
constexpr GuestAddress kSpeedScale = 0x0048805cU;  // C: lui 0x49, -32676
constexpr GuestAddress kClassRows = 0x0055efecU;   // lui 0x56, -4116: 48-byte rows, one per class
constexpr unsigned kClassRowBytes = 48U;

// Float bit patterns the original builds with lui.
constexpr std::uint32_t kHalf = 0x3f000000U;         // 0.5
constexpr std::uint32_t kOne = 0x3f800000U;          // 1.0
constexpr std::uint32_t kTwo = 0x40000000U;          // 2.0
constexpr std::uint32_t kTwoAndHalf = 0x40200000U;   // 2.5
constexpr std::uint32_t kTen = 0x41200000U;          // 10.0
constexpr std::uint32_t kSixtyFive = 0x42820000U;    // 65.0
constexpr std::uint32_t kHundred = 0x42c80000U;      // 100.0
constexpr std::uint32_t kOneSixtyFive = 0x43250000U; // 165.0

// MAX.S and MIN.S on the raw sign/magnitude patterns (the same ordering as guest::ee_cop1::max_s and min_s).
std::uint32_t ee_max(const std::uint32_t a, const std::uint32_t b) {
    const auto left = std::bit_cast<std::int32_t>(a);
    const auto right = std::bit_cast<std::int32_t>(b);
    return std::bit_cast<std::uint32_t>(left < 0 && right < 0 ? std::min(left, right) : std::max(left, right));
}
std::uint32_t ee_min(const std::uint32_t a, const std::uint32_t b) {
    const auto left = std::bit_cast<std::int32_t>(a);
    const auto right = std::bit_cast<std::int32_t>(b);
    return std::bit_cast<std::uint32_t>(left < 0 && right < 0 ? std::max(left, right) : std::min(left, right));
}

template <class Context>
n3::Run car_input_timers_update(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress s = call.arg(0);
    const GuestAddress car = call.arg(1);
    const std::uint32_t gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);

    // Every variable is declared before the first goto, so no goto skips an initialisation.
    // f0..f7 and acc are the original's float registers at the same points of the code.
    std::uint32_t retired = 0U;
    std::uint32_t f0 = 0U;
    std::uint32_t f1 = 0U;
    std::uint32_t f2 = 0U;
    std::uint32_t f3 = 0U;
    std::uint32_t f4 = 0U;
    std::uint32_t f5 = 0U;
    std::uint32_t f6 = 0U;
    std::uint32_t f7 = 0U;
    std::uint32_t acc = 0U;
    std::uint32_t v = 0U;     // V: the car pointer at S+0 (re-read where the original re-reads it)
    std::uint32_t v1 = 0U;    // the byte at V+82 (or the flag test)
    std::uint32_t flag = 0U;  // the byte at Q+4147, Q being the word at P+3268

    // ENTRY (23): dt into f2, G1 * (P+5076) into f1, G2 * (word 3 of the quadword at P+176) into f0.
    retired = 23U;
    f2 = m.read_u32_le(kFrameStep);
    const auto car_class = static_cast<std::int8_t>(m.read_u8(car + 6572U));
    const GuestAddress params = m.read_u32_le(kClassRows + kClassRowBytes * static_cast<std::uint32_t>(car_class));
    f1 = fpu::mul(m.read_u32_le(gp - 30484U), m.read_u32_le(params + 5076U));
    f0 = fpu::mul(m.read_u32_le(gp - 30980U), n3::load_quad_any(m, params + 176U).words[3]);
    flag = m.read_u8(m.read_u32_le(params + 3268U) + 4147U);
    if (flag == 0U) {
        goto L23C;
    }

    // B1 (6): when the S+20 timer is zero, B1b below runs.
    retired += 6U;
    f3 = m.read_u32_le(s + 20U);
    f7 = 0U;
    if (!fpu::eq(f7, f3)) {
        goto L23C;
    }
    // B1b (14)
    retired += 14U;
    m.write_u32_le(s + 20U, kTwo);
    f3 = m.read_u32_le(s + 24U);
    f4 = kTwoAndHalf;
    f5 = m.read_u32_le(kSpeedScale);
    f3 = fpu::div(f3, f4);
    v = m.read_u32_le(s + 0U);
    f6 = m.read_u32_le(v + 56U);
    f4 = fpu::div(f6, f5);
    f3 = ee_min(f3, f4);
    m.write_u32_le(s + 28U, f3);
    m.write_u32_le(s + 24U, f7);

L23C:
    // (2)
    retired += 2U;
    if (flag != 0U) {
        goto L448;
    }
    // B244 (4)
    retired += 4U;
    v = m.read_u32_le(s + 0U);
    v1 = m.read_u8(v + 82U);
    if (v1 != 0U) {
        goto L448;
    }
    // B254 (9)
    retired += 9U;
    f4 = m.read_u32_le(s + 20U);
    f3 = 0U;
    f5 = kTwo;
    acc = fpu::add(f3, f4);
    f4 = fpu::msub(acc, f5, f2);
    f3 = ee_max(f4, f3);
    m.write_u32_le(s + 20U, f3);
    goto L278;

L448:
    // (11)
    retired += 11U;
    f5 = m.read_u32_le(s + 20U);
    f4 = 0U;
    f6 = kTwoAndHalf;
    f3 = kOne;
    acc = fpu::add(f4, f5);
    f4 = fpu::msub(acc, f6, f2);
    f3 = ee_max(f4, f3);
    m.write_u32_le(s + 20U, f3);

L278:
    // (10)
    retired += 10U;
    v = m.read_u32_le(s + 0U);
    f3 = m.read_u32_le(kSpeedScale);
    f4 = m.read_u32_le(v + 56U);
    v1 = m.read_u8(v + 82U);
    f4 = fpu::div(f4, f3);
    if (v1 == 0U) {
        goto L48C;
    }
    // B2A0 (4)
    retired += 4U;
    f3 = m.read_u32_le(gp - 30264U);
    if (fpu::le(f4, f3)) {
        goto L48C;
    }
    // B2B0 (14): S+16 ramps up by G4 * dt, capped at 1.0; a result above 0.5 takes L474.
    retired += 14U;
    f3 = m.read_u32_le(s + 16U);
    f4 = m.read_u32_le(gp - 30248U);
    f5 = 0U;
    f6 = kOne;
    acc = fpu::add(f5, f3);
    f3 = fpu::madd(acc, f4, f2);
    f3 = ee_min(f3, f6);
    f4 = kHalf;
    m.write_u32_le(s + 16U, f3);
    if (!fpu::le(f3, f4)) {
        goto L474;
    }

L2E8:
    // (7)
    retired += 7U;
    f4 = m.read_u32_le(s + 16U);
    f3 = kHalf;
    if (fpu::le(f4, f3)) {
        goto L32C;
    }
    // B304 (10)
    retired += 10U;
    f4 = m.read_u32_le(s + 32U);
    f3 = 0U;
    f5 = kTwo;
    acc = fpu::add(f3, f4);
    f4 = fpu::msub(acc, f5, f2);
    f3 = ee_max(f4, f3);
    m.write_u32_le(s + 32U, f3);
    goto L350;

L32C:
    // (9): f6 is 1.0 here on every path that reaches it.
    retired += 9U;
    f4 = m.read_u32_le(s + 32U);
    f3 = 0U;
    f5 = kTen;
    acc = fpu::add(f3, f4);
    f3 = fpu::madd(acc, f5, f2);
    f3 = ee_min(f3, f6);
    m.write_u32_le(s + 32U, f3);

L350:
    // (5)
    retired += 5U;
    f5 = 0U;
    if (fpu::le(f0, f5)) {
        goto L37C;
    }
    // B364 (3)
    retired += 3U;
    if (!fpu::lt(f0, f1)) {
        goto L4F0;
    }
    // B370 (3)
    retired += 3U;
    f1 = fpu::sub(f1, f5);
    f3 = fpu::sub(f0, f5);
    f5 = fpu::div(f3, f1);

L37C:
    // (1)
    retired += 1U;
    f3 = m.read_u32_le(s + 4U);

L380:
    // (3)
    retired += 3U;
    if (fpu::le(f3, f5)) {
        goto L3AC;
    }
    // B38C (8)
    retired += 8U;
    f3 = kTwo;
    f1 = m.read_u32_le(s + 4U);
    f4 = fpu::mul(f3, f2);
    f1 = fpu::add(f1, f4);
    f1 = ee_min(f1, f5);
    m.write_u32_le(s + 4U, f1);
    goto L3C8;

L3AC:
    // (7)
    retired += 7U;
    f1 = kTwo;
    f4 = fpu::mul(f1, f2);
    f1 = fpu::sub(f3, f4);
    f1 = ee_max(f1, f5);
    m.write_u32_le(s + 4U, f1);

L3C8:
    // (6): f2 takes 100.0 here, so dt is gone after this point.
    retired += 6U;
    f2 = kHundred;
    if (fpu::le(f0, f2)) {
        goto L510;
    }
    // B3E0 (6)
    retired += 6U;
    f1 = kOneSixtyFive;
    if (!fpu::lt(f0, f1)) {
        goto L500;
    }
    // B3F8 (5)
    retired += 5U;
    f1 = fpu::sub(f0, f2);
    f0 = kSixtyFive;
    f5 = fpu::div(f1, f0);

L40C:
    // (1)
    retired += 1U;
    f0 = m.read_u32_le(s + 4U);

L410:
    // (3)
    retired += 3U;
    if (fpu::le(f0, f5)) {
        goto L430;
    }
    // B41C (5)
    retired += 5U;
    f0 = m.read_u32_le(s + 8U);
    f0 = fpu::add(f0, f4);
    f0 = ee_min(f0, f5);
    m.write_u32_le(s + 8U, f0);
    goto L440;

L430:
    // (4)
    retired += 4U;
    f0 = m.read_u32_le(s + 8U);
    f0 = fpu::sub(f0, f4);
    f0 = ee_max(f0, f5);
    m.write_u32_le(s + 8U, f0);

L440:
    // L440 (2) and the return jump with its delay slot (2).
    retired += 4U;
    return call.finish(retired);

L474:
    // (6): S+12 ramps up by 0.5 * dt, capped at 1.0 (f4 = 0.5, f5 = 0, f6 = 1.0 from B2B0).
    retired += 6U;
    f3 = m.read_u32_le(s + 12U);
    acc = fpu::add(f5, f3);
    f3 = fpu::madd(acc, f4, f2);
    f3 = ee_min(f3, f6);
    m.write_u32_le(s + 12U, f3);
    goto L2E8;

L48C:
    // (25): the decay branch, which ends in the shared L350 tail.
    retired += 25U;
    f3 = m.read_u32_le(s + 16U);
    f5 = 0U;
    f3 = fpu::sub(f3, f2);
    f3 = ee_max(f3, f5);
    m.write_u32_le(s + 16U, f3);
    f3 = m.read_u32_le(s + 12U);
    f4 = kHalf;
    acc = fpu::add(f5, f3);
    f3 = fpu::msub(acc, f4, f2);
    f3 = ee_max(f3, f5);
    m.write_u32_le(s + 12U, f3);
    f3 = m.read_u32_le(s + 32U);
    f4 = kTwo;
    acc = fpu::add(f5, f3);
    f3 = fpu::msub(acc, f4, f2);
    f3 = ee_max(f3, f5);
    m.write_u32_le(s + 32U, f3);
    f3 = m.read_u32_le(s + 24U);
    f3 = fpu::add(f3, f2);
    m.write_u32_le(s + 24U, f3);
    goto L350;

L4F0:
    // (4): f0 is above zero and not below f1, so the ratio is 1.0.
    retired += 4U;
    f5 = kOne;
    f3 = m.read_u32_le(s + 4U);
    goto L380;

L500:
    // (4)
    retired += 4U;
    f5 = kOne;
    f0 = m.read_u32_le(s + 4U);
    goto L410;

L510:
    // (3)
    retired += 3U;
    f5 = 0U;
    goto L40C;
}

} // namespace

// Declared cost 164 = the longest control-flow path (bin/brief.py); the feasible maximum is 160, a larger
// ceiling is safe (the exact count is what a call reports).
extern constinit const EngineFunction n3_0030b190{0x0030b190U, 920U, 0xaa13d9d1bce30071ULL, "CarInputTimersUpdate",
                                                  164U, n3::live<&car_input_timers_update<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(0030b190, car_input_timers_update)
