// 0x001ab090 (a0 owner, a1 source record): appends one record to the owner's
// fixed-capacity queue (96 rows). The queue header at owner+512 is
// {s32 count @0, ptr rows @4 (64-byte stride), ptr ids @8 (u16 each)}. The new
// row takes the record's quadwords at +32, +48, +64, +16 (stored to row +0, +16,
// +32, +48), and its id is the u16 at +4 of the pointer stored at record+96.
// A nonzero byte at $gp-24428 is a sticky overflow flag: while it is set every
// push is refused without reading the queue, and a refusal sets it again.
// Returns v0 = 1 when the record was appended, 0 when it was refused.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kQueueOffset = 512U;   // owner + 512: queue header pointer
constexpr std::int32_t kFlagOffset = -24428;  // $gp - 24428: sticky overflow byte
constexpr std::int32_t kCapacity = 96;

template <class Context>
n3::Run queue_push_record(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress owner = call.arg(0);
    const GuestAddress record = call.arg(1);
    const GuestAddress flag = call.cpu().read_gpr_u32(n3::kGlobalPointer) + static_cast<GuestAddress>(kFlagOffset);

    if (m.read_u8(flag) != 0U) {
        m.write_u8(flag, 1U);
        call.return_value(0U);
        return call.finish(8U);
    }
    const GuestAddress queue = m.read_u32_le(owner + kQueueOffset);
    const std::uint32_t count = m.read_u32_le(queue);
    if (static_cast<std::int32_t>(count) >= kCapacity) {
        m.write_u8(flag, 1U);
        call.return_value(0U);
        return call.finish(13U);
    }

    // The row and id slots are indexed by the count read above (a negative
    // count wraps exactly as the original's 32-bit shifts and adds do).
    const GuestAddress rows = m.read_u32_le(queue + 4U);
    const std::uint32_t row_offset = count << 6U;
    const std::uint32_t id_offset = count << 1U;
    const std::uint32_t next_count = count + 1U;
    const auto from_32 = n3::load_quad_any(m, record + 32U);
    const auto from_48 = n3::load_quad_any(m, record + 48U);
    const auto from_64 = n3::load_quad_any(m, record + 64U);
    const auto from_16 = n3::load_quad_any(m, record + 16U);
    const GuestAddress id_record = m.read_u32_le(record + 96U);
    const GuestAddress row = rows + row_offset;
    n3::store_quad_any(m, row, from_32);
    n3::store_quad_any(m, row + 16U, from_48);
    n3::store_quad_any(m, row + 32U, from_64);
    n3::store_quad_any(m, row + 48U, from_16);

    // The id is read after the row stores. The queue header is read again before
    // the id store and before the count store, as the original does, in case a
    // store lands on it.
    const std::uint16_t id = m.read_u16_le(id_record + 4U);
    const GuestAddress ids_queue = m.read_u32_le(owner + kQueueOffset);
    const GuestAddress ids = m.read_u32_le(ids_queue + 8U);
    m.write_u16_le(ids + id_offset, id);
    const GuestAddress count_queue = m.read_u32_le(owner + kQueueOffset);
    m.write_u32_le(count_queue, next_count);
    call.return_value(1U);
    return call.finish(33U);
}

} // namespace

extern constinit const EngineFunction n3_001ab090{0x001ab090U, 144U, 0xa0c3b4ff8bf8b455ULL, "QueuePushRecord", 33U,
                                           n3::live<&queue_push_record<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(001ab090, queue_push_record)
