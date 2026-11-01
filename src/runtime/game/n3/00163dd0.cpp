// 0x00163dd0 (a0 = pool P, a1 = state record R): InitAndAppendStateRecord. Resets R with InitStateRecord
// (0x00160e50), then links R in as the newest entry of the pool's chain: R's byte +68 takes byte +73 of the old
// tail and the tail pointer becomes R. Then bit (R's byte +73) of the pool's 256-bit bitmap is cleared, and bit 3
// of R's flags byte at +75 is cleared. Returns nothing: v0 is never written.
//
// The jal at 0x00163de8 goes to 0x00162be0, which is only `j 0x00160e50` and a nop, so InitStateRecord is inlined
// here store for store (47 straight-line instructions, no call). The bgez on the byte at +73 always branches (a
// zero-extended byte is never negative), so the block at 0x00163e1c..0x00163e24 never runs. Every call retires the
// same 91 instructions: 42 here, 2 in the stub and 47 in InitStateRecord.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// lui 0x0003 gives 0x00030000; each access adds its own displacement to the pool pointer P.
constexpr GuestAddress kChainTail = 0x00030000U + 25452U;  // lw/sw at P + 0x3636C (the tail pointer)
constexpr GuestAddress kBitmap = 0x00030000U + 25344U;     // lw/sw at P + 0x36300 + 4 * word
constexpr std::uint32_t kOwnCost = 42U;                     // this function's own path, delay slots included
constexpr std::uint32_t kStubCost = 2U;                     // 0x00162be0: j and its nop
constexpr std::uint32_t kInitStateRecordCost = 47U;         // 0x00160e50, through its jr and delay slot

// Raw IEEE-754 bit patterns the original builds with lui. No float arithmetic is involved.
constexpr std::uint32_t kFive = 0x40a00000U;       // 5.0
constexpr std::uint32_t kMinusFive = 0xc0a00000U;  // -5.0
constexpr std::uint32_t kMinusOne = 0xbf800000U;   // -1.0

// One flags update (lbu, andi with `mask`, sb). The original reads the byte from memory every time.
void mask_flags_byte(auto& memory, const GuestAddress address, const std::uint8_t mask) {
    memory.write_u8(address, static_cast<std::uint8_t>(memory.read_u8(address) & mask));
}

// InitStateRecord (0x00160e50), inlined: the same stores, in the listing's order.
void init_state_record(auto& m, const GuestAddress record) {
    m.write_u8(record + 70U, 0U);                   // 0x00160e50 sb $zero, 70
    m.write_u32_le(record + 56U, 0U);               // 0x00160e58 sw $zero, 56
    m.write_u8(record + 71U, 0xffU);                // 0x00160e60 sb $t1, 71
    m.write_u8(record + 65U, 0xffU);                // 0x00160e68 sb $t1, 65
    m.write_u8(record + 68U, 0xffU);                // 0x00160e70 sb $t1, 68
    m.write_u8(record + 69U, 0xffU);                // 0x00160e78 sb $t1, 69
    mask_flags_byte(m, record + 75U, 0xfeU);        // 0x00160e7c..0x00160e84 lbu, andi, sb 75
    m.write_u32_le(record + 44U, 0U);               // 0x00160e88 sw $zero, 44
    m.write_u32_le(record + 0U, 0U);                // 0x00160e8c sw $zero, 0
    m.write_u32_le(record + 4U, 0U);                // 0x00160e90 sw $zero, 4
    m.write_u32_le(record + 48U, 0U);               // 0x00160e94 sw $zero, 48
    m.write_u32_le(record + 52U, 0U);               // 0x00160e98 sw $zero, 52
    m.write_u32_le(record + 32U, 0U);               // 0x00160e9c sw $zero, 32
    m.write_u32_le(record + 28U, 0U);               // 0x00160ea0 sw $zero, 28
    m.write_u32_le(record + 8U, kFive);             // 0x00160ea4 sw $a3, 8
    m.write_u32_le(record + 12U, kMinusFive);       // 0x00160ea8 sw $a2, 12
    mask_flags_byte(m, record + 75U, 0xefU);        // 0x00160eac..0x00160eb4 lbu, andi, sb 75
    m.write_u8(record + 74U, 0xffU);                // 0x00160eb8 sb $t1, 74
    m.write_u32_le(record + 40U, 0U);               // 0x00160ebc sw $zero, 40
    m.write_u32_le(record + 16U, kMinusOne);        // 0x00160ec0 sw $a1, 16
    m.write_u32_le(record + 20U, kFive);            // 0x00160ec4 sw $a3, 20
    m.write_u8(record + 77U, 0U);                   // 0x00160ec8 sb $zero, 77
    m.write_u32_le(record + 24U, 0U);               // 0x00160ecc sw $zero, 24
    m.write_u8(record + 64U, 0xffU);                // 0x00160ed0 sb $t1, 64
    mask_flags_byte(m, record + 75U, 0xf7U);        // 0x00160ed4..0x00160edc lbu, andi, sb 75
    m.write_u32_le(record + 36U, 0U);               // 0x00160ee0 sw $zero, 36
    m.write_u8(record + 76U, 0U);                   // 0x00160ee4 sb $zero, 76
    mask_flags_byte(m, record + 75U, 0xfbU);        // 0x00160ee8..0x00160ef0 lbu, andi, sb 75
    m.write_u8(record + 72U, 0U);                   // 0x00160ef4 sb $zero, 72
    m.write_u8(record + 66U, 0xffU);                // 0x00160ef8 sb $t1, 66
    m.write_u8(record + 67U, 0xffU);                // 0x00160efc sb $t1, 67
    m.write_u16_le(record + 60U, 0xffffU);          // 0x00160f00 sh $v1, 60
    m.write_u16_le(record + 62U, 0xffffU);          // 0x00160f08 sh $v1, 62 (jr $ra delay slot)
}

template <class Context>
n3::Run init_and_append_state_record(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress pool = call.arg(0);
    const GuestAddress record = call.arg(1);

    init_state_record(m, record);

    // 0x00163df0..0x00163e0c: the old tail's byte +73 goes to R's byte +68, then R becomes the tail.
    const GuestAddress tail = pool + kChainTail;
    const GuestAddress previous = m.read_u32_le(tail);
    const std::uint8_t copied = m.read_u8(previous + 73U);
    m.write_u8(record + 68U, copied);
    m.write_u32_le(tail, record);

    // 0x00163e10..0x00163e60: n = R's byte +73, read after the tail update as the listing does. Bit (n & 31) of
    // bitmap word (n >> 5) is cleared. The bgez is always taken, so the 0x00163e1c block is never reached.
    const std::uint32_t n = m.read_u8(record + 73U);
    const GuestAddress word = pool + kBitmap + 4U * (n >> 5U);
    const std::uint32_t keep = ~(1U << (n & 31U));
    const std::uint32_t bits = m.read_u32_le(word);
    m.write_u32_le(word, bits & keep);

    // 0x00163e64..0x00163e6c
    mask_flags_byte(m, record + 75U, 0xf7U);
    return call.finish(kOwnCost + kStubCost + kInitStateRecordCost);
}

} // namespace

extern constinit const EngineFunction n3_00163dd0{0x00163dd0U, 180U, 0xc43b2ca249ba0423ULL, "InitAndAppendStateRecord",
                                                  91U, n3::live<&init_and_append_state_record<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00163dd0, init_and_append_state_record)
