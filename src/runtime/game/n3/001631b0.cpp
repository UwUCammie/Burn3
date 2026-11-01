// 0x001631b0 (a0 = record): InitStateRecordSetByte73. Runs InitStateRecord (0x00160e50) on the record, then stores
// 0xff at its byte +73 (the one byte of the record that InitStateRecord leaves alone) and returns 1 in v0.
//
//   001631b0  addiu $sp, $sp, -32        frame of 32 bytes
//   001631b4  sd $ra, 16($sp)            saved registers sit below the entry $sp, so the native does not write them
//   001631b8  sq $s0, 0($sp)
//   001631bc  jal 0x00160e50
//   001631c0  daddu $s0, $a0, $zero      (delay slot: a0 is still the record when the jal runs)
//   001631c4  addiu $v1, $zero, 255
//   001631c8  addiu $v0, $zero, 1
//   001631cc  sb $v1, 73($s0)
//   001631d0..001631dc                   restore ra and s0, pop the frame, return
//
// Five instructions up to the jal and its delay slot, seven after the call, and the callee's 47 on every path.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00160e50)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFrameBytes = 32U;
constexpr GuestAddress kFlagByteOffset = 73U;
constexpr std::uint32_t kBeforeCall = 5U;   // 0x001631b0-0x001631c0: up to the jal and its delay slot
constexpr std::uint32_t kAfterCall = 7U;    // 0x001631c4-0x001631dc
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run init_state_record_set_byte73(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress record = call.arg(0);
    const n3::Run callee = call.invoke([](auto& m) { return n3::fn_00160e50(m); }, kFrameBytes);
    if (callee.declined) {
        return call.decline();
    }
    call.cpu().write_gpr_sign_extended_32(3U, 255U);   // v1: the original's last write to v1 (a temporary)
    call.return_value(1U);
    call.memory().write_u8(record + kFlagByteOffset, 0xffU);
    return call.finish(kBeforeCall + callee.retired + kAfterCall);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(001631b0, 48U, 0x45aecf008ccc7aefULL, "InitStateRecordSetByte73", kCeiling, init_state_record_set_byte73,
         EngineReturn::Integer)
