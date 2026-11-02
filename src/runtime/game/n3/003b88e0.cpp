// 0x003b88e0 ArenaAlloc: bump allocator. a0 = arena object, a1 = size in halfword units.
// The arena's current pointer lives in the word at arena + 6144; the call returns the old pointer and
// advances it by 2 * a1 bytes (32-bit wrap).
//
//   003b88e0  lw v0, 6144(a0) ; sll v1, a1, 1 ; addu v1, v0, v1 ; jr ra ; sw v1, 6144(a0)  (delay slot)

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kBumpPointer = 6144U;
constexpr std::uint32_t kRetired = 5U;

template <class Context>
n3::Run arena_alloc(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress arena = call.arg(0);
    const std::uint32_t old = m.read_u32_le(arena + kBumpPointer);
    m.write_u32_le(arena + kBumpPointer, old + (call.arg(1) << 1U));
    call.return_value(old);
    return call.finish(kRetired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(003b88e0, 20U, 0x4ff5329377f372d2ULL, "ArenaAlloc", 5U, arena_alloc, EngineReturn::Integer)
