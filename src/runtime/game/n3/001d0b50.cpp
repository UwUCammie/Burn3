// 0x001d0b50 (a0 = object O): UpdateEntryOrderState. P = [O+5108] is a sub-record.
//
// When [P+6420] is zero (the reset path, no call): state [O+5464] = 1, ratio [O+5468] = 0.0, the index byte [O+5460]
// is the low byte of [P+10176], and the flag byte [O+5438] = 0.
//
// Otherwise the index byte [O+5460] is chosen: 0 while the word at 0x0064d3a0 is 1; else bit 0 of the signed byte at
// [P+6572] (a negative byte gives -1 when that bit is set and 0 when it is clear). The word at 0x0064d3a0 is tested
// again: when it is nonzero, 0x0015ae40 compares [O+5480] + 4288 with 0x0064d3b0 + 10192 * index + 4288; when it is
// zero, the second argument is 4288. The state is 3 when that comparison returns 0, else 2. The ratio [O+5468] is
// min(max((F - 1600.0) / 18000.0, 0.0), 1.0) with F = the float at [O + 5472 + 4 * index] (index -1 reads the ratio
// word itself, as the listing does). The flag [O+5438] is 1 only for state 3.
//
// Retired counts follow the listing's blocks (the constants below). The comparator call makes this a callable body
// with a dry run (kCeiling) rather than a plain entry.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(0015ae40)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr std::uint32_t kFrameBytes = 32U;                        // addiu $sp, $sp, -32
constexpr GuestAddress kSubRecordOffset = 5108U;                  // O+5108: the sub-record P
constexpr GuestAddress kSubFlagOffset = 6420U;                    // P+6420: zero takes the reset path
constexpr GuestAddress kSubSignedByteOffset = 6572U;              // P+6572: signed byte, bit 0 gives the index
constexpr GuestAddress kSubIndexedWordOffset = 10176U;            // P+10176: low byte stored by the reset path
constexpr GuestAddress kIndexOffset = 5460U;                      // O+5460: signed index byte (-1, 0 or 1)
constexpr GuestAddress kStateOffset = 5464U;                      // O+5464: state word (1 reset, 2 or 3 compared)
constexpr GuestAddress kRatioOffset = 5468U;                      // O+5468: float ratio, clamped to [0, 1]
constexpr GuestAddress kFloatTableOffset = 5472U;                 // O+5472 + 4 * index: float table
constexpr GuestAddress kFlagOffset = 5438U;                       // O+5438: flag byte
constexpr GuestAddress kRecordOffset = 5480U;                     // O+5480: record compared with the array element
constexpr GuestAddress kRecordBlock = 4288U;                      // both comparator arguments sit this far in
constexpr GuestAddress kModeWord = 0x0064d3a0U;                   // lui 0x65, -11360 (tested against 1, then 0)
constexpr GuestAddress kArrayBase = 0x00522660U + 0x0012ad50U;    // lui/addiu 0x522660 + lui/ori 0x12ad50
constexpr std::uint32_t kArrayStride = 10192U;                    // one array record per index step
constexpr std::uint32_t kOffsetBits = 0x44c80000U;                // 1600.0f (lui 0x44c8)
constexpr std::int32_t kScale = 18000;                            // cvt.s.w of 18000
constexpr std::uint32_t kOneBits = 0x3f800000U;                   // 1.0f (lui 0x3f80)
constexpr std::uint8_t kSetByte = 1U;
constexpr std::uint8_t kClearByte = 0U;
constexpr std::uint32_t kCeiling = 20000U;

// Retired instruction counts of the listing's blocks, delay slots included.
constexpr std::uint32_t kEntryCost = 8U;          // 0x001d0b50-0x001d0b6c: up to the beq on [P+6420] and its nop
constexpr std::uint32_t kResetCost = 11U;         // 0x001d0c80-0x001d0ca8: the reset stores and the epilogue
constexpr std::uint32_t kModeTestCost = 5U;       // 0x001d0b70-0x001d0b80: lui, addiu, lw, bne and its nop
constexpr std::uint32_t kModeOneCost = 2U;        // 0x001d0b84-0x001d0b88: beq to 0x001d0ba8 and its sb
constexpr std::uint32_t kSignedByteCost = 3U;     // 0x001d0b8c-0x001d0b94: lb, bgez and its andi
constexpr std::uint32_t kNegativeByteCost = 2U;   // 0x001d0b98-0x001d0b9c: beq and its nop
constexpr std::uint32_t kOddNegativeCost = 1U;    // 0x001d0ba0: addiu, only when bit 0 is set
constexpr std::uint32_t kIndexStoreCost = 1U;     // 0x001d0ba4: sb
constexpr std::uint32_t kGlobalTestCost = 4U;     // 0x001d0ba8-0x001d0bb4: lui, lw, bne and its nop
constexpr std::uint32_t kZeroGlobalCost = 2U;     // 0x001d0bb8-0x001d0bbc: beq and daddu (v0 = 0)
constexpr std::uint32_t kIndexAddressCost = 9U;   // 0x001d0bc0-0x001d0be0: lb, the multiply and the address adds
constexpr std::uint32_t kCallSiteCost = 6U;       // 0x001d0be4-0x001d0bf8 without the callee: a1, a0, jal, beq
constexpr std::uint32_t kZeroResultCost = 2U;     // 0x001d0c08-0x001d0c0c: addiu and sw (state 3)
constexpr std::uint32_t kNonzeroResultCost = 3U;  // 0x001d0bfc-0x001d0c04: addiu, beq and its sw (state 2)
constexpr std::uint32_t kRatioCost = 22U;         // 0x001d0c10-0x001d0c64: the ratio stores and the state test
constexpr std::uint32_t kFlagSetCost = 3U;        // 0x001d0c68-0x001d0c70: state 3 sets the flag
constexpr std::uint32_t kFlagClearCost = 1U;      // 0x001d0c74: any other state clears it
constexpr std::uint32_t kJoinCost = 2U;           // 0x001d0c78-0x001d0c7c
constexpr std::uint32_t kEpilogueCost = 3U;       // 0x001d0ca0-0x001d0ca8

// MAX.S and MIN.S as takedown/guest/ee_cop1_arithmetic.hpp does them: the raw words compared as signed integers,
// with two negative words ordered the other way round.
std::uint32_t max_bits(const std::uint32_t left, const std::uint32_t right) {
    const auto l = std::bit_cast<std::int32_t>(left);
    const auto r = std::bit_cast<std::int32_t>(right);
    const std::int32_t result = (l < 0 && r < 0) ? (l < r ? l : r) : (l > r ? l : r);
    return std::bit_cast<std::uint32_t>(result);
}

std::uint32_t min_bits(const std::uint32_t left, const std::uint32_t right) {
    const auto l = std::bit_cast<std::int32_t>(left);
    const auto r = std::bit_cast<std::int32_t>(right);
    const std::int32_t result = (l < 0 && r < 0) ? (l > r ? l : r) : (l < r ? l : r);
    return std::bit_cast<std::uint32_t>(result);
}

// max.s against 0.0, then min.s against 1.0 (the listing keeps both constants in f0).
std::uint32_t clamp_to_unit(const std::uint32_t ratio) {
    return min_bits(max_bits(ratio, 0U), kOneBits);
}

template <class Context>
n3::Run update_entry_order_state(Context& context) {
    auto call = n3::guest_call(context);
    auto& memory = call.memory();
    const GuestAddress object = call.arg(0);

    // 0x001d0b50-0x001d0b68: the sub-record pointer, then the test of its flag word.
    const GuestAddress sub = memory.read_u32_le(object + kSubRecordOffset);
    if (memory.read_u32_le(sub + kSubFlagOffset) == 0U) {
        // 0x001d0c80-0x001d0c9c: the reset path, no call.
        memory.write_u32_le(object + kStateOffset, 1U);
        memory.write_u32_le(object + kRatioOffset, 0U);
        const GuestAddress sub_again = memory.read_u32_le(object + kSubRecordOffset);
        const std::uint32_t indexed = memory.read_u32_le(sub_again + kSubIndexedWordOffset);
        memory.write_u8(object + kIndexOffset, static_cast<std::uint8_t>(indexed));
        memory.write_u8(object + kFlagOffset, kClearByte);
        return call.finish(kEntryCost + kResetCost);
    }

    // 0x001d0b70-0x001d0b80: the global word at 0x0064d3a0 is tested against 1.
    std::uint32_t retired = kEntryCost + kModeTestCost;
    if (memory.read_u32_le(kModeWord) == 1U) {
        // 0x001d0b84-0x001d0b88: the index is 0.
        memory.write_u8(object + kIndexOffset, kClearByte);
        retired += kModeOneCost;
    } else {
        // 0x001d0b8c-0x001d0ba4: the index comes from the signed byte at P+6572.
        const std::int32_t signed_byte = static_cast<std::int8_t>(memory.read_u8(sub + kSubSignedByteOffset));
        retired += kSignedByteCost;
        std::uint32_t index_bits = static_cast<std::uint32_t>(signed_byte) & 1U;  // andi (bit 0)
        if (signed_byte < 0) {
            // 0x001d0b98-0x001d0ba0: a negative byte with bit 0 set gives -1 (addiu), otherwise 0.
            retired += kNegativeByteCost;
            if (index_bits != 0U) {
                retired += kOddNegativeCost;
                index_bits -= 2U;
            }
        }
        memory.write_u8(object + kIndexOffset, static_cast<std::uint8_t>(index_bits));
        retired += kIndexStoreCost;
    }

    // 0x001d0ba8-0x001d0bb4: the global word again. Zero leaves the array address at 0 (v0 = 0).
    retired += kGlobalTestCost;
    std::uint32_t array_address = 0U;
    if (memory.read_u32_le(kModeWord) != 0U) {
        // 0x001d0bc0-0x001d0be0: 0x0064d3b0 + 10192 * index (the low word of the multiply).
        retired += kIndexAddressCost;
        const std::int32_t stored_index = static_cast<std::int8_t>(memory.read_u8(object + kIndexOffset));
        array_address = kArrayBase + static_cast<std::uint32_t>(stored_index) * kArrayStride;
    } else {
        retired += kZeroGlobalCost;
    }

    // 0x001d0be4-0x001d0bf8: the comparator's arguments, the jal to 0x0015ae40 and the test of its result.
    const GuestAddress record = memory.read_u32_le(object + kRecordOffset);
    call.set_arg(0, record + kRecordBlock);
    call.set_arg(1, array_address + kRecordBlock);
    const n3::Run order = call.invoke([](auto& machine) { return n3::fn_0015ae40(machine); }, kFrameBytes);
    if (order.declined) {
        return call.decline();
    }
    retired += kCallSiteCost + order.retired;

    // 0x001d0c08-0x001d0c0c (result 0, state 3) or 0x001d0bfc-0x001d0c04 (state 2).
    const std::uint32_t state = call.result() == 0U ? 3U : 2U;
    retired += state == 3U ? kZeroResultCost : kNonzeroResultCost;
    memory.write_u32_le(object + kStateOffset, state);

    // 0x001d0c10-0x001d0c64: the ratio of the table entry the index selects, clamped to [0, 1]. The state test
    // at the end of this block reads back the state just written.
    retired += kRatioCost;
    const std::int32_t table_index = static_cast<std::int8_t>(memory.read_u8(object + kIndexOffset));
    const GuestAddress entry = object + (static_cast<std::uint32_t>(table_index) << 2U) + kFloatTableOffset;
    const std::uint32_t offset = fpu::sub(memory.read_u32_le(entry), kOffsetBits);
    const std::uint32_t ratio = fpu::div(offset, fpu::from_int(kScale));
    memory.write_u32_le(object + kRatioOffset, clamp_to_unit(ratio));

    // 0x001d0c5c-0x001d0c74: the flag is set only for state 3.
    if (memory.read_u32_le(object + kStateOffset) == 3U) {
        retired += kFlagSetCost;
        memory.write_u8(object + kFlagOffset, kSetByte);
    } else {
        retired += kFlagClearCost;
        memory.write_u8(object + kFlagOffset, kClearByte);
    }

    // 0x001d0c78-0x001d0ca8: the join and the epilogue.
    retired += kJoinCost + kEpilogueCost;
    return call.finish(retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(001d0b50, 348U, 0x21d3b5dc718ba22aULL, "UpdateEntryOrderState", kCeiling, update_entry_order_state,
         EngineReturn::Nothing)
