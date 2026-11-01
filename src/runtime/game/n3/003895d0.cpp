// 0x003895d0 (a0 = entry E, a1 = reference R): StoreBase40NameKey. E receives the reference at +8 and a 64-bit key
// at +0. The function stores R into E+8, packs the NUL-terminated name whose pointer is at R+4 into a 64-bit
// base-40 key with PackBase40Name12 (0x0021aef0), stores the whole key into E+0 and returns it in v0. The callee's
// v0 is not changed after the call, so the returned value is the key itself.
//
//   003895d0  addiu $sp, $sp, -32        frame of 32 bytes
//   003895d4  sd $ra, 16($sp)            saves, restored at the end (not reproduced: stack words)
//   003895d8  sq $s0, 0($sp)
//   003895dc  sw $a1, 8($a0)             E+8 = R, before R+4 is read (the two can overlap)
//   003895e0  daddu $s0, $a0, $zero      s0 = E
//   003895e4  jal 0x0021aef0             v0 = PackBase40Name12(a0)
//   003895e8  lw $a0, 4($a1)             (delay slot) a0 = the name pointer at R+4
//   003895ec  sd $v0, 0($s0)             E+0 = key, all 64 bits
//   003895f0..003895fc                   restore ra and s0, return
//
// Retired count: 12 own instructions (the jal and its delay slot included, the return's delay slot too) plus the
// callee's count, which the invoke reports exactly.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0021aef0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFrameBytes = 32U;
constexpr GuestAddress kReferenceOffset = 8U;   // E+8: the reference (sw $a1, 8($a0))
constexpr GuestAddress kNameOffset = 4U;        // R+4: the name pointer (lw $a0, 4($a1))
constexpr std::uint32_t kBeforeCall = 7U;       // 003895d0-003895e8: up to the jal and its delay slot
constexpr std::uint32_t kAfterCall = 5U;        // 003895ec-003895fc: key store, restores, jr and its delay slot
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run store_base40_name_key(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress entry = call.arg(0);
    const GuestAddress reference = call.arg(1);
    call.memory().write_u32_le(entry + kReferenceOffset, reference);
    call.set_arg(0, call.memory().read_u32_le(reference + kNameOffset));
    const n3::Run callee = call.invoke([](auto& machine) { return n3::fn_0021aef0(machine); }, kFrameBytes);
    if (callee.declined) {
        return call.decline();
    }
    call.memory().write_u64_le(entry, call.result64());
    return call.finish(kBeforeCall + callee.retired + kAfterCall);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(003895d0, 48U, 0xc666c09438d3fa78ULL, "StoreBase40NameKey", kCeiling, store_base40_name_key,
         EngineReturn::Integer)
