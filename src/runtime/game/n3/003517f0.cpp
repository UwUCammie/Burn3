// 0x003517f0 InitUvTables: a0 = context C.
//  1. Builds four texture-coordinate quads (u0, v0, u1, v1) with TexRectToUvFlip (0x003713a0) from three rows of
//     a float table (rectangle origin, rectangle size, texture size; 8 bytes per quad) and copies them into the
//     64-byte global block at 0x01ea3f40.
//  2. Carves memory out of the arena at 0x01e90438 with ArenaAlloc (0x003b88e0) and stores the pointers into C:
//     two 64-unit buffers at C[12500], C[12504] and a 5 x 15 grid of 2-unit buffers from C[12508] (60-byte rows).
// Returns 1. Every call retires the same instruction count apart from the callees' own.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(003713a0)
N3_CALLEE(003b88e0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFrameBytes = 144U;
constexpr GuestAddress kRectOriginRow = 0x004c7070U;
constexpr GuestAddress kRectSizeRow = 0x004c7090U;
constexpr GuestAddress kTextureSizeRow = 0x004c70b0U;
constexpr GuestAddress kUvBlock = 0x01ea3f40U;
constexpr GuestAddress kArena = 0x01e90438U;
constexpr std::uint32_t kQuadCount = 4U;
constexpr std::uint32_t kQuadFrameOffset = 128U;   // quad k is built at sp - 144 + 128 - 16 * k

constexpr GuestAddress kBigBufferA = 12500U;
constexpr GuestAddress kBigBufferB = 12504U;
constexpr GuestAddress kGrid = 12508U;
constexpr std::uint32_t kGridRows = 5U;
constexpr std::uint32_t kGridColumns = 15U;
constexpr std::uint32_t kBigBufferUnits = 64U;
constexpr std::uint32_t kSmallBufferUnits = 2U;

constexpr std::uint32_t kHead = 123U;        // 0x003517f0-0x003519d8 including both setup allocator calls' own slots
constexpr std::uint32_t kRoundHead = 5U;
constexpr std::uint32_t kInnerPass = 11U;    // includes the jal and its delay slot
constexpr std::uint32_t kRoundTail = 4U;
constexpr std::uint32_t kEpilogue = 8U;
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run init_uv_tables(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress ctx = call.arg(0);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    std::uint32_t retired = kHead;

    for (std::uint32_t k = 0U; k < kQuadCount; ++k) {
        const GuestAddress slot = frame + kQuadFrameOffset - 16U * k;
        call.set_arg(0, slot);
        call.set_arg(1, 0U);
        for (std::uint32_t i = 0U; i < 2U; ++i) {
            call.set_float_arg(i, m.read_u32_le(kRectOriginRow + 8U * k + 4U * i));
            call.set_float_arg(2U + i, m.read_u32_le(kRectSizeRow + 8U * k + 4U * i));
            call.set_float_arg(4U + i, m.read_u32_le(kTextureSizeRow + 8U * k + 4U * i));
        }
        const n3::Run r = call.invoke([](auto& mem) { return n3::fn_003713a0(mem); }, kFrameBytes);
        if (r.declined) {
            return call.decline();
        }
        retired += r.retired;
        for (std::uint32_t w = 0U; w < 4U; ++w) {
            m.write_u32_le(kUvBlock + 16U * k + 4U * w, m.read_u32_le(slot + 4U * w));
        }
    }

    bool declined = false;
    const auto alloc = [&](std::uint32_t units) {
        call.set_arg(0, kArena);
        call.set_arg(1, units);
        const n3::Run r = call.invoke([](auto& mem) { return n3::fn_003b88e0(mem); }, kFrameBytes);
        declined = declined || r.declined;
        retired += r.retired;
        return call.result();
    };

    m.write_u32_le(ctx + kBigBufferA, alloc(kBigBufferUnits));
    m.write_u32_le(ctx + kBigBufferB, alloc(kBigBufferUnits));
    for (std::uint32_t row = 0U; row < kGridRows; ++row) {
        retired += kRoundHead;
        for (std::uint32_t col = 0U; col < kGridColumns; ++col) {
            const std::uint32_t ptr = alloc(kSmallBufferUnits);
            m.write_u32_le(ctx + kGrid + 4U * (kGridColumns * row + col), ptr);
            retired += kInnerPass;
        }
        retired += kRoundTail;
    }
    if (declined) {
        return call.decline();
    }

    call.return_value(1U);
    return call.finish(retired + kEpilogue);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(003517f0, 604U, 0xefa7c82c579bdb50ULL, "InitUvTables", kCeiling, init_uv_tables, EngineReturn::Integer)
