// 0x00354350 (a0 = object): SelectBestTargetEntry. The object holds a signed count at +4144 and 112-byte
// records from +736 (record i at a0 + 112 * i + 736). A record is considered when the car pointer at record + 64
// leads to a signed kind byte (car + 533) of 1..4, and for kind 4 only when the byte at car + 9256 is zero.
// It needs x (record + 80) not <= 0 and y (record + 84) < 1.5 (FPU compares on the raw bits). Its score is the
// float at $gp - 30980 times word 3 of the quadword at car + 176 (lq drops the low four address bits). The first
// record with the largest score above 20.0 wins (a tie keeps the earlier one). Returns that record's pointer in
// v0, or 0. The loop runs over data, so call_cost runs the same body on a DryMemory and declines above kCeiling
// (the original then runs).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kCountOffset = 4144U;       // int32 count at object + 4144
constexpr GuestAddress kRecordOffset = 736U;       // record i at object + 112 * i + 736
constexpr std::uint32_t kRecordStride = 112U;      // ((i << 3) - i) << 4
constexpr GuestAddress kCarOffset = 64U;           // car pointer at record + 64
constexpr GuestAddress kKindOffset = 533U;         // signed kind byte at car + 533
constexpr GuestAddress kFlagOffset = 9256U;        // flag byte at car + 9256 (read for kind 4 only)
constexpr GuestAddress kQuadOffset = 176U;         // quadword at car + 176
constexpr GuestAddress kXOffset = 80U;             // float x at record + 80
constexpr GuestAddress kYOffset = 84U;             // float y at record + 84
constexpr GuestAddress kScaleOffset = 30980U;      // float scale at $gp - 30980
constexpr std::uint32_t kStartBest = 0x41a00000U;  // 20.0f
constexpr std::uint32_t kLimitY = 0x3fc00000U;     // 1.5f

// Retired instruction counts of the blocks, delay slots included.
constexpr std::uint32_t kPrologueCost = 16U;       // 0x354350..0x35438c
constexpr std::uint32_t kHeadCost = 3U;            // slt, beq and nop of the loop test
constexpr std::uint32_t kExitCost = 2U;            // jr $ra and its delay slot
constexpr std::uint32_t kRecordCost = 9U;          // record and car, the kind byte, the kind == 1 test
constexpr std::uint32_t kPairCost = 2U;            // a two-instruction block (a test or a jump with its delay slot)

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a call reports
// is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

struct Selection {
    GuestAddress result;
    std::uint32_t retired;
};

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost). Returns the record
// pointer (or 0) and the retired count, or a count above kCeiling as soon as it passes that.
template <class Memory>
Selection select_best_target(Memory& m, const GuestAddress object, const GuestAddress gp) {
    // The prologue loads the scale once (lwc1 $f1, -30980($gp)); the loop only reads memory.
    const std::uint32_t scale = m.read_u32_le(gp - kScaleOffset);
    std::uint32_t best = kStartBest;
    GuestAddress result = 0U;
    std::uint32_t retired = kPrologueCost;
    const auto count = static_cast<std::int32_t>(m.read_u32_le(object + kCountOffset));
    for (std::int32_t index = 0;; ++index) {
        retired += kHeadCost;
        if (retired > kCeiling) {
            return Selection{result, retired};
        }
        if (index >= count) {
            break;
        }
        const GuestAddress record = object + kRecordStride * static_cast<std::uint32_t>(index) + kRecordOffset;
        const GuestAddress car = m.read_u32_le(record + kCarOffset);
        retired += kRecordCost;
        const auto kind = static_cast<std::int8_t>(m.read_u8(car + kKindOffset));
        if (kind < 1 || kind > 4) {
            // Three failed tests (2 + 2 + 2), 0x3543d8 (1), 0x3543dc (2), then the increment at 0x354444 (2).
            retired += 2U + 2U + 2U + 1U + kPairCost + kPairCost;
            continue;
        }
        // Kind 1 jumps to 0x35444c at once; kinds 2..4 first run the tests at 0x3543c0, 0x3543c8, 0x3543d0.
        // Then 0x35444c and 0x3543dc, where t5 is 1 and the branch falls through.
        retired += 2U * static_cast<std::uint32_t>(kind - 1) + kPairCost + kPairCost;
        // 0x3543e4: kinds 1..3 jump to 0x3543f4 and the flag is 0; kind 4 falls to 0x3543ec, which loads the flag.
        retired += kPairCost;
        std::uint8_t flag = 0U;
        if (kind == 4) {
            retired += kPairCost;
            flag = m.read_u8(car + kFlagOffset);
        } else {
            retired += 1U;
        }
        // 0x3543f8: a set flag skips the record (only kind 4 can have one).
        retired += kPairCost;
        if (flag != 0U) {
            retired += kPairCost;
            continue;
        }
        // 0x354400: a record whose x compares <= 0 is skipped (fpu::le on the raw bits, as c.ole.s).
        const std::uint32_t x = m.read_u32_le(record + kXOffset);
        retired += 4U;
        if (fpu::le(x, 0U)) {
            retired += kPairCost;
            continue;
        }
        // 0x354410: y must compare less than 1.5.
        const std::uint32_t y = m.read_u32_le(record + kYOffset);
        retired += 4U;
        if (!fpu::lt(y, kLimitY)) {
            retired += kPairCost;
            continue;
        }
        // 0x354420: the score is the scale times word 3 of the quadword (lq drops the low four address bits).
        retired += 7U;
        const std::uint32_t word3 = n3::load_quad_any(m, car + kQuadOffset).words[3];
        const std::uint32_t score = fpu::mul(scale, word3);
        if (fpu::le(score, best)) {
            retired += kPairCost;
            continue;
        }
        // 0x35443c (v0 = record, f4 = score), then the increment at 0x354444.
        retired += 2U * kPairCost;
        result = record;
        best = score;
    }
    return Selection{result, retired + kExitCost};
}

template <class Context>
std::uint32_t select_best_target_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const Selection selection = select_best_target(dry, context.cpu.read_gpr_u32(4U),
                                                   context.cpu.read_gpr_u32(n3::kGlobalPointer));
    return selection.retired <= kCeiling ? selection.retired : 0U;
}

template <class Context>
n3::Run select_best_target_entry(Context& context) {
    auto call = n3::guest_call(context);
    const Selection selection =
        select_best_target(call.memory(), call.arg(0), call.cpu().read_gpr_u32(n3::kGlobalPointer));
    call.return_value(selection.result);
    return call.finish(selection.retired);
}

} // namespace

extern constinit const EngineFunction n3_00354350{0x00354350U, 268U, 0x9f0866e4b528a379ULL, "SelectBestTargetEntry",
                                                  kCeiling, n3::live<&select_best_target_entry<NativeFunctionContext>>, EngineReturn::Integer,
                                                  {}, 1e-4, 1e-6, {}, &select_best_target_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00354350, select_best_target_entry, select_best_target_cost)
