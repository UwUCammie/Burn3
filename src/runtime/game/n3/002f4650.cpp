// 0x002f4650 (a0 = object, a1 = flag): AppendDmaRefUnpackTag. Appends one 16-byte DMA REF tag quadword to the
// packet buffer at the write pointer stored at $gp-24240. A non-zero a1 (the whole 64-bit register) first appends
// a 16-byte filler quadword. The tag's address is the pointer at object+16 and its count is the low 15 bits of the
// word it points at, plus one. The write pointer then moves past the new quadword. Returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::int32_t kWritePointerOffset = -24240;   // lw/sw at $gp-24240: the packet buffer's write pointer
constexpr std::uint32_t kQuadBytes = 16U;
constexpr std::uint32_t kFillerHalf = 0x10000000U;     // filler quadword: lui 0x1000, copied to both halves
constexpr std::uint32_t kRefTagId = 0x30000000U;       // DMA tag ID 3 (REF) in bits 28..30 of the low word
constexpr std::uint32_t kVifLowWord = 0x01000101U;     // word 2 of the tag quadword
constexpr std::uint32_t kVifCode = 0x6c0003b5U;        // word 3 of the tag quadword, the count ORed into bits 16..31
constexpr std::uint32_t kCountMask = 0x7fffU;
constexpr std::uint32_t kBaseCost = 33U;               // a1 == 0 path, delay slots included
constexpr std::uint32_t kFillerCost = 5U;              // a1 != 0 path: the filler quadword's five instructions

template <class Context>
n3::Run append_dma_ref_unpack_tag(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    // bne $a1,$zero tests the whole register, so the 64-bit value decides the branch.
    const bool filler = call.arg64(1U) != 0U;
    const GuestAddress object = call.arg(0U);
    const GuestAddress slot =
        call.cpu().read_gpr_u32(n3::kGlobalPointer) + static_cast<GuestAddress>(kWritePointerOffset);
    GuestAddress cursor = m.read_u32_le(slot);
    std::uint32_t retired = kBaseCost;
    if (filler) {
        n3::store_quad_any(m, cursor, n3::Gpr128{{kFillerHalf, 0U, kFillerHalf, 0U}});
        cursor += kQuadBytes;
        retired += kFillerCost;
    }
    const GuestAddress source = m.read_u32_le(object + 16U);
    const std::uint32_t count = (m.read_u32_le(source) & kCountMask) + 1U;
    n3::store_quad_any(m, cursor,
                   n3::Gpr128{{kRefTagId | count, source, kVifLowWord, (count << 16U) | kVifCode}});
    m.write_u32_le(slot, cursor + kQuadBytes);
    return call.finish(retired);
}

} // namespace

extern constinit const EngineFunction n3_002f4650{0x002f4650U, 152U, 0x92c521a95105d3e7ULL, "AppendDmaRefUnpackTag",
                                                  38U, n3::live<&append_dma_ref_unpack_tag<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(002f4650, append_dma_ref_unpack_tag)
