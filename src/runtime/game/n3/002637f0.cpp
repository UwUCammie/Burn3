// 0x002637f0 (a0 = one 168-byte entry of the six-entry queue; the caller loops over the six): QueueOverLimitObject.
// The entry's +152 word points at a sub-record whose floats at +64, +68, +72 and +76 are chained through MAX.S
// (in the listing's order) into one maximum m, and f1 = G - m with G the float at 0x00522680. a1 is set when the
// entry's +68 float is above f1. v1 is the same test on the +144 float, made only when the gp byte at -23652 is set.
// The byte v1 is always stored to the entry's +164 byte. The entry is queued when a1 is set or the stored byte is
// set: the count at gp-23664 is bumped first, the entry is written to 0x01e327b0 + 4 * count (no bounds check),
// and the sub-record's +92 float is folded into the running maximum at gp-23656. The caller ignores v0 and f0,
// so the native returns nothing.
//
// Declared cost 49 = the longest structural path (bin/brief.py). The feasible maximum is 48: the 49-instruction
// listing path would need the stored byte to be clear and set at once. A larger ceiling is safe; the exact count
// is what a call reports.

#include "takedown/runtime/game/n3_support.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kLimitFloat = 0x00522680U;  // lui 0x52, 9856: the float G
constexpr GuestAddress kQueueBase = 0x01e327b0U;   // lui 0x01e3, 10160: an address, not a read

// MAX.S on the raw sign/magnitude patterns (the same ordering as guest::ee_cop1::max_s; see n3/0030b190.cpp).
std::uint32_t ee_max(const std::uint32_t a, const std::uint32_t b) {
    const auto left = std::bit_cast<std::int32_t>(a);
    const auto right = std::bit_cast<std::int32_t>(b);
    return std::bit_cast<std::uint32_t>(left < 0 && right < 0 ? std::min(left, right) : std::max(left, right));
}

template <class Context>
n3::Run queue_over_limit_object(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress entry = call.arg(0);
    const std::uint32_t gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);

    // B1: 15 retired when the bc1f branch is taken, 16 when it falls through to the daddu.
    const GuestAddress record = m.read_u32_le(entry + 152U);
    std::uint32_t f1 = m.read_u32_le(record + 68U);
    std::uint32_t f0 = m.read_u32_le(record + 64U);
    f1 = ee_max(f0, f1);
    f0 = m.read_u32_le(record + 72U);
    f1 = ee_max(f0, f1);
    f0 = m.read_u32_le(record + 76U);
    const std::uint32_t top = ee_max(f0, f1);
    f1 = m.read_u32_le(kLimitFloat);
    f0 = m.read_u32_le(entry + 68U);
    f1 = fpu::sub(f1, top);
    std::uint32_t retired = 15U;
    std::uint32_t v1 = 1U;  // the addiu in the bc1f delay slot, which runs on both paths
    if (fpu::le(f0, f1)) {
        retired = 16U;  // daddu v1, zero, zero on the fall-through
        v1 = 0U;
    }
    const std::uint32_t a1 = v1 & 0xffU;

    // B2 (5): andi, lbu the gp byte, sltu v1 = (byte != 0), beq (taken when the byte is clear) and its nop.
    retired += 5U;
    v1 = m.read_u8(gp - 23652U) != 0U ? 1U : 0U;
    if (v1 != 0U) {
        // B4: 4 retired when the bc1f branch is taken (v1 = 1), 5 when it falls through to the daddu (v1 = 0).
        f0 = m.read_u32_le(entry + 144U);
        if (!fpu::le(f0, f1)) {
            v1 = 1U;
            retired += 4U;
        } else {
            v1 = 0U;
            retired += 5U;
        }
    }

    // B3 (2): bne a1 (taken: queue). Its delay slot, the store of v1 to the entry's +164 byte, always runs.
    retired += 2U;
    m.write_u8(entry + 164U, static_cast<std::uint8_t>(v1));
    if (a1 == 0U) {
        // B5 (3): lbu the byte just stored, then bne (taken when it is set: queue).
        retired += 3U;
        if (m.read_u8(entry + 164U) == 0U) {
            // R1 (beq and its nop) and R2 (jr and its nop).
            retired += 4U;
            return call.finish(retired);
        }
    }

    // P (14): the swc1 at the end is the delay slot of the final beq, which goes on to R1 and R2.
    retired += 14U;
    const std::uint32_t count = m.read_u32_le(gp - 23664U);
    f0 = m.read_u32_le(gp - 23656U);
    const GuestAddress slot = kQueueBase + (count << 2U);
    m.write_u32_le(gp - 23664U, count + 1U);
    m.write_u32_le(slot, entry);
    const GuestAddress record_again = m.read_u32_le(entry + 152U);
    f1 = m.read_u32_le(record_again + 92U);
    f0 = ee_max(f0, f1);
    m.write_u32_le(gp - 23656U, f0);
    retired += 4U;  // R1 and R2
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_002637f0{0x002637f0U, 196U, 0xe9d41224de91bc35ULL, "QueueOverLimitObject", 49U,
                                                  n3::live<&queue_over_limit_object<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(002637f0, queue_over_limit_object)
