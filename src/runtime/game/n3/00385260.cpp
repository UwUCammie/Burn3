// 0x00385260 (a0 = effect record E): StepFadeState, a small state machine that ramps the scale of the object O
// attached to E. The phase byte at E+24 picks the step and the byte at E+25 counts the ramp's steps. Phase 3 ramps
// the scale up (old scale * counter/5) and phase 1 ramps it down (old scale * (1 - counter/5)). A fade out that
// runs its five steps sets bit 0x08 of E+27 and moves to phase 2; a control mode of 3 or 1 instead hands the fade
// over (mode 1 set, phase 2, bit 0x08 cleared). Phases 2 and 0 return at once. O is the word at E+20, its scale the
// float at O+160 and its flags the halfword at O+92. The control block C (the word at O+48) has a mode word at C+0
// that the steps write (1 or 2) and read. Nothing reads a result (EngineReturn::Nothing), so v0 is left alone.
//
// Each load and store is made in the listing's order through freshly read pointers, so an overlap between E, O and
// C behaves as in the original. The bltz tests in the ramps are taken only for a negative counter, which a byte
// never is, so the ramps convert the counter directly; their taken (dead) forms are covered by the declared cost,
// the longest listed path.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

// The effect record E (a0).
constexpr GuestAddress kSavedWord = 8U;         // E+8: copied from C+20 when a fade out finishes, written to C+24 by a restart
constexpr GuestAddress kObject = 20U;           // E+20: the object O
constexpr GuestAddress kPhase = 24U;            // E+24: byte
constexpr GuestAddress kCounter = 25U;          // E+25: byte
constexpr GuestAddress kFlags = 27U;            // E+27: byte
constexpr std::uint32_t kFinishedBit = 0x08U;   // in E+27: set by a finished fade out, cleared by a handover
// The object O and its control block C.
constexpr GuestAddress kControl = 48U;          // O+48: C
constexpr GuestAddress kMode = 0U;              // C+0
constexpr GuestAddress kControlSaved = 20U;     // C+20
constexpr GuestAddress kControlRestore = 24U;   // C+24
constexpr GuestAddress kObjectBits = 92U;       // O+92: halfword
constexpr GuestAddress kScale = 160U;           // O+160: float
constexpr std::uint32_t kActiveBit = 0x0002U;   // O+92
constexpr std::uint32_t kRestoreBit = 0x0080U;  // O+92
constexpr std::uint32_t kSteps = 5U;
constexpr std::uint32_t kFiveBits = 0x40a00000U;  // 5.0f
constexpr std::uint32_t kOneBits = 0x3f800000U;   // 1.0f

// Retired instruction counts of the listing's blocks, delay slots included, named by their first address.
constexpr std::uint32_t kEntryCost = 4U;       // 0x385260: lbu, addiu, beq (phase 3), nop
constexpr std::uint32_t kPhaseTwoCost = 3U;    // 0x385270: addiu, beq (phase 2), nop
constexpr std::uint32_t kPhaseOneCost = 3U;    // 0x38527c: addiu, beq (phase 1), nop
constexpr std::uint32_t kPhaseZeroCost = 2U;   // 0x385288: beq (phase 0), nop
constexpr std::uint32_t kOtherPhaseCost = 2U;  // 0x385290: beq to 0x3854bc, nop
constexpr std::uint32_t kExitCost = 2U;        // 0x3854bc: beq to the return, nop
constexpr std::uint32_t kReturnCost = 2U;      // 0x385534: jr ra, nop

// 0x385500 (13): a fade that begins again: mode 2, bit 0x80 on O+92, C+24 = E+8.
template <class Memory>
void restart_fade(Memory& m, const GuestAddress e) {
    const GuestAddress first = m.read_u32_le(e + kObject);
    const GuestAddress first_control = m.read_u32_le(first + kControl);
    m.write_u32_le(first_control + kMode, 2U);
    const GuestAddress second = m.read_u32_le(e + kObject);
    const std::uint32_t bits = m.read_u16_le(second + kObjectBits);
    m.write_u16_le(second + kObjectBits, static_cast<std::uint16_t>(bits | kRestoreBit));
    const GuestAddress third = m.read_u32_le(e + kObject);
    const std::uint32_t saved = m.read_u32_le(e + kSavedWord);
    const GuestAddress third_control = m.read_u32_le(third + kControl);
    m.write_u32_le(third_control + kControlRestore, saved);
}

// The common step (0x385398 on), which ends a fade in. Returns the retired count up to the exit's branch.
template <class Memory>
std::uint32_t common_step(Memory& m, const GuestAddress e) {
    // 0x385398 (4): five or more steps end the ramp (0x3854b4, 2).
    std::uint32_t retired = 4U;
    const std::uint32_t counter = m.read_u8(e + kCounter);
    if (counter >= kSteps) {
        m.write_u8(e + kPhase, 0U);
        m.write_u8(e + kCounter, 0U);
        return retired + 2U;
    }
    // 0x3853a8 (4) and the phase test. Phase 2 takes bit 0x08 of E+27 as its condition (0x38541c, 4, then 2). Phases
    // 3, 1 and 0 take "the control mode is neither 3 nor 1": 3, 6 or 8 retired up to 0x3853e0, then the mode test.
    // Any other phase is false (10 retired).
    retired += 4U;
    const std::uint32_t phase = m.read_u8(e + kPhase);
    bool condition = false;
    if (phase == 2U) {
        retired += 4U + 2U;
        condition = (m.read_u8(e + kFlags) & kFinishedBit) != 0U;
    } else if (phase == 3U || phase == 1U || phase == 0U) {
        retired += phase == 3U ? 3U : (phase == 1U ? 6U : 8U);
        // 0x3853e0 (7): the control mode. Modes other than 3 also run 0x3853fc (2), and 0x385404 (1) unless it is 1.
        const GuestAddress object = m.read_u32_le(e + kObject);
        const GuestAddress control = m.read_u32_le(object + kControl);
        const std::uint32_t mode = m.read_u32_le(control + kMode);
        retired += 7U;
        if (mode != 3U) {
            retired += 2U;
            if (mode != 1U) {
                retired += 1U;
            }
        }
        retired += 5U;  // 0x385408
        condition = mode != 3U && mode != 1U;
    } else {
        retired += 10U;
    }
    // 0x385440 (2): a false condition clears the phase and the counter (0x3854b4, 2).
    retired += 2U;
    if (!condition) {
        m.write_u8(e + kPhase, 0U);
        m.write_u8(e + kCounter, 0U);
        return retired + 2U;
    }
    // 0x385448 (3), 0x385454 (3), 0x38547c (14): the scale becomes old * counter/5 and the counter steps.
    retired += 6U;
    const GuestAddress target = m.read_u32_le(e + kObject);
    const std::uint32_t old_scale = m.read_u32_le(target + kScale);
    const std::uint32_t ratio = fpu::div(fpu::from_int(static_cast<std::int32_t>(counter)), kFiveBits);
    const std::uint32_t bits = m.read_u16_le(target + kObjectBits);
    m.write_u16_le(target + kObjectBits, static_cast<std::uint16_t>(bits | kActiveBit));
    const GuestAddress target_again = m.read_u32_le(e + kObject);
    m.write_u32_le(target_again + kScale, fpu::mul(old_scale, ratio));
    const std::uint32_t next = m.read_u8(e + kCounter) + 1U;
    m.write_u8(e + kCounter, static_cast<std::uint8_t>(next));
    return retired + 14U;
}

// Phase 3 (0x38537c on): the fade in. Returns the retired count up to the exit's branch.
template <class Memory>
std::uint32_t fade_in(Memory& m, const GuestAddress e) {
    // 0x38537c (4): without bit 0x08 the phase is cleared (0x3854f8, 2).
    std::uint32_t retired = 4U;
    const std::uint32_t flags = m.read_u8(e + kFlags);
    if ((flags & kFinishedBit) == 0U) {
        m.write_u8(e + kPhase, 0U);
        return retired + 2U;
    }
    // 0x38538c (3): a counter of 0 begins the fade again (0x385500, 13), then the common step.
    retired += 3U;
    if (m.read_u8(e + kCounter) == 0U) {
        restart_fade(m, e);
        retired += 13U;
    }
    return retired + common_step(m, e);
}

// Phase 1 (0x385298 on): the fade out. Returns the retired count up to the exit's branch.
template <class Memory>
std::uint32_t fade_out(Memory& m, const GuestAddress e) {
    // 0x385298 (4): five or more steps finish the fade (0x3854c4, 13).
    std::uint32_t retired = 4U;
    const std::uint32_t counter = m.read_u8(e + kCounter);
    if (counter >= kSteps) {
        const std::uint32_t flags = m.read_u8(e + kFlags);
        m.write_u8(e + kFlags, static_cast<std::uint8_t>(flags | kFinishedBit));
        const GuestAddress first = m.read_u32_le(e + kObject);
        const GuestAddress first_control = m.read_u32_le(first + kControl);
        const std::uint32_t saved = m.read_u32_le(first_control + kControlSaved);
        m.write_u32_le(e + kSavedWord, saved);
        const GuestAddress second = m.read_u32_le(e + kObject);
        const GuestAddress second_control = m.read_u32_le(second + kControl);
        m.write_u32_le(second_control + kMode, 1U);
        m.write_u8(e + kCounter, 0U);
        m.write_u8(e + kPhase, 2U);
        return retired + 13U;
    }
    // 0x3852a8 (5): the object and its control mode. Mode 3 (0x3852c8, 5); mode 1 (0x3852bc, 2; 0x3852c8, 5); any
    // other mode (0x3852bc, 2; 0x3852c4, 1; 0x3852c8, 5). Modes 3 and 1 hand the fade over (0x385350).
    const GuestAddress object = m.read_u32_le(e + kObject);
    const GuestAddress control = m.read_u32_le(object + kControl);
    const std::uint32_t mode = m.read_u32_le(control + kMode);
    retired += 5U;
    if (mode == 3U) {
        retired += 5U;
    } else if (mode == 1U) {
        retired += 7U;
    } else {
        retired += 8U;
    }
    if (mode == 3U || mode == 1U) {
        // 0x385350 (11): counter 0, control mode 1, phase 2, bit 0x08 cleared.
        m.write_u8(e + kCounter, 0U);
        const GuestAddress handover = m.read_u32_le(e + kObject);
        const GuestAddress handover_control = m.read_u32_le(handover + kControl);
        m.write_u32_le(handover_control + kMode, 1U);
        m.write_u8(e + kPhase, 2U);
        const std::uint32_t flags = m.read_u8(e + kFlags);
        m.write_u8(e + kFlags, static_cast<std::uint8_t>(flags & 0xf7U));
        return retired + 11U;
    }
    // 0x3852dc (2), 0x3852e4 (3), 0x38530c (17): the scale becomes old * (1 - counter/5); the counter steps.
    const std::uint32_t old_scale = m.read_u32_le(object + kScale);
    const std::uint32_t ratio = fpu::div(fpu::from_int(static_cast<std::int32_t>(counter)), kFiveBits);
    const std::uint32_t bits = m.read_u16_le(object + kObjectBits);
    m.write_u16_le(object + kObjectBits, static_cast<std::uint16_t>(bits | kActiveBit));
    const GuestAddress target = m.read_u32_le(e + kObject);
    m.write_u32_le(target + kScale, fpu::mul(old_scale, fpu::sub(kOneBits, ratio)));
    const std::uint32_t next = m.read_u8(e + kCounter) + 1U;
    m.write_u8(e + kCounter, static_cast<std::uint8_t>(next));
    return retired + 22U;
}

// The whole function on guest memory: returns the retired count.
template <class Memory>
std::uint32_t step_fade_state_body(Memory& m, const GuestAddress e) {
    std::uint32_t retired = kEntryCost;
    const std::uint32_t phase = m.read_u8(e + kPhase);
    if (phase == 3U) {
        return retired + fade_in(m, e) + kExitCost + kReturnCost;
    }
    retired += kPhaseTwoCost;
    if (phase == 2U) {
        return retired + kReturnCost;
    }
    retired += kPhaseOneCost;
    if (phase == 1U) {
        return retired + fade_out(m, e) + kExitCost + kReturnCost;
    }
    retired += kPhaseZeroCost;
    if (phase == 0U) {
        return retired + kReturnCost;
    }
    return retired + kOtherPhaseCost + kExitCost + kReturnCost;
}

template <class Context>
n3::Run step_fade_state(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(step_fade_state_body(call.memory(), call.arg(0)));
}

} // namespace

// The scale is a float the check compares bit for bit: the default tolerance would accept a one-ULP error here.
extern constinit const EngineFunction n3_00385260{0x00385260U, 732U, 0xcd7a6d719ef89ddeULL, "StepFadeState", 85U,
                                                  n3::live<&step_fade_state<NativeFunctionContext>>, EngineReturn::Nothing, {}, 0.0, 0.0};

} // namespace takedown::runtime

N3_BODY_PLAIN(00385260, step_fade_state)
