// 0x002a7830 (ConstructDefaultRecord, leaf): a0 is an object pointer that is only handed back, a1 the record D.
// Both calls are unconditional and go to tiny leaves, so both are inlined here exactly:
//   0x002ad630(a0, D) stores D+0 = -1, D+4 = -1, D+16 = 0, D+8 = 0, D+12 = 1, D+20 = 0, D+24 = 0 and sets v0 = a0.
//   The original's delay slot of the second call (0x002a7854) then clears D+0 again.
//   0x002ab010(T) fills its stack temporary T (sp+48): T+0 = 0x002ab0d0, T+4 = 0x002ab070, and every other word the
//   copies read (T+8..T+44, T+56, T+60) is 0. T lies in this function's own frame, below the entry sp, which a caller
//   cannot pass as D, so the copies take those values from constants.
// The fourteen copies into D run in listing order, and the function returns v0 = a0 (daddu, all 64 bits).
// Retired: 44 own + 11 + 23 = 78 on the single path, which is the declared cost (no branches, no loops).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// Callee 0x002ab010 stores these two words into its temporary: lui 0x2b with addiu -20272 and -20368.
constexpr std::uint32_t kTempWord0 = 0x002B0000U - 20272U;  // 0x002ab0d0
constexpr std::uint32_t kTempWord1 = 0x002B0000U - 20368U;  // 0x002ab070

template <class Context>
n3::Run construct_default_record(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const std::uint64_t object = call.arg64(0);
    const GuestAddress record = call.arg(1);
    const GuestAddress entry_sp = call.stack_pointer();

    // Callee 0x002ad630, inlined: its stores in listing order.
    m.write_u32_le(record + 0U, 0xFFFFFFFFU);
    m.write_u32_le(record + 4U, 0xFFFFFFFFU);
    m.write_u32_le(record + 16U, 0U);
    m.write_u32_le(record + 8U, 0U);
    m.write_u32_le(record + 12U, 1U);
    m.write_u32_le(record + 20U, 0U);
    m.write_u32_le(record + 24U, 0U);

    // The original's delay slot after the second call (0x002a7854) clears the first word again.
    m.write_u32_le(record + 0U, 0U);

    // Callee 0x002ab010, inlined, then the copies out of its temporary T (the offset of each T word is noted).
    m.write_u32_le(record + 28U, kTempWord0);  // T+0
    m.write_u32_le(record + 52U, 0U);          // T+24
    m.write_u32_le(record + 56U, 0U);          // T+28
    m.write_u32_le(record + 36U, 0U);          // T+8
    m.write_u32_le(record + 44U, 0U);          // T+16
    m.write_u32_le(record + 48U, 0U);          // T+20
    m.write_u32_le(record + 40U, 0U);          // T+12
    m.write_u32_le(record + 32U, kTempWord1);  // T+4
    m.write_u32_le(record + 88U, 0U);          // T+60
    m.write_u32_le(record + 84U, 0U);          // T+56
    m.write_u32_le(record + 60U, 0U);          // T+32
    m.write_u32_le(record + 64U, 0U);          // T+36
    m.write_u32_le(record + 72U, 0U);          // T+44
    m.write_u32_le(record + 68U, 0U);          // T+40

    // Result: v0 = a0 in all 64 bits (daddu). Temporaries the original leaves: v1 = 0 (its last lw reads T+40),
    // a0 = entry sp - 80 (addiu a0, sp, 48 after the frame is opened with addiu sp, sp, -128).
    call.return_value64(object);
    call.cpu().write_gpr_sign_extended_32(3U, 0U);
    call.cpu().write_gpr_sign_extended_32(4U, entry_sp - 80U);
    return call.finish(78U);
}

} // namespace

extern constinit const EngineFunction n3_002a7830{0x002a7830U, 176U, 0x79a39ca44bd5bd1aULL, "ConstructDefaultRecord",
                                                  78U, n3::live<&construct_default_record<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(002a7830, construct_default_record)
