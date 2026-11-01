// 0x00375840 (a0 = the trail world W, a1 = the trail emitter E, a2 = the point quad, a3 = the up quad, t0 = the trail
// type, t1 = a 4x4 matrix or 0): UpdateTrailEmitter, the caller of AppendTrailPoint (0x003755e0).
//
// E keeps its trail object at +16 and the time of that trail's last point (float) at +20; -1.0 there means idle. While
// the trail is alive (now <= last time + 1.5 times the float at 0x0052267c), has type t0 and its buffer (the object's
// +1092) is not full, the point goes to it. Otherwise a free object is taken from the pool (W+0x8a00 is the free list,
// W+0x8a04 its count) and enrolled as the newest object of type t0 (W + 136 t0: its count at +0x8b0c, its pointers from
// +0x8a8c). An old trail that is still within its time and whose other buffer (+1088) holds 16 points continues in the
// new object with that buffer's last point and normal. With an empty pool nothing else happens and v0 is unchanged.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(003755e0)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using guest::Gpr128;
namespace fpu = n3::fpu;

constexpr std::uint32_t kCeiling = 20000U;
constexpr std::uint32_t kFrameBytes = 80U;
constexpr GuestAddress kPointSlot = 64U;  // frame: the point quad, the callee's a1
constexpr GuestAddress kUpSlot = 48U;     // frame: the up quad, the callee's a2

constexpr GuestAddress kNow = 0x00522680U;      // float: the current time
constexpr GuestAddress kSpacing = 0x0052267cU;  // float: a trail lives 1.5 of it past its last point
constexpr std::uint32_t kOneAndHalf = 0x3fc00000U;
constexpr std::uint32_t kMinusOne = 0xbf800000U;
constexpr GuestAddress kTypeTable = 0x01ea4910U;  // 64 bytes per trail type
constexpr GuestAddress kPoolList = 0x8a00U;       // world: the free objects (an array of pointers)
constexpr GuestAddress kPoolCount = 0x8a04U;      // world: how many of them are free
constexpr std::uint32_t kGroupStride = 136U;      // world + 136 * type: the objects of that type
constexpr GuestAddress kGroupSlots = 0x8a8cU;     // group: 32 object pointers
constexpr GuestAddress kGroupCount = 0x8b0cU;     // group: how many slots are in use
constexpr std::uint32_t kRandomA = static_cast<std::uint32_t>(-22560);  // $gp offsets of the two-word random state
constexpr std::uint32_t kRandomB = static_cast<std::uint32_t>(-22556);

constexpr GuestAddress kTrail = 16U;     // emitter: the current trail object
constexpr GuestAddress kLastTime = 20U;  // emitter: the time of the trail's last point (float bits)

constexpr GuestAddress kPhase = 0U;           // object: 0..3 from the random state
constexpr GuestAddress kLength = 4U;          // object: the trail length (float bits)
constexpr GuestAddress kType = 8U;            // object: the trail type
constexpr GuestAddress kTable = 12U;          // object: the type's table entry
constexpr GuestAddress kFlags = 16U;          // object: halfword flags (bit 1: a continuation)
constexpr GuestAddress kFirstBuffer = 32U;    // object: the buffer that +1088 points to (count, then 16 points)
constexpr GuestAddress kSecondBuffer = 560U;  // object: the buffer that +1092 points to
constexpr GuestAddress kOtherPointer = 1088U;
constexpr GuestAddress kBufferPointer = 1092U;  // the buffer AppendTrailPoint appends to

// Retired instruction counts of the original's blocks, delay slots included (listing addresses in brackets).
constexpr std::uint32_t kEntryCost = 14U;      // [0x375840..0x375874] the quads, the time and the idle test
constexpr std::uint32_t kTimeCost = 11U;       // [0x375878..0x3758a0] the time test
constexpr std::uint32_t kTypeCost = 4U;        // [0x3758a4..0x3758b0] the trail's type
constexpr std::uint32_t kFullCost = 5U;        // [0x3758b4..0x3758c4] the trail's buffer count
constexpr std::uint32_t kCallCost = 8U;        // [0x3758c8..0x3758e4] the call and the jump to the epilogue
constexpr std::uint32_t kPoolCost = 5U;        // [0x3758e8..0x3758f8] the pool count test
constexpr std::uint32_t kEmptyPoolCost = 1U;   // [0x3758fc] daddu $v1, $zero, $zero
constexpr std::uint32_t kObjectTestCost = 2U;  // [0x375900..0x375904] the object test and its delay slot
constexpr std::uint32_t kAllocCost = 54U;      // [0x3759b4..0x375a88] taking the object from the pool
constexpr std::uint32_t kOldTrailCost = 3U;    // [0x375908..0x375910] the emitter's previous trail test
constexpr std::uint32_t kOldTimeCost = 11U;    // [0x375914..0x37593c] the time test on the previous trail
constexpr std::uint32_t kOldFullCost = 5U;     // [0x375940..0x375950] the previous trail's other buffer
constexpr std::uint32_t kContinueCost = 22U;   // [0x375954..0x3759a8] the continuation's first point
constexpr std::uint32_t kStoreTrailCost = 2U;  // [0x3759ac..0x3759b0] the emitter's trail becomes the object
constexpr std::uint32_t kLoadRaCost = 1U;      // [0x375a8c] ld $ra
constexpr std::uint32_t kReturnCost = 2U;      // [0x375a90..0x375a94] jr $ra and its delay slot

// The trail's limit: ADDA.S gives ACC = 0 + the last point's time, then MADD.S adds 1.5 times the spacing.
std::uint32_t trail_limit(const std::uint32_t last_time, const std::uint32_t spacing) {
    return fpu::madd(fpu::add(0U, last_time), kOneAndHalf, spacing);
}

// A 32-bit word as the register holds it after LW (sign-extended), for the comparisons the original makes on registers.
std::uint64_t loaded_word(const std::uint32_t word) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(word)));
}

// 0x3759b4..0x375a88: takes the free object at index count - 1 (the pool count goes down by one), resets it, mixes the
// two-word random state (its low two bits are the object's phase) and enrolls the object as the newest of type t0.
template <class Memory>
GuestAddress take_object(Memory& m, const GuestAddress world, const GuestAddress gp, const std::uint32_t type,
                         const std::uint32_t pool) {
    const std::uint32_t top = pool - 1U;
    m.write_u32_le(world + kPoolCount, top);
    const GuestAddress free_list = m.read_u32_le(world + kPoolList);
    const GuestAddress object = m.read_u32_le(free_list + 4U * top);
    const GuestAddress group = world + kGroupStride * type;
    m.write_u32_le(object + kLength, kMinusOne);
    m.write_u32_le(object + kTable, kTypeTable + (type << 6U));
    m.write_u16_le(object + kFlags + 2U, static_cast<std::uint16_t>(0U));
    m.write_u16_le(object + kFlags, static_cast<std::uint16_t>(0U));
    m.write_u32_le(object + kFirstBuffer, 0U);
    m.write_u32_le(object + kSecondBuffer, 0U);
    m.write_u32_le(object + kOtherPointer, object + kFirstBuffer);
    m.write_u32_le(object + kBufferPointer, object + kSecondBuffer);
    const std::uint32_t a = m.read_u32_le(gp + kRandomA);
    const std::uint32_t b = m.read_u32_le(gp + kRandomB);
    const std::uint32_t mixed = (a << 16U) + static_cast<std::uint32_t>(static_cast<std::int32_t>(a) >> 16U) + b;
    m.write_u32_le(gp + kRandomA, mixed);
    m.write_u32_le(gp + kRandomB, b + mixed);
    m.write_u32_le(object + kPhase, mixed & 3U);
    const std::uint32_t count = m.read_u32_le(group + kGroupCount);
    m.write_u32_le(group + kGroupCount, count + 1U);
    m.write_u32_le(group + kGroupSlots + 4U * count, object);
    m.write_u32_le(object + kType, type);
    return object;
}

// 0x375954..0x3759a8: the object's buffer takes point 15 and normal 15 of the previous trail's other buffer, its length
// becomes that normal's w, and flag 2 is set.
template <class Memory>
void continue_object(Memory& m, const GuestAddress object, const GuestAddress previous) {
    const GuestAddress buffer = m.read_u32_le(object + kBufferPointer);
    const Gpr128 last_point = n3::load_quad_any(m, previous + 496U);
    std::uint32_t count = m.read_u32_le(buffer);
    n3::store_quad_any(m, buffer + (count << 5U) + 16U, last_point);
    count = m.read_u32_le(buffer);
    const Gpr128 last_normal = n3::load_quad_any(m, previous + 512U);
    n3::store_quad_any(m, buffer + (count << 5U) + 32U, last_normal);
    count = m.read_u32_le(buffer);
    m.write_u32_le(buffer, count + 1U);
    const Gpr128 length_source = n3::load_quad_any(m, previous + 512U);
    m.write_u32_le(object + kLength, length_source.words[3]);
    const std::uint16_t flags = m.read_u16_le(object + kFlags);
    m.write_u16_le(object + kFlags, static_cast<std::uint16_t>(flags | 2U));
}

template <class Context>
n3::Run update_trail_emitter(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress world = call.arg(0);
    const GuestAddress emitter = call.arg(1);
    const Gpr128 point = n3::load_quad_any(m, call.arg(2));
    const Gpr128 up = n3::load_quad_any(m, call.arg(3));
    const std::uint32_t type = call.arg(4);
    const std::uint64_t type64 = call.arg64(4);
    const std::uint64_t matrix = call.arg64(5);
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress frame = call.stack_pointer() - kFrameBytes;
    const std::uint32_t now = m.read_u32_le(kNow);
    const std::uint32_t last_time = m.read_u32_le(emitter + kLastTime);
    std::uint32_t retired = kEntryCost;

    // 0x375878..0x3758c4: the current trail takes the point while it is alive, of this type and not full.
    bool extend = false;
    if (!fpu::eq(kMinusOne, last_time)) {
        retired += kTimeCost;
        if (fpu::le(now, trail_limit(last_time, m.read_u32_le(kSpacing)))) {
            retired += kTypeCost;
            const GuestAddress trail = m.read_u32_le(emitter + kTrail);
            if (type64 == loaded_word(m.read_u32_le(trail + kType))) {
                retired += kFullCost;
                const GuestAddress buffer = m.read_u32_le(trail + kBufferPointer);
                extend = m.read_u32_le(buffer) != 16U;
            }
        }
    }

    if (!extend) {
        // 0x3758e8..0x3758f8: the pool; an empty pool returns through the epilogue with v0 unchanged
        retired += kPoolCost;
        const std::uint32_t pool = m.read_u32_le(world + kPoolCount);
        if (pool == 0U) {
            return call.finish(retired + kEmptyPoolCost + kObjectTestCost + kLoadRaCost + kReturnCost);
        }
        // 0x3759b4..0x375a88: the new object
        retired += kAllocCost;
        const GuestAddress object = take_object(m, world, gp, type, pool);
        if (object == 0U) {
            return call.finish(retired + kObjectTestCost + kLoadRaCost + kReturnCost);
        }
        // 0x375908..0x375950: a previous trail still within its time, with its other buffer full, hands its last point
        // to the new object
        retired += kObjectTestCost + kOldTrailCost;
        const GuestAddress old = m.read_u32_le(emitter + kTrail);
        if (old != 0U) {
            retired += kOldTimeCost;
            if (fpu::le(now, trail_limit(last_time, m.read_u32_le(kSpacing)))) {
                retired += kOldFullCost;
                const GuestAddress previous = m.read_u32_le(old + kOtherPointer);
                if (m.read_u32_le(previous) == 16U) {
                    retired += kContinueCost;
                    continue_object(m, object, previous);
                }
            }
        }
        // 0x3759ac: the emitter's trail is the new object
        retired += kStoreTrailCost;
        m.write_u32_le(emitter + kTrail, object);
    }

    // 0x3758c8..0x3758e4: the emitter's time, the point and up quads on the frame, then AppendTrailPoint on the trail
    retired += kCallCost;
    m.write_u32_le(emitter + kLastTime, now);
    n3::store_quad_any(m, frame + kPointSlot, point);
    n3::store_quad_any(m, frame + kUpSlot, up);
    call.set_arg(0, m.read_u32_le(emitter + kTrail));
    call.set_arg(1, frame + kPointSlot);
    call.set_arg(2, frame + kUpSlot);
    call.set_arg64(3, matrix);
    const n3::Run appended = call.invoke([](auto& callee) { return n3::fn_003755e0(callee); }, kFrameBytes);
    if (appended.declined) {
        return call.decline();
    }
    return call.finish(retired + appended.retired + kReturnCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00375840, 600U, 0x402c66efdb3300f0ULL, "UpdateTrailEmitter", kCeiling, update_trail_emitter,
         EngineReturn::Integer)
