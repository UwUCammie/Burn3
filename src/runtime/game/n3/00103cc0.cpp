// 0x00103cc0 ConvertDmaAddress (a0 = address): turns a scratchpad address (bits 31..28 of the low word equal 7, i.e.
// 0x7xxxxxxx) into the 0x8xxxxxxx form the DMA engine wants; any other address comes back unchanged (whole 64-bit
// register). Leaf, no stores.
//
//   other address:      4 + 2 = 6 retired
//   scratchpad address: 4 + 4 + 2 = 10 retired

#include "takedown/runtime/game/n3_support.hpp"

namespace takedown::runtime {
namespace {

constexpr std::uint32_t kScratchpadNibble = 7U;       // addiu $v1, $zero, 7 against a0 >> 28
constexpr std::uint32_t kLowMask = 0x0fffffffU;       // lui 0x0fff / ori 0xffff
constexpr std::uint32_t kDmaBase = 0x80000000U;       // lui $v1, 0x8000
constexpr std::uint32_t kOtherPathCost = 6U;
constexpr std::uint32_t kScratchpadPathCost = 10U;

template <class Context>
n3::Run convert_dma_address(Context& context) {
    auto call = n3::guest_call(context);
    const std::uint64_t address = call.arg64(0);
    if ((static_cast<std::uint32_t>(address) >> 28U) == kScratchpadNibble) {
        call.return_value((static_cast<std::uint32_t>(address) & kLowMask) | kDmaBase);
        return call.finish(kScratchpadPathCost);
    }
    call.return_value64(address);
    return call.finish(kOtherPathCost);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY_PLAIN(00103cc0, 40U, 0xe050b588af531273ULL, "ConvertDmaAddress", 10U, convert_dma_address,
               EngineReturn::Integer)
