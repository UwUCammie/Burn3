// 0x00319390 ClearByte10: `jr $ra` with `sb $zero, 10($a0)` in the delay slot. Clears the byte at +10 of the object
// in a0. Cold leaf, reached only from ReleaseObjectSlots (0x0031e1a0).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kFlagByte = 10U;
constexpr std::uint32_t kRetired = 2U;   // jr + the sb in its delay slot

template <class Context>
n3::Run clear_byte_10(Context& context) {
    auto call = n3::guest_call(context);
    call.memory().write_u8(call.arg(0) + kFlagByte, 0U);
    return call.finish(kRetired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(00319390, 8U, 0x9a0d7f231e4c7e4eULL, "ClearByte10", 2U, clear_byte_10, EngineReturn::Nothing)
