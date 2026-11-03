// 0x00427ad0 (a0 = record O, f12 = the frame step): one frame of O's fade and pulse timer.
//
//   O+152  mode: 3 fades in, 1 fades out, 0 holds the fade at 1.0, 2 calls 0x00284260 (not native yet)
//   O+164  clock (float)
//   O+176  fade (float): mode 3 adds 5 * step (ends at 1.0 and mode 0), mode 1 takes 5 * step (ends at 0 and mode 2)
//   O+184  time of the next pulse (float)
//   O+188  index of the next of five pulse slots
//   O+192  the five pulse slots (object pointers, 0 when empty)
//   O+160  record R: the frame stores a quadword at R + 16, x, y, z from the quadword at 0x01e90350 and w = fade
//
// A pulse is due once the clock is past its time. A due pulse with an empty slot changes nothing else; with a slot
// it calls 0x001d6070, which is not native yet. Mode 2 and a due pulse with a slot are declined, so the original
// runs the whole call. Otherwise the frame ends with the VU0 sequence of the original (VF2 from a1, lqc2, VMULx.w,
// sqc2), which is what the guest sees in VU0 state.

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kModeOffset = 152U;
constexpr GuestAddress kRecordOffset = 160U;       // R: the frame stores at R + 16
constexpr GuestAddress kClockOffset = 164U;
constexpr GuestAddress kFadeOffset = 176U;
constexpr GuestAddress kPulseTimeOffset = 184U;
constexpr GuestAddress kPulseIndexOffset = 188U;
constexpr GuestAddress kPulseSlotsOffset = 192U;   // five object pointers
constexpr std::uint32_t kRecordSpan = 212U;        // O+0..O+211 holds every field above
constexpr GuestAddress kStoreOffset = 16U;
constexpr GuestAddress kVectorSource = 0x01e90350U;  // lui 0x01e9, 848: x, y, z of the stored vector

constexpr std::uint32_t kModeFull = 0U;
constexpr std::uint32_t kModeFadeOut = 1U;
constexpr std::uint32_t kModeCalls = 2U;
constexpr std::uint32_t kModeFadeIn = 3U;
constexpr std::uint32_t kFloatOne = 0x3f800000U;     // 1.0f
constexpr std::uint32_t kFloatFive = 0x40a00000U;    // 5.0f: the fade's rate

// Retired instruction counts from the listing, delay slots included. Each is a block between two branches.
constexpr std::uint32_t kEntryCost = 7U;             // 0x427ad0..0x427ae8: frame, mode load, mode 3 test
constexpr std::uint32_t kFadeInCost = 12U;           // 0x427b68..0x427b94: mode 3 step, fade below 1.0
constexpr std::uint32_t kFadeInEndCost = 2U;         // 0x427b98..0x427b9c: mode 3 reaches 1.0, mode becomes 0
constexpr std::uint32_t kCallsTestCost = 3U;         // 0x427aec..0x427af4: mode 2 test
constexpr std::uint32_t kFadeOutTestCost = 3U;       // 0x427af8..0x427b00: mode 1 test
constexpr std::uint32_t kFadeOutCost = 9U;           // 0x427b20..0x427b40: mode 1 step
constexpr std::uint32_t kFadeOutEndCost = 3U;        // 0x427b44..0x427b4c: mode 1 reaches 0, mode becomes 2
constexpr std::uint32_t kFullTestCost = 2U;          // 0x427b04..0x427b08: mode 0 test
constexpr std::uint32_t kFullCost = 3U;              // 0x427b14..0x427b1c: mode 0 sets the fade to 1.0
constexpr std::uint32_t kOtherCost = 2U;             // 0x427b0c..0x427b10: other modes skip to the clock test
constexpr std::uint32_t kClockCost = 1U;             // 0x427ba0: clock reload after the mode paths
constexpr std::uint32_t kPulseTestCost = 4U;         // 0x427ba4..0x427bb0: clock against the pulse time
constexpr std::uint32_t kSlotTestCost = 6U;          // 0x427bb4..0x427bc8: pulse index and its slot
constexpr std::uint32_t kStoreCost = 12U;            // 0x427c00..0x427c2c: fade reload, VU0 store, return

// The longest path through the listing (its declined call path included). No native path retires more than 48.
constexpr std::uint32_t kCeiling = 61U;

// The interpreter's checks for the accesses this frame makes: a word needs a 4-byte aligned address inside guest
// memory, lqc2 and sqc2 a 16-byte aligned one. A failing check declines the call, so the original faults itself.
template <class Memory>
bool record_accessible(Memory& memory, const GuestAddress record) {
    return (record & 3U) == 0U && memory.contains(record, kRecordSpan);
}

template <class Memory>
bool word_accessible(Memory& memory, const GuestAddress address) {
    return (address & 3U) == 0U && memory.contains(address, 4U);
}

template <class Memory>
bool quad_accessible(Memory& memory, const GuestAddress address) {
    return (address & 15U) == 0U && memory.contains(address, 16U);
}

template <class Context>
n3::Run fade_and_pulse_step(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    if (!record_accessible(m, record) || !quad_accessible(m, kVectorSource)) {
        return call.decline();
    }
    const GuestAddress target = m.read_u32_le(record + kRecordOffset) + kStoreOffset;
    if (!quad_accessible(m, target)) {
        return call.decline();
    }
    const std::uint32_t step = cpu.read_fpr_bits(12U);
    const std::uint32_t mode = m.read_u32_le(record + kModeOffset);
    std::uint32_t fade = m.read_u32_le(record + kFadeOffset);
    std::uint32_t retired = kEntryCost;
    if (mode == kModeFadeIn) {
        // ADDA.S ACC = 0 + fade, then MADD.S: fade = ACC + 5.0 * step.
        fade = fpu::madd(fpu::add(0U, fade), kFloatFive, step);
        m.write_u32_le(record + kFadeOffset, fade);
        retired += kFadeInCost;
        if (!fpu::lt(fade, kFloatOne)) {
            fade = kFloatOne;
            m.write_u32_le(record + kFadeOffset, fade);
            m.write_u32_le(record + kModeOffset, kModeFull);
            retired += kFadeInEndCost;
        }
        retired += kClockCost;
    } else {
        retired += kCallsTestCost;
        if (mode == kModeCalls) {
            return call.decline();
        }
        retired += kFadeOutTestCost;
        if (mode == kModeFadeOut) {
            // ADDA.S ACC = 0 + fade, then MSUB.S: fade = ACC - 5.0 * step.
            fade = fpu::msub(fpu::add(0U, fade), kFloatFive, step);
            m.write_u32_le(record + kFadeOffset, fade);
            retired += kFadeOutCost;
            if (fpu::le(fade, 0U)) {
                fade = 0U;
                m.write_u32_le(record + kFadeOffset, fade);
                m.write_u32_le(record + kModeOffset, kModeCalls);
                retired += kFadeOutEndCost;
            }
            retired += kClockCost;
        } else {
            retired += kFullTestCost;
            if (mode == kModeFull) {
                fade = kFloatOne;
                m.write_u32_le(record + kFadeOffset, fade);
                retired += kFullCost + kClockCost;
            } else {
                retired += kOtherCost;
            }
        }
    }
    // The clock against the pulse time: a due pulse with an empty slot changes nothing; one with a slot is a call.
    const std::uint32_t clock_now = m.read_u32_le(record + kClockOffset);
    const std::uint32_t next_pulse = m.read_u32_le(record + kPulseTimeOffset);
    retired += kPulseTestCost;
    if (!fpu::le(clock_now, next_pulse)) {
        retired += kSlotTestCost;
        const GuestAddress slot = record + kPulseSlotsOffset + (m.read_u32_le(record + kPulseIndexOffset) << 2U);
        if (!word_accessible(m, slot) || m.read_u32_le(slot) != 0U) {
            return call.decline();
        }
    }
    retired += kStoreCost;
    // lw $a1 (the fade), qmtc2 $a1 -> VF2, lqc2 -> VF1, VMULx.w: VF1.w = VF0.w * VF2.x, then sqc2 VF1 -> R + 16.
    cpu.write_gpr_sign_extended_32(5U, fade);
    cpu.write_vu0_vector(2U, cpu.read_gpr(5U));
    cpu.write_vu0_vector(1U, n3::load_quad_any(m, kVectorSource));
    ee_cop2::vmul_broadcast(cpu, 1U, 0U, 2U, n3::kMaskW, 0U);
    n3::store_quad_any(m, target, cpu.read_vu0_vector(1U));
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00427ad0, 352U, 0x6aa8cc16cf20b401ULL, "FadeAndPulseStep", kCeiling, fade_and_pulse_step, EngineReturn::Nothing)
