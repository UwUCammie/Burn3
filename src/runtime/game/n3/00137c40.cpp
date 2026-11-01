// 0x00137c40 (IsStatAtLeastLimit, leaf): a0 is a stats/car object (obj) and a1 a stat index k. Both callees
// are six-instruction table reads, inlined here exactly (nothing is stored): idx = u32 at 0x004ce7a0 + 4*k
// (callee 0x003f5f90) and limit = u32 at 0x004ce7f0 + 4*k (callee 0x003f5f70). For idx below 15 the switch
// (jump table at 0x004b5dd0, entry i = case block 0x00137c94 + 8*i) reads one field of obj; idx 14 reads
// u32(obj + 512) + u32(0x0051c124), wrapping; idx 15 or more leaves the value at 0. Returns v0 = 1 when the value
// is not below the limit (signed slt compare), else 0. Retired counts from the listing with both callees counted,
// delay slots included: 39 or 40 for idx 15 or more, 48 or 49 for idx 0..13, 50 or 51 for idx 14. The declared
// cost is the longest path, 51. The registers the original leaves changed are reproduced: a0 = a1 (daddu),
// v1 = 4*k (sll in callee 0x003f5f70), at = idx < 15 (sltiu).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// The two word tables the inlined callees read, one word per stat index k.
constexpr GuestAddress kIndexTable = 0x004ce7a0U;   // callee 0x003f5f90: lui 0x4d, addiu -6240
constexpr GuestAddress kLimitTable = 0x004ce7f0U;   // callee 0x003f5f70: lui 0x4d, addiu -6160
constexpr GuestAddress kGlobal = 0x0051c124U;       // case 14: lui 0x52, lw -16092
constexpr GuestAddress kCase14Field = 512U;         // case 14: lw $v1, 512($s3)

// Index values below kSwitchCount select a field; idx i below kDefaultIndex reads obj + kFieldOffsets[i].
constexpr std::uint32_t kSwitchCount = 15U;
constexpr std::uint32_t kDefaultIndex = 14U;
constexpr std::array<GuestAddress, 14> kFieldOffsets{856U, 836U, 848U, 840U, 844U, 852U, 872U,
                                                     876U, 880U, 884U, 860U, 888U, 892U, 864U};

// Retired instruction counts from the listing with both callees counted (6 each), delay slots included.
constexpr std::uint32_t kHeadCost = 20U;        // 0x00137c40-0x00137c74 (14) and callee 0x003f5f90 (6)
constexpr std::uint32_t kDispatchCost = 7U;     // lui, sll, addiu, addu, lw, jr and its delay slot
constexpr std::uint32_t kFieldCaseCost = 2U;    // beq and its delay slot (idx 0..13)
constexpr std::uint32_t kDefaultCaseCost = 4U;  // lui, lw, lw, addu (idx 14)
constexpr std::uint32_t kLimitTestCost = 19U;   // jal, delay slot, slt, bne, nop (5), callee 0x003f5f70 (6), final 8
constexpr std::uint32_t kAboveCost = 1U;        // daddiu, only when the result is 1

template <class Context>
n3::Run is_stat_at_least_limit(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress obj = call.arg(0);
    const std::uint32_t k = call.arg(1);

    // Callee 0x003f5f90, inlined: the index that selects the field.
    const std::uint32_t idx = m.read_u32_le(kIndexTable + (k << 2U));
    const bool in_switch = idx < kSwitchCount;
    std::uint32_t value = 0U;
    std::uint32_t retired = kHeadCost;
    if (in_switch) {
        retired += kDispatchCost;
        if (idx == kDefaultIndex) {
            value = m.read_u32_le(obj + kCase14Field) + m.read_u32_le(kGlobal);
            retired += kDefaultCaseCost;
        } else {
            value = m.read_u32_le(obj + kFieldOffsets[idx]);
            retired += kFieldCaseCost;
        }
    }

    // Callee 0x003f5f70, inlined: the limit for k.
    const std::uint32_t limit = m.read_u32_le(kLimitTable + (k << 2U));
    retired += kLimitTestCost;
    const bool at_least = static_cast<std::int32_t>(value) >= static_cast<std::int32_t>(limit);
    if (at_least) {
        retired += kAboveCost;
    }

    call.cpu().write_gpr_u64(4U, call.arg64(1));
    call.cpu().write_gpr_sign_extended_32(3U, k << 2U);
    call.cpu().write_gpr_sign_extended_32(1U, in_switch ? 1U : 0U);
    call.return_value(at_least ? 1U : 0U);
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_00137c40{0x00137c40U, 268U, 0x435848551b9c09b4ULL, "IsStatAtLeastLimit", 51U,
                                                  n3::live<&is_stat_at_least_limit<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(00137c40, is_stat_at_least_limit)
