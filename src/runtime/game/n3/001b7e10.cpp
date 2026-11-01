// 0x001b7e10 (a0 = world base, a1 = box index): AddBoxEndpointsToSweepLists, a leaf that inserts a box into
// the three axis lists of a sweep-and-prune broadphase. The box record is at a0 + 48*a1 (a1 * 3 << 4). Its min
// vector is the quadword at +128 and its max vector the quadword at +144 (lq ignores the low four address bits).
// Passes i = 0, 1, 2 (x, y, z) take word i of each quadword: lo from the min vector, hi from the max vector.
// Axis i's list starts at a0 + 0x4c80*i + 0xe5f0 and holds 8-byte entries (float at +0, u16 at +4 = box index,
// u8 at +6 = flag). The u32 counter at a0 + 0x1cb74 counts the entries pushed so far: the start entry
// {lo, flag 1} goes at the counter's index, the end entry {hi, flag 0} at the next one. The counter is read again
// before the end entry (the original reloads it after the start store) and advanced by 2 after the three axes.
//
// The three passes are a fixed count: the pass index is a register counter (slti ..., 3), not data. brief.py's
// back edge therefore does not make the count data dependent, and every path through the original retires exactly
// 129 instructions: 16 entry, 4 loop tests of 3, 29 / 33 / 32 for the passes (i = 0 / 1 / 2), 5 exit, 2 return.
// Passes 1 and 2 copy the quadwords into the original's own stack frame to pick word i from them (pass 0 uses mtc1
// on the loaded words); that frame scratch is not reproduced. The native reads word i of each quadword in guest
// memory, which holds the same value.
// Returns nothing (v1 and f0 are scratch).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr unsigned kAxisCount = 3U;
constexpr GuestAddress kCounterOffset = 0x0001cb74U;   // lui 0x1 / ori 0xcb74: the u32 entry counter
constexpr GuestAddress kAxisListOffset = 0x0000e5f0U;  // ori 0xe5f0: axis 0's list, from the world base
constexpr GuestAddress kAxisStride = 0x00004c80U;      // 153 * 128 = 19584 bytes between the axis lists
constexpr GuestAddress kBoxStride = 48U;               // a1 * 3 << 4
constexpr GuestAddress kMinOffset = 128U;              // min vector quadword in the box record
constexpr GuestAddress kMaxOffset = 144U;              // max vector quadword in the box record
constexpr std::uint8_t kStartFlag = 1U;                // flag byte of the start (min) entry
constexpr std::uint8_t kEndFlag = 0U;                  // flag byte of the end (max) entry
constexpr std::uint32_t kRetired = 129U;               // the same for every path (see above)

// Reads and writes happen in the original's order: per pass the quadword words, the counter, the start entry,
// the counter again, then the end entry. An entry can overlap the counter when the lists are full, so that order
// is kept on purpose.
template <class Context>
n3::Run add_box_endpoints_to_sweep_lists(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress world = call.arg(0);
    const std::uint32_t box_index = call.arg(1);
    const GuestAddress box = world + kBoxStride * box_index;
    const GuestAddress counter = world + kCounterOffset;
    const auto id = static_cast<std::uint16_t>(box_index);
    for (unsigned axis = 0U; axis < kAxisCount; ++axis) {
        // The original loads both quadwords (lq) in every pass, before that pass stores anything.
        const GuestAddress min_quad = (box + kMinOffset) & ~15U;
        const GuestAddress max_quad = (box + kMaxOffset) & ~15U;
        const std::uint32_t lo = m.read_u32_le(min_quad + 4U * axis);
        const std::uint32_t hi = m.read_u32_le(max_quad + 4U * axis);
        const GuestAddress list = world + kAxisStride * axis + kAxisListOffset;

        const std::uint32_t start_index = m.read_u32_le(counter);
        const GuestAddress start = list + (start_index << 3U);
        m.write_u32_le(start, lo);
        m.write_u8(start + 6U, kStartFlag);
        m.write_u16_le(start + 4U, id);

        // Reloaded from memory before the end entry, as the original does.
        const std::uint32_t end_index = m.read_u32_le(counter);
        const GuestAddress end = list + ((end_index + 1U) << 3U);
        m.write_u32_le(end, hi);
        m.write_u8(end + 6U, kEndFlag);
        m.write_u16_le(end + 4U, id);
    }
    const std::uint32_t count = m.read_u32_le(counter);
    m.write_u32_le(counter, count + 2U);
    return call.finish(kRetired);
}

} // namespace

extern constinit const EngineFunction n3_001b7e10{0x001b7e10U, 284U, 0x124f15eabb6d85ceULL,
                                                  "AddBoxEndpointsToSweepLists", kRetired,
                                                  n3::live<&add_box_endpoints_to_sweep_lists<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(001b7e10, add_box_endpoints_to_sweep_lists)
