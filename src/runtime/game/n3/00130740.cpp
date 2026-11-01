// 0x00130740 StoreQuadword (a0 = destination, a1 = the 128-bit value passed by value). Listing:
//
//   00130740  sq $a1, 0($a0)           stores a1 at a0 rounded down to 16 bytes (sq ignores the low four bits)
//   00130744  jr $ra
//   00130748  daddu $v0, $a0, $zero    (delay slot) v0 = a0, the whole 64-bit register
//
// Three instructions on every path; a leaf. When guest memory does not contain the 16 bytes, the interpreter's Sq
// faults before it writes anything, so that call declines and the original runs it.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kRetired = 3U;             // 0x00130740-0x00130748: sq, jr and the daddu in its delay slot
constexpr std::uint32_t kQuadwordBytes = 16U;
constexpr GuestAddress kQuadwordMask = ~15U;

// Calls retiring more than this are left to the original (call_cost returns 0).
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run store_quadword(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress destination = call.arg(0) & kQuadwordMask;
    if (!m.contains(destination, kQuadwordBytes)) {
        return call.decline();
    }
    n3::store_quad_any(m, destination, call.arg128(1));
    call.return_value64(call.arg64(0));
    return call.finish(kRetired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00130740, 12U, 0x3d757b104cfb3aa0ULL, "StoreQuadword", kCeiling, store_quadword, EngineReturn::Integer)
