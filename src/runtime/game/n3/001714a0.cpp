// 0x001714a0 (a0 = vehicle, a1 = record array, a2 = record index, f12 = distance): ApplyProximityNudge.
// Record a1 + 112 * a2 has its position quadword at +48 and a flag byte at +105. The table entry
// 0x00556870 + 80 * (byte +73 of the state, the state being the vehicle word +276) has the reference point at +48
// and the weights at +0. A VU0 sequence turns them into f2, the weighted signed offset of the position from the
// reference. The flag picks one of two tests on the distance f12 and on the vehicle's float timer at +240 + 4 * index,
// and f2 must then lie in a window. A hit raises the vehicle float +292 to at least 3.0 (flag set) or 2.0 (flag
// clear), adds 1.5 (flag set) or 1.0 (flag clear) to the state float +40 when f2 is positive and subtracts it when f2
// is zero or negative, and returns 1. A flag-clear hit whose timer is above 10 also stores the index at +368; unless
// byte +370 is already set, it first sets +370 to 2, copies the global counter (at $gp - 24508) to +376, advances the
// counter modulo 5 and clears word +324 and byte +377. Straight-line code with no calls: the retired count is 26 on
// the shortest listing path and 121 on the longest (the declared cost).

#include "takedown/guest/ee_cop2_macro.hpp"
#include "takedown/runtime/game/n3_support.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;
namespace ee_cop2 = guest::ee_cop2;

constexpr GuestAddress kEntryTable = 0x00556870U;  // 0x00522660 + 0x00034210 (lui, addiu, lui, ori), plus 80 per row
constexpr std::uint32_t kEntryStride = 80U;
constexpr std::uint32_t kEntryReference = 48U;     // entry + 48: reference point (VF1)
constexpr std::uint32_t kEntryWeights = 0U;        // entry + 0: weights (VF3)
constexpr std::uint32_t kRecordStride = 112U;
constexpr std::uint32_t kRecordPosition = 48U;     // record + 48: position (VF2)
constexpr std::uint32_t kRecordFlag = 105U;        // record + 105: byte, nonzero selects branch A
constexpr std::uint32_t kStateRow = 73U;           // state + 73: byte selecting the table row
constexpr std::uint32_t kStateNudge = 40U;         // state + 40: float
constexpr std::uint32_t kVehicleState = 276U;      // vehicle + 276: state pointer
constexpr std::uint32_t kVehicleTimers = 240U;     // vehicle + 240 + 4 * index: float timer
constexpr std::uint32_t kVehicleNearest = 292U;    // vehicle + 292: float raised to at least 3.0 or 2.0
constexpr std::uint32_t kVehicleWord324 = 324U;
constexpr std::uint32_t kVehicleIndex = 368U;      // byte: the record index
constexpr std::uint32_t kVehicleByte370 = 370U;    // byte: latch
constexpr std::uint32_t kVehicleCopy376 = 376U;    // byte: copy of the global counter
constexpr std::uint32_t kVehicleByte377 = 377U;
constexpr std::uint32_t kCounterOffset = 0xffffa044U;  // $gp - 24508: the global counter byte
constexpr std::uint8_t kLatchValue = 2U;
constexpr std::uint32_t kCounterModulus = 5U;

// Float bit patterns (the listing loads them with lui).
constexpr std::uint32_t kForty = 0x42200000U;       // 40.0
constexpr std::uint32_t kTwentyFive = 0x41c80000U;  // 25.0
constexpr std::uint32_t kTen = 0x41200000U;         // 10.0
constexpr std::uint32_t kMinusTen = 0xc1200000U;    // -10.0
constexpr std::uint32_t kSix = 0x40c00000U;         // 6.0
constexpr std::uint32_t kMinusSix = 0xc0c00000U;    // -6.0
constexpr std::uint32_t kOne = 0x3f800000U;         // 1.0
constexpr std::uint32_t kMinusOne = 0xbf800000U;    // -1.0
constexpr std::uint32_t kTwo = 0x40000000U;         // 2.0
constexpr std::uint32_t kThree = 0x40400000U;       // 3.0
constexpr std::uint32_t kOneAndHalf = 0x3fc00000U;  // 1.5

constexpr std::uint8_t kMaskX = 8U;                 // VU mask bit of the x lane

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kPrologueCost = 17U;        // 0x1714a0..0x1714e0, the same for both flag outcomes
constexpr std::uint32_t kBlockCost = 6U;            // a six-instruction distance, range or sign test
constexpr std::uint32_t kTimerTestCost = 8U;        // 0x1714fc..0x171518 (branch A), 0x171620..0x17163c (branch B)
constexpr std::uint32_t kDotCost = 19U;             // 0x17153c..0x171584 (branch A), 0x171660..0x1716a8 (branch B)
constexpr std::uint32_t kHitCost = 11U;             // the window hit, with the max.s store
constexpr std::uint32_t kNegativeCost = 6U;         // f2 <= 0: state float minus its step
constexpr std::uint32_t kPositiveCost = 7U;         // f2 > 0: state float plus its step, then a jump
constexpr std::uint32_t kTailCost = 9U;             // 0x171754..0x171774: the timer reload and its test
constexpr std::uint32_t kTailBranchCost = 3U;       // 0x171778..0x171780: the latch test
constexpr std::uint32_t kLatchCost = 12U;           // 0x171784..0x1717b0: latch, counter and flag stores
constexpr std::uint32_t kReturnOneCost = 4U;        // beq and addiu (v0 = 1), then the return jr and nop
constexpr std::uint32_t kReturnOneTailCost = 5U;    // 0x1717b4 (index store), then the return
constexpr std::uint32_t kReturnZeroCost = 3U;       // daddu (v0 = 0), then the return
constexpr std::uint32_t kReturnZeroJumpCost = 4U;   // beq and daddu (v0 = 0), then the return

// MAX.S on raw words, with the EE's sign/magnitude ordering (ee_cop1::max_s).
std::uint32_t max_word(const std::uint32_t left, const std::uint32_t right) {
    const auto signed_left = std::bit_cast<std::int32_t>(left);
    const auto signed_right = std::bit_cast<std::int32_t>(right);
    return std::bit_cast<std::uint32_t>(signed_left < 0 && signed_right < 0 ? std::min(signed_left, signed_right)
                                                                            : std::max(signed_left, signed_right));
}

// The VU0 sequence of 0x17153c..0x171584 on the real VU0 state: VF1..VF3, ACC and the MAC/STATUS flags end as the
// original leaves them. Returns f2, the low word of VF1 after the final VMADDZ.X.
std::uint32_t dot_on_vu0(guest::R5900State& cpu, const guest::Gpr128& reference, const guest::Gpr128& position,
                         const guest::Gpr128& weights) {
    cpu.write_vu0_vector(1U, reference);                                          // lqc2 vf1,48(entry)
    cpu.write_vu0_vector(2U, position);                                           // lqc2 vf2,48(record)
    cpu.write_vu0_vector(3U, weights);                                            // lqc2 vf3,0(entry)
    ee_cop2::vadd_vector(cpu, 1U, 2U, 1U, n3::kMaskXyz, true);                    // vsub.xyz vf1,vf2,vf1
    ee_cop2::vmul(cpu, 2U, 1U, 3U, n3::kMaskXyz);                                 // vmul.xyz vf2,vf1,vf3
    ee_cop2::vadd_broadcast(cpu, 1U, 0U, 0U, kMaskX, 3U, false);                  // vaddw.x vf1,vf0,vf0w
    const guest::Gpr128 saved = cpu.read_vu0_vector(1U);                          // qmfc2 v0,vf1
    ee_cop2::vadda_broadcast(cpu, 2U, 2U, kMaskX, 1U);                            // vadday.x acc,vf2,vf2y
    cpu.write_vu0_vector(1U, saved);                                              // qmtc2 v0,vf1
    ee_cop2::vmadd_broadcast(cpu, 1U, 1U, 2U, kMaskX, 2U, false, false, false);   // vmaddz.x vf1,vf1,vf2z
    return cpu.read_vu0_vector(1U).words[0];                                      // qmfc2 v0,vf1; mtc1 v0,f2
}

// The same sequence on values alone, for call_cost: the path depends only on f2, and the VU0 flags never change
// f2. The old ACC is not an input, because its x lane is written from the operands and only x is read.
std::uint32_t dot_value(const guest::Gpr128& reference, const guest::Gpr128& position,
                        const guest::Gpr128& weights) {
    const guest::Gpr128 difference =
        guest::execute_vu0_macro_vector_add_subtract(reference, position, reference, n3::kMaskXyz, true).value;
    const guest::Gpr128 weighted =
        guest::execute_vu0_macro_vector_multiply(position, difference, weights, n3::kMaskXyz).value;
    const guest::Gpr128 offset =
        guest::execute_vu0_macro_broadcast_add_subtract(difference, n3::kVf0, n3::kVf0, kMaskX, 3U, false).value;
    const guest::Gpr128 accumulator =
        guest::execute_vu0_macro_broadcast_add_subtract(weighted, weighted, weighted, kMaskX, 1U, false).value;
    return guest::execute_vu0_macro_broadcast_multiply_accumulate(offset, accumulator, offset, weighted, kMaskX, 2U,
                                                                  false)
        .value.words[0];
}

// The read-only view call_cost runs the body on: a DryMemory that also records whether the original would have
// faulted (an address outside guest memory, or a word access that is not 4-byte aligned; lqc2 needs 16). The
// implementation is then not run and the original faults by itself.
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
        if (!word_ok(address)) {
            return 0U;
        }
        return dry_.read_u32_le(address);
    }
    void write_u8(const GuestAddress address, const std::uint8_t value) {
        if (!memory_.contains(address, 1U)) {
            faulted_ = true;
            return;
        }
        dry_.write_u8(address, value);
    }
    void write_u32_le(const GuestAddress address, const std::uint32_t value) {
        if (!word_ok(address)) {
            return;
        }
        dry_.write_u32_le(address, value);
    }
    // lqc2 faults unless the address is 16-byte aligned and inside guest memory.
    void check_quad(const GuestAddress address) const {
        if ((address & 15U) != 0U || !memory_.contains(address, 16U)) {
            faulted_ = true;
        }
    }
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

private:
    [[nodiscard]] bool word_ok(const GuestAddress address) const {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            faulted_ = true;
            return false;
        }
        return true;
    }

    const Base& memory_;
    n3::DryMemory dry_;
    mutable bool faulted_ = false;
};

// What one call did: the retired count, the v0 result, and LO/HI when the call reached the division.
struct Outcome {
    std::uint32_t retired{};
    std::uint32_t result{};
    bool divided{};
    std::uint32_t quotient{};
    std::uint32_t remainder{};
};

// The whole function on guest memory (the implementation) or on a CheckedDryMemory (call_cost). `dot` gives f2 for
// (entry, record) and is called before any store, as the original's VU0 sequence runs before them.
template <class Memory, class Dot>
Outcome nudge(Memory& m, const GuestAddress vehicle, const GuestAddress records, const std::uint32_t index,
              const std::uint32_t distance, const GuestAddress counter, Dot&& dot) {
    const GuestAddress state = m.read_u32_le(vehicle + kVehicleState);
    const GuestAddress record = records + kRecordStride * index;
    const std::uint32_t flag = m.read_u8(record + kRecordFlag);
    const std::uint32_t row = m.read_u8(state + kStateRow);
    const GuestAddress entry = kEntryTable + kEntryStride * row;
    const GuestAddress timer = vehicle + kVehicleTimers + 4U * index;
    const auto index_byte = static_cast<std::uint8_t>(index);
    std::uint32_t retired = kPrologueCost;

    if (flag != 0U) {
        // Branch A (0x1714e4): the distance must be below 40, and below 25 unless the timer is at most 0.
        retired += kBlockCost;
        if (!fpu::lt(distance, kForty)) {
            return {retired + kReturnZeroCost, 0U};
        }
        retired += kTimerTestCost;
        if (!fpu::le(m.read_u32_le(timer), 0U)) {
            retired += kBlockCost;
            if (!fpu::lt(distance, kTwentyFive)) {
                return {retired + kReturnZeroJumpCost, 0U};
            }
        }
        // 0x17153c: f2 must lie in (-10, 10).
        retired += kDotCost;
        const std::uint32_t f2 = dot(entry, record);
        if (fpu::le(f2, kMinusTen)) {
            return {retired + kReturnZeroCost, 0U};
        }
        retired += kBlockCost;
        if (!fpu::lt(f2, kTen)) {
            return {retired + kReturnZeroCost, 0U};
        }
        // 0x1715a0: the hit.
        retired += kHitCost;
        m.write_u32_le(vehicle + kVehicleNearest, max_word(kThree, m.read_u32_le(vehicle + kVehicleNearest)));
        if (fpu::le(f2, 0U)) {
            retired += kNegativeCost;
            const GuestAddress holder = m.read_u32_le(vehicle + kVehicleState);
            const std::uint32_t nudged = fpu::sub(m.read_u32_le(holder + kStateNudge), kOneAndHalf);
            m.write_u32_le(holder + kStateNudge, nudged);
        } else {
            retired += kPositiveCost;
            const GuestAddress holder = m.read_u32_le(vehicle + kVehicleState);
            const std::uint32_t nudged = fpu::add(m.read_u32_le(holder + kStateNudge), kOneAndHalf);
            m.write_u32_le(holder + kStateNudge, nudged);
        }
        return {retired + kReturnOneCost, 1U};
    }

    // Branch B (0x171608): the flag is clear. A negative timer makes the distance test "below 10".
    retired += kBlockCost;
    if (fpu::lt(distance, kTwentyFive)) {
        retired += kTimerTestCost;
        if (fpu::lt(m.read_u32_le(timer), 0U)) {
            retired += kBlockCost;
            if (!fpu::lt(distance, kTen)) {
                return {retired + kReturnZeroJumpCost, 0U};
            }
        }
    }
    // 0x171660: f2 must lie in (-6, -1) or (1, 6).
    retired += kDotCost;
    const std::uint32_t f2 = dot(entry, record);
    bool hit = false;
    if (!fpu::le(f2, kMinusSix)) {
        retired += kBlockCost;
        hit = fpu::lt(f2, kMinusOne);
    }
    if (!hit) {
        retired += kBlockCost;
        if (fpu::le(f2, kOne)) {
            return {retired + kReturnZeroCost, 0U};
        }
        retired += kBlockCost;
        if (!fpu::lt(f2, kSix)) {
            return {retired + kReturnZeroCost, 0U};
        }
    }
    // 0x1716f4: the hit.
    retired += kHitCost;
    m.write_u32_le(vehicle + kVehicleNearest, max_word(kTwo, m.read_u32_le(vehicle + kVehicleNearest)));
    if (fpu::le(f2, 0U)) {
        retired += kNegativeCost;
        const GuestAddress holder = m.read_u32_le(vehicle + kVehicleState);
        const std::uint32_t nudged = fpu::sub(m.read_u32_le(holder + kStateNudge), kOne);
        m.write_u32_le(holder + kStateNudge, nudged);
    } else {
        retired += kPositiveCost;
        const GuestAddress holder = m.read_u32_le(vehicle + kVehicleState);
        const std::uint32_t nudged = fpu::add(m.read_u32_le(holder + kStateNudge), kOne);
        m.write_u32_le(holder + kStateNudge, nudged);
    }
    // 0x171754: the timer is read again, after the stores above.
    retired += kTailCost;
    if (fpu::le(m.read_u32_le(timer), kTen)) {
        return {retired + kReturnOneCost, 1U};
    }
    retired += kTailBranchCost;
    if (static_cast<std::int8_t>(m.read_u8(vehicle + kVehicleByte370)) != 0) {
        m.write_u8(vehicle + kVehicleIndex, index_byte);
        return {retired + kReturnOneTailCost, 1U};
    }
    // 0x171784: the latch, the counter copy, the counter step and the flags.
    retired += kLatchCost;
    m.write_u8(vehicle + kVehicleByte370, kLatchValue);
    m.write_u8(vehicle + kVehicleCopy376, m.read_u8(counter));
    const std::uint32_t next = m.read_u8(counter) + 1U;
    const std::uint32_t quotient = next / kCounterModulus;
    const std::uint32_t remainder = next % kCounterModulus;
    m.write_u8(counter, static_cast<std::uint8_t>(remainder));
    m.write_u32_le(vehicle + kVehicleWord324, 0U);
    m.write_u8(vehicle + kVehicleByte377, 0U);
    m.write_u8(vehicle + kVehicleIndex, index_byte);
    return {retired + kReturnOneTailCost, 1U, true, quotient, remainder};
}

template <class Context>
n3::Run apply_proximity_nudge(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    auto& m = call.memory();
    const Outcome outcome = nudge(
        m, call.arg(0U), call.arg(1U), call.arg(2U), cpu.read_fpr_bits(12U),
        cpu.read_gpr_u32(n3::kGlobalPointer) + kCounterOffset,
        [&cpu, &m](const GuestAddress entry, const GuestAddress record) {
            return dot_on_vu0(cpu, n3::load_quad_any(m, entry + kEntryReference),
                              n3::load_quad_any(m, record + kRecordPosition), n3::load_quad_any(m, entry + kEntryWeights));
        });
    if (outcome.divided) {
        cpu.write_lo(0U, outcome.quotient);
        cpu.write_hi(0U, outcome.remainder);
    }
    call.return_value(outcome.result);
    return call.finish(outcome.retired);
}

// Runs the same body on a CheckedDryMemory: the exact count of the path the call takes, or 0 when the original would
// fault on one of its accesses (the original then runs and faults itself).
template <class Context>
std::uint32_t apply_proximity_nudge_cost(const Context& context) {
    CheckedDryMemory dry{context.memory};
    const Outcome outcome = nudge(
        dry, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u32(5U), context.cpu.read_gpr_u32(6U),
        context.cpu.read_fpr_bits(12U), context.cpu.read_gpr_u32(n3::kGlobalPointer) + kCounterOffset,
        [&dry](const GuestAddress entry, const GuestAddress record) {
            dry.check_quad(entry + kEntryReference);
            dry.check_quad(record + kRecordPosition);
            dry.check_quad(entry + kEntryWeights);
            return dot_value(n3::load_quad_any(dry, entry + kEntryReference),
                             n3::load_quad_any(dry, record + kRecordPosition),
                             n3::load_quad_any(dry, entry + kEntryWeights));
        });
    return dry.faulted() ? 0U : outcome.retired;
}

} // namespace

extern constinit const EngineFunction n3_001714a0{0x001714a0U, 812U, 0x45395fffccb3dcd4ULL, "ApplyProximityNudge",
                                                  121U, n3::live<&apply_proximity_nudge<NativeFunctionContext>>, EngineReturn::Integer, {}, 1e-4, 1e-6,
                                                  {}, &apply_proximity_nudge_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(001714a0, apply_proximity_nudge, apply_proximity_nudge_cost)
