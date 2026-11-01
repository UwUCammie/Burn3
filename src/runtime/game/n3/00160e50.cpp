// 0x00160e50 (a0 = record): InitStateRecord. Resets an ~80-byte state record to its initial values: zero and
// 0xff bytes, zero words, the raw float bit patterns 5.0 (+8, +20), -5.0 (+12) and -1.0 (+16), and two 0xffff
// halfwords. The flags byte at +75 is cleared by four separate load-AND-store pairs; each one re-reads the byte
// from memory, in the listing's order. A leaf with no loop and no call: all 47 instructions retire on every path.
// Returns nothing (v0 is not written; v1 and the other temporaries are scratch).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// Raw IEEE-754 bit patterns the original builds with lui. No float arithmetic is involved.
constexpr std::uint32_t kFive = 0x40a00000U;       // 5.0
constexpr std::uint32_t kMinusFive = 0xc0a00000U;  // -5.0
constexpr std::uint32_t kMinusOne = 0xbf800000U;   // -1.0

// One flags update at +75 (lbu, andi with `mask`, sb). The original reads the byte from memory every time.
void mask_flags_byte(auto& memory, const GuestAddress address, const std::uint8_t mask) {
    memory.write_u8(address, static_cast<std::uint8_t>(memory.read_u8(address) & mask));
}

template <class Context>
n3::Run init_state_record(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);

    m.write_u8(record + 70U, 0U);                     // 0x00160e50 sb $zero
    m.write_u32_le(record + 56U, 0U);                // 0x00160e58 sw $zero
    m.write_u8(record + 71U, 0xffU);                 // 0x00160e60 sb $t1
    m.write_u8(record + 65U, 0xffU);                 // 0x00160e68 sb $t1
    m.write_u8(record + 68U, 0xffU);                 // 0x00160e70 sb $t1
    m.write_u8(record + 69U, 0xffU);                 // 0x00160e78 sb $t1
    mask_flags_byte(m, record + 75U, 0xfeU);         // 0x00160e7c..84
    m.write_u32_le(record + 44U, 0U);                // 0x00160e88
    m.write_u32_le(record + 0U, 0U);                 // 0x00160e8c
    m.write_u32_le(record + 4U, 0U);                 // 0x00160e90
    m.write_u32_le(record + 48U, 0U);                // 0x00160e94
    m.write_u32_le(record + 52U, 0U);                // 0x00160e98
    m.write_u32_le(record + 32U, 0U);                // 0x00160e9c
    m.write_u32_le(record + 28U, 0U);                // 0x00160ea0
    m.write_u32_le(record + 8U, kFive);              // 0x00160ea4 sw $a3
    m.write_u32_le(record + 12U, kMinusFive);        // 0x00160ea8 sw $a2
    mask_flags_byte(m, record + 75U, 0xefU);         // 0x00160eac..b4
    m.write_u8(record + 74U, 0xffU);                 // 0x00160eb8 sb $t1
    m.write_u32_le(record + 40U, 0U);                // 0x00160ebc
    m.write_u32_le(record + 16U, kMinusOne);         // 0x00160ec0 sw $a1
    m.write_u32_le(record + 20U, kFive);             // 0x00160ec4 sw $a3
    m.write_u8(record + 77U, 0U);                    // 0x00160ec8 sb $zero
    m.write_u32_le(record + 24U, 0U);                // 0x00160ecc
    m.write_u8(record + 64U, 0xffU);                 // 0x00160ed0 sb $t1
    mask_flags_byte(m, record + 75U, 0xf7U);         // 0x00160ed4..dc
    m.write_u32_le(record + 36U, 0U);                // 0x00160ee0
    m.write_u8(record + 76U, 0U);                    // 0x00160ee4 sb $zero
    mask_flags_byte(m, record + 75U, 0xfbU);         // 0x00160ee8..f0
    m.write_u8(record + 72U, 0U);                    // 0x00160ef4 sb $zero
    m.write_u8(record + 66U, 0xffU);                 // 0x00160ef8 sb $t1
    m.write_u8(record + 67U, 0xffU);                 // 0x00160efc sb $t1
    m.write_u16_le(record + 60U, 0xffffU);           // 0x00160f00 sh $v1
    m.write_u16_le(record + 62U, 0xffffU);           // 0x00160f08 sh $v1 (jr $ra delay slot)
    return call.finish(47U);
}

} // namespace

extern constinit const EngineFunction n3_00160e50{0x00160e50U, 188U, 0x9ecbbbde7464bc08ULL, "InitStateRecord", 47U,
                                                  n3::live<&init_state_record<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00160e50, init_state_record)
