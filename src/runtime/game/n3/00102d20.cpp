// 0x00102d20 (a0 = GIF DMA tag pointer): StartGifDmaFromTag. Waits while the GIF channel (DMAC channel 2) is busy
// (CHCR bit 8, STR), then starts a transfer from memory at the tag: QWC = the tag's first word & 0x7fff, plus one;
// MADR = the tag address, with bit 31 set when the tag lies in the scratchpad (bits 28-30 all set); CHCR = 0x101.
// The stores go in that order and v0 returns 0. A wait that retires more than kCeiling instructions, or reaches the
// timeout path (a cold call to 0x0010e048), is declined: call_cost returns 0 and the original runs the call.

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr GuestAddress kGifChcr = 0x1000a000U;      // CHCR of the GIF channel (bit 8 = STR, transfer busy)
constexpr GuestAddress kGifMadr = 0x1000a010U;      // MADR of the GIF channel
constexpr GuestAddress kGifQwc = 0x1000a020U;       // QWC of the GIF channel
constexpr std::uint32_t kStrBit = 0x0100U;
constexpr std::uint32_t kStartFromMemory = 0x0101U; // CHCR value that starts a transfer from memory
constexpr std::uint32_t kQwcMask = 0x7fffU;
constexpr std::uint32_t kMadrMask = 0x0fffffffU;
constexpr std::uint32_t kScratchpadRegion = 0x70000000U;
constexpr std::uint32_t kScratchpadFlag = 0x80000000U;
// The wait gives up (the cold timeout path) once its pass counter is above this.
constexpr std::uint32_t kTimeoutPasses = 0x01000000U;

// Retired instruction counts of the listing's blocks, delay slots included (brief.py's table).
constexpr std::uint32_t kEntryCost = 9U;            // 0x00102d20-0x00102d40: CHCR test and its delay slot
constexpr std::uint32_t kWaitSetupCost = 5U;        // 0x00102d44-0x00102d54: setup of the busy case
constexpr std::uint32_t kPassTestCost = 3U;         // 0x00102d58-0x00102d60: timeout test and its delay slot
constexpr std::uint32_t kPassPollCost = 4U;         // 0x00102d64-0x00102d70: CHCR re-read, STR test and delay slot
constexpr std::uint32_t kTagCost = 12U;             // 0x00102d74-0x00102da0: tag read, QWC store, scratchpad test
constexpr std::uint32_t kScratchpadMadrCost = 7U;   // 0x00102da4-0x00102dbc: scratchpad MADR value and its branch
constexpr std::uint32_t kMemoryMadrCost = 4U;       // 0x00102dd4-0x00102de0: main-memory MADR value
constexpr std::uint32_t kStartCost = 6U;            // 0x00102de4-0x00102df8: MADR and CHCR stores, v0
constexpr std::uint32_t kReturnCost = 3U;           // 0x00102dfc-0x00102e04: ra reload, jr and its delay slot

// Calls retiring more than this are left to the original (call_cost returns 0), so the count a call reports is
// always the original's exact count.
constexpr std::uint32_t kCeiling = 20000U;

// The retired count of a call, and whether its path reaches the cold timeout call or runs past kCeiling.
struct DmaStart {
    std::uint32_t retired{};
    bool declined{};
};

// The whole function on guest memory (the implementation) or on a DryMemory (call_cost). The stores come after the
// wait loop, so a declined run has changed no guest memory.
template <class Memory>
DmaStart start_gif_dma(Memory& m, const GuestAddress tag) {
    std::uint32_t retired = kEntryCost;
    if ((m.read_u32_le(kGifChcr) & kStrBit) != 0U) {
        retired += kWaitSetupCost;
        // The pass counter is tested before it is incremented, as the original's sltu runs before its delay-slot addiu.
        std::uint32_t passes = 0U;
        for (;;) {
            if (retired > kCeiling) {
                return {retired, true};
            }
            retired += kPassTestCost;
            if (passes > kTimeoutPasses) {
                return {retired, true};
            }
            ++passes;
            const std::uint32_t chcr = m.read_u32_le(kGifChcr);
            retired += kPassPollCost;
            if ((chcr & kStrBit) == 0U) {
                break;
            }
        }
    }
    // The tag's low word is all the original keeps (ld, then andi 0x7fff). QWC is stored before MADR and CHCR.
    const std::uint32_t first_word = static_cast<std::uint32_t>(m.read_u64_le(tag));
    m.write_u32_le(kGifQwc, (first_word & kQwcMask) + 1U);
    retired += kTagCost;
    const bool scratchpad = (tag & kScratchpadRegion) == kScratchpadRegion;
    std::uint32_t madr = tag & kMadrMask;
    if (scratchpad) {
        madr |= kScratchpadFlag;
        retired += kScratchpadMadrCost;
    } else {
        retired += kMemoryMadrCost;
    }
    m.write_u32_le(kGifMadr, madr);
    m.write_u32_le(kGifChcr, kStartFromMemory);
    return {retired + kStartCost + kReturnCost, false};
}

// call_cost: the same body on a DryMemory, so no guest state changes. The dry run's memory is static, so a wait that
// starts busy stays busy and runs past kCeiling: that call is declined and the original runs it.
template <class Context>
std::uint32_t start_gif_dma_cost(const Context& context) {
    n3::DryMemory dry{context.memory};
    const DmaStart result = start_gif_dma(dry, context.cpu.read_gpr_u32(4U));
    return (result.declined || result.retired > kCeiling) ? 0U : result.retired;
}

template <class Context>
n3::Run start_gif_dma_from_tag(Context& context) {
    auto call = n3::guest_call(context);
    // The runtime runs this only when call_cost returned a count, so this call is not declined.
    const DmaStart result = start_gif_dma(call.memory(), call.arg(0));
    call.return_value(0U);
    return call.finish(result.retired);
}

} // namespace

extern constinit const EngineFunction n3_00102d20{0x00102d20U, 232U, 0xb7c0284c9ef24c51ULL, "StartGifDmaFromTag",
                                                  kCeiling, n3::live<&start_gif_dma_from_tag<NativeFunctionContext>>, EngineReturn::Integer, {}, 1e-4,
                                                  1e-6, {}, &start_gif_dma_cost<NativeFunctionPrepareContext>};

} // namespace takedown::runtime

N3_BODY(00102d20, start_gif_dma_from_tag, start_gif_dma_cost)
