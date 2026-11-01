// 0x002f3b60 (no arguments, leaf): BuildStaticCallDmaPacket. Appends a fixed-shape DMA/GIF packet at the
// global write cursor c = read_u32(gp - 24240) and stores the advanced cursor back to that word as the
// last store (in the jr delay slot). Ten quadwords are copied out of a record row of the table at
// 0x00665ec0: row X = read_u32(0x00667050) starts 560 * X bytes in, and the copied quadwords start 1424
// bytes into that row. The copy goes to c + 48 onwards and the tail packet follows it at c + 208.
//
// The copy loop counts a3 from 0 and leaves when a3 reaches 10 (slti a3, 10), so it runs exactly ten times
// on every path and the count is fixed at 38 + 10 * 7 + 25 = 133 instructions per call: no call_cost
// resolver is needed. Each quadword is loaded and then stored before the next load, as the listing does,
// because the source and the destination could overlap. Callers ignore results (the listing leaves a
// temporary in v1), so the native returns nothing.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;
using n3::Gpr128;

// Distances below $gp of the cursor word and of the word that receives the key K.
constexpr std::uint32_t kBelowCursor = 24240U;
constexpr std::uint32_t kBelowKeyWord = 28060U;
constexpr std::uint32_t kKey = 0x00475910U;          // lui 0x0047, addiu 22800
constexpr GuestAddress kRowIndexWord = 0x00667050U;  // lui 0x0066, lw 28752
constexpr GuestAddress kTable = 0x00665ec0U;         // lui 0x0066, addiu 24256
constexpr std::uint32_t kRowStride = 560U;           // 7X (subu), 35X, then 560X: the listing's shifts and adds
constexpr std::uint32_t kRowOffset = 1424U;
constexpr std::uint32_t kCopyCount = 10U;

// Retired instruction counts from the listing, delay slots included.
constexpr std::uint32_t kHeadCost = 38U;     // 0x002f3b60..0x002f3bf4, up to the first copy load
constexpr std::uint32_t kCopyStepCost = 7U;  // 0x002f3bf8..0x002f3c10: lq, addiu, slti, sq, addiu, bne, addiu
constexpr std::uint32_t kTailCost = 25U;     // 0x002f3c14..0x002f3c74: tail stores, jr $ra and its delay slot
constexpr std::uint32_t kRetired = kHeadCost + kCopyCount * kCopyStepCost + kTailCost;
static_assert(kRetired == 133U, "the copy loop runs exactly ten times");

// A quadword from its two 64-bit halves (lo holds bits 63:0), as the listing assembles them with pcpyld.
constexpr Gpr128 make_quad(const std::uint64_t lo, const std::uint64_t hi) {
    return Gpr128{{static_cast<std::uint32_t>(lo), static_cast<std::uint32_t>(lo >> 32U),
                   static_cast<std::uint32_t>(hi), static_cast<std::uint32_t>(hi >> 32U)}};
}

template <class Context>
n3::Run build_static_call_dma_packet(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress gp = call.cpu().read_gpr_u32(n3::kGlobalPointer);
    const GuestAddress cursor = m.read_u32_le(gp - kBelowCursor);

    // Head, in listing order: quadword c+0, the key word, then quadwords c+16 and c+32.
    n3::store_quad_any(m, cursor + 0U, make_quad((std::uint64_t{kKey} << 32U) | 0x50000000U, 0U));
    m.write_u32_le(gp - kBelowKeyWord, kKey);
    n3::store_quad_any(m, cursor + 16U, make_quad(0x10000000U, 0x15000000U));
    n3::store_quad_any(m, cursor + 32U, make_quad(0x1000000aU, (std::uint64_t{0x6c0a03f6U} << 32U) | 0x01000101U));

    // Copy: the row index is read after the head stores, as the listing reads it. Each quadword is loaded
    // from the row and stored at c + 48 + 16*i before the next load (lq then sq, ten times).
    const GuestAddress row_index = m.read_u32_le(kRowIndexWord);
    const GuestAddress source = kTable + kRowStride * row_index + kRowOffset;
    for (std::uint32_t i = 0U; i < kCopyCount; ++i) {
        const Gpr128 copied = n3::load_quad_any(m, source + 16U * i);
        n3::store_quad_any(m, cursor + 48U + 16U * i, copied);
    }

    // Tail packet at d = c + 208 (the copy ends at c + 48 + 160). The cursor is stored last, as the
    // listing does in the jr delay slot, and moves to d + 64.
    const GuestAddress tail = cursor + 208U;
    n3::store_quad_any(m, tail + 0U, make_quad(0x10000003U, std::uint64_t{0x50000003U} << 32U));
    n3::store_quad_any(m, tail + 16U, make_quad((std::uint64_t{0x10000000U} << 32U) | 0x8002U, 14U));
    n3::store_quad_any(m, tail + 32U, make_quad(68U, 66U));
    n3::store_quad_any(m, tail + 48U, make_quad(0x00070003U, 71U));
    m.write_u32_le(gp - kBelowCursor, tail + 64U);

    return call.finish(kRetired);
}

} // namespace

extern constinit const EngineFunction n3_002f3b60{0x002f3b60U, 280U, 0xa3cbfb19f90c7550ULL, "BuildStaticCallDmaPacket",
                                                  kRetired, n3::live<&build_static_call_dma_packet<NativeFunctionContext>>, EngineReturn::Nothing};

} // namespace takedown::runtime

N3_BODY_PLAIN(002f3b60, build_static_call_dma_packet)
