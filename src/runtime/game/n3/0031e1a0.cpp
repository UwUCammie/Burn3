// 0x0031e1a0 ReleaseObjectSlots: an object (a0) holds 32-bit pointers to nodes of the pool at 0x01e903e0, in
// pairs. After clearing the byte at +10 (0x00319390) it hands every non-null first pointer and then its partner
// back to the pool (ReleaseSlotNode 0x001d7560, slot index = int16 at +8), zeroing each slot as it goes.
//
//   pairs A: first O[28+4i], second O[72+4i], i = 0..10
//   pairs B: first O[116+4i], second O[132+4i], i = 0..3
//   pair  C: first O[148], second O[152]
//   single:  O[156]
//
// The partner is read after the first release (the callee may rewrite it) and is released even when it is 0.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00319390)
N3_CALLEE(001d7560)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kPool = 0x01e903e0U;
constexpr GuestAddress kSlotIndex = 8U;
constexpr GuestAddress kFirstA = 28U;
constexpr GuestAddress kSecondA = 72U;
constexpr GuestAddress kFirstB = 116U;
constexpr GuestAddress kSecondB = 132U;
constexpr GuestAddress kFirstC = 148U;
constexpr GuestAddress kSecondC = 152U;
constexpr GuestAddress kSingle = 156U;
constexpr std::uint32_t kPairsA = 11U;
constexpr std::uint32_t kPairsB = 4U;
constexpr std::uint32_t kFrameBytes = 112U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired instructions of the original outside the callees.
constexpr std::uint32_t kEntry = 14U;       // through the jal 0x00319390 and its delay slot
constexpr std::uint32_t kLoopEmpty = 11U;   // 7 test + 4 step
constexpr std::uint32_t kLoopUsed = 21U;    // 7 test + 10 body + 4 step
constexpr std::uint32_t kBetweenLoops = 1U;
constexpr std::uint32_t kPairEmpty = 3U;
constexpr std::uint32_t kPairUsed = 12U;
constexpr std::uint32_t kSingleUsed = 7U;
constexpr std::uint32_t kExit = 9U;

template <class Context>
n3::Run release_object_slots(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const std::uint32_t slot =
        static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(m.read_u16_le(object + kSlotIndex))));
    std::uint32_t retired = kEntry;

    call.set_arg(0, object);
    const n3::Run cleared = call.invoke([](auto& mem) { return n3::fn_00319390(mem); }, kFrameBytes);
    if (cleared.declined) {
        return call.decline();
    }
    retired += cleared.retired;

    // Returns the node to the pool; false when the callee declined.
    const auto release = [&](const std::uint32_t node) {
        call.set_arg(0, kPool);
        call.set_arg(1, slot);
        call.set_arg(2, node);
        const n3::Run r = call.invoke([](auto& mem) { return n3::fn_001d7560(mem); }, kFrameBytes);
        retired += r.retired;
        return !r.declined;
    };
    // One pair: first pointer, then (read afterwards) its partner.
    const auto release_pair = [&](const GuestAddress first, const GuestAddress second) {
        const std::uint32_t node = m.read_u32_le(object + first);
        if (node == 0U) {
            return true;
        }
        if (!release(node)) {
            return false;
        }
        m.write_u32_le(object + first, 0U);
        if (!release(m.read_u32_le(object + second))) {
            return false;
        }
        m.write_u32_le(object + second, 0U);
        return true;
    };

    for (std::uint32_t i = 0U; i < kPairsA; ++i) {
        const bool empty = m.read_u32_le(object + kFirstA + 4U * i) == 0U;
        if (!release_pair(kFirstA + 4U * i, kSecondA + 4U * i)) {
            return call.decline();
        }
        retired += empty ? kLoopEmpty : kLoopUsed;
    }
    retired += kBetweenLoops;
    for (std::uint32_t i = 0U; i < kPairsB; ++i) {
        const bool empty = m.read_u32_le(object + kFirstB + 4U * i) == 0U;
        if (!release_pair(kFirstB + 4U * i, kSecondB + 4U * i)) {
            return call.decline();
        }
        retired += empty ? kLoopEmpty : kLoopUsed;
    }

    {
        const bool empty = m.read_u32_le(object + kFirstC) == 0U;
        if (!release_pair(kFirstC, kSecondC)) {
            return call.decline();
        }
        retired += empty ? kPairEmpty : kPairUsed;
    }
    {
        const std::uint32_t node = m.read_u32_le(object + kSingle);
        retired += kPairEmpty;
        if (node != 0U) {
            if (!release(node)) {
                return call.decline();
            }
            m.write_u32_le(object + kSingle, 0U);
            retired += kSingleUsed - kPairEmpty;
        }
    }
    return call.finish(retired + kExit);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(0031e1a0, 340U, 0x4e3a6c1e77753496ULL, "ReleaseObjectSlots", kCeiling, release_object_slots,
         EngineReturn::Nothing)
