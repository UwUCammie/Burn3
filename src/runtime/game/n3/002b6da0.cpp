// 0x002b6da0 (TransferListHeadToFront): takes the first node N off the list whose
// head pointer is at X+16 (X = *a0 is the source record) and pushes it onto the
// front of the list whose head pointer is at a0+4, then returns v0 = N. When the
// source list is empty it returns 0 and stores nothing. The loads and stores are
// made in the listing's order, and memory is re-read exactly where the listing
// re-reads it, so aliased records behave the same. Leaf, no loops, no calls.
// Retired counts (delay slots included): 9 when the source list is empty,
// 19 to 21 otherwise; declared cost 21.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

template <class Context>
n3::Run transfer_list_head_to_front(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0U);
    const GuestAddress source = m.read_u32_le(record);                  // lw $v0, 0($a0)
    const GuestAddress source_head = source + 16U;                      // addiu $a1, $v0, 16
    call.cpu().write_gpr_sign_extended_32(5U, source_head);
    const GuestAddress node = m.read_u32_le(source_head);               // lw $v0, 16($v0)
    if (node == 0U) {
        call.return_value(0U);                                          // daddu $v0, $zero, $zero
        return call.finish(9U);
    }

    // Instructions up to and including 0x002b6dc4: 10 retired.
    std::uint32_t retired = 10U;
    const std::uint32_t next = m.read_u32_le(node);                     // lw $v1, 0($v0)
    m.write_u32_le(source_head, next);                                  // sw $v1, 0($a1)
    const std::uint32_t next_reloaded = m.read_u32_le(node);            // lw $v1, 0($v0)
    if (next_reloaded != 0U) {
        m.write_u32_le(next_reloaded + 4U, 0U);                         // sw $zero, 4($v1)
        retired += 1U;
    }

    std::uint32_t v1 = m.read_u32_le(record + 4U);                      // lw $v1, 4($a0)
    retired += 3U;
    if (v1 == 0U) {
        m.write_u32_le(node, 0U);                                       // sw $zero, 0($v0)
        retired += 4U;
    } else {
        m.write_u32_le(node, v1);                                       // sw $v1, 0($v0)
        v1 = m.read_u32_le(node);                                       // lw $v1, 0($v0)
        m.write_u32_le(v1 + 4U, node);                                  // sw $v0, 4($v1)
        retired += 5U;
    }
    m.write_u32_le(record + 4U, node);                                  // sw $v0, 4($a0)
    call.cpu().write_gpr_sign_extended_32(3U, v1);
    call.return_value(node);
    return call.finish(retired + 2U);                                   // jr $ra ; nop
}

} // namespace

extern constinit const EngineFunction n3_002b6da0{0x002b6da0U, 100U, 0xd74abad3fae538adULL, "TransferListHeadToFront", 21U,
                                                  n3::live<&transfer_list_head_to_front<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(002b6da0, transfer_list_head_to_front)
