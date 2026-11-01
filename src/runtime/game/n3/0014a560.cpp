// 0x0014a560 (a0 car, a1 road path, a2 START, a3 END): SegmentRangeFlags. Scans the road's
// 10-byte segment records from START up to END inclusive (wrapping from the ring's end back
// to 0 when END is before START) and returns in v0 the OR of the flag bits 1, 2 and 4 that the
// segments raise for this car. Returns 4 alone when START's segment has type 5.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count. Each scan loop stops at this ceiling, which
// keeps the dry run finite even for inputs on which the original never terminates.
constexpr std::uint32_t kCeiling = 20000U;

// The car fields the scan reads. Nothing writes memory during the call, so reading them once
// per loop setup is the same as the original's per-use loads.
struct CarFields {
    std::uint32_t w;   // u32[u32[car+4] + 6420]
    std::uint32_t a;   // u8[car+497]
    std::uint32_t v;   // u8[car+496]
    std::uint32_t bf;  // u8[car+657]
    std::uint32_t t7;  // u8[car+658]
    std::uint32_t t8;  // u32[car+508]
};

// What the body returns: the retired count and the flags for v0 (written only by the implementation).
struct ScanResult {
    std::uint32_t retired;
    std::uint32_t flags;
};

template <class Memory>
CarFields read_car(const Memory& m, const GuestAddress car) {
    const GuestAddress object = m.read_u32_le(car + 4U);
    return CarFields{
        .w = m.read_u32_le(object + 6420U),
        .a = m.read_u8(car + 497U),
        .v = m.read_u8(car + 496U),
        .bf = m.read_u8(car + 657U),
        .t7 = m.read_u8(car + 658U),
        .t8 = m.read_u32_le(car + 508U),
    };
}

// Segment type: the low three bits of the byte at +3 of the 10-byte record (index * 10 bytes in).
template <class Memory>
std::uint32_t segment_type(const Memory& m, const GuestAddress recs, const std::uint32_t index) {
    return m.read_u8(recs + index * 10U + 3U) & 7U;
}

// Retired cost of one scan iteration at `index`, ORing its flag bits into `flags`. The first
// loop differs from the other two only while W is zero: it tests the segment type there, while
// the later loops raise bit 1 without looking at the segment.
template <class Memory>
std::uint32_t scan_iteration(const Memory& m, const GuestAddress recs, const std::uint32_t index,
                             const CarFields& car, const bool first_loop, std::uint32_t& flags) {
    if (car.w == 0U) {
        if (!first_loop) {
            flags |= 1U;
            return 4U + 2U;  // head, then tail
        }
        std::uint32_t cost = 2U + 10U + 2U;  // head, type load and ty == 4 test, tail
        if (segment_type(m, recs, index) == 4U) {
            flags |= 1U;
            cost += 2U;
        }
        return cost;
    }
    std::uint32_t cost = 4U;  // W and A tests, with their delay slots
    if (car.a == 0U) {
        const std::uint32_t ty = segment_type(m, recs, index);
        cost += 10U;  // type load and ty == 4 test
        if (ty == 4U) {
            if (car.bf != 0U) {
                flags |= 1U;
                cost += 4U;
            } else {
                flags |= 4U;
                cost += 3U;
            }
        }
    }
    cost += 2U;  // V test
    if (car.v == 0U) {
        const std::uint32_t ty = segment_type(m, recs, index);
        cost += 8U + 2U;  // type load and ty == 1 test
        if (ty != 1U) {
            cost += 2U;  // ty == 3 test
        }
        if (ty == 1U || ty == 3U) {
            cost += 2U;  // T8 test
            if (car.t8 != 0U) {
                cost += 2U;  // T7 test
                if (car.t7 != 0U) {
                    flags |= 1U;
                    cost += 2U;
                } else {
                    flags |= 4U;
                    cost += 1U;
                }
            }
        }
    }
    return cost + 2U;  // tail
}

// The whole scan on guest memory (the implementation) or on a DryMemory (call_cost). Past kCeiling
// the result is partial: call_cost then declines the call, so the implementation never reports it.
template <class Memory>
ScanResult segment_range_flags_body(const Memory& m, const GuestAddress car, const GuestAddress path,
                                    const std::uint32_t start_raw, const std::uint32_t end_raw) {
    const std::uint32_t start = start_raw & 0xffffU;
    std::uint32_t end = end_raw & 0xffffU;

    const GuestAddress road = m.read_u32_le(path + 4U);
    const GuestAddress recs = m.read_u32_le(road + 8U);
    // Prologue (16 retired, the type test's delay slot included), then START's type.
    std::uint32_t retired = 16U;
    if (segment_type(m, recs, start) == 5U) {
        return ScanResult{retired + 6U, 4U};
    }

    // END at or past the road's count wraps (wrap flag set) or stops at the last segment and
    // raises bit 2 (wrap flag clear).
    const std::uint32_t count = m.read_u16_le(road + 12U);
    retired += 5U;
    std::uint32_t flags = 0U;
    if (end >= count) {
        retired += 6U;
        const std::uint32_t wrap = m.read_u8(road + 14U);
        if (wrap != 0U) {
            end = (end - count) & 0xffffU;
        } else {
            end = (count - 1U) & 0xffffU;
            flags |= 2U;
        }
    }
    retired += 5U;

    if (end >= start) {
        // START through END.
        retired += 3U + 12U;
        const CarFields fields = read_car(m, car);
        for (std::uint32_t index = start;; index = (index + 1U) & 0xffffU) {
            retired += 4U;  // loop test
            if (retired > kCeiling) {
                return ScanResult{retired, flags};
            }
            if (end < index) {
                break;
            }
            retired += scan_iteration(m, recs, index, fields, false, flags);
        }
        retired += 6U;  // return
    } else {
        // START through count - 1 (only when START is below the count), then 0 through END.
        retired += 3U;
        if (start < count) {
            retired += 13U;
            const CarFields first = read_car(m, car);
            for (std::uint32_t index = start;; index = (index + 1U) & 0xffffU) {
                retired += 4U;  // loop test
                if (retired > kCeiling) {
                    return ScanResult{retired, flags};
                }
                if (!(index < count)) {
                    break;
                }
                retired += scan_iteration(m, recs, index, first, true, flags);
            }
        }
        retired += 3U + 12U;
        const CarFields fields = read_car(m, car);
        for (std::uint32_t index = 0U;; index = (index + 1U) & 0xffffU) {
            retired += 4U;  // loop test
            if (retired > kCeiling) {
                return ScanResult{retired, flags};
            }
            if (end < index) {
                break;
            }
            retired += scan_iteration(m, recs, index, fields, false, flags);
        }
        retired += 8U;  // return
    }
    return ScanResult{retired, flags};
}

// call_cost: the same body on a DryMemory, which leaves the guest memory of the call unchanged.
template <class Context>
std::uint32_t segment_range_flags_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const ScanResult r = segment_range_flags_body(dry, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr_u32(5U),
                                                  context.cpu.read_gpr_u32(6U), context.cpu.read_gpr_u32(7U));
    return r.retired <= kCeiling ? r.retired : 0U;
}

template <class Context>
n3::Run segment_range_flags(Context& context) {
    auto call = n3::guest_call(context);
    const ScanResult r = segment_range_flags_body(call.memory(), call.arg(0), call.arg(1), call.arg(2), call.arg(3));
    call.return_value(r.flags);
    return call.finish(r.retired);
}

} // namespace

extern constinit const EngineFunction n3_0014a560{0x0014a560U, 980U, 0x5777684a9d058687ULL, "SegmentRangeFlags", kCeiling,
                                                  n3::live<&segment_range_flags<NativeFunctionContext>>, EngineReturn::Integer, {}, 1e-4, 1e-6, {},
                                                  &segment_range_flags_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(0014a560, segment_range_flags, segment_range_flags_cost)
