// 0x002630f0 (a0 = size in bytes, a1 = alignment, a power of two): AllocateAlignedFromArena. A bump allocator on a
// static arena whose cursor is the word at $gp - 27888. With m = alignment - 1, the block starts at the first
// multiple of the alignment at or after the cursor: aligned = (cursor + m) & ~m. Returns v0 = 0x01DBCA80 + aligned
// (EngineReturn::Integer, the pointer) and stores the new cursor, aligned + size. A leaf with no branches, loops or
// calls: every path retires the same 11 instructions.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kCursor = static_cast<GuestAddress>(-27888);  // gp-relative offset of the cursor word
constexpr GuestAddress kArenaBase = 0x01dbca80U;                     // lui 0x01dc, addiu -13696

// Retired instructions of the listing (0x002630f0..0x00263118). There is no branch, so every path retires all of them.
constexpr std::uint32_t kCost = 11U;

template <class Context>
n3::Run allocate_aligned_from_arena(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const std::uint32_t size = call.arg(0);
    const std::uint32_t mask = call.arg(1) - 1U;
    const GuestAddress cursor_address = call.cpu().read_gpr_u32(n3::kGlobalPointer) + kCursor;
    const std::uint32_t aligned = (m.read_u32_le(cursor_address) + mask) & ~mask;
    m.write_u32_le(cursor_address, aligned + size);
    call.return_value(kArenaBase + aligned);
    return call.finish(kCost);
}

} // namespace

extern constinit const EngineFunction n3_002630f0{0x002630f0U, 44U, 0x35ec335a11ba9d6fULL, "AllocateAlignedFromArena",
                                                  11U, n3::live<&allocate_aligned_from_arena<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(002630f0, allocate_aligned_from_arena)
