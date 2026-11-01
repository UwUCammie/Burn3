// 0x00283510 (a0 = timer record, a1 = value for +52, a2 = address of a quadword, f12/f13 = floats):
// initialises the record. Stores a1 at +52, then the raw bits of f13 at +44 and of f12 at +48, then copies the 16
// bytes at a2 to a0 + 0 (lq/sq ignore the low four address bits). It then reads the signed word at +56,
// which none of those stores touched: a word below 25 is replaced by word + 2 (wrapping), and the byte
// at +60 is set to 1 on every path. A straight-line leaf: no loops, no calls, no return value.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// Offsets inside the record at a0.
constexpr GuestAddress kF13Offset = 44U;     // swc1 $f13
constexpr GuestAddress kF12Offset = 48U;     // swc1 $f12
constexpr GuestAddress kArg1Offset = 52U;    // sw $a1
constexpr GuestAddress kCounterOffset = 56U; // signed word, tested against kLimit
constexpr GuestAddress kFlagOffset = 60U;    // sb 1

// slti is a signed compare.
constexpr std::int32_t kLimit = 25;
// Retired instructions per path, delay slots included. The first 9 run on every path (up to and
// including the bne's delay slot). Then 3 + 2 when the word is at or above the limit (the fall-through
// addiu/beq/sb, then jr and its delay slot), or 3 + 3 + 2 through the add path below the limit.
constexpr std::uint32_t kRetiredAtOrAboveLimit = 14U;
constexpr std::uint32_t kRetiredBelowLimit = 17U;

template <class Context>
n3::Run init_timer_record(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress record = call.arg(0);
    const GuestAddress source = call.arg(2);
    // Same order as the original: the three scalar stores first, then the quadword copy (its load sees
    // those stores when source overlaps the record). The copy's store reaches at most record + 15, so
    // it cannot touch the word at +56.
    m.write_u32_le(record + kArg1Offset, call.arg(1));
    m.write_u32_le(record + kF13Offset, call.cpu().read_fpr_bits(13U));
    m.write_u32_le(record + kF12Offset, call.cpu().read_fpr_bits(12U));
    const auto quad = n3::load_quad_any(m, source);
    n3::store_quad_any(m, record, quad);
    const std::uint32_t counter = m.read_u32_le(record + kCounterOffset);
    std::uint32_t retired = kRetiredAtOrAboveLimit;
    if (static_cast<std::int32_t>(counter) < kLimit) {
        // The original's sw sits in the delay slot of the branch back to the flag store.
        m.write_u32_le(record + kCounterOffset, counter + 2U);
        retired = kRetiredBelowLimit;
    }
    m.write_u8(record + kFlagOffset, 1U);
    // The original leaves v1 = 1 (the value it stored as the flag) and a1 = the word it read.
    call.cpu().write_gpr_sign_extended_32(3U, 1U);
    call.cpu().write_gpr_sign_extended_32(5U, counter);
    return call.finish(retired);
}

} // namespace

// Longest path (17 retired) is the declared cost; the function has no loops and no calls.
extern constinit const EngineFunction n3_00283510{0x00283510U, 68U, 0xa4019c6227a71b26ULL, "InitTimerRecord", 17U,
                                                  n3::live<&init_timer_record<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00283510, init_timer_record)
