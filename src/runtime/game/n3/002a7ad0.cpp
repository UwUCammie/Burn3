// 0x002a7ad0 (a0 = owner A, a1 = flag, a2 = record R): InitInlineEntryTables. Returns nothing (v0 is the callee's).
//   flag != 0: R[104] = 1, then the cold call 0x002ad6e0 with a0 = R.
//   flag == 0: R[560] = 0x002a77b0 and R[564] = R. Then for i < R[484] (unsigned), with e = R[548] + 20*i and
//   x = R[544] + 16*i: e[0] = 0, e[4] = R, e[16] = R+560, e[8] = *(*(*(A+292)+24)+32), e[12] = x, x[4] = 3, x[12] = 2.
//   Then the cold call 0x002ad2c0 (a0 = R+552, a1 = R[548], a2 = R[484]), then the cold call 0x002ad6e0 (a0 = R).
// Both calls never ran in the recorded game, and every path reaches 0x002ad6e0 (cold-decline). call_cost therefore
// returns 0 on every input and the original runs every call. The implementation makes the stores before the first
// cold call and then throws, because a cold call cannot run natively.

#include "takedown/runtime/game/n3_support.hpp"

#include <stdexcept>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

// Declared cost: the loop count is data dependent (the same ceiling as the other loop natives).
constexpr std::uint32_t kCeiling = 20000U;

constexpr GuestAddress kFlagSlot = 104U;          // R + 104
constexpr GuestAddress kCountSlot = 484U;         // R + 484: count of 20-byte entries (unsigned)
constexpr GuestAddress kTableSlot = 544U;         // R + 544: base of the 16-byte table
constexpr GuestAddress kEntrySlot = 548U;         // R + 548: base of the 20-byte table
constexpr GuestAddress kConstantSlot = 560U;      // R + 560: holds kConstant, and e[16] points here
constexpr GuestAddress kRecordSlot = 564U;        // R + 564: holds R
constexpr std::uint32_t kConstant = 0x002a77b0U;  // lui 0x2a, +30640
constexpr GuestAddress kOwnerChain = 292U;        // A + 292 -> P; P + 24 -> Q; Q + 32 -> the word stored at e[8]
constexpr std::uint32_t kEntryStride = 20U;
constexpr std::uint32_t kTableStride = 16U;

// call_cost: 0 on every input. Every path reaches a cold call, so the original runs every call and this native never
// completes one. The answer does not depend on guest memory, so call_cost reads none of it.
std::uint32_t init_inline_entry_tables_cost(const auto&) { return 0U; }

// Runs only if call_cost returned nonzero, which it never does. It makes the original's stores up to the first cold
// call, then refuses: a cold call cannot run natively, and a partial result must not pass for the whole function.
template <class Context>
n3::Run init_inline_entry_tables(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress owner = call.arg(0);
    const std::uint32_t flag = call.arg(1);
    const GuestAddress record = call.arg(2);
    if (flag != 0U) {
        m.write_u32_le(record + kFlagSlot, 1U);
    } else {
        const GuestAddress table = m.read_u32_le(record + kTableSlot);
        const std::uint32_t count = m.read_u32_le(record + kCountSlot);
        const GuestAddress entries = m.read_u32_le(record + kEntrySlot);
        m.write_u32_le(record + kConstantSlot, kConstant);
        m.write_u32_le(record + kRecordSlot, record);
        for (std::uint32_t i = 0U; i < count; ++i) {
            const GuestAddress entry = entries + kEntryStride * i;
            const GuestAddress slot = table + kTableStride * i;
            m.write_u32_le(entry, 0U);
            m.write_u32_le(entry + 4U, record);
            m.write_u32_le(entry + 16U, record + kConstantSlot);
            // Three dependent loads, made after the stores above as the original makes them.
            const GuestAddress holder = m.read_u32_le(owner + kOwnerChain);
            const GuestAddress target = m.read_u32_le(holder + 24U);
            const std::uint32_t word = m.read_u32_le(target + 32U);
            m.write_u32_le(entry + 8U, word);
            m.write_u32_le(entry + 12U, slot);
            m.write_u32_le(slot + 4U, 3U);
            m.write_u32_le(slot + 12U, 2U);
        }
    }
    throw std::logic_error("InitInlineEntryTables: a cold call (0x002ad2c0 or 0x002ad6e0) cannot run natively");
}

} // namespace

extern constinit const EngineFunction n3_002a7ad0{0x002a7ad0U, 196U, 0x6af24d09801a1b73ULL, "InitInlineEntryTables",
                                                  kCeiling, n3::live<&init_inline_entry_tables<NativeFunctionContext>>, EngineReturn::Nothing, {}, 1e-4,
                                                  1e-6, {}, init_inline_entry_tables_cost};

} // namespace takedown::runtime

N3_BODY_PLAIN(002a7ad0, init_inline_entry_tables)
