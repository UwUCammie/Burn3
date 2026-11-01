// 0x001549f0 CompareEntryPairSign: compares the records two handles point at (each record's entry block starts
// 4288 bytes in) with CompareEntriesForSort (0x0015ae40) and turns its byte result into a sign:
// 1 when the low byte of the comparison is 0, otherwise -1.
//
//   001549f0  addiu $sp, $sp, -16        frame of 16 bytes
//   001549f4  lw $v1, 0($a0)             record A
//   001549f8  lw $v0, 0($a1)             record B
//   001549fc  sd $ra, 0($sp)
//   00154a00  addiu $a0, $v1, 4288
//   00154a04  jal 0x0015ae40
//   00154a08  addiu $a1, $v0, 4288       (delay slot)
//   00154a0c..00154a24                   v0 = (v0 & 0xff) == 0 ? 1 : -1, restore, return
//
// First caller written in the callable-body form (include/takedown/runtime/game/n3_call.hpp).

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0015ae40)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFrameBytes = 16U;
constexpr GuestAddress kEntryBlock = 4288U;
constexpr std::uint32_t kBeforeCall = 7U;   // 0x001549f0-0x00154a08: up to the jal and its delay slot
constexpr std::uint32_t kAfterCall = 7U;    // 0x00154a0c-0x00154a24
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run compare_entry_pair_sign(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress a = call.memory().read_u32_le(call.arg(0));
    const GuestAddress b = call.memory().read_u32_le(call.arg(1));
    call.set_arg(0, a + kEntryBlock);
    call.set_arg(1, b + kEntryBlock);
    const n3::Run callee = call.invoke([](auto& m) { return n3::fn_0015ae40(m); }, kFrameBytes);
    if (callee.declined) {
        return call.decline();
    }
    const std::uint32_t low = call.result() & 0xffU;
    call.return_value(low == 0U ? 1U : 0xffffffffU);
    return call.finish(kBeforeCall + callee.retired + kAfterCall);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(001549f0, 56U, 0xd3c75ceac47e3e7aULL, "CompareEntryPairSign", kCeiling, compare_entry_pair_sign,
         EngineReturn::Integer)
