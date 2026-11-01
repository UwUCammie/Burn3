// 0x00137a40 (IsAdjustedStatAtLeastLimit, leaf): a0 is a stats object (obj) and a1 a stat index k. Returns v0 = 1 when the
// stat's adjusted value is not below its limit (signed compare), else 0. The two callees are six-instruction table reads,
// inlined exactly and nothing is stored: idx = u32 at 0x004ce7a0 + 4*k (callee 0x003f5f90) and limit = u32 at 0x004ce7f0 + 4*k
// (callee 0x003f5f70). For idx below 15 the jump table at 0x004b5d90 (entry i = case block 0x00137a9c + 24*i, read from the ELF)
// selects the value: cases 0..13 take a field of obj floored at a global (unsigned max), case 14 the sum of two globals and
// u32(obj + 512) with 32-bit wrap; idx 15 or more leaves the value at 0. Retired counts from the listing with both callees
// counted, delay slots included: 41 outside the switch, 54 inside it, one more when the result is 1; the declared cost is the
// longest path, 55. The registers the original leaves changed are reproduced: a0 = a1 (daddu in the second jal's delay slot),
// v1 = 4*k (sll in callee 0x003f5f70), at = the last compare (sltu in a field case, 1 in case 14, 0 outside the switch).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// The two word tables the inlined callees read, one word per stat index k.
constexpr GuestAddress kIndexTable = 0x004ce7a0U;    // callee 0x003f5f90: lui 0x4d, addiu -6240
constexpr GuestAddress kLimitTable = 0x004ce7f0U;    // callee 0x003f5f70: lui 0x4d, addiu -6160
constexpr GuestAddress kCase14Global = 0x0064e4d8U;  // case 14: lui 0x65, addiu -11344, lw 4392
constexpr GuestAddress kCase14Offset = 0x0051c124U;  // case 14: lui 0x52, lw -16092
constexpr GuestAddress kCase14Field = 512U;          // case 14: lw 512($s3)

// idx below kSwitchCount selects a case. idx kDefaultIndex is case 14; the others (0..13) read one field of obj and take
// the unsigned max of it and a floor global.
constexpr std::uint32_t kSwitchCount = 15U;
constexpr std::uint32_t kDefaultIndex = 14U;
constexpr std::array<GuestAddress, 14> kFieldOffsets{856U, 836U, 848U, 840U, 844U, 852U, 872U,
                                                     876U, 880U, 884U, 860U, 888U, 892U, 864U};
constexpr std::array<GuestAddress, 14> kFloors{0x0051c638U, 0x0051c624U, 0x0051c630U, 0x0051c628U,
                                               0x0051c62cU, 0x0051c634U, 0x0051c648U, 0x0051c64cU,
                                               0x0051c650U, 0x0051c654U, 0x0051c63cU, 0x0051c658U,
                                               0x0051c65cU, 0x0051c640U};

// Retired instruction counts from the listing with both callees counted (six each), delay slots included.
constexpr std::uint32_t kHeadCost = 22U;     // 0x00137a40-0x00137a7c (16) and callee 0x003f5f90 (6)
constexpr std::uint32_t kDispatchCost = 7U;  // 0x00137a80-0x00137a98: lui, sll, addiu, addu, lw, jr and its delay slot
constexpr std::uint32_t kCaseCost = 6U;      // one case block (the same length for all 15 cases)
constexpr std::uint32_t kLimitCost = 11U;    // 0x00137c04-0x00137c14 (5) and callee 0x003f5f70 (6)
constexpr std::uint32_t kTailCost = 8U;      // 0x00137c1c-0x00137c38: result copy, restores, jr and its delay slot
constexpr std::uint32_t kAboveCost = 1U;     // daddiu at 0x00137c18, only when the result is 1

template <class Context>
n3::Run is_adjusted_stat_at_least_limit(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress obj = call.arg(0);
    const std::uint32_t k = call.arg(1);

    // Callee 0x003f5f90, inlined: the index that selects the case.
    const std::uint32_t idx = m.read_u32_le(kIndexTable + (k << 2U));
    std::uint32_t value = 0U;
    std::uint32_t at = 0U;
    std::uint32_t retired = kHeadCost;
    if (idx < kSwitchCount) {
        retired += kDispatchCost + kCaseCost;
        if (idx == kDefaultIndex) {
            value = m.read_u32_le(kCase14Global) + (m.read_u32_le(kCase14Offset) + m.read_u32_le(obj + kCase14Field));
            at = 1U;
        } else {
            const std::uint32_t field = m.read_u32_le(obj + kFieldOffsets[idx]);
            const std::uint32_t floor_value = m.read_u32_le(kFloors[idx]);
            at = field < floor_value ? 1U : 0U;
            value = at != 0U ? floor_value : field;
        }
    }

    // Callee 0x003f5f70, inlined: the limit for k.
    const std::uint32_t limit = m.read_u32_le(kLimitTable + (k << 2U));
    retired += kLimitCost + kTailCost;
    const bool at_least = static_cast<std::int32_t>(value) >= static_cast<std::int32_t>(limit);
    if (at_least) {
        retired += kAboveCost;
    }

    call.cpu().write_gpr_u64(4U, call.arg64(1));
    call.cpu().write_gpr_sign_extended_32(3U, k << 2U);
    call.cpu().write_gpr_sign_extended_32(1U, at);
    call.return_value(at_least ? 1U : 0U);
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_00137a40{0x00137a40U, 508U, 0x56fd0b380555e4afULL,
                                                  "IsAdjustedStatAtLeastLimit", 55U,
                                                  n3::live<&is_adjusted_stat_at_least_limit<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00137a40, is_adjusted_stat_at_least_limit)
