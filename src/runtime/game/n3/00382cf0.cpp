// 0x00382cf0 (no arguments, no result): fills two tables of eleven texture-coordinate rectangles.
// Row r (0..10) converts two pixel rectangles with TexRectToUvFlip (0x003713a0). The first takes its
// origin and size from the float pairs at 0x004c95c0 and 0x004c9530 and its 16-byte result goes to row r
// of the table at 0x01ea49a0. The second takes them from 0x004c9680 and 0x004c9620 and goes to row r of
// the table at 0x01ea4a50. Rows 5 and 8 use 64 x 32 textures, the other rows 128 x 64. Both results pass
// through a scratch pair in this function's frame, as the original's do.
//
// Only the row number decides any branch, so the count is fixed. The function's own instructions are 6
// (prologue), 7 (exit and return) and, per row, 60 (58 for row 5, 61 for row 8), the jal and delay slot of
// both calls included. Each of the 22 calls adds the callee's count, 26 with flags 0.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(003713a0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kRows = 11U;
constexpr std::uint32_t kFrameBytes = 160U;  // addiu $sp, $sp, -160
constexpr std::uint32_t kResultBytes = 16U;  // u0, v0, u1, v1, as TexRectToUvFlip writes them
constexpr std::uint32_t kCeiling = 20000U;

// Pairs of floats, 8 bytes per row.
constexpr GuestAddress kFirstOrigins = 0x004c95c0U;   // (x, y) of the first rectangle
constexpr GuestAddress kFirstSizes = 0x004c9530U;     // (w, h) of the first rectangle
constexpr GuestAddress kSecondOrigins = 0x004c9680U;  // (x, y) of the second rectangle
constexpr GuestAddress kSecondSizes = 0x004c9620U;    // (w, h) of the second rectangle

// Results, 16 bytes per row.
constexpr GuestAddress kFirstResults = 0x01ea49a0U;
constexpr GuestAddress kSecondResults = 0x01ea4a50U;

// Texture sizes as raw float bits.
constexpr std::uint32_t kSmallTextureWidth = 0x42800000U;   // 64.0
constexpr std::uint32_t kSmallTextureHeight = 0x42000000U;  // 32.0
constexpr std::uint32_t kLargeTextureWidth = 0x43000000U;   // 128.0
constexpr std::uint32_t kLargeTextureHeight = 0x42800000U;  // 64.0

// The original's own instructions, delay slots included.
constexpr std::uint32_t kPrologueCost = 6U;   // 0x00382cf0-0x00382d04
constexpr std::uint32_t kExitCost = 7U;       // 0x00382ec4-0x00382ec8 and 0x00382f98-0x00382fa8
constexpr std::uint32_t kRowCost = 60U;       // both row tests, the 50-instruction block, the row step
constexpr std::uint32_t kRowFiveCost = 58U;   // row 5 leaves after the first test: 3 + 51 + 4
constexpr std::uint32_t kRowEightCost = 61U;  // both row tests, the 51-instruction block, the row step

constexpr bool is_small_row(const std::uint32_t row) { return row == 5U || row == 8U; }

constexpr std::uint32_t row_cost(const std::uint32_t row) {
    if (row == 5U) {
        return kRowFiveCost;
    }
    if (row == 8U) {
        return kRowEightCost;
    }
    return kRowCost;
}

// Frame offset of the second call's result. The first call's result sits kResultBytes above it.
constexpr std::uint32_t second_result_offset(const std::uint32_t row) {
    if (row == 8U) {
        return 96U;
    }
    if (row == 5U) {
        return 128U;
    }
    return 64U;
}

// One jal to TexRectToUvFlip, with the arguments the original leaves in its registers.
template <class Caller>
n3::Run convert_rectangle(Caller& call, const GuestAddress origin, const GuestAddress size,
                          const std::uint32_t texture_width, const std::uint32_t texture_height,
                          const GuestAddress result) {
    auto& m = call.memory();
    call.set_arg(0, result);
    call.set_arg(1, 0U);
    call.set_float_arg(0, m.read_u32_le(origin));
    call.set_float_arg(1, m.read_u32_le(origin + 4U));
    call.set_float_arg(2, m.read_u32_le(size));
    call.set_float_arg(3, m.read_u32_le(size + 4U));
    call.set_float_arg(4, texture_width);
    call.set_float_arg(5, texture_height);
    return call.invoke([](auto& machine) { return n3::fn_003713a0(machine); }, kFrameBytes);
}

// The original copies each result with two 64-bit loads followed by two 64-bit stores.
template <class Memory>
void copy_result(Memory& m, const GuestAddress from, const GuestAddress to) {
    const std::uint64_t low = m.read_u64_le(from);
    const std::uint64_t high = m.read_u64_le(from + 8U);
    m.write_u64_le(to, low);
    m.write_u64_le(to + 8U, high);
}

template <class Context>
n3::Run build_uv_rect_tables(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    std::uint32_t retired = kPrologueCost + kExitCost;
    for (std::uint32_t row = 0U; row < kRows; ++row) {
        const std::uint32_t texture_width = is_small_row(row) ? kSmallTextureWidth : kLargeTextureWidth;
        const std::uint32_t texture_height = is_small_row(row) ? kSmallTextureHeight : kLargeTextureHeight;
        const GuestAddress pair_offset = 8U * row;
        const GuestAddress result_offset = 16U * row;
        const GuestAddress second = frame + second_result_offset(row);
        const GuestAddress first = second + kResultBytes;

        const n3::Run first_run = convert_rectangle(call, kFirstOrigins + pair_offset, kFirstSizes + pair_offset,
                                                    texture_width, texture_height, first);
        if (first_run.declined) {
            return call.decline();
        }
        copy_result(m, first, kFirstResults + result_offset);

        const n3::Run second_run = convert_rectangle(call, kSecondOrigins + pair_offset, kSecondSizes + pair_offset,
                                                     texture_width, texture_height, second);
        if (second_run.declined) {
            return call.decline();
        }
        copy_result(m, second, kSecondResults + result_offset);

        retired += row_cost(row) + first_run.retired + second_run.retired;
    }
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00382cf0, 700U, 0xa500667d72994eb4ULL, "BuildUvRectTables", kCeiling, build_uv_rect_tables,
         EngineReturn::Nothing)
