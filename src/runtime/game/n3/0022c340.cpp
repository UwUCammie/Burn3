// 0x0022c340 (a0 slot block, a1 slot index in its low byte): advances one slot of a
// HUD meter. The slot's byte map (+4001, indexed by the slot) selects an enable byte
// (+map+4005); a disabled slot returns 0 untouched. An enabled slot is cleared when its
// reset byte (+4065) is set, which the car table and the two gate bytes can raise.
// Otherwise the timer at +4032 counts down by the frame delta at 0x004f509c and, once it
// has run out, the latch at +4008 drops to 0.0; the timer at +4040 then counts down the
// same way and the displayed value at +4016 follows the floor at +4024, or the decaying
// value at +4048 (divided by the global divisor at $gp-31516 once the timer is past the
// threshold at +4056). Returns v0 = 1 when a stored value changed, 0 otherwise.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kPointerWord = 0x0051ba88U;   // lui 0x52, -17784
constexpr GuestAddress kSentinel = 0x0051a6a8U;      // 0x520000 - 22872
constexpr GuestAddress kCountWord = 0x0064d3a0U;     // lui 0x65, -11360
constexpr GuestAddress kCarTable = 0x00522660U;      // 0x520000 + 9824
constexpr GuestAddress kCarBias = 0x0012ad50U;       // added after the table stride
constexpr std::uint32_t kCarStride = 10192U;
constexpr GuestAddress kGateByteA = 0x00665ea0U;     // lui 0x66, 24224
constexpr GuestAddress kGateByteB = 0x00665e53U;     // lui 0x66, 24147
constexpr GuestAddress kFrameDelta = 0x004f509cU;    // lui 0x4f, 20636 (float)
constexpr std::int32_t kDivisorOffset = -31516;      // $gp - 31516 (float)
constexpr std::uint32_t kFloatOne = 0x3f800000U;
constexpr std::uint32_t kReturnPath = 2U;            // jr $ra and its delay slot, on every exit

constexpr GuestAddress kMapByte = 4001U;             // int8 at slot + 4001 selects the enable byte
constexpr GuestAddress kEnableByte = 4005U;          // u8 at slot + map + 4005
constexpr GuestAddress kLatchWord = 4008U;
constexpr GuestAddress kValueWord = 4016U;
constexpr GuestAddress kFloorWord = 4024U;
constexpr GuestAddress kTimerOneWord = 4032U;
constexpr GuestAddress kTimerTwoWord = 4040U;
constexpr GuestAddress kDecayWord = 4048U;
constexpr GuestAddress kThresholdWord = 4056U;
constexpr GuestAddress kResetByte = 4065U;

constexpr GuestAddress kCarFlagByte = 10184U;        // u8 in the car table entry
constexpr GuestAddress kCarStateWord = 4924U;        // word in the car table entry

template <class Context>
n3::Run advance_meter_slot_state(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress base = call.arg(0);
    // Only the low byte of a1 counts, sign-extended (dsll32/dsra32 by 56).
    const auto slot_signed =
        static_cast<std::int32_t>(static_cast<std::int8_t>(static_cast<std::uint8_t>(call.arg(1))));
    const auto slot_offset = static_cast<GuestAddress>(slot_signed);  // 32-bit wrapping address math
    const GuestAddress reset_byte = base + slot_offset + kResetByte;
    std::uint32_t retired = 8U;  // 0x22c340..0x22c35c, delay slot included
    std::uint32_t changed = 0U;  // v0

    const auto map = static_cast<std::int8_t>(m.read_u8(base + slot_offset + kMapByte));
    const GuestAddress enable_address = base + static_cast<GuestAddress>(static_cast<std::int32_t>(map)) + kEnableByte;
    if (m.read_u8(enable_address) == 0U) {
        call.return_value(0U);
        return call.finish(retired + kReturnPath);
    }

    // Reset request for this slot (0x22c364..0x22c424). The branch structure follows the
    // original: a null or sentinel object pointer skips the checks, and a slot past the
    // count is always reset.
    retired += 4U;
    const GuestAddress pointer = m.read_u32_le(kPointerWord);
    if (pointer != 0U) {
        retired += 4U;
        if (pointer != kSentinel) {
            retired += 5U;
            const auto count = static_cast<std::int32_t>(m.read_u32_le(kCountWord));
            if (slot_signed < count) {
                retired += 10U;
                const GuestAddress car = kCarTable + slot_offset * kCarStride + kCarBias;
                if (car != 0U) {
                    retired += 3U;
                    if (m.read_u8(car + kCarFlagByte) != 0U) {
                        retired += 3U;
                        m.write_u8(reset_byte, 1U);
                    } else {
                        retired += 4U;
                        if (m.read_u32_le(car + kCarStateWord) == 3U) {
                            retired += 3U;
                            m.write_u8(reset_byte, 1U);
                        }
                    }
                }
                // With car == 0 the words below are read at guest address 4924, as the original does.
                retired += 4U;
                bool set_reset = true;
                if (m.read_u8(kGateByteA) == 0U) {
                    retired += 4U;
                    if (m.read_u8(kGateByteB) == 0U) {
                        retired += 3U;
                        set_reset = m.read_u32_le(car + kCarStateWord) == 0U;
                    }
                }
                if (set_reset) {
                    retired += 4U;
                    m.write_u8(reset_byte, 1U);
                }
            } else {
                retired += 2U;
                m.write_u8(reset_byte, 1U);
            }
        }
    }

    // 0x22c428: a set reset byte clears the slot (0x22c4a4).
    retired += 5U;
    const GuestAddress slot = base + (slot_offset << 2U);
    if (m.read_u8(reset_byte) != 0U) {
        retired += 12U;
        m.write_u32_le(slot + kLatchWord, 0U);
        m.write_u32_le(slot + kValueWord, 0U);
        m.write_u32_le(slot + kTimerOneWord, 0U);
        m.write_u32_le(slot + kTimerTwoWord, 0U);
        m.write_u32_le(slot + kFloorWord, 0U);
        m.write_u32_le(slot + kDecayWord, 0U);
        m.write_u32_le(slot + kThresholdWord, 0U);
        m.write_u8(reset_byte, 0U);
        call.return_value(1U);
        return call.finish(retired + kReturnPath);
    }

    // 0x22c43c: the first timer counts down while it is positive (0x22c4d4).
    retired += 8U;
    const std::uint32_t timer_one = m.read_u32_le(slot + kTimerOneWord);
    if (!fpu::le(timer_one, 0U)) {
        retired += 6U;
        const std::uint32_t delta = m.read_u32_le(kFrameDelta);
        const std::uint32_t timer_one_next = fpu::sub(timer_one, delta);
        m.write_u32_le(slot + kTimerOneWord, timer_one_next);
        if (fpu::le(timer_one_next, 0U)) {
            retired += 3U;
            m.write_u32_le(slot + kLatchWord, 0U);
            changed = 1U;
        } else {
            retired += 7U;
            if (!fpu::eq(kFloatOne, m.read_u32_le(slot + kLatchWord))) {
                retired += 3U;
                m.write_u32_le(slot + kLatchWord, kFloatOne);
                changed = 1U;
            }
        }
    }

    // 0x22c45c: the second timer decides between holding the value and running it.
    retired += 7U;
    const std::uint32_t timer_two = m.read_u32_le(slot + kTimerTwoWord);
    if (fpu::le(timer_two, 0U)) {
        // 0x22c478: the value takes the floor when they differ, then joins the return block (0x22c49c).
        retired += 7U;
        const std::uint32_t floor_value = m.read_u32_le(slot + kFloorWord);
        const std::uint32_t value = m.read_u32_le(slot + kValueWord);
        if (!fpu::eq(floor_value, value)) {
            retired += 2U;
            m.write_u32_le(slot + kValueWord, floor_value);
            changed = 1U;
            retired += 2U;
        }
    } else {
        // 0x22c520: the second timer counts down.
        retired += 9U;
        const std::uint32_t delta = m.read_u32_le(kFrameDelta);
        const std::uint32_t timer_two_next = fpu::sub(timer_two, delta);
        m.write_u32_le(slot + kTimerTwoWord, timer_two_next);
        if (fpu::le(timer_two_next, 0U)) {
            // 0x22c5e0: the timer ran out, so the value follows the floor and the decay resets.
            retired += 5U;
            const std::uint32_t floor_value = m.read_u32_le(slot + kFloorWord);
            const std::uint32_t value = m.read_u32_le(slot + kValueWord);
            if (!fpu::eq(floor_value, value)) {
                retired += 2U;
                m.write_u32_le(slot + kValueWord, floor_value);
                changed = 1U;
            }
            retired += 3U;
            m.write_u32_le(slot + kDecayWord, 0U);
            retired += 2U;
        } else {
            // 0x22c544: the timer is still running.
            retired += 4U;
            const std::uint32_t threshold = m.read_u32_le(slot + kThresholdWord);
            if (fpu::le(timer_two_next, threshold)) {
                // 0x22c574: the timer is within the threshold, so the decay is divided by the divisor.
                retired += 10U;
                const std::uint32_t decay = m.read_u32_le(slot + kDecayWord);
                const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
                const std::uint32_t divisor = m.read_u32_le(gp + static_cast<GuestAddress>(kDivisorOffset));
                const std::uint32_t floor_value = m.read_u32_le(slot + kFloorWord);
                const std::uint32_t quotient = fpu::div(decay, divisor);
                if (fpu::le(quotient, floor_value)) {
                    retired += 4U;
                    const std::uint32_t value = m.read_u32_le(slot + kValueWord);
                    if (!fpu::eq(value, floor_value)) {
                        retired += 5U;
                        m.write_u32_le(slot + kValueWord, floor_value);
                        changed = 1U;
                        m.write_u32_le(slot + kDecayWord, floor_value);
                        retired += 2U;
                    }
                } else {
                    retired += 4U;
                    const std::uint32_t value = m.read_u32_le(slot + kValueWord);
                    if (!fpu::eq(value, quotient)) {
                        retired += 4U;
                        m.write_u32_le(slot + kValueWord, quotient);
                        changed = 1U;
                        m.write_u32_le(slot + kDecayWord, quotient);
                        retired += 2U;
                    }
                }
            } else {
                // 0x22c554: above the threshold the value takes the decay when they differ.
                retired += 5U;
                const std::uint32_t decay = m.read_u32_le(slot + kDecayWord);
                const std::uint32_t value = m.read_u32_le(slot + kValueWord);
                if (!fpu::eq(value, decay)) {
                    retired += 3U;
                    m.write_u32_le(slot + kValueWord, decay);
                    changed = 1U;
                    retired += 2U;
                }
            }
        }
    }

    call.return_value(changed);
    return call.finish(retired + kReturnPath);
}

} // namespace

extern constinit const EngineFunction n3_0022c340{0x0022c340U, 720U, 0xbbee9612ef75c195ULL, "AdvanceMeterSlotState",
                                                  128U, n3::live<&advance_meter_slot_state<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(0022c340, advance_meter_slot_state)
