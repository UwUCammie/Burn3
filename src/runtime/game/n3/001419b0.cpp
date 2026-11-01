// 0x001419b0 (a0 = car/object struct): GetLinkedRecordByte. The record pointer at +9264 selects the byte: a null
// pointer gives 0, otherwise the unsigned byte at record +5456. Returns v0 = 0..255. A leaf with no stores; the
// original's temporary v0 (the pointer) is caller-saved and not reproduced.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kRecordPointer = 9264U;  // lw 9264(a0)
constexpr GuestAddress kRecordByte = 5456U;     // lbu 5456(record)

// Retired instruction counts of the two paths, delay slots included.
constexpr std::uint32_t kNullCost = 6U;         // lw, beq taken to 0x1419c4 and its nop, daddu, jr and its nop
constexpr std::uint32_t kRecordCost = 7U;       // lw, beq not taken and its nop, beq zero,zero, the lbu in its
                                                // delay slot, jr and its nop

template <class Context>
n3::Run get_linked_record_byte(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const GuestAddress record = m.read_u32_le(object + kRecordPointer);
    if (record == 0U) {
        // The lbu at 0x001419c0 is skipped on this path, so the byte at record +5456 is never read.
        call.return_value(0U);
        return call.finish(kNullCost);
    }
    call.return_value(m.read_u8(record + kRecordByte));
    return call.finish(kRecordCost);
}

} // namespace

extern constinit const EngineFunction n3_001419b0{0x001419b0U, 32U, 0x0385671de71fc03fULL, "GetLinkedRecordByte", 7U,
                                                  n3::live<&get_linked_record_byte<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(001419b0, get_linked_record_byte)
