// 0x0015ffa0 SelectLinkedCell: picks one of the 80-byte cell records at 0x00551910 for a position x (f12).
//
// The byte map at *(0x0055d7dc) + S.u16[4] + (x truncated to 16 bits; x - 2^31 first when x >= 2^31) gives a cell
// index b. 255 means none: the fallback cell S[70] is returned (0 when that is 255 too). Cell b holds its position
// (float at +48) and a link (byte at +69; 255 means no link, and cell b is returned). Cell a is the link.
// S[0] points at a sample table P: count at P[16], entries at P[4] with an 8-byte stride (the sample of an entry is
// the float at its offset 0). Positions are clamped to the count and the table is read as a piecewise-linear
// interpolation at pos(b), at x and at pos(a). Cell b is returned when (L(b) - L(x)) < (L(x) - L(a)), else cell a.
//
// No loops and no calls: the retired count is the number of instructions on the path taken, delay slots included.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kByteMapPointer = 0x0055d7dcU;   // lui $v0,0x56 + lw -10276($v0)
constexpr GuestAddress kCellBase = 0x00551910U;         // 0x00522660 + 0x0002f2b0
constexpr std::uint32_t kCellBytes = 80U;
constexpr std::uint32_t kNoCell = 255U;
constexpr std::uint32_t kTwoTo31 = 0x4f000000U;         // 2147483648.0F
constexpr std::uint32_t kSampleStride = 8U;

// Address of sample `index` (lwc1 at entries + index * 8, 32-bit wrapping as the sll/addu of the original).
inline GuestAddress sample_address(const GuestAddress entries, const std::uint32_t index) {
    return entries + index * kSampleStride;
}

// The original's bltz on a 32-bit result.
inline bool is_negative(const std::uint32_t value) {
    return static_cast<std::int32_t>(value) < 0;
}

template <class Context>
n3::Run select_linked_cell(Context& context) {
    auto call = n3::guest_call(context);
    auto& memory = call.memory();
    const GuestAddress structure = call.arg(0);
    const std::uint32_t x = call.cpu().read_fpr_bits(12U);

    // 0x0015ffa0-0x0015ffc0 (9): the byte map row of this structure.
    const std::uint32_t row = memory.read_u32_le(kByteMapPointer)
                            + static_cast<std::uint32_t>(memory.read_u16_le(structure + 4U));
    std::uint32_t retired = 9U;
    std::uint32_t index = 0U;
    if (n3::fpu::le(kTwoTo31, x)) {
        // 0x0015ffd4-0x0015ffec (7). The OR with 0x80000000 is removed again by the 16-bit mask.
        index = static_cast<std::uint32_t>(n3::fpu::to_int(n3::fpu::sub(x, kTwoTo31))) & 0xffffU;
        retired += 7U;
    } else {
        // 0x0015ffc4-0x0015ffd0 (4).
        index = static_cast<std::uint32_t>(n3::fpu::to_int(x)) & 0xffffU;
        retired += 4U;
    }
    // 0x0015fff0-0x00160004 (6): the cell index byte.
    const std::uint32_t cell = memory.read_u8(row + index);
    retired += 6U;
    if (cell == kNoCell) {
        // 0x00160244-0x0016024c (3): the structure's fallback cell index.
        const std::uint32_t fallback = memory.read_u8(structure + 70U);
        retired += 3U;
        if (fallback == kNoCell) {
            // 0x0016028c-0x00160290 (2), jr (2).
            call.return_value(0U);
            return call.finish(retired + 2U + 2U);
        }
        // 0x00160250-0x00160254 (2), 0x00160260-0x00160280 (9), 0x00160284-0x00160288 (2), jr (2).
        call.return_value(kCellBase + fallback * kCellBytes);
        return call.finish(retired + 2U + 9U + 2U + 2U);
    }

    // 0x00160008-0x0016000c (2) and 0x00160018-0x00160038 (9): cell b.
    const GuestAddress cell_b = kCellBase + cell * kCellBytes;
    retired += 2U + 9U;
    // 0x0016003c-0x00160048 (4): the link of cell b.
    const std::uint32_t link = memory.read_u8(cell_b + 69U);
    retired += 4U;
    if (link == kNoCell) {
        call.return_value(cell_b);
        return call.finish(retired + 2U);  // jr
    }
    // 0x0016004c-0x00160050 (2) and 0x0016005c-0x0016007c (9): cell a, the link.
    const GuestAddress cell_a = kCellBase + link * kCellBytes;
    retired += 2U + 9U;

    // 0x00160080-0x001600a0 (9): pos(b), the sample table and its count and entries.
    const std::uint32_t pos_b = memory.read_u32_le(cell_b + 48U);
    const GuestAddress table = memory.read_u32_le(structure);
    const std::int32_t count = static_cast<std::int32_t>(memory.read_u32_le(table + 16U));
    const GuestAddress entries = memory.read_u32_le(table + 4U);
    retired += 9U;
    const std::int32_t b_raw = n3::fpu::to_int(pos_b);
    const bool b_clamped = count < b_raw;
    const std::int32_t b_index = b_clamped ? count : b_raw;
    if (b_clamped) {
        retired += 1U;  // 0x001600a4
    }

    // The sample below pos(b): 0x001600a8-0x001600b0 (3), then 0x001600b4-0x001600c0 (4) or
    // 0x00160294-0x0016029c (3, the sample is zero).
    std::uint32_t sample_b = 0U;
    if (is_negative(static_cast<std::uint32_t>(b_index) - 1U)) {
        retired += 3U + 3U;
    } else {
        sample_b = memory.read_u32_le(sample_address(entries, static_cast<std::uint32_t>(b_index) - 1U));
        retired += 3U + 4U;
    }
    // 0x001600c4-0x001600c8 (2), then 0x001600cc-0x001600f4 (11) when b_index < count: lerp at pos(b).
    retired += 2U;
    if (b_index < count) {
        const std::uint32_t upper = memory.read_u32_le(sample_address(entries, static_cast<std::uint32_t>(b_index)));
        const std::uint32_t offset = n3::fpu::sub(pos_b, n3::fpu::from_int(b_index));
        const std::uint32_t acc = n3::fpu::add(0U, sample_b);
        const std::uint32_t slope = n3::fpu::sub(upper, sample_b);
        sample_b = n3::fpu::madd(acc, offset, slope);
        retired += 11U;
    }

    // 0x001600f8-0x0016010c (6), 0x00160110 (1) when clamped: the sample index of x.
    const std::int32_t x_raw = n3::fpu::to_int(x);
    const bool x_clamped = count < x_raw;
    const std::int32_t x_index = x_clamped ? count : x_raw;
    retired += 6U;
    if (x_clamped) {
        retired += 1U;
    }

    // The sample below x: 0x00160114-0x0016011c (3), then 0x00160120-0x0016012c (4) or
    // 0x001602a0-0x001602a8 (3, the sample is zero). Then 0x00160130-0x00160134 (2) and, when
    // x_index < count, 0x00160138-0x0016015c (10): lerp at x.
    retired += 3U;
    std::uint32_t sample_x = 0U;
    if (is_negative(static_cast<std::uint32_t>(x_index) - 1U)) {
        retired += 3U;
    } else {
        sample_x = memory.read_u32_le(sample_address(entries, static_cast<std::uint32_t>(x_index) - 1U));
        retired += 4U;
    }
    retired += 2U;
    if (x_index < count) {
        const std::uint32_t upper = memory.read_u32_le(sample_address(entries, static_cast<std::uint32_t>(x_index)));
        const std::uint32_t slope = n3::fpu::sub(upper, sample_x);
        const std::uint32_t acc = n3::fpu::add(0U, sample_x);
        const std::uint32_t offset = n3::fpu::sub(x, n3::fpu::from_int(x_index));
        sample_x = n3::fpu::madd(acc, offset, slope);
        retired += 10U;
    }

    // 0x00160160-0x00160168 (3; the sub.s in the delay slot runs on both paths), 0x0016016c (1) when clamped.
    retired += 3U;
    const std::uint32_t lower_gap = n3::fpu::sub(sample_b, sample_x);
    if (x_clamped) {
        retired += 1U;
    }

    // The same lerp at x again (the original runs it a second time; the sample values are the same):
    // 0x00160170-0x00160178 (3), 0x0016017c-0x00160188 (4) or 0x001602ac-0x001602b4 (3), then
    // 0x0016018c-0x00160190 (2) and, when x_index < count, 0x00160194-0x001601bc (11).
    retired += 3U;
    std::uint32_t sample_x2 = 0U;
    if (is_negative(static_cast<std::uint32_t>(x_index) - 1U)) {
        retired += 3U;
    } else {
        sample_x2 = memory.read_u32_le(sample_address(entries, static_cast<std::uint32_t>(x_index) - 1U));
        retired += 4U;
    }
    retired += 2U;
    if (x_index < count) {
        const std::uint32_t upper = memory.read_u32_le(sample_address(entries, static_cast<std::uint32_t>(x_index)));
        const std::uint32_t offset = n3::fpu::sub(x, n3::fpu::from_int(x_index));
        const std::uint32_t acc = n3::fpu::add(0U, sample_x2);
        const std::uint32_t slope = n3::fpu::sub(upper, sample_x2);
        sample_x2 = n3::fpu::madd(acc, offset, slope);
        retired += 11U;
    }

    // 0x001601c0-0x001601d8 (7): pos(a) and its clamped sample index; 0x001601dc (1) when clamped.
    const std::uint32_t pos_a = memory.read_u32_le(cell_a + 48U);
    const std::int32_t a_raw = n3::fpu::to_int(pos_a);
    const bool a_clamped = count < a_raw;
    const std::int32_t a_index = a_clamped ? count : a_raw;
    retired += 7U;
    if (a_clamped) {
        retired += 1U;
    }

    // The sample below pos(a): 0x001601e0-0x001601e8 (3), then 0x001601ec-0x001601f8 (4) or
    // 0x001602b8-0x001602c0 (3, the sample is zero).
    std::uint32_t sample_a = 0U;
    if (is_negative(static_cast<std::uint32_t>(a_index) - 1U)) {
        retired += 3U + 3U;
    } else {
        sample_a = memory.read_u32_le(sample_address(entries, static_cast<std::uint32_t>(a_index) - 1U));
        retired += 3U + 4U;
    }
    // 0x001601fc-0x00160200 (2), then 0x00160204-0x00160230 (12) when a_index < count: lerp at pos(a).
    retired += 2U;
    if (a_index < count) {
        const std::uint32_t upper = memory.read_u32_le(sample_address(entries, static_cast<std::uint32_t>(a_index)));
        const std::uint32_t offset = n3::fpu::sub(pos_a, n3::fpu::from_int(a_index));
        const std::uint32_t acc = n3::fpu::add(0U, sample_a);
        const std::uint32_t slope = n3::fpu::sub(upper, sample_a);
        sample_a = n3::fpu::madd(acc, offset, slope);
        retired += 12U;
    }

    // 0x001602c4-0x001602d0 (4): cell b when lower_gap < (L(x) - L(a)), else cell a.
    retired += 4U;
    const std::uint32_t gap = n3::fpu::sub(sample_x2, sample_a);
    if (n3::fpu::lt(lower_gap, gap)) {
        // 0x001602d4-0x001602d8 (2), 0x00160234-0x00160238 (2), jr (2).
        call.return_value(cell_b);
        return call.finish(retired + 2U + 2U + 2U);
    }
    // 0x0016023c-0x00160240 (2), jr (2).
    call.return_value(cell_a);
    return call.finish(retired + 2U + 2U);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(0015ffa0, 836U, 0x75aed0fe38e6b29eULL, "SelectLinkedCell", 167U, select_linked_cell,
               EngineReturn::Integer)
