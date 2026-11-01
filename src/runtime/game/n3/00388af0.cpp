// 0x00388af0 (StoreSlotRecord, leaf, 64 bytes): writes slot a1 of the 20-byte
// slot array that starts 7172 bytes into the object at a0. The first two words
// of each slot are raw copies of the 8-byte entry for slot a1 in the read-only
// table at 0x01ea4b00 (no float arithmetic); then come a2 and the raw bits of
// f12, twice. f0 is left holding the second copied table word, as the original
// leaves it. Callers read nothing back.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kTable = 0x01ea4b00U;        // lui 0x01ea, +19200
constexpr GuestAddress kSlotArrayOffset = 7172U;

template <class Context>
n3::Run store_slot_record(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const std::uint32_t slot = call.arg(1);
    const std::uint32_t word = call.arg(2);
    const std::uint32_t f12 = call.cpu().read_fpr_bits(12U);

    // a1 * 20 as the original computes it, ((a1 << 2) + a1) << 2, all 32-bit.
    const GuestAddress source = kTable + (slot << 3U);
    const GuestAddress destination = object + (((slot << 2U) + slot) << 2U) + kSlotArrayOffset;

    // Same order as the original: the second table word is read after the first
    // destination word is written, so overlapping ranges behave identically.
    const std::uint32_t first = m.read_u32_le(source);
    m.write_u32_le(destination, first);
    const std::uint32_t second = m.read_u32_le(source + 4U);
    m.write_u32_le(destination + 4U, second);
    m.write_u32_le(destination + 8U, word);
    m.write_u32_le(destination + 12U, f12);
    m.write_u32_le(destination + 16U, f12);
    call.cpu().write_fpr_bits(0U, second);
    return call.finish(16U);
}

} // namespace

extern constinit const EngineFunction n3_00388af0{0x00388af0U, 64U, 0xc38dc332357cc6c5ULL, "StoreSlotRecord", 16U,
                                           n3::live<&store_slot_record<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00388af0, store_slot_record)
