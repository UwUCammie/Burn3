// 0x001f5510 SetDrawStateSlot (a0 = slot id, a1 = value; slots 4 and 5 read their value from the address a1).
// The id picks one of five blocks through the jump table at 0x004b8620; any other id returns 0 at once.
//   1 DIMX:  while the gate word at gp-24048 is non-zero, reserve two quadwords (ReservePacketBuffer 0x80000000, 2),
//            write a PACKED GIF tag for one A+D register at the packet cursor (gp-24240) and the DIMX data quadword
//            after it. The data is 0 for a1 == 0 (state word gp-24044 = 0), 0x1212303012123030 for a1 == 0x55555555
//            (state = a1) and 0x6071243571603524 otherwise (state = 1).
//   2 ALPHA: as slot 1 with ALPHA_1 (0x42); the data (a1 & 0xff) | (((a1 >> 24) & 0xff) << 32) is also kept at gp-28168.
//   3 TEST:  as slot 1 with TEST_1 (0x47); the data is the 64-bit word at gp-28208 with its low 16 bits replaced by a1's,
//            and that word is stored back to gp-28208.
//   4 float: f0 = the word at a1; gp-24000 = 0 and gp-23996 = that word.
//   5 word:  v = the word at a1; when v - 1 is below 7 (unsigned), gp-28136 = v and the result is 1, else 0.
// v0 is 1 when the slot was written and 0 for an id outside 1..5, a closed gate, a full packet buffer or a refused value.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(001f3080)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using n3::Gpr128;

// Words beside $gp, as the listing addresses them (the address is gp - offset).
constexpr std::uint32_t kGateBelow = 24048U;
constexpr std::uint32_t kDimxStateBelow = 24044U;
constexpr std::uint32_t kCursorBelow = 24240U;
constexpr std::uint32_t kFloatFlagBelow = 24000U;
constexpr std::uint32_t kFloatBelow = 23996U;
constexpr std::uint32_t kAlphaShadowBelow = 28168U;
constexpr std::uint32_t kTestShadowBelow = 28208U;
constexpr std::uint32_t kWordBelow = 28136U;

constexpr std::uint32_t kFrameBytes = 32U;
constexpr std::uint32_t kCeiling = 20000U;

// The packet: a PACKED GIF tag for one register (NLOOP 1, EOP, NREG 1) whose descriptor 0x0e is A+D, then one
// {data, register address} quadword per write.
constexpr std::uint64_t kGifTagLow = 0x1000000000008001ULL;
constexpr std::uint64_t kGifTagAd = 0x0eU;
constexpr std::uint64_t kAlphaAddress = 0x42U;
constexpr std::uint64_t kDimxAddress = 0x44U;
constexpr std::uint64_t kTestAddress = 0x47U;

constexpr std::uint32_t kDimxSentinel = 0x55555555U;
constexpr std::uint64_t kDimxSentinelData = 0x1212303012123030ULL;
constexpr std::uint64_t kDimxOtherData = 0x6071243571603524ULL;

// Retired original instructions per block, delay slots included. The callee's count is added where it is called.
constexpr std::uint32_t kEnterCost = 7U;               // 0x1f5510-0x1f5528: frame, slot test, delay slot
constexpr std::uint32_t kDispatchCost = 7U;            // 0x1f552c-0x1f5544: table load, jr and its delay slot
constexpr std::uint32_t kZeroExitCost = 2U;            // 0x1f5554-0x1f5558: v0 = 0 and the branch to the exit
constexpr std::uint32_t kOneExitCost = 1U;             // 0x1f56f0: v0 = 1
constexpr std::uint32_t kExitCost = 4U;                // 0x1f56f4-0x1f5700: restore s0 and ra, return
constexpr std::uint32_t kGateCost = 3U;                // 0x1f5548-0x1f5550: gate load, bne and its delay slot
constexpr std::uint32_t kReserveCost = 5U;             // 0x1f555c-0x1f556c: lui, jal and its delay slot, result test
constexpr std::uint32_t kTagDimxCost = 9U;             // 0x1f5570-0x1f5590: tag quadword, cursor, DIMX state branch
constexpr std::uint32_t kDimxZeroCost = 3U;            // 0x1f5594-0x1f559c
constexpr std::uint32_t kDimxSentinelCost = 4U + 8U;   // 0x1f55a0-0x1f55ac, 0x1f55b0-0x1f55cc
constexpr std::uint32_t kDimxOtherCost = 4U + 7U;      // 0x1f55a0-0x1f55ac, 0x1f55d0-0x1f55e8
constexpr std::uint32_t kDimxDataCost = 4U;            // 0x1f55ec-0x1f55f8: register address, pcpyld, branch, cursor
constexpr std::uint32_t kTagAlphaCost = 8U;            // 0x1f5614-0x1f5630
constexpr std::uint32_t kAlphaDataCost = 10U;          // 0x1f5634-0x1f5658: ALPHA data and its shadow store
constexpr std::uint32_t kTagTestCost = 8U;             // 0x1f5674-0x1f5690
constexpr std::uint32_t kTestDataCost = 9U;            // 0x1f5694-0x1f56b4: TEST read-modify-write and its store
constexpr std::uint32_t kStoreCost = 4U;               // 0x1f56b8-0x1f56c4: data quadword, cursor, branch to the exit
constexpr std::uint32_t kFloatCost = 4U;               // 0x1f56c8-0x1f56d4
constexpr std::uint32_t kWordTestCost = 5U;            // 0x1f56d8-0x1f56e8
constexpr std::uint32_t kWordStoreCost = 1U;           // 0x1f56ec

// A quadword from its two doublewords (lane 0 is the low word of `low`, as the R5900 orders them).
Gpr128 quadword(const std::uint64_t low, const std::uint64_t high) {
    return Gpr128{{static_cast<std::uint32_t>(low), static_cast<std::uint32_t>(low >> 32U),
                   static_cast<std::uint32_t>(high), static_cast<std::uint32_t>(high >> 32U)}};
}

struct Reserved {
    bool declined{};
    bool ok{};
    std::uint32_t retired{};
};

// The original's `lui $a0, 0x8000; jal 0x001f3080; addiu $a1, $zero, 2`: ReservePacketBuffer(0x80000000, 2).
template <class Memory>
Reserved reserve_packet(n3::Call<Memory>& call) {
    call.set_arg(0, 0x80000000U);
    call.set_arg(1, 2U);
    const n3::Run run = call.invoke([](auto& m) { return n3::fn_001f3080(m); }, kFrameBytes);
    // The original's beq tests all 64 bits of v0, not only the low word.
    return Reserved{run.declined, call.result64() != 0U, run.retired};
}

// The GIF tag quadword at the cursor. The cursor then moves past it; returns the address of the next quadword.
template <class Memory>
GuestAddress put_tag(Memory& m, const GuestAddress cursor_word) {
    const GuestAddress at = m.read_u32_le(cursor_word);
    n3::store_quad_any(m, at, quadword(kGifTagLow, kGifTagAd));
    m.write_u32_le(cursor_word, at + 16U);
    return at + 16U;
}

// The data quadword {data, register address} at `at`, then the cursor moves past it.
template <class Memory>
void put_data(Memory& m, const GuestAddress cursor_word, const GuestAddress at, const std::uint64_t data,
              const std::uint64_t address) {
    n3::store_quad_any(m, at, quadword(data, address));
    m.write_u32_le(cursor_word, at + 16U);
}

// v0 = 0 and the epilogue. `retired` counts everything before the exit.
template <class Memory>
n3::Run refused(n3::Call<Memory>& call, const std::uint32_t retired) {
    call.return_value(0U);
    return call.finish(retired + kZeroExitCost + kExitCost);
}

// v0 = 1 and the epilogue.
template <class Memory>
n3::Run accepted(n3::Call<Memory>& call, const std::uint32_t retired) {
    call.return_value(1U);
    return call.finish(retired + kOneExitCost + kExitCost);
}

template <class Memory>
n3::Run dimx_slot(n3::Call<Memory>& call, const GuestAddress gp, const std::uint64_t value) {
    auto& m = call.memory();
    if (m.read_u32_le(gp - kGateBelow) == 0U) {
        return refused(call, kEnterCost + kDispatchCost + kGateCost);
    }
    const Reserved reserved = reserve_packet(call);
    if (reserved.declined) {
        return call.decline();
    }
    if (!reserved.ok) {
        return refused(call, kEnterCost + kDispatchCost + kGateCost + kReserveCost + reserved.retired);
    }
    const GuestAddress data_at = put_tag(m, gp - kCursorBelow);
    std::uint64_t data = 0U;
    std::uint32_t state = 0U;
    std::uint32_t data_cost = kDimxZeroCost;
    if (value == kDimxSentinel) {
        data = kDimxSentinelData;
        state = kDimxSentinel;
        data_cost = kDimxSentinelCost;
    } else if (value != 0U) {
        data = kDimxOtherData;
        state = 1U;
        data_cost = kDimxOtherCost;
    }
    m.write_u32_le(gp - kDimxStateBelow, state);
    put_data(m, gp - kCursorBelow, data_at, data, kDimxAddress);
    return accepted(call, kEnterCost + kDispatchCost + kGateCost + kReserveCost + kTagDimxCost + data_cost +
                              kDimxDataCost + kStoreCost + reserved.retired);
}

template <class Memory>
n3::Run alpha_slot(n3::Call<Memory>& call, const GuestAddress gp, const std::uint64_t value) {
    const Reserved reserved = reserve_packet(call);
    if (reserved.declined) {
        return call.decline();
    }
    if (!reserved.ok) {
        return refused(call, kEnterCost + kDispatchCost + kReserveCost + reserved.retired);
    }
    auto& m = call.memory();
    // Bits 0..7 of the value, then bits 24..31 of it moved up to bits 32..39.
    const std::uint64_t data = (value & 0xffU) | (((value >> 24U) & 0xffU) << 32U);
    const GuestAddress data_at = put_tag(m, gp - kCursorBelow);
    m.write_u64_le(gp - kAlphaShadowBelow, data);
    put_data(m, gp - kCursorBelow, data_at, data, kAlphaAddress);
    return accepted(call, kEnterCost + kDispatchCost + kReserveCost + kTagAlphaCost + kAlphaDataCost + kStoreCost +
                              reserved.retired);
}

template <class Memory>
n3::Run test_slot(n3::Call<Memory>& call, const GuestAddress gp, const std::uint64_t value) {
    const Reserved reserved = reserve_packet(call);
    if (reserved.declined) {
        return call.decline();
    }
    if (!reserved.ok) {
        return refused(call, kEnterCost + kDispatchCost + kReserveCost + reserved.retired);
    }
    auto& m = call.memory();
    const GuestAddress data_at = put_tag(m, gp - kCursorBelow);
    // The shadow keeps its upper 48 bits; the low 16 come from the value.
    const std::uint64_t test_word =
        (m.read_u64_le(gp - kTestShadowBelow) & 0xffffffffffff0000ULL) | (value & 0xffffU);
    m.write_u64_le(gp - kTestShadowBelow, test_word);
    put_data(m, gp - kCursorBelow, data_at, test_word, kTestAddress);
    return accepted(call, kEnterCost + kDispatchCost + kReserveCost + kTagTestCost + kTestDataCost + kStoreCost +
                              reserved.retired);
}

template <class Memory>
n3::Run float_slot(n3::Call<Memory>& call, const GuestAddress gp, const std::uint64_t value) {
    auto& m = call.memory();
    const std::uint32_t bits = m.read_u32_le(static_cast<GuestAddress>(value));
    call.cpu().write_fpr_bits(0U, bits);
    m.write_u32_le(gp - kFloatFlagBelow, 0U);
    m.write_u32_le(gp - kFloatBelow, bits);
    return accepted(call, kEnterCost + kDispatchCost + kFloatCost);
}

template <class Memory>
n3::Run word_slot(n3::Call<Memory>& call, const GuestAddress gp, const std::uint64_t value) {
    auto& m = call.memory();
    const std::uint32_t word = m.read_u32_le(static_cast<GuestAddress>(value));
    if (word - 1U >= 7U) {
        call.return_value(0U);
        return call.finish(kEnterCost + kDispatchCost + kWordTestCost + kExitCost);
    }
    m.write_u32_le(gp - kWordBelow, word);
    return accepted(call, kEnterCost + kDispatchCost + kWordTestCost + kWordStoreCost);
}

template <class Context>
n3::Run set_draw_state_slot(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    const std::uint64_t value = call.arg64(1);
    switch (call.arg(0)) {
    case 1U:
        return dimx_slot(call, gp, value);
    case 2U:
        return alpha_slot(call, gp, value);
    case 3U:
        return test_slot(call, gp, value);
    case 4U:
        return float_slot(call, gp, value);
    case 5U:
        return word_slot(call, gp, value);
    default:
        return refused(call, kEnterCost);
    }
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(001f5510, 500U, 0x0574e05e75e05045ULL, "SetDrawStateSlot", kCeiling, set_draw_state_slot,
         EngineReturn::Integer)
