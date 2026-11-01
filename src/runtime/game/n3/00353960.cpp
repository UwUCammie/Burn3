// 0x00353960 (a0 = table T): the address of record n, where n is the word at T + 4100. The records are 112 bytes
// each and start at T + 736, so the result is T + 736 + 112 * n. The original forms 112 * n as ((n << 3) - n) << 4,
// which is the same 32-bit product. Leaf function: 7 retired instructions (lw, sll, subu, sll, addu, jr, and the
// addiu in the delay slot of jr).

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kIndexOffset = 4100U;
constexpr GuestAddress kRecordsOffset = 736U;
constexpr std::uint32_t kRecordBytes = 112U;
constexpr std::uint32_t kCost = 7U;

template <class Context>
n3::Run record_address_at_index(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress table = call.arg(0);
    const std::uint32_t index = call.memory().read_u32_le(table + kIndexOffset);
    call.return_value(table + kRecordsOffset + kRecordBytes * index);
    return call.finish(kCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(00353960, 28U, 0x59dfdde3baee24b6ULL, "RecordAddressAtIndex", 7U, record_address_at_index,
               EngineReturn::Integer)
