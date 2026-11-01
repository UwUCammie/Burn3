// 0x0015acc0 (a0 record A, a1 record B): ordering predicate for two ring records.
// Returns 1 (A orders before B) or 0 in v0.
//
// The whole positions (+844, signed) decide first, as a wrapping difference. A zero
// difference falls to the float fractions (+840, raw bits): an unordered or A > B pair
// gives 1, A < B gives 0, and equal fractions compare the owners' signed ranks (+6572
// of the object at +200). A non-zero difference orders A first when it is below half the
// ring count (0x0064d368, halved toward zero) if positive, or below minus that half if
// negative; when the byte at 0x0064d378 is set, the sign of the difference alone decides.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
namespace fpu = n3::fpu;

constexpr GuestAddress kRingCount = 0x0064d368U;  // lui 0x65, -11416 (word)
constexpr GuestAddress kFlag = 0x0064d378U;       // lui 0x65, -11400 (byte)
constexpr GuestAddress kFraction = 840U;          // float fraction, raw bits
constexpr GuestAddress kWhole = 844U;             // signed whole position
constexpr GuestAddress kOwner = 200U;             // pointer to the owner object
constexpr GuestAddress kRank = 6572U;             // signed rank byte in the owner

template <class Context>
n3::Run ring_position_order(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress a = call.arg(0);
    const GuestAddress b = call.arg(1);

    // lui, lw, bgez and its delay slot sra. The fall-through (negative count) also runs
    // addiu and a second sra, so half = (count + 1) >> 1 in that case.
    const auto count = static_cast<std::int32_t>(m.read_u32_le(kRingCount));
    std::uint32_t retired = 4U;
    if (count < 0) {
        retired += 2U;
    }
    const std::int32_t half = (count < 0 ? count + 1 : count) >> 1;

    // lw, lw, subu, bne and its delay slot nop.
    const std::uint32_t a_whole = m.read_u32_le(a + kWhole);
    const std::uint32_t b_whole = m.read_u32_le(b + kWhole);
    const std::uint32_t diff_bits = a_whole - b_whole;
    const auto diff = static_cast<std::int32_t>(diff_bits);
    retired += 5U;

    std::uint32_t result = 0U;
    if (diff_bits == 0U) {
        // lwc1, lwc1, c.ole.s, bc1f and its delay slot nop.
        const std::uint32_t b_fraction = m.read_u32_le(b + kFraction);
        const std::uint32_t a_fraction = m.read_u32_le(a + kFraction);
        retired += 5U;
        if (!fpu::le(a_fraction, b_fraction)) {
            // beq, addiu (taken bc1f leads here).
            result = 1U;
            retired += 2U;
        } else if (fpu::lt(a_fraction, b_fraction)) {
            // c.olt.s, bc1f (not taken), nop, beq, daddu.
            result = 0U;
            retired += 3U + 2U;
        } else {
            // c.olt.s, bc1f (taken), nop, lw, lw, lb, lb, beq, slt.
            retired += 3U + 6U;
            const GuestAddress b_owner = m.read_u32_le(b + kOwner);
            const GuestAddress a_owner = m.read_u32_le(a + kOwner);
            const auto b_rank = static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(b_owner + kRank)));
            const auto a_rank = static_cast<std::int32_t>(static_cast<std::int8_t>(m.read_u8(a_owner + kRank)));
            result = b_rank < a_rank ? 1U : 0U;
        }
    } else {
        // lui, lbu, bne and its delay slot nop.
        retired += 4U;
        if (m.read_u8(kFlag) != 0U) {
            // beq, slt.
            result = diff > 0 ? 1U : 0U;
            retired += 2U;
        } else {
            // blez and its delay slot nop.
            retired += 2U;
            if (diff > 0) {
                // slt, beq, nop.
                retired += 3U;
                if (diff < half) {
                    // beq, addiu.
                    result = 1U;
                    retired += 2U;
                } else {
                    // blez, nop (not taken), slt, bne, nop (not taken), beq, daddu.
                    result = 0U;
                    retired += 2U + 3U + 2U;
                }
            } else {
                // diff < 0 here (diff != 0). blez, nop (taken), bltz, nop (taken),
                // subu, slt, bne, nop.
                retired += 2U + 2U + 4U;
                const auto neg_half = static_cast<std::int32_t>(0U - static_cast<std::uint32_t>(half));
                if (diff < neg_half) {
                    // beq, addiu.
                    result = 1U;
                    retired += 2U;
                } else {
                    // daddu.
                    result = 0U;
                    retired += 1U;
                }
            }
        }
    }
    // jr and its delay slot nop: every path ends here.
    retired += 2U;
    call.return_value(result);
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_0015acc0{0x0015acc0U, 240U, 0x2af8fb92a1327f07ULL, "RingPositionOrder", 35U,
                                                  n3::live<&ring_position_order<NativeFunctionContext>>, EngineReturn::Integer};

} // namespace takedown::runtime

N3_BODY_PLAIN(0015acc0, ring_position_order)
