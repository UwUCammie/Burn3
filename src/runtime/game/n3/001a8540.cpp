// 0x001a8540 (a0 = record): ResetSlotLinkRecordTail. A tail call into ResetSlotLinkRecord (0x0022da20):
//
//   001a8540  j 0x0022da20       tail jump: $a0 (the record) and $ra are passed on unchanged
//   001a8544  nop                (delay slot, retired before the jump takes effect)
//
// The callee's own `jr $ra` returns to this function's caller, so nothing is returned here and v0 is untouched.
// Every call retires the j and its delay slot (2) plus the callee's fixed 11 instructions.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0022da20)

namespace takedown::runtime {
namespace {

// The j and its delay slot (nop) retire before the jump takes effect.
constexpr std::uint32_t kJumpCost = 2U;
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run reset_slot_link_record_tail(Context& context) {
    auto call = n3::guest_call(context);
    // A tail call has no frame of its own: the callee runs with this function's $sp and $ra.
    const n3::Run callee = call.invoke([](auto& m) { return n3::fn_0022da20(m); }, 0U);
    if (callee.declined) {
        return call.decline();
    }
    return call.finish(kJumpCost + callee.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(001a8540, 8U, 0x1f881b9524ab387bULL, "ResetSlotLinkRecordTail", kCeiling, reset_slot_link_record_tail,
         EngineReturn::Nothing)
