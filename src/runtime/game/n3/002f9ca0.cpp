// 0x002f9ca0 (a0 = arena record, a1 = size): BumpAllocAligned. The record holds the end limit at +12,
// the cursor at +16, the alignment (a power of two) at +20 and the last result at +24. The cursor is
// rounded up to the alignment, the size is added and the sum is stored back at +16 even when the
// allocation then fails. An unsigned compare of the end limit with the stored cursor decides: past the
// end, +24 is cleared and 0 is returned; otherwise +24 is the aligned address and that is returned.
// Leaf with no loops: the declared cost is the longest path.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kEndOffset = 12U;
constexpr GuestAddress kCursorOffset = 16U;
constexpr GuestAddress kAlignmentOffset = 20U;
constexpr GuestAddress kResultOffset = 24U;

// Retired instruction counts from the listing, delay slots included: the success path takes the branch
// at 0x002f9ccc, the failure path falls through to its clearing store and the unconditional jump.
constexpr std::uint32_t kSuccessCost = 17U;
constexpr std::uint32_t kFailureCost = 18U;

template <class Context>
n3::Run bump_alloc_aligned(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    const std::uint32_t size = call.arg(1);
    // An alignment of 0 gives a mask of all ones and an aligned address of 0, by plain wrapping arithmetic.
    const std::uint32_t align_mask = m.read_u32_le(record + kAlignmentOffset) - 1U;
    const std::uint32_t aligned = (m.read_u32_le(record + kCursorOffset) + align_mask) & ~align_mask;
    m.write_u32_le(record + kCursorOffset, aligned + size);
    const std::uint32_t cursor = m.read_u32_le(record + kCursorOffset);
    if (m.read_u32_le(record + kEndOffset) < cursor) {
        m.write_u32_le(record + kResultOffset, 0U);
        call.return_value(0U);
        return call.finish(kFailureCost);
    }
    m.write_u32_le(record + kResultOffset, aligned);
    call.return_value(m.read_u32_le(record + kResultOffset));
    return call.finish(kSuccessCost);
}

} // namespace

extern constinit const EngineFunction n3_002f9ca0{0x002f9ca0U, 80U, 0xcc5eb257651f07f3ULL, "BumpAllocAligned", 18U,
                                                  n3::live<&bump_alloc_aligned<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(002f9ca0, bump_alloc_aligned)
