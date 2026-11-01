// 0x00254970 (a0 object): queues the object's payloads into draw bucket N of the table at 0x01d9d010
// (16 bytes a bucket: owner at +0, payload slots at +4 and +8, count at +12). Nothing is stored when the
// byte at *(object+228)+93 is clear and the skip byte at $gp-23692 is set. Otherwise the bucket's count
// is cleared, the payload at object+232 is pushed when the word at object+272 is set, and the payload at
// object+276 is pushed when the enable byte at $gp-23696 and the word at object+316 are set. A push stores
// the new count, then the payload pointer, then the running total at $gp-23708 (+22). When the bucket
// then holds a payload, the owner is stored and N at $gp-23712 advances by one. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kBucketTable = 0x01d9d010U;   // lui 0x01da, addiu -12272
constexpr GuestAddress kBucketsUsed = 0xFFFFA360U;   // -23712 from $gp (N)
constexpr GuestAddress kRunningTotal = 0xFFFFA364U;  // -23708 from $gp
constexpr GuestAddress kSecondEnable = 0xFFFFA370U;  // -23696 from $gp (byte)
constexpr GuestAddress kSkipFlag = 0xFFFFA374U;      // -23692 from $gp (byte)

// The payload push (0x002549c8 and the 0x00254a04 entry): the old count is read, then the running total,
// then the count, the payload pointer and the total are stored in that order. The pointer lands in the
// slot after the old count, so it may overwrite the count word; the caller re-reads the count afterwards.
template <class Memory>
void push_payload(Memory& m, const GuestAddress gp, const GuestAddress bucket, const GuestAddress payload) {
    const std::uint32_t old_count = m.read_u32_le(bucket + 12U);
    const std::uint32_t total = m.read_u32_le(gp + kRunningTotal);
    m.write_u32_le(bucket + 12U, old_count + 1U);
    m.write_u32_le(bucket + 4U + (old_count << 2U), payload);
    m.write_u32_le(gp + kRunningTotal, total + 22U);
}

template <class Context>
n3::Run queue_draw_bucket(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress object = call.arg(0);
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);

    // lw, lbu, bne and its delay slot (4); when the flag byte is clear, lbu, bne, nop (3) and, if the
    // skip byte is set, the return (jr and its delay slot, 2).
    std::uint32_t retired = 4U;
    if (m.read_u8(m.read_u32_le(object + 228U) + 93U) == 0U) {
        retired += 3U;
        if (m.read_u8(gp + kSkipFlag) != 0U) {
            return call.finish(retired + 2U);
        }
    }

    // lw N, lui, addiu, sll, addu, sw (count cleared), lw (272), bne and its delay slot (9).
    const GuestAddress bucket = kBucketTable + (m.read_u32_le(gp + kBucketsUsed) << 4U);
    m.write_u32_le(bucket + 12U, 0U);
    retired += 9U;
    if (m.read_u32_le(object + 272U) != 0U) {
        push_payload(m, gp, bucket, object + 232U);
        retired += 11U;  // the push, its beq back to the enable test, and the beq's delay slot
    }

    // lbu, beq, nop (3); when enabled, lw (316), beq, nop (3) and the second push (10) if it is set.
    retired += 3U;
    if (m.read_u8(gp + kSecondEnable) != 0U) {
        retired += 3U;
        if (m.read_u32_le(object + 316U) != 0U) {
            push_payload(m, gp, bucket, object + 276U);
            retired += 10U;
        }
    }

    // lw the count again (the pushes may have changed it), bne, nop (3).
    retired += 3U;
    if (m.read_u32_le(bucket + 12U) != 0U) {
        // lw N, sw owner, addiu, beq, sw N (5): N is read before the owner store, as the original does.
        retired += 5U;
        const std::uint32_t buckets_used = m.read_u32_le(gp + kBucketsUsed);
        m.write_u32_le(bucket, object);
        m.write_u32_le(gp + kBucketsUsed, buckets_used + 1U);
    }
    // beq and its delay slot (2) into the return, then jr and its delay slot (2).
    retired += 4U;
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_00254970{0x00254970U, 220U, 0x659776d23ec0219bULL, "QueueDrawBucket", 55U,
                                                 n3::live<&queue_draw_bucket<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(00254970, queue_draw_bucket)
