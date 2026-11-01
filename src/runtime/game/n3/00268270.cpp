// 0x00268270 (a0 = object O, a1 = select flag, f12 = header float): RefreshSlotTargetValues. Returns nothing.
// Unless O[276] is zero while the byte at 0x0055d7f7 is set (then it returns at once), each item i below the count
// at Q+8 (Q = the word at O+64) reads its source pointer S = the word at O+16+4i. For slots 1..6 with S[slot] > 0,
// the item's table at O+28i+156+4*slot takes a value: slot 6 takes the header float; slots 1..5 take a jittered
// value when a1 is zero, or when a1 is set and the table entry is <= the header. The jitter steps the seed pair
// R0 = 0x0055d788 and R1 = 0x0055d78c (seed = (R0 << 16) + (R0 >> 16) + R1, R1 += seed, R0 = seed) and is
// j = 2 * (seed * 2^-32 - 0.5). The value is (header + d) + j * (g * d), with d = 60 / S[slot] and g the float at
// $gp - 29812. The FPU accumulator is left at the last (header + d).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kFlagOffset = 276U;             // byte at O + 276
constexpr GuestAddress kQueueOffset = 64U;             // word at O + 64: pointer Q
constexpr GuestAddress kCountOffset = 8U;              // word at Q + 8: item count
constexpr GuestAddress kSourceOffset = 16U;            // word at O + 16 + 4*item: source pointer S
constexpr GuestAddress kItemOffset = 156U;             // item table at O + 28*item + 156, floats by slot
constexpr std::uint32_t kItemStride = 28U;
constexpr std::uint32_t kSlotCount = 7U;               // slots 0..6; the loop covers 1..6
constexpr std::uint32_t kLastSlot = 6U;                // slot 6 takes the header float
constexpr GuestAddress kRandomA = 0x0055d788U;         // R0, the first seed word (lui 0x56, addiu -10360)
constexpr GuestAddress kRandomB = 0x0055d78cU;         // R1, the second seed word (lui 0x56, addiu -10356)
constexpr GuestAddress kEnableByte = 0x0055d7f7U;      // byte B (lui 0x56, lbu -10249)
constexpr std::uint32_t kGainDisplacement = 29812U;    // float g at $gp - 29812

constexpr std::uint32_t kScale = 0x2f800000U;          // 2^-32 (f7)
constexpr std::uint32_t kHalf = 0x3f000000U;           // 0.5 (f6)
constexpr std::uint32_t kTwo = 0x40000000U;            // 2.0 (f5)
constexpr std::uint32_t kSixty = 0x42700000U;          // 60.0 (f4)
constexpr std::uint32_t kZero = 0U;                    // +0.0 (f8)

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kEntryCost = 3U;               // 0x268270: lbu, beq and its nop
constexpr std::uint32_t kFlagClearCost = 4U;           // 0x2683d0: lui, lbu, bne and its nop
constexpr std::uint32_t kFlagSetCost = 1U;             // 0x26827c: lui
constexpr std::uint32_t kSetupBridgeCost = 2U;         // 0x2683e0: beq and the lui in its delay slot
constexpr std::uint32_t kSetupCost = 16U;              // 0x268280: constants, gain, counters
constexpr std::uint32_t kEarlyReturnCost = 2U;         // 0x26847c: jr and its nop
constexpr std::uint32_t kOuterTestCost = 5U;           // 0x2683b4: lw, lw, slt, bne and its nop
constexpr std::uint32_t kOuterExitCost = 4U;           // 0x2683c8 and 0x26847c: beq, nop, jr, nop
constexpr std::uint32_t kOuterBodyCost = 7U;           // 0x2682c0: item pointers and slot 1
constexpr std::uint32_t kOuterStepCost = 1U;           // 0x2683b0: addiu item
constexpr std::uint32_t kSourceTestCost = 7U;          // 0x2682dc: source load and its <= 0 test
constexpr std::uint32_t kSlotTestCost = 3U;            // 0x2682f8: slti, beq and its nop (slot < 6)
constexpr std::uint32_t kLastSlotStoreCost = 3U;       // 0x268470: addu, beq and the header store
constexpr std::uint32_t kSelectCost = 2U;              // 0x268304: beq a1 and its nop
constexpr std::uint32_t kExistingCost = 5U;            // 0x26830c: table load, c.ole.s, bc1f and its slot
constexpr std::uint32_t kSeedCost = 10U;               // 0x268320 / 0x2683e8: the seed step and its stores
constexpr std::uint32_t kNegativeSeedCost = 7U;        // seed < 0: the halving conversion
constexpr std::uint32_t kPositiveSeedCost = 3U;        // seed >= 0: mtc1, branch and cvt.s.w
constexpr std::uint32_t kFormulaCostA = 12U;           // 0x268370: the value and its store (falls through)
constexpr std::uint32_t kFormulaCostB = 14U;           // 0x268438: the value and its store (branches back)
constexpr std::uint32_t kIncrementCost = 4U;           // 0x2683a0: slot step and its test

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// One seed step: j = 2 * (seed * 2^-32 - 0.5), and whether the seed was negative (its conversion then takes
// the longer path).
struct Jitter {
    std::uint32_t value{};
    bool negative{};
};

// One computed table value (f0 after madd.s), and the FPU accumulator the adda.s before it left.
struct Value {
    std::uint32_t bits{};
    std::uint32_t accumulator{};
};

// What the body reports: the retired count (it stops early once that passes kCeiling), and the accumulator the
// last adda.s left when a slot took a value.
struct Outcome {
    std::uint32_t retired{};
    bool sets_accumulator{};
    std::uint32_t accumulator{};
};

// The seed step (0x268320..0x268344 or 0x2683e8..0x26840c): seed = (R0 << 16) + (R0 >> 16, arithmetic) + R1, then
// R1 += seed, R0 = seed. Both stores happen on every step, R0 first. The seed converts as a signed word; a negative
// seed (an unsigned word above 2^31) is halved with its low bit kept, converted, and doubled, as the listing does.
template <class Memory>
Jitter step_seed(Memory& m) {
    const std::uint32_t a = m.read_u32_le(kRandomA);
    const std::uint32_t b = m.read_u32_le(kRandomB);
    const std::uint32_t seed =
        ((a << 16U) + static_cast<std::uint32_t>(static_cast<std::int32_t>(a) >> 16U)) + b;
    m.write_u32_le(kRandomA, seed);
    m.write_u32_le(kRandomB, b + seed);
    Jitter jitter{};
    jitter.negative = static_cast<std::int32_t>(seed) < 0;
    std::uint32_t unit = 0U;
    if (jitter.negative) {
        const std::uint32_t halved = (seed >> 1U) | (seed & 1U);
        unit = fpu::from_int(static_cast<std::int32_t>(halved));
        unit = fpu::add(unit, unit);
    } else {
        unit = fpu::from_int(static_cast<std::int32_t>(seed));
    }
    jitter.value = fpu::mul(fpu::sub(fpu::mul(kScale, unit), kHalf), kTwo);
    return jitter;
}

// The value of one slot (0x268370 and 0x268438): d = 60 / sample, the accumulator is header + d (adda.s with
// ACC = f8 + f0, f8 = +0), and the value is ACC + j * (g * d) (madd.s fd = ACC + fs * ft).
template <class Memory>
Value slot_value(Memory& m, const GuestAddress source_slot, const std::uint32_t slot, const std::uint32_t header,
                 const std::uint32_t gain, const std::uint32_t jitter) {
    const GuestAddress samples = m.read_u32_le(source_slot);
    const std::uint32_t sample = m.read_u32_le(samples + 4U * slot);
    const std::uint32_t quotient = fpu::div(kSixty, sample);
    const std::uint32_t product = fpu::mul(gain, quotient);
    const std::uint32_t sum = fpu::add(header, quotient);
    const std::uint32_t accumulator = fpu::add(kZero, sum);
    return {fpu::madd(accumulator, jitter, product), accumulator};
}

// A seed step, the value and its store to the table entry. formula_cost is the rest of the path's count.
template <class Memory>
void write_jittered(Memory& m, Outcome& out, const GuestAddress source_slot, const GuestAddress target,
                    const std::uint32_t slot, const std::uint32_t header, const std::uint32_t gain,
                    const std::uint32_t formula_cost) {
    out.retired += kSeedCost;
    const Jitter jitter = step_seed(m);
    out.retired += jitter.negative ? kNegativeSeedCost : kPositiveSeedCost;
    const Value value = slot_value(m, source_slot, slot, header, gain, jitter.value);
    out.retired += formula_cost;
    out.sets_accumulator = true;
    out.accumulator = value.accumulator;
    m.write_u32_le(target, value.bits);
}

// The whole function on guest memory (the implementation) or on a CheckedDryMemory (call_cost). Above kCeiling the
// loops return early, so the dry run of a call the original would run long stays bounded.
template <class Memory>
Outcome refresh_slot_targets_body(Memory& m, const GuestAddress object, const bool select_zero,
                                  const std::uint32_t header, const GuestAddress gp) {
    Outcome out{};
    out.retired = kEntryCost;
    if (m.read_u8(object + kFlagOffset) == 0U) {
        out.retired += kFlagClearCost;
        if (m.read_u8(kEnableByte) != 0U) {
            out.retired += kEarlyReturnCost;
            return out;
        }
        out.retired += kSetupBridgeCost;
    } else {
        out.retired += kFlagSetCost;
    }
    out.retired += kSetupCost;
    const std::uint32_t gain = m.read_u32_le(gp - kGainDisplacement);
    for (std::uint32_t item = 0U;; ++item) {
        if (out.retired > kCeiling) {
            return out;
        }
        out.retired += kOuterTestCost;
        const GuestAddress queue = m.read_u32_le(object + kQueueOffset);
        const auto item_count = static_cast<std::int32_t>(m.read_u32_le(queue + kCountOffset));
        if (!(static_cast<std::int32_t>(item) < item_count)) {
            out.retired += kOuterExitCost;
            return out;
        }
        out.retired += kOuterBodyCost;
        const GuestAddress item_base = object + kItemStride * item;
        const GuestAddress source_slot = object + kSourceOffset + 4U * item;
        for (std::uint32_t slot = 1U; slot < kSlotCount; ++slot) {
            if (out.retired > kCeiling) {
                return out;
            }
            out.retired += kSourceTestCost;
            const GuestAddress samples = m.read_u32_le(source_slot);
            const std::uint32_t sample = m.read_u32_le(samples + 4U * slot);
            if (!fpu::le(sample, kZero)) {
                out.retired += kSlotTestCost;
                const GuestAddress target = item_base + kItemOffset + 4U * slot;
                if (slot >= kLastSlot) {
                    out.retired += kLastSlotStoreCost;
                    m.write_u32_le(target, header);
                } else {
                    out.retired += kSelectCost;
                    if (select_zero) {
                        write_jittered(m, out, source_slot, target, slot, header, gain, kFormulaCostB);
                    } else {
                        out.retired += kExistingCost;
                        if (fpu::le(m.read_u32_le(target), header)) {
                            write_jittered(m, out, source_slot, target, slot, header, gain, kFormulaCostA);
                        }
                    }
                }
            }
            out.retired += kIncrementCost;
        }
        out.retired += kOuterStepCost;
    }
}

// The dry run's view of guest memory. A word the original would fault on (unmapped or misaligned) is recorded and
// the call is declined, so the original runs and faults itself. Stores stay in the dry run's own overlay.
template <class Base>
class CheckedDryMemory {
public:
    explicit CheckedDryMemory(const Base& memory) noexcept : memory_(memory), dry_(memory) {}
    [[nodiscard]] std::uint8_t read_u8(const GuestAddress address) const {
        if (!memory_.contains(address, 1U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u8(address);
    }
    [[nodiscard]] std::uint32_t read_u32_le(const GuestAddress address) const {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            faulted_ = true;
            return 0U;
        }
        return dry_.read_u32_le(address);
    }
    void write_u32_le(const GuestAddress address, const std::uint32_t value) {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            faulted_ = true;
            return;
        }
        dry_.write_u32_le(address, value);
    }
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

private:
    const Base& memory_;
    n3::DryMemory dry_;
    mutable bool faulted_ = false;
};

template <class Context>
std::uint32_t refresh_slot_target_values_cost(const Context& context) {
    CheckedDryMemory dry{context.memory};
    const Outcome outcome = refresh_slot_targets_body(dry, context.cpu.read_gpr_u32(4U),
                                                      context.cpu.read_gpr_u64(5U) == 0U,
                                                      context.cpu.read_fpr_bits(12U),
                                                      context.cpu.read_gpr_u32(n3::kGlobalPointer));
    if (dry.faulted() || outcome.retired > kCeiling) {
        return 0U;
    }
    return outcome.retired;
}

template <class Context>
n3::Run refresh_slot_target_values(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    // beq $a1, $zero compares the whole doubleword.
    const Outcome outcome = refresh_slot_targets_body(call.memory(), call.arg(0U), call.arg64(1U) == 0U,
                                                      cpu.read_fpr_bits(12U),
                                                      cpu.read_gpr_u32(n3::kGlobalPointer));
    if (outcome.sets_accumulator) {
        cpu.set_fpu_accumulator_bits(outcome.accumulator);
    }
    return call.finish(outcome.retired);
}

} // namespace

extern constinit const EngineFunction n3_00268270{0x00268270U, 532U, 0xee36e3978636a5dbULL, "RefreshSlotTargetValues",
                                                  kCeiling, n3::live<&refresh_slot_target_values<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &refresh_slot_target_values_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00268270, refresh_slot_target_values, refresh_slot_target_values_cost)
