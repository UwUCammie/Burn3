// 0x00307440 (a0 = entry record): FlushQuadEntryRange. The record holds a cursor (u32 at +32) and an end (u32 at
// +36) bounding a run of 32-byte entries, a halfword at +40, a table of halfword slots from +42 (slot i at +42 + 2*i)
// and a signed index byte at +48. The run's length in entries, (end - cursor) / 32 rounded toward zero, is taken as
// a halfword and subtracted from the halfword at +40; the difference is stored in the slot the index selects. The
// index is then read again (the store can have changed it) and, when the slot it selects is nonzero and the cursor
// is below the end, every quadword from the cursor up to the end goes through VU0: vsubw.w sets its w lane to +0,
// vaddx.w adds vf2.x (the low word of the qmtc2 source, which the daddu cleared), and sqc2 stores it back, the cursor
// advancing 32 bytes per quadword. Then the cursor and end are zeroed, the index byte becomes -1 and v1 holds -1.
// Returns nothing.
//
// The loop count is data dependent, so the exact count comes from a dry run (call_cost) on n3::DryMemory; above
// kCeiling the original runs.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kCursor = 32U;        // u32 cursor
constexpr GuestAddress kEnd = 36U;           // u32 end
constexpr GuestAddress kHead = 40U;          // halfword the stored difference is measured from
constexpr GuestAddress kSlots = 42U;         // halfword slot i at +42 + 2*i
constexpr GuestAddress kIndex = 48U;         // signed index byte
constexpr std::uint8_t kExitIndex = 0xFFU;   // the index byte the exit leaves behind (-1)

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kPrefixCost = 5U;    // 0x307440..0x307450: two loads, subu, bgez and its delay slot
constexpr std::uint32_t kNegativeCost = 2U;  // 0x307454..0x307458: the rounding of a negative difference
constexpr std::uint32_t kSlotCost = 14U;     // 0x30745c..0x307490: count, store, read back, zero test
constexpr std::uint32_t kRangeCost = 5U;     // 0x307494..0x3074a4: cursor against end
constexpr std::uint32_t kEnterCost = 3U;     // 0x3074a8..0x3074b0: daddu, the jump and the qmtc2
constexpr std::uint32_t kCheckCost = 5U;     // 0x3074d0..0x3074e0: the loop test
constexpr std::uint32_t kPassCost = 7U;      // 0x3074b4..0x3074cc: one quadword and the cursor step
constexpr std::uint32_t kExitCost = 5U;      // 0x3074e4..0x3074f4: the clears and the jr's delay slot

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a
// call reports is always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// The interpreter's access checks, made by the dry run before it touches memory. An access the original would fault
// on makes call_cost decline, so the original runs and faults itself. The real run never sees one.
struct Fault {};

template <class Base>
class DryEnv {
public:
    explicit DryEnv(const Base& memory) noexcept : memory_(memory) {}
    void word(const GuestAddress address) const {
        if ((address & 3U) != 0U || !memory_.contains(address, 4U)) {
            throw Fault{};
        }
    }
    void half(const GuestAddress address) const {
        if ((address & 1U) != 0U || !memory_.contains(address, 2U)) {
            throw Fault{};
        }
    }
    void byte(const GuestAddress address) const {
        if (!memory_.contains(address, 1U)) {
            throw Fault{};
        }
    }
    void quad(const GuestAddress address) const {
        if ((address & 15U) != 0U || !memory_.contains(address, 16U)) {
            throw Fault{};
        }
    }
    void fmac(const std::uint16_t) noexcept {}
    void vf1(const guest::Gpr128&) noexcept {}
    void vf2(const guest::Gpr128&) noexcept {}

private:
    const Base& memory_;
};

class RealEnv {
public:
    explicit RealEnv(guest::R5900State& cpu) noexcept : cpu_(cpu) {}
    void word(const GuestAddress) const noexcept {}
    void half(const GuestAddress) const noexcept {}
    void byte(const GuestAddress) const noexcept {}
    void quad(const GuestAddress) const noexcept {}
    void fmac(const std::uint16_t mac_flags) { guest::publish_vu0_macro_fmac_result(cpu_, mac_flags); }
    void vf1(const guest::Gpr128& value) { cpu_.write_vu0_vector(1U, value); }
    void vf2(const guest::Gpr128& value) { cpu_.write_vu0_vector(2U, value); }

private:
    guest::R5900State& cpu_;
};

// The address of slot `index`: sll by 1, addu with the record, and the 42-byte table offset of the sh and lh.
GuestAddress slot_address(const GuestAddress record, const std::int8_t index) {
    return record + kSlots + 2U * static_cast<std::uint32_t>(static_cast<std::int32_t>(index));
}

// lqc2 vf1,0(a1); vsubw.w vf1,vf0,vf0w; vaddx.w vf1,vf1,vf2x; sqc2 vf1,0(a1). Both VU0 ops are the interpreter's own
// FMAC kernel, and each publishes its MAC/STATUS flags in order through Env::fmac.
template <class Memory, class Env>
void quad_pass(Memory& m, Env& env, const GuestAddress address, const guest::Gpr128& vf2) {
    env.quad(address);
    const guest::Gpr128 loaded = n3::load_quad_any(m, address);
    const auto zeroed =
        guest::execute_vu0_macro_broadcast_add_subtract(loaded, n3::kVf0, n3::kVf0, n3::kMaskW, 3U, true);
    env.fmac(zeroed.mac_flags);
    const auto summed =
        guest::execute_vu0_macro_broadcast_add_subtract(zeroed.value, zeroed.value, vf2, n3::kMaskW, 0U, false);
    env.fmac(summed.mac_flags);
    env.vf1(summed.value);
    n3::store_quad_any(m, address, summed.value);
}

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost), returning the retired count.
// Above kCeiling the walk stops early. `entry_v1` is v1 on entry: the daddu that clears v1 touches only its low
// doubleword, so the qmtc2 copies the high doubleword the caller left there.
template <class Memory, class Env>
std::uint32_t flush_entries(Memory& m, Env& env, const GuestAddress record, const guest::Gpr128& entry_v1) {
    env.word(record + kEnd);
    env.word(record + kCursor);
    env.half(record + kHead);
    env.byte(record + kIndex);
    const auto difference = static_cast<std::int32_t>(m.read_u32_le(record + kEnd) - m.read_u32_le(record + kCursor));
    std::uint32_t retired = kPrefixCost;
    std::int32_t entries = difference >> 5;
    if (difference < 0) {
        retired += kNegativeCost;
        entries = (difference + 31) >> 5;
    }
    // dsll32/dsra32 by 16 keep the low halfword of the count, sign-extended.
    const auto count = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(entries)));
    const auto head = static_cast<std::uint32_t>(
        static_cast<std::int32_t>(static_cast<std::int16_t>(m.read_u16_le(record + kHead))));
    const std::uint32_t stored = head - count;
    const auto first_index = static_cast<std::int8_t>(m.read_u8(record + kIndex));
    env.half(slot_address(record, first_index));
    m.write_u16_le(slot_address(record, first_index), static_cast<std::uint16_t>(stored));
    const auto second_index = static_cast<std::int8_t>(m.read_u8(record + kIndex));
    env.half(slot_address(record, second_index));
    const auto slot = m.read_u16_le(slot_address(record, second_index));
    retired += kSlotCost;
    if (slot != 0U) {
        retired += kRangeCost;
        if (m.read_u32_le(record + kCursor) < m.read_u32_le(record + kEnd)) {
            retired += kEnterCost;
            guest::Gpr128 vf2 = entry_v1;
            vf2.words[0] = 0U;
            vf2.words[1] = 0U;
            env.vf2(vf2);
            for (;;) {
                if (retired > kCeiling) {
                    return retired;
                }
                retired += kCheckCost;
                const GuestAddress cursor = m.read_u32_le(record + kCursor);
                if (!(cursor < m.read_u32_le(record + kEnd))) {
                    break;
                }
                retired += kPassCost;
                quad_pass(m, env, cursor, vf2);
                m.write_u32_le(record + kCursor, m.read_u32_le(record + kCursor) + 32U);
            }
        }
    }
    m.write_u32_le(record + kCursor, 0U);
    m.write_u32_le(record + kEnd, 0U);
    m.write_u8(record + kIndex, kExitIndex);
    return retired + kExitCost;
}

template <class Context>
std::uint32_t flush_quad_entry_range_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    DryEnv env{context.memory};
    try {
        const std::uint32_t retired =
            flush_entries(dry, env, context.cpu.read_gpr_u32(4U), context.cpu.read_gpr(3U));
        return retired <= kCeiling ? retired : 0U;
    } catch (const Fault&) {
        return 0U;
    }
}

template <class Context>
n3::Run flush_quad_entry_range(Context& context) {
    auto call = n3::guest_call(context);
    auto& cpu = call.cpu();
    RealEnv env{cpu};
    const std::uint32_t retired = flush_entries(call.memory(), env, call.arg(0), cpu.read_gpr(3U));
    cpu.write_gpr_sign_extended_32(3U, 0xFFFFFFFFU);  // addiu v1,zero,-1 before the jr
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_00307440{0x00307440U, 184U, 0x7e28e73c09be2b39ULL, "FlushQuadEntryRange",
                                                  kCeiling, n3::live<&flush_quad_entry_range<NativeFunctionContext>>, EngineReturn::Nothing,
                                                  {}, 1e-4, 1e-6, {}, &flush_quad_entry_range_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00307440, flush_quad_entry_range, flush_quad_entry_range_cost)
