// 0x001040b8 DmaStartNormal (a0 = DMA channel register block S, a1 = source address X, a2 = quadword count Q):
// starts a normal-mode transfer from memory. S+0 is CHCR, S+16 MADR, S+32 QWC.
//   1. X is converted with ConvertDmaAddress (0x00103cc0: scratchpad 0x7xxxxxxx -> 0x8xxxxxxx),
//   2. WaitWhileBusyFlag (0x00103bc8) waits for the channel to be idle (declines when the busy bit is set),
//   3. MADR = converted X unless MADR holds 0xffffffff, 4. QWC = Q,
//   5. CHCR = (CHCR & ~0xc) | 0x101 (from memory, normal mode, STR = 1: this last store starts the transfer).
// Returns the new CHCR value in v0. The channel registers are device registers; every store happens in the dry run too.

#include "takedown/runtime/game/n3_support.hpp"

N3_CALLEE(00103cc0)
N3_CALLEE(00103bc8)

namespace takedown::runtime {
namespace {

using n3::GuestAddress;

constexpr std::uint32_t kFrameBytes = 64U;
constexpr GuestAddress kMadrOffset = 16U;
constexpr GuestAddress kQwcOffset = 32U;
constexpr std::uint32_t kMadrUnset = 0xffffffffU;
constexpr std::uint32_t kModeMask = 0xcU;           // DIR and MOD bits cleared
constexpr std::uint32_t kStartNormal = 0x101U;      // STR | DIR (from memory), normal mode
constexpr std::uint32_t kOwnCost = 28U;             // own instructions with MADR unset; +1 (the delay-slot store) when set
constexpr std::uint32_t kCeiling = 20000U;

template <class Context>
n3::Run dma_start_normal(Context& context) {
    auto call = n3::guest_call(context);
    const GuestAddress channel = call.arg(0);
    const std::uint32_t quadwords = call.arg(2);

    call.set_arg64(0, call.arg64(1));
    const n3::Run convert = call.invoke([](auto& m) { return n3::fn_00103cc0(m); }, kFrameBytes);
    if (convert.declined) {
        return call.decline();
    }
    const std::uint32_t source = static_cast<std::uint32_t>(call.result64());

    call.set_arg(0, channel);
    const n3::Run wait = call.invoke([](auto& m) { return n3::fn_00103bc8(m); }, kFrameBytes);
    if (wait.declined) {
        return call.decline();
    }

    auto& m = call.memory();
    const bool madr_set = m.read_u32_le(channel + kMadrOffset) != kMadrUnset;
    if (madr_set) {
        m.write_u32_le(channel + kMadrOffset, source);
    }
    m.write_u32_le(channel + kQwcOffset, quadwords);
    const std::uint32_t control = (m.read_u32_le(channel) & ~kModeMask) | kStartNormal;
    m.write_u32_le(channel, control);

    call.return_value(control);
    return call.finish(kOwnCost + (madr_set ? 1U : 0U) + convert.retired + wait.retired);
}

} // namespace
} // namespace takedown::runtime

N3_ENTRY(001040b8, 116U, 0xd9a8cf3fbd0272a0ULL, "DmaStartNormal", kCeiling, dma_start_normal, EngineReturn::Integer)
