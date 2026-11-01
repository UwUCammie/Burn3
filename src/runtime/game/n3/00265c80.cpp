// 0x00265c80 (a0 = lane tracker T): UpdateLaneCandidates. T+0 points to a table P: byte P+21 = entry
// count, P+12 -> array of record indices r, P+4 -> array of float-words (read as integers), P+0 -> array
// of integers. T+16+i is the entry's current lane byte. Records live at 0x00522660 + 76*r (curve
// pointer at +556); lane records at 0x00551910 + 80*n (byte +65 = record index, +69 = next lane, +70 =
// state, +73 = lane byte to publish, floats +16 best, +44 limit, +48 position).
// For each entry: lane 0xFF starts from a lane found through the byte map at (0x0055d7dc) + (u16 at
// curve record +4) + P[+4][i]; otherwise it starts from the entry's lane. A lane of another record is
// retired (best = -1.0, state 1). A lane of this record is scored by CurveDifference. The chain of lanes
// (+69) is then walked and the best improving lane is published into T+16+i. Returns nothing.
//
// CurveDifference (0x001602f0, a native in track.cpp that reports a fixed cost) is transcribed here
// exactly (curve_difference below, with the original's per-path instruction count) so the retired
// count is the original's.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kRecordBase = 0x00522660U;     // lui 0x52, +9824
constexpr GuestAddress kLaneBase = 0x00551910U;       // 0x522660 + 0x2f2b0
constexpr GuestAddress kByteMap = 0x0055d7dcU;        // lui 0x56, -10276
constexpr std::int32_t kGlobalLimitOffset = -31792;   // $gp-relative float
constexpr std::uint32_t kCeiling = 20000U;

struct Curve {
    std::uint32_t bits;
    std::uint32_t count;
};

inline std::uint32_t from_word(const std::uint32_t word) {
    return fpu::from_int(static_cast<std::int32_t>(word));
}

// 0x001602f0 on raw bits: the curve (count at +16, 8-byte samples through +4) sampled at f12 minus at f13.
template <class Memory>
Curve curve_difference(Memory& m, const GuestAddress holder, const std::uint32_t f12, const std::uint32_t f13) {
    const GuestAddress curve = m.read_u32_le(holder);
    const auto count = static_cast<std::int32_t>(m.read_u32_le(curve + 16U));
    const GuestAddress samples = m.read_u32_le(curve + 4U);
    std::uint32_t retired = 8U;
    std::int32_t index = fpu::to_int(f12);
    if (count < index) {
        index = count;
        retired += 1U;
    }
    retired += 3U;
    std::uint32_t f4 = 0U;
    if (index - 1 < 0) {
        retired += 3U;
    } else {
        f4 = m.read_u32_le(samples + static_cast<std::uint32_t>(index - 1) * 8U);
        retired += 4U;
    }
    retired += 2U;
    if (index < count) {
        const std::uint32_t f2 = m.read_u32_le(samples + static_cast<std::uint32_t>(index) * 8U);
        const std::uint32_t f1 = fpu::sub(f12, fpu::from_int(index));
        const std::uint32_t acc = fpu::add(0U, f4);
        const std::uint32_t f0 = fpu::sub(f2, f4);
        f4 = fpu::madd(acc, f1, f0);
        retired += 11U;
    }
    retired += 6U;
    index = fpu::to_int(f13);
    if (count < index) {
        index = count;
        retired += 1U;
    }
    retired += 3U;
    std::uint32_t f3 = 0U;
    if (index - 1 < 0) {
        retired += 3U;
    } else {
        f3 = m.read_u32_le(samples + static_cast<std::uint32_t>(index - 1) * 8U);
        retired += 4U;
    }
    retired += 2U;
    if (index < count) {
        const std::uint32_t f2 = m.read_u32_le(samples + static_cast<std::uint32_t>(index) * 8U);
        const std::uint32_t f1 = fpu::sub(f13, fpu::from_int(index));
        const std::uint32_t acc = fpu::add(0U, f3);
        const std::uint32_t f0 = fpu::sub(f2, f3);
        f3 = fpu::madd(acc, f1, f0);
        retired += 11U;
    }
    retired += 2U + 2U;
    return {fpu::sub(f4, f3), retired};
}

// The whole function on guest memory or a DryMemory: the retired count (more than kCeiling once it passes that).
template <class Memory>
std::uint32_t update(Memory& m, const GuestAddress tracker, const std::uint32_t global_pointer) {
    std::uint32_t retired = 11U;
    for (std::int32_t entry = 0;; ++entry) {
        const GuestAddress table = m.read_u32_le(tracker);
        retired += 5U;
        if (!(entry < static_cast<std::int32_t>(m.read_u8(table + 21U)))) {
            return retired + 10U;
        }
        if (retired > kCeiling) {
            return retired;
        }
        const GuestAddress lane_byte = tracker + static_cast<std::uint32_t>(entry) + 16U;
        const std::uint32_t offset = static_cast<std::uint32_t>(entry) << 2U;
        const std::uint32_t current = m.read_u8(lane_byte);
        const std::uint32_t record = m.read_u32_le(m.read_u32_le(table + 12U) + offset);
        const GuestAddress curve_holder = kRecordBase + 76U * record + 556U;
        retired += 18U;
        GuestAddress lane = 0U;
        if (current != 0xffU) {
            retired += 9U;
            lane = kLaneBase + 80U * current;
            retired += 3U;
            if (m.read_u8(lane + 65U) != record) {
                retired += 4U;
                const std::uint32_t state = m.read_u8(lane + 70U);
                if (state == 2U) {
                    retired += 5U;
                    m.write_u8(lane + 70U, 1U);
                    m.write_u32_le(lane + 16U, 0xbf800000U);
                }
                retired += 4U;
                m.write_u8(lane_byte, 0xffU);
                continue;
            }
            // The entry's lane belongs to this record: score it.
            retired += 7U;
            const std::uint32_t x = m.read_u32_le(m.read_u32_le(table + 4U) + offset);
            const Curve difference = curve_difference(m, curve_holder, from_word(x), m.read_u32_le(lane + 48U));
            retired += difference.count;
            std::uint32_t score = difference.bits;
            retired += 5U;
            if (fpu::lt(score, 0U)) {
                score = 0U;
                retired += 1U;
            }
            retired += 4U;
            if (m.read_u8(lane + 70U) == 2U) {
                retired += 2U;
                m.write_u32_le(lane + 16U, score);
            }
            retired += 2U;
            continue;
        }
        // No lane yet: find the starting lane through the byte map.
        retired += 11U;
        const std::uint32_t q = m.read_u32_le(m.read_u32_le(table + 4U) + offset);
        const GuestAddress map = m.read_u32_le(kByteMap) + m.read_u16_le(curve_holder + 4U) + q;
        const std::uint32_t first = m.read_u8(map);
        if (first == 0xffU) {
            retired += 3U;
            const std::uint32_t n = m.read_u8(curve_holder + 70U);
            if (n == 0xffU) {
                retired += 2U;
                lane = 0U;
            } else {
                retired += 2U + 7U + 2U;
                lane = kLaneBase + 80U * n;
            }
        } else {
            retired += 2U + 7U + 4U;
            const std::uint32_t n = m.read_u8(kLaneBase + 80U * first + 69U);
            if (n == 0xffU) {
                retired += 2U;
                lane = 0U;
            } else {
                retired += 2U + 9U;
                lane = kLaneBase + 80U * n;
            }
        }
        retired += 2U;  // e5c: beq / nop
        for (;;) {
            retired += 2U;  // f2c: bne / nop
            if (lane == 0U) {
                retired += 2U;  // f34 / f38
                break;
            }
            if (retired > kCeiling) {
                return retired;
            }
            retired += 8U;
            const std::uint32_t x1 = m.read_u32_le(m.read_u32_le(m.read_u32_le(tracker) + 4U) + offset);
            const Curve first_difference = curve_difference(m, curve_holder, from_word(x1), m.read_u32_le(lane + 48U));
            retired += first_difference.count;
            const std::uint32_t f20 = first_difference.bits;
            retired += 9U;
            const std::uint32_t x0 = m.read_u32_le(m.read_u32_le(m.read_u32_le(tracker)) + offset);
            const Curve second_difference = curve_difference(m, curve_holder, m.read_u32_le(lane + 48U), from_word(x0));
            retired += second_difference.count;
            const std::uint32_t f0 = second_difference.bits;
            retired += 5U;
            bool improving = false;
            if (!fpu::le(f20, 0U)) {
                retired += 3U;
                if (!fpu::le(f0, 0U)) {
                    retired += 4U + 2U;
                    const std::uint32_t limit = fpu::mul(m.read_u32_le(global_pointer + static_cast<std::uint32_t>(kGlobalLimitOffset)), f20);
                    improving = fpu::lt(limit, m.read_u32_le(lane + 44U));
                }
            }
            if (improving) {
                retired += 4U;
                if (m.read_u8(lane + 70U) == 1U) {
                    retired += 6U;
                    m.write_u8(lane + 70U, 2U);
                    m.write_u32_le(lane + 16U, f20);
                    m.write_u8(lane_byte, m.read_u8(lane + 73U));
                    break;
                }
                retired += 4U;
                if (!fpu::lt(f20, m.read_u32_le(lane + 16U))) {
                    break;
                }
                retired += 6U;
                m.write_u8(lane + 70U, 2U);
                m.write_u32_le(lane + 16U, f20);
                m.write_u8(lane_byte, m.read_u8(lane + 73U));
                break;
            }
            // f18: step to the next lane in the chain.
            retired += 4U;
            const std::uint32_t next = m.read_u8(lane + 69U);
            if (next == 0xffU) {
                retired += 1U;
                lane = 0U;
            } else {
                retired += 2U + 9U + 2U;
                lane = kLaneBase + 80U * next;
            }
        }
        retired += 2U;  // d58 / d5c: loop back
    }
}

template <class Context>
std::uint32_t update_lane_candidates_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const std::uint32_t retired =
        update(dry, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u32(n3::kGlobalPointer));
    return retired <= kCeiling ? retired : 0U;
}

template <class Context>
n3::Run update_lane_candidates(Context& context) {
    auto call = n3::guest_call(context);
    return call.finish(update(call.memory(), call.arg(0), call.cpu().read_gpr_u32(n3::kGlobalPointer)));
}

} // namespace

extern constinit const EngineFunction n3_00265c80{0x00265c80U, 904U, 0x60cb695eaa438faaULL, "UpdateLaneCandidates",
                                                  kCeiling, n3::live<&update_lane_candidates<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &update_lane_candidates_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00265c80, update_lane_candidates, update_lane_candidates_cost)
