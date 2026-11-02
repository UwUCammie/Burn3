// 0x00104050 DmaStartChain (a0 = DMA channel register block S, a1 = address X of the first DMA tag): starts a
// source-chain transfer on the channel.
//   1. X is converted with ConvertDmaAddress (0x00103cc0: scratchpad 0x7xxxxxxx -> 0x8xxxxxxx),
//   2. WaitWhileBusyFlag (0x00103bc8) waits for the channel's busy bit (CHCR bit 8) to clear; a set bit is declined,
//      so the original runs its spin,
//   3. TADR (S+48) = converted X, unless TADR holds 0xffffffff,
//   4. QWC (S+32) = 0,
//   5. CHCR (S+0) = (CHCR & ~0xc) | 0x105 (from memory, chain mode, STR = 1): this store starts the transfer, so it
//      comes last. These are device registers: the three accesses are exactly the original's.
// Returns the new CHCR value in v0. Own instructions: 26 when TADR is written (bnel taken, delay slot runs), 25 when not.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00103cc0)
N3_CALLEE(00103bc8)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFrameBytes = 48U;
constexpr GuestAddress kChcr = 0U;
constexpr GuestAddress kQwc = 32U;
constexpr GuestAddress kTadr = 48U;
constexpr std::uint32_t kTadrNone = 0xffffffffU;      // TADR value that is left alone
constexpr std::uint32_t kChcrClear = ~0xcU;           // addiu $v1, $zero, -13
constexpr std::uint32_t kChcrStart = 0x0105U;         // STR | MOD chain | DIR from memory
constexpr std::uint32_t kOwnWithTadr = 26U;
constexpr std::uint32_t kOwnWithoutTadr = 25U;
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run dma_start_chain(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress channel = call.arg(0);

    call.set_arg64(0, call.arg64(1));
    const n3::Run convert = call.invoke([](auto& m) { return n3::fn_00103cc0(m); }, kFrameBytes);
    if (convert.declined) {
        return call.decline();
    }
    const std::uint32_t tag_address = call.result();

    call.set_arg(0, channel);
    const n3::Run wait = call.invoke([](auto& m) { return n3::fn_00103bc8(m); }, kFrameBytes);
    if (wait.declined) {
        return call.decline();
    }

    auto& m = call.memory();
    const bool tadr_written = m.read_u32_le(channel + kTadr) != kTadrNone;
    if (tadr_written) {
        m.write_u32_le(channel + kTadr, tag_address);
    }
    m.write_u32_le(channel + kQwc, 0U);
    const std::uint32_t chcr = (m.read_u32_le(channel + kChcr) & kChcrClear) | kChcrStart;
    m.write_u32_le(channel + kChcr, chcr);

    call.return_value(chcr);
    return call.finish((tadr_written ? kOwnWithTadr : kOwnWithoutTadr) + convert.retired + wait.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(00104050, 104U, 0x7898d7b35fd97113ULL, "DmaStartChain", kCeiling, dma_start_chain, EngineReturn::Integer)
